#include "capture.hpp"

#include "frame_store.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "safety.hpp"
#include "settings.hpp"

#include <memory/mappedmemory.h>

#include <coreinit/cache.h>
#include <coreinit/debug.h>
#include <coreinit/memory.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/surface.h>

#include <turbojpeg.h>
#include <wups.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <vector>

namespace {

constexpr uint32_t MAX_CAPTURE_WIDTH = 4096;
constexpr uint32_t MAX_CAPTURE_HEIGHT = 2160;
constexpr uint32_t MAX_CAPTURE_BYTES = 32 * 1024 * 1024;
constexpr uint32_t ENCODER_STACK_SIZE = 128 * 1024;
constexpr size_t MAX_ENCODER_WORK_BYTES = 24 * 1024 * 1024;

enum : uint32_t { ENCODER_COMMAND_PROCESS = 1, ENCODER_COMMAND_STOP = 2 };

struct CaptureContext {
    VideoSource source;
    GX2ColorBuffer linearBuffer{};
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    bool convertLinearRgbToSrgb = false;
    std::atomic_bool busy{false};
    std::atomic_bool oneShotRequested{false};
    std::atomic_int adaptiveFps{0};
    uint64_t lastCaptureTime = 0;
    uint64_t lastBusyTime = 0;
    uint64_t lastAdaptiveIncreaseTime = 0;
};

struct EncoderWorker {
    OSThread *thread = nullptr;
    uint8_t *stack = nullptr;
    OSMessageQueue queue{};
    OSMessage messages[4]{};
    bool setup = false;
};

std::atomic_bool gRunning{false};
CaptureContext gTVContext{VideoSource::TV};
CaptureContext gGamePadContext{VideoSource::GamePad};
EncoderWorker gEncoder;
std::atomic_uint32_t gTVSurfaceFormat{static_cast<uint32_t>(GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8)};
std::atomic_uint32_t gGamePadSurfaceFormat{static_cast<uint32_t>(GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8)};
GX2ColorBuffer gLastTVColorBuffer{};
GX2ColorBuffer gLastGamePadColorBuffer{};
std::atomic_bool gHaveLastTV{false};
std::atomic_bool gHaveLastGamePad{false};

CaptureContext &ContextFor(VideoSource source) { return source == VideoSource::TV ? gTVContext : gGamePadContext; }
int TargetFps(VideoSource source) {
    int target = std::clamp(source == VideoSource::TV ? Settings::tvFps.load() : Settings::gamepadFps.load(),
                            1, Settings::HighRiskAccepted() ? 60 : 15);
    const int safetyCap = Safety::FpsCap(source);
    if (safetyCap > 0) target = std::min(target, safetyCap);
    return target;
}
bool SourceEnabled(VideoSource source) {
    if (!Settings::enabled.load()) return false;
    return source == VideoSource::TV ? Settings::tvEnabled.load() : Settings::gamepadEnabled.load();
}

int EffectiveFps(CaptureContext &context, uint64_t now) {
    const int target = TargetFps(context.source);
    if (!Settings::adaptiveFps.load()) {
        context.adaptiveFps.store(target);
        Metrics::SetEffectiveFps(context.source, target);
        return target;
    }

    int current = context.adaptiveFps.load();
    if (current <= 0 || current > target) current = target;

    const uint64_t recoverDelay = OSMillisecondsToTicks(2000);
    const uint64_t recoverStep = OSMillisecondsToTicks(1000);
    if (current < target &&
        (context.lastBusyTime == 0 || now - context.lastBusyTime > recoverDelay) &&
        (context.lastAdaptiveIncreaseTime == 0 || now - context.lastAdaptiveIncreaseTime > recoverStep)) {
        ++current;
        context.lastAdaptiveIncreaseTime = now;
    }

    context.adaptiveFps.store(current);
    Metrics::SetEffectiveFps(context.source, current);
    return current;
}

void PenalizeAdaptive(CaptureContext &context, uint64_t now) {
    if (!Settings::adaptiveFps.load()) return;
    int current = context.adaptiveFps.load();
    if (current <= 0) current = TargetFps(context.source);
    current = std::max(2, current - 1);
    context.adaptiveFps.store(current);
    context.lastBusyTime = now;
    Metrics::SetEffectiveFps(context.source, current);
}

void OutputDimensions(VideoSource source, uint32_t &width, uint32_t &height) {
    Settings::OutputDimensions(source == VideoSource::GamePad, width, height);
    Safety::ClampDimensions(source, width, height);
}

std::array<uint8_t, 256> BuildLinearToSrgbTable() {
    std::array<uint8_t, 256> table{};
    for (size_t i = 0; i < table.size(); ++i) {
        const double linear = static_cast<double>(i) / 255.0;
        const double srgb = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
        table[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(srgb * 255.0 + 0.5), 0, 255));
    }
    return table;
}
const std::array<uint8_t, 256> &LinearToSrgbTable() { static const auto table = BuildLinearToSrgbTable(); return table; }

void FreeLinearBuffer(CaptureContext &context) {
    if (context.linearBuffer.surface.image != nullptr && MEMFreeToMappedMemory != nullptr) MEMFreeToMappedMemory(context.linearBuffer.surface.image);
    std::memset(&context.linearBuffer, 0, sizeof(context.linearBuffer));
    context.sourceWidth = 0; context.sourceHeight = 0;
}

bool PrepareLinearBuffer(CaptureContext &context, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > MAX_CAPTURE_WIDTH || height > MAX_CAPTURE_HEIGHT) return false;
    if (context.linearBuffer.surface.image != nullptr && context.sourceWidth == width && context.sourceHeight == height) return true;
    FreeLinearBuffer(context);

    GX2ColorBuffer &target = context.linearBuffer;
    std::memset(&target, 0, sizeof(target));
    target.surface.use = static_cast<GX2SurfaceUse>(GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
    target.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    target.surface.width = width; target.surface.height = height; target.surface.depth = 1; target.surface.mipLevels = 1;
    target.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    target.surface.aa = GX2_AA_MODE1X; target.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    target.viewMip = 0; target.viewFirstSlice = 0; target.viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&target.surface);
    GX2InitColorBufferRegs(&target);

    if (target.surface.imageSize == 0 || target.surface.imageSize > MAX_CAPTURE_BYTES || MEMAllocFromMappedMemoryForGX2Ex == nullptr) {
        Log::Error("invalid mapped buffer request: %u bytes", target.surface.imageSize);
        std::memset(&target, 0, sizeof(target)); return false;
    }
    target.surface.image = MEMAllocFromMappedMemoryForGX2Ex(target.surface.imageSize, target.surface.alignment);
    if (target.surface.image == nullptr) {
        Log::Error("mapped allocation failed: %u bytes", target.surface.imageSize);
        std::memset(&target, 0, sizeof(target)); return false;
    }
    context.sourceWidth = width; context.sourceHeight = height;
    Log::Info("%s capture buffer %ux%u, %u bytes", VideoSourceName(context.source), width, height, target.surface.imageSize);
    return true;
}

bool CopyColorBufferToLinear(CaptureContext &context, const GX2ColorBuffer *source) {
    if (source == nullptr || !PrepareLinearBuffer(context, source->surface.width, source->surface.height)) return false;
    GX2ColorBuffer &target = context.linearBuffer;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, target.surface.image, target.surface.imageSize);

    GX2Surface temporaryResolved{};
    bool temporaryAllocated = false;
    if (source->surface.aa == GX2_AA_MODE1X) {
        GX2CopySurface(&source->surface, source->viewMip, source->viewFirstSlice, &target.surface, 0, 0);
    } else {
        temporaryResolved = source->surface; temporaryResolved.aa = GX2_AA_MODE1X;
        GX2CalcSurfaceSizeAndAlignment(&temporaryResolved);
        if (temporaryResolved.imageSize == 0 || temporaryResolved.imageSize > MAX_CAPTURE_BYTES || MEMAllocFromMappedMemoryForGX2Ex == nullptr) return false;
        temporaryResolved.image = MEMAllocFromMappedMemoryForGX2Ex(temporaryResolved.imageSize, temporaryResolved.alignment);
        if (temporaryResolved.image == nullptr) return false;
        temporaryAllocated = true;
        GX2ResolveAAColorBuffer(source, &temporaryResolved, 0, 0);
        GX2CopySurface(&temporaryResolved, 0, 0, &target.surface, 0, 0);
    }

    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, target.surface.image, target.surface.imageSize);
    GX2DrawDone();
    if (temporaryAllocated && MEMFreeToMappedMemory != nullptr) MEMFreeToMappedMemory(temporaryResolved.image);
    return true;
}

bool BuildScaledRgba(CaptureContext &context, std::vector<uint8_t> &scratch, uint32_t outputWidth, uint32_t outputHeight, const uint8_t *&data, int &pitchBytes) {
    const auto &surface = context.linearBuffer.surface;
    if (surface.image == nullptr || surface.pitch == 0 || context.sourceWidth == 0 || context.sourceHeight == 0) return false;
    const auto *sourceBytes = static_cast<const uint8_t *>(surface.image);
    const bool needsScale = context.sourceWidth != outputWidth || context.sourceHeight != outputHeight;
    const bool needsSrgb = context.convertLinearRgbToSrgb;
    if (!needsScale && !needsSrgb) { data = sourceBytes; pitchBytes = static_cast<int>(surface.pitch * 4); return true; }

    const size_t rgbaBytes = static_cast<size_t>(outputWidth) * outputHeight * 4;
    if (rgbaBytes > MAX_ENCODER_WORK_BYTES) return false;
    try {
        scratch.resize(rgbaBytes);
    } catch (const std::bad_alloc &) {
        Log::Error("RGBA scratch allocation failed for %s: %u x %u", VideoSourceName(context.source), outputWidth, outputHeight);
        return false;
    }
    const auto &srgb = LinearToSrgbTable();
    const uint32_t sourcePitch = surface.pitch;
    for (uint32_t y = 0; y < outputHeight; ++y) {
        const uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(y) * context.sourceHeight) / outputHeight);
        for (uint32_t x = 0; x < outputWidth; ++x) {
            const uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(x) * context.sourceWidth) / outputWidth);
            const size_t si = (static_cast<size_t>(sy) * sourcePitch + sx) * 4;
            const size_t di = (static_cast<size_t>(y) * outputWidth + x) * 4;
            uint8_t r = sourceBytes[si], g = sourceBytes[si + 1], b = sourceBytes[si + 2];
            if (needsSrgb) { r = srgb[r]; g = srgb[g]; b = srgb[b]; }
            scratch[di] = r; scratch[di + 1] = g; scratch[di + 2] = b; scratch[di + 3] = sourceBytes[si + 3];
        }
    }
    data = scratch.data(); pitchBytes = static_cast<int>(outputWidth * 4); return true;
}

bool EncodeContext(tjhandle compressor, CaptureContext &context, std::vector<uint8_t> &rgbaScratch) {
    uint32_t outputWidth = 0, outputHeight = 0; OutputDimensions(context.source, outputWidth, outputHeight);
    const uint8_t *input = nullptr; int inputPitch = 0;
    const auto scaleStart = std::chrono::steady_clock::now();
    if (!BuildScaledRgba(context, rgbaScratch, outputWidth, outputHeight, input, inputPitch)) { Metrics::EncodeFailure(context.source); return false; }
    const auto scaleEnd = std::chrono::steady_clock::now();

    int quality = std::clamp(Settings::jpegQuality.load(), 35, Settings::HighRiskAccepted() ? 95 : 85);
    const int safetyQuality = Safety::JpegQualityCap();
    if (safetyQuality > 0) quality = std::min(quality, safetyQuality);
    const unsigned long maximumSize = tjBufSize(static_cast<int>(outputWidth), static_cast<int>(outputHeight), TJSAMP_420);
    const size_t rgbaBudget = static_cast<size_t>(outputWidth) * outputHeight * 4;
    if (maximumSize == 0 || rgbaBudget + static_cast<size_t>(maximumSize) > MAX_ENCODER_WORK_BYTES) {
        Metrics::EncodeFailure(context.source);
        Log::Error("encoder memory budget rejected %s %ux%u", VideoSourceName(context.source), outputWidth, outputHeight);
        return false;
    }
    std::vector<uint8_t> jpeg;
    try {
        jpeg.resize(maximumSize);
    } catch (const std::bad_alloc &) {
        Metrics::EncodeFailure(context.source);
        Log::Error("JPEG buffer allocation failed for %s", VideoSourceName(context.source));
        return false;
    }
    unsigned char *jpegPointer = jpeg.data(); unsigned long jpegSize = maximumSize;
    const auto encodeStart = std::chrono::steady_clock::now();
    const int result = tjCompress2(compressor, input, static_cast<int>(outputWidth), inputPitch, static_cast<int>(outputHeight), TJPF_RGBA,
                                   &jpegPointer, &jpegSize, TJSAMP_420, quality, TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
    const auto encodeEnd = std::chrono::steady_clock::now();
    if (result != 0) { Metrics::EncodeFailure(context.source); Log::Error("JPEG encode failed for %s", VideoSourceName(context.source)); return false; }
    jpeg.resize(jpegSize);
    const uint64_t scaleUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(scaleEnd - scaleStart).count());
    const uint64_t encodeUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(encodeEnd - encodeStart).count());
    Metrics::Encoded(context.source, jpegSize, scaleUs, encodeUs);
    FrameStore::Publish(context.source, std::move(jpeg), outputWidth, outputHeight);
    return true;
}

int32_t EncoderThreadCallback([[maybe_unused]] int argc, const char **argv) {
    auto *worker = (EncoderWorker *) argv;
    tjhandle compressor = tjInitCompress();
    if (compressor == nullptr) Log::Error("tjInitCompress failed");
    std::vector<uint8_t> rgbaScratch; OSMessage message{};
    while (OSReceiveMessage(&worker->queue, &message, OS_MESSAGE_FLAGS_BLOCKING)) {
        if (message.args[0] == ENCODER_COMMAND_STOP) break;
        if (message.args[0] != ENCODER_COMMAND_PROCESS) continue;
        auto *context = static_cast<CaptureContext *>(message.message);
        if (context != nullptr && compressor != nullptr) EncodeContext(compressor, *context, rgbaScratch);
        if (context != nullptr) context->busy.store(false);
    }
    if (compressor != nullptr) tjDestroy(compressor);
    return 0;
}

bool StartEncoderWorker() {
    if (gEncoder.setup) return true;
    OSInitMessageQueue(&gEncoder.queue, gEncoder.messages, static_cast<int32_t>(sizeof(gEncoder.messages) / sizeof(gEncoder.messages[0])));
    gEncoder.thread = static_cast<OSThread *>(memalign(8, sizeof(OSThread)));
    if (gEncoder.thread == nullptr) return false;
    gEncoder.stack = static_cast<uint8_t *>(memalign(0x20, ENCODER_STACK_SIZE));
    if (gEncoder.stack == nullptr) { free(gEncoder.thread); gEncoder.thread = nullptr; return false; }
    if (!OSCreateThread(gEncoder.thread, EncoderThreadCallback, 1, reinterpret_cast<char *>(&gEncoder), gEncoder.stack + ENCODER_STACK_SIZE,
                        ENCODER_STACK_SIZE, 31, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        free(gEncoder.stack); free(gEncoder.thread); gEncoder.stack = nullptr; gEncoder.thread = nullptr; return false;
    }
    OSSetThreadName(gEncoder.thread, "WiiUWebStream Encoder"); OSResumeThread(gEncoder.thread); gEncoder.setup = true; return true;
}

void StopEncoderWorker() {
    if (!gEncoder.setup) return;
    OSMessage stopMessage{}; stopMessage.args[0] = ENCODER_COMMAND_STOP;
    OSSendMessage(&gEncoder.queue, &stopMessage, OS_MESSAGE_FLAGS_BLOCKING);
    if (OSIsThreadSuspended(gEncoder.thread)) OSResumeThread(gEncoder.thread);
    OSJoinThread(gEncoder.thread, nullptr);
    free(gEncoder.stack); free(gEncoder.thread); gEncoder.stack = nullptr; gEncoder.thread = nullptr; gEncoder.setup = false;
}

bool QueueForEncoding(CaptureContext &context) {
    OSMessage message{}; message.message = &context; message.args[0] = ENCODER_COMMAND_PROCESS; OSMemoryBarrier();
    return OSSendMessage(&gEncoder.queue, &message, OS_MESSAGE_FLAGS_NONE);
}

void MaybeCapture(VideoSource source, const GX2ColorBuffer *colorBuffer, GX2SurfaceFormat scanBufferFormat) {
    if (!gRunning.load() || !SourceEnabled(source) || colorBuffer == nullptr || !gEncoder.setup) return;
    CaptureContext &context = ContextFor(source);
    const bool hasStreamClients = FrameStore::ClientCount(source) > 0;
    const bool oneShot = context.oneShotRequested.load();
    if (!hasStreamClients && !oneShot && !(Settings::HighRiskAccepted() && Settings::continuousCapture.load())) return;

    Metrics::CaptureAttempt(source);
    const uint64_t now = OSGetTime();
    const int fps = EffectiveFps(context, now);
    const uint64_t interval = OSMillisecondsToTicks(static_cast<uint32_t>(std::max(1, 1000 / fps)));
    if (context.lastCaptureTime != 0 && (now - context.lastCaptureTime) < interval) { Metrics::DroppedRate(source); return; }

    bool expected = false;
    if (!context.busy.compare_exchange_strong(expected, true)) { Metrics::DroppedBusy(source); PenalizeAdaptive(context, now); return; }
    context.lastCaptureTime = now;
    if (!CopyColorBufferToLinear(context, colorBuffer)) { Metrics::CopyFailure(source); context.busy.store(false); return; }
    Metrics::Copied(source);
    context.convertLinearRgbToSrgb = (static_cast<uint32_t>(scanBufferFormat) & 0x400U) != 0;
    if (!QueueForEncoding(context)) { Metrics::QueueFailure(source); context.busy.store(false); return; }
    context.oneShotRequested.store(false);
}

DECL_FUNCTION(void, GX2CopyColorBufferToScanBuffer, const GX2ColorBuffer *colorBuffer, GX2ScanTarget scanTarget) {
    if (scanTarget == GX2_SCAN_TARGET_TV) MaybeCapture(VideoSource::TV, colorBuffer, static_cast<GX2SurfaceFormat>(gTVSurfaceFormat.load()));
    else if (scanTarget == GX2_SCAN_TARGET_DRC0) MaybeCapture(VideoSource::GamePad, colorBuffer, static_cast<GX2SurfaceFormat>(gGamePadSurfaceFormat.load()));
    real_GX2CopyColorBufferToScanBuffer(colorBuffer, scanTarget);
}
DECL_FUNCTION(void, GX2SetTVBuffer, void *buffer, uint32_t bufferSize, int32_t tvRenderMode, GX2SurfaceFormat surfaceFormat, GX2BufferingMode bufferingMode) {
    gTVSurfaceFormat.store(static_cast<uint32_t>(surfaceFormat)); real_GX2SetTVBuffer(buffer, bufferSize, tvRenderMode, surfaceFormat, bufferingMode);
}
DECL_FUNCTION(void, GX2SetDRCBuffer, void *buffer, uint32_t bufferSize, uint32_t drcMode, GX2SurfaceFormat surfaceFormat, GX2BufferingMode bufferingMode) {
    gGamePadSurfaceFormat.store(static_cast<uint32_t>(surfaceFormat)); real_GX2SetDRCBuffer(buffer, bufferSize, drcMode, surfaceFormat, bufferingMode);
}
DECL_FUNCTION(void, GX2GetCurrentScanBuffer, GX2ScanTarget scanTarget, GX2ColorBuffer *colorBuffer) {
    real_GX2GetCurrentScanBuffer(scanTarget, colorBuffer); if (colorBuffer == nullptr) return;
    if (scanTarget == GX2_SCAN_TARGET_TV) { std::memcpy(&gLastTVColorBuffer, colorBuffer, sizeof(GX2ColorBuffer)); gHaveLastTV.store(true); }
    else if (scanTarget == GX2_SCAN_TARGET_DRC0) { std::memcpy(&gLastGamePadColorBuffer, colorBuffer, sizeof(GX2ColorBuffer)); gHaveLastGamePad.store(true); }
}
DECL_FUNCTION(void, GX2MarkScanBufferCopied, GX2ScanTarget scanTarget) {
    if (scanTarget == GX2_SCAN_TARGET_TV && gHaveLastTV.load()) MaybeCapture(VideoSource::TV, &gLastTVColorBuffer, static_cast<GX2SurfaceFormat>(gTVSurfaceFormat.load()));
    else if (scanTarget == GX2_SCAN_TARGET_DRC0 && gHaveLastGamePad.load()) MaybeCapture(VideoSource::GamePad, &gLastGamePadColorBuffer, static_cast<GX2SurfaceFormat>(gGamePadSurfaceFormat.load()));
    real_GX2MarkScanBufferCopied(scanTarget);
}

WUPS_MUST_REPLACE(GX2CopyColorBufferToScanBuffer, WUPS_LOADER_LIBRARY_GX2, GX2CopyColorBufferToScanBuffer);
WUPS_MUST_REPLACE(GX2SetTVBuffer, WUPS_LOADER_LIBRARY_GX2, GX2SetTVBuffer);
WUPS_MUST_REPLACE(GX2SetDRCBuffer, WUPS_LOADER_LIBRARY_GX2, GX2SetDRCBuffer);
WUPS_MUST_REPLACE(GX2GetCurrentScanBuffer, WUPS_LOADER_LIBRARY_GX2, GX2GetCurrentScanBuffer);
WUPS_MUST_REPLACE(GX2MarkScanBufferCopied, WUPS_LOADER_LIBRARY_GX2, GX2MarkScanBufferCopied);

} // namespace

namespace Capture {
bool Start() {
    bool expected = false; if (!gRunning.compare_exchange_strong(expected, true)) return true;
    FrameStore::Clear(); Metrics::Reset();
    for (auto *context : {&gTVContext, &gGamePadContext}) {
        context->busy.store(false); context->oneShotRequested.store(false); context->adaptiveFps.store(TargetFps(context->source));
        context->lastCaptureTime = 0; context->lastBusyTime = 0; context->lastAdaptiveIncreaseTime = 0;
    }
    if (!StartEncoderWorker()) { gRunning.store(false); Log::Error("failed to start encoder thread"); return false; }
    Log::Info("capture/encoder started"); return true;
}
void Stop() {
    if (!gRunning.exchange(false)) return;
    StopEncoderWorker(); gTVContext.busy.store(false); gGamePadContext.busy.store(false);
    FreeLinearBuffer(gTVContext); FreeLinearBuffer(gGamePadContext); gHaveLastTV.store(false); gHaveLastGamePad.store(false); FrameStore::Clear();
    Log::Info("capture/encoder stopped");
}
bool IsRunning() { return gRunning.load(); }
void RequestOne(VideoSource source) { ContextFor(source).oneShotRequested.store(true); }
} // namespace Capture
