#include "capture.hpp"

#include "frame_store.hpp"
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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <vector>

namespace {

constexpr uint32_t MAX_CAPTURE_WIDTH = 4096;
constexpr uint32_t MAX_CAPTURE_HEIGHT = 2160;
constexpr uint32_t MAX_CAPTURE_BYTES = 32 * 1024 * 1024;
constexpr uint32_t ENCODER_STACK_SIZE = 128 * 1024;

enum : uint32_t {
    ENCODER_COMMAND_PROCESS = 1,
    ENCODER_COMMAND_STOP = 2,
};

struct CaptureContext {
    VideoSource source;
    GX2ColorBuffer linearBuffer{};
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    bool convertLinearRgbToSrgb = false;

    std::atomic_bool busy{false};
    std::atomic_bool oneShotRequested{false};

    uint64_t lastCaptureTime = 0;
};

struct EncoderWorker {
    OSThread *thread = nullptr;
    uint8_t *stack = nullptr;
    OSMessageQueue queue{};
    OSMessage messages[4]{};
    bool setup = false;
};

std::atomic_bool gRunning{false};
std::atomic_int gActiveCaptureCalls{0};
std::atomic_uint32_t gLastGpuReadbackMs{0};

CaptureContext gTVContext{VideoSource::TV};
CaptureContext gGamePadContext{VideoSource::GamePad};
EncoderWorker gEncoder;

std::atomic_uint32_t gTVSurfaceFormat{
        static_cast<uint32_t>(GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8)};
std::atomic_uint32_t gGamePadSurfaceFormat{
        static_cast<uint32_t>(GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8)};

GX2ColorBuffer gLastTVColorBuffer{};
GX2ColorBuffer gLastGamePadColorBuffer{};
std::atomic_bool gHaveLastTV{false};
std::atomic_bool gHaveLastGamePad{false};

CaptureContext &ContextFor(VideoSource source) {
    return source == VideoSource::TV ? gTVContext : gGamePadContext;
}

int ConfiguredFps(VideoSource source) {
    const int fps = source == VideoSource::TV
            ? Settings::tvFps.load()
            : Settings::gamepadFps.load();

    return std::clamp(fps, 1, 15);
}

bool SourceEnabled(VideoSource source) {
    if (!Settings::enabled.load()) {
        return false;
    }

    return source == VideoSource::TV
            ? Settings::tvEnabled.load()
            : Settings::gamepadEnabled.load();
}

void OutputDimensions(VideoSource source, uint32_t &width, uint32_t &height) {
    if (source == VideoSource::TV) {
        width = Settings::TV_OUTPUT_WIDTH;
        height = Settings::TV_OUTPUT_HEIGHT;
    } else {
        width = Settings::GAMEPAD_OUTPUT_WIDTH;
        height = Settings::GAMEPAD_OUTPUT_HEIGHT;
    }
}

std::array<uint8_t, 256> BuildLinearToSrgbTable() {
    std::array<uint8_t, 256> table{};

    for (size_t i = 0; i < table.size(); ++i) {
        const double linear = static_cast<double>(i) / 255.0;
        const double srgb =
                linear <= 0.0031308
                ? linear * 12.92
                : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;

        const int value = static_cast<int>(srgb * 255.0 + 0.5);
        table[i] = static_cast<uint8_t>(std::clamp(value, 0, 255));
    }

    return table;
}

const std::array<uint8_t, 256> &LinearToSrgbTable() {
    static const auto table = BuildLinearToSrgbTable();
    return table;
}

void FreeLinearBuffer(CaptureContext &context) {
    if (context.linearBuffer.surface.image != nullptr &&
        MEMFreeToMappedMemory != nullptr) {
        MEMFreeToMappedMemory(context.linearBuffer.surface.image);
    }

    std::memset(&context.linearBuffer, 0, sizeof(context.linearBuffer));
    context.sourceWidth = 0;
    context.sourceHeight = 0;
}

bool PrepareLinearBuffer(CaptureContext &context,
                         uint32_t width,
                         uint32_t height) {
    if (width == 0 || height == 0 ||
        width > MAX_CAPTURE_WIDTH || height > MAX_CAPTURE_HEIGHT) {
        return false;
    }

    if (context.linearBuffer.surface.image != nullptr &&
        context.sourceWidth == width &&
        context.sourceHeight == height) {
        return true;
    }

    FreeLinearBuffer(context);

    GX2ColorBuffer &target = context.linearBuffer;
    std::memset(&target, 0, sizeof(target));

    target.surface.use =
            static_cast<GX2SurfaceUse>(
                    GX2_SURFACE_USE_COLOR_BUFFER | GX2_SURFACE_USE_TEXTURE);
    target.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    target.surface.width = width;
    target.surface.height = height;
    target.surface.depth = 1;
    target.surface.mipLevels = 1;
    target.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    target.surface.aa = GX2_AA_MODE1X;
    target.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    target.surface.swizzle = 0;
    target.surface.alignment = 0;
    target.surface.pitch = 0;

    target.viewMip = 0;
    target.viewFirstSlice = 0;
    target.viewNumSlices = 1;
    target.aaBuffer = nullptr;
    target.aaSize = 0;

    GX2CalcSurfaceSizeAndAlignment(&target.surface);
    GX2InitColorBufferRegs(&target);

    if (target.surface.imageSize == 0 ||
        target.surface.imageSize > MAX_CAPTURE_BYTES ||
        MEMAllocFromMappedMemoryForGX2Ex == nullptr) {
        OSReport("[WiiUWebStream] Invalid mapped buffer request: %u bytes\n",
                 target.surface.imageSize);
        std::memset(&target, 0, sizeof(target));
        return false;
    }

    target.surface.image =
            MEMAllocFromMappedMemoryForGX2Ex(
                    target.surface.imageSize,
                    target.surface.alignment);

    if (target.surface.image == nullptr) {
        OSReport("[WiiUWebStream] Mapped allocation failed: %u bytes\n",
                 target.surface.imageSize);
        std::memset(&target, 0, sizeof(target));
        return false;
    }

    context.sourceWidth = width;
    context.sourceHeight = height;

    OSReport("[WiiUWebStream] %s capture buffer: %ux%u, %u bytes\n",
             VideoSourceName(context.source),
             width,
             height,
             target.surface.imageSize);

    return true;
}

bool CopyColorBufferToLinear(CaptureContext &context,
                             const GX2ColorBuffer *source) {
    if (source == nullptr) {
        return false;
    }

    const uint32_t width = source->surface.width;
    const uint32_t height = source->surface.height;

    if (!PrepareLinearBuffer(context, width, height)) {
        return false;
    }

    GX2ColorBuffer &target = context.linearBuffer;

    // The worker read this memory after the previous frame. Invalidate CPU
    // cache lines before the GPU writes a new frame into the same mapped buffer.
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU,
                  target.surface.image,
                  target.surface.imageSize);

    GX2Surface temporaryResolved{};
    bool temporaryAllocated = false;

    if (source->surface.aa == GX2_AA_MODE1X) {
        GX2CopySurface(&source->surface,
                       source->viewMip,
                       source->viewFirstSlice,
                       &target.surface,
                       0,
                       0);
    } else {
        temporaryResolved = source->surface;
        temporaryResolved.aa = GX2_AA_MODE1X;

        GX2CalcSurfaceSizeAndAlignment(&temporaryResolved);

        if (temporaryResolved.imageSize == 0 ||
            temporaryResolved.imageSize > MAX_CAPTURE_BYTES ||
            MEMAllocFromMappedMemoryForGX2Ex == nullptr) {
            return false;
        }

        temporaryResolved.image =
                MEMAllocFromMappedMemoryForGX2Ex(
                        temporaryResolved.imageSize,
                        temporaryResolved.alignment);

        if (temporaryResolved.image == nullptr) {
            return false;
        }

        temporaryAllocated = true;

        GX2ResolveAAColorBuffer(source, &temporaryResolved, 0, 0);
        GX2CopySurface(&temporaryResolved,
                       0,
                       0,
                       &target.surface,
                       0,
                       0);
    }

    // Mirror the proven ScreenshotWUPS readback sequence: flush the color
    // destination and wait until GX2 has completed before the CPU encoder reads.
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER,
                  target.surface.image,
                  target.surface.imageSize);
    GX2DrawDone();

    if (temporaryAllocated && MEMFreeToMappedMemory != nullptr) {
        MEMFreeToMappedMemory(temporaryResolved.image);
    }

    return true;
}

bool BuildScaledRgba(CaptureContext &context,
                     std::vector<uint8_t> &scratch,
                     uint32_t outputWidth,
                     uint32_t outputHeight,
                     const uint8_t *&data,
                     int &pitchBytes) {
    const auto &surface = context.linearBuffer.surface;

    if (surface.image == nullptr ||
        surface.pitch == 0 ||
        context.sourceWidth == 0 ||
        context.sourceHeight == 0) {
        return false;
    }

    const auto *sourceBytes =
            static_cast<const uint8_t *>(surface.image);

    const bool needsScale =
            context.sourceWidth != outputWidth ||
            context.sourceHeight != outputHeight;

    const bool needsSrgb = context.convertLinearRgbToSrgb;

    if (!needsScale && !needsSrgb) {
        data = sourceBytes;
        pitchBytes = static_cast<int>(surface.pitch * 4);
        return true;
    }

    const size_t outputSize =
            static_cast<size_t>(outputWidth) *
            static_cast<size_t>(outputHeight) * 4;

    scratch.resize(outputSize);

    const auto &srgb = LinearToSrgbTable();
    const uint32_t sourcePitch = surface.pitch;

    for (uint32_t y = 0; y < outputHeight; ++y) {
        const uint32_t sourceY =
                static_cast<uint32_t>(
                        (static_cast<uint64_t>(y) * context.sourceHeight) /
                        outputHeight);

        for (uint32_t x = 0; x < outputWidth; ++x) {
            const uint32_t sourceX =
                    static_cast<uint32_t>(
                            (static_cast<uint64_t>(x) * context.sourceWidth) /
                            outputWidth);

            const size_t sourceIndex =
                    (static_cast<size_t>(sourceY) * sourcePitch + sourceX) * 4;
            const size_t targetIndex =
                    (static_cast<size_t>(y) * outputWidth + x) * 4;

            uint8_t r = sourceBytes[sourceIndex + 0];
            uint8_t g = sourceBytes[sourceIndex + 1];
            uint8_t b = sourceBytes[sourceIndex + 2];
            const uint8_t a = sourceBytes[sourceIndex + 3];

            if (needsSrgb) {
                r = srgb[r];
                g = srgb[g];
                b = srgb[b];
            }

            scratch[targetIndex + 0] = r;
            scratch[targetIndex + 1] = g;
            scratch[targetIndex + 2] = b;
            scratch[targetIndex + 3] = a;
        }
    }

    data = scratch.data();
    pitchBytes = static_cast<int>(outputWidth * 4);
    return true;
}

bool EncodeContext(tjhandle compressor,
                   CaptureContext &context,
                   std::vector<uint8_t> &rgbaScratch) {
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    OutputDimensions(context.source, outputWidth, outputHeight);

    const uint8_t *input = nullptr;
    int inputPitch = 0;

    if (!BuildScaledRgba(context,
                         rgbaScratch,
                         outputWidth,
                         outputHeight,
                         input,
                         inputPitch)) {
        return false;
    }

    const int quality = std::clamp(Settings::jpegQuality.load(), 10, 95);

    const unsigned long maximumSize =
            tjBufSize(static_cast<int>(outputWidth),
                      static_cast<int>(outputHeight),
                      TJSAMP_420);

    if (maximumSize == 0) {
        return false;
    }

    std::vector<uint8_t> jpeg(maximumSize);
    unsigned char *jpegPointer = jpeg.data();
    unsigned long jpegSize = maximumSize;

    const int result =
            tjCompress2(
                    compressor,
                    input,
                    static_cast<int>(outputWidth),
                    inputPitch,
                    static_cast<int>(outputHeight),
                    TJPF_RGBA,
                    &jpegPointer,
                    &jpegSize,
                    TJSAMP_420,
                    quality,
                    TJFLAG_FASTDCT | TJFLAG_NOREALLOC);

    if (result != 0) {
        OSReport("[WiiUWebStream] JPEG encode failed for %s\n",
                 VideoSourceName(context.source));
        return false;
    }

    jpeg.resize(jpegSize);

    FrameStore::Publish(
            context.source,
            std::move(jpeg),
            outputWidth,
            outputHeight);

    return true;
}

int32_t EncoderThreadCallback([[maybe_unused]] int argc,
                              const char **argv) {
    auto *worker = (EncoderWorker *) argv;

    tjhandle compressor = tjInitCompress();
    if (compressor == nullptr) {
        OSReport("[WiiUWebStream] tjInitCompress failed\n");
    }

    std::vector<uint8_t> rgbaScratch;
    OSMessage message{};

    while (OSReceiveMessage(
            &worker->queue,
            &message,
            OS_MESSAGE_FLAGS_BLOCKING)) {
        if (message.args[0] == ENCODER_COMMAND_STOP) {
            break;
        }

        if (message.args[0] != ENCODER_COMMAND_PROCESS) {
            continue;
        }

        auto *context =
                static_cast<CaptureContext *>(message.message);

        if (context != nullptr && compressor != nullptr) {
            EncodeContext(compressor, *context, rgbaScratch);
        }

        if (context != nullptr) {
            context->busy.store(false);
        }
    }

    if (compressor != nullptr) {
        tjDestroy(compressor);
    }

    return 0;
}

bool StartEncoderWorker() {
    if (gEncoder.setup) {
        return true;
    }

    OSInitMessageQueue(
            &gEncoder.queue,
            gEncoder.messages,
            static_cast<int32_t>(
                    sizeof(gEncoder.messages) /
                    sizeof(gEncoder.messages[0])));

    gEncoder.thread =
            static_cast<OSThread *>(memalign(8, sizeof(OSThread)));
    if (gEncoder.thread == nullptr) {
        return false;
    }

    gEncoder.stack =
            static_cast<uint8_t *>(memalign(0x20, ENCODER_STACK_SIZE));
    if (gEncoder.stack == nullptr) {
        free(gEncoder.thread);
        gEncoder.thread = nullptr;
        return false;
    }

    if (!OSCreateThread(
            gEncoder.thread,
            EncoderThreadCallback,
            1,
            reinterpret_cast<char *>(&gEncoder),
            gEncoder.stack + ENCODER_STACK_SIZE,
            ENCODER_STACK_SIZE,
            31,
            OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        free(gEncoder.stack);
        free(gEncoder.thread);
        gEncoder.stack = nullptr;
        gEncoder.thread = nullptr;
        return false;
    }

    OSSetThreadName(gEncoder.thread, "WiiUWebStream Encoder");
    OSResumeThread(gEncoder.thread);
    gEncoder.setup = true;

    return true;
}

void StopEncoderWorker() {
    if (!gEncoder.setup) {
        return;
    }

    OSMessage stopMessage{};
    stopMessage.args[0] = ENCODER_COMMAND_STOP;

    OSSendMessage(
            &gEncoder.queue,
            &stopMessage,
            OS_MESSAGE_FLAGS_BLOCKING);

    if (OSIsThreadSuspended(gEncoder.thread)) {
        OSResumeThread(gEncoder.thread);
    }

    OSJoinThread(gEncoder.thread, nullptr);

    free(gEncoder.stack);
    free(gEncoder.thread);

    gEncoder.stack = nullptr;
    gEncoder.thread = nullptr;
    gEncoder.setup = false;
}

bool QueueForEncoding(CaptureContext &context) {
    OSMessage message{};
    message.message = &context;
    message.args[0] = ENCODER_COMMAND_PROCESS;

    OSMemoryBarrier();

    return OSSendMessage(
            &gEncoder.queue,
            &message,
            OS_MESSAGE_FLAGS_NONE);
}

class ActiveCaptureGuard {
public:
    ActiveCaptureGuard() {
        gActiveCaptureCalls.fetch_add(1);
    }

    ~ActiveCaptureGuard() {
        gActiveCaptureCalls.fetch_sub(1);
    }
};

void MaybeCapture(VideoSource source,
                  const GX2ColorBuffer *colorBuffer,
                  GX2SurfaceFormat scanBufferFormat) {
    ActiveCaptureGuard activeGuard;

    // Stop() flips gRunning first and waits for every call that already
    // crossed this boundary before freeing mapped GX2 memory.
    if (!gRunning.load() ||
        !SourceEnabled(source) ||
        colorBuffer == nullptr ||
        !gEncoder.setup) {
        return;
    }

    CaptureContext &context = ContextFor(source);

    const bool hasStreamClients =
            FrameStore::ClientCount(source) > 0;
    const bool oneShot =
            context.oneShotRequested.load();

    // Stage 7 optimization: no framebuffer readback when nobody is watching.
    if (!hasStreamClients && !oneShot) {
        return;
    }

    const int fps = ConfiguredFps(source);
    const uint64_t now = OSGetTime();
    const uint64_t interval =
            OSMillisecondsToTicks(
                    static_cast<uint32_t>(
                            std::max(1, 1000 / fps)));

    if (context.lastCaptureTime != 0 &&
        (now - context.lastCaptureTime) < interval) {
        return;
    }

    // Avoid two synchronous GPU readbacks in the same render instant.
    // TV and GamePad naturally become staggered by at least one frame.
    const uint32_t nowMs =
            static_cast<uint32_t>(OSTicksToMilliseconds(now));
    const uint32_t lastGpuMs = gLastGpuReadbackMs.load();
    if (lastGpuMs != 0 && (nowMs - lastGpuMs) < 8) {
        return;
    }

    bool expected = false;
    if (!context.busy.compare_exchange_strong(expected, true)) {
        // Encoder still owns the previous buffer. Drop this frame instead of
        // blocking the game's render thread.
        return;
    }

    context.lastCaptureTime = now;
    gLastGpuReadbackMs.store(nowMs);

    if (!CopyColorBufferToLinear(context, colorBuffer)) {
        context.busy.store(false);
        return;
    }

    context.convertLinearRgbToSrgb =
            (static_cast<uint32_t>(scanBufferFormat) & 0x400U) != 0;

    if (!QueueForEncoding(context)) {
        context.busy.store(false);
        return;
    }

    context.oneShotRequested.store(false);
}

} // namespace

// -----------------------------------------------------------------------------
// Current Aroma/WUPS capture hooks.
// This follows the same GX2 interception points used by ScreenshotWUPS.
// -----------------------------------------------------------------------------

DECL_FUNCTION(void,
              GX2CopyColorBufferToScanBuffer,
              const GX2ColorBuffer *colorBuffer,
              GX2ScanTarget scanTarget) {
    if (scanTarget == GX2_SCAN_TARGET_TV) {
        MaybeCapture(
                VideoSource::TV,
                colorBuffer,
                static_cast<GX2SurfaceFormat>(gTVSurfaceFormat.load()));
    } else if (scanTarget == GX2_SCAN_TARGET_DRC0) {
        MaybeCapture(
                VideoSource::GamePad,
                colorBuffer,
                static_cast<GX2SurfaceFormat>(gGamePadSurfaceFormat.load()));
    }

    real_GX2CopyColorBufferToScanBuffer(colorBuffer, scanTarget);
}

DECL_FUNCTION(void,
              GX2SetTVBuffer,
              void *buffer,
              uint32_t bufferSize,
              int32_t tvRenderMode,
              GX2SurfaceFormat surfaceFormat,
              GX2BufferingMode bufferingMode) {
    gTVSurfaceFormat.store(static_cast<uint32_t>(surfaceFormat));

    real_GX2SetTVBuffer(
            buffer,
            bufferSize,
            tvRenderMode,
            surfaceFormat,
            bufferingMode);
}

DECL_FUNCTION(void,
              GX2SetDRCBuffer,
              void *buffer,
              uint32_t bufferSize,
              uint32_t drcMode,
              GX2SurfaceFormat surfaceFormat,
              GX2BufferingMode bufferingMode) {
    gGamePadSurfaceFormat.store(static_cast<uint32_t>(surfaceFormat));

    real_GX2SetDRCBuffer(
            buffer,
            bufferSize,
            drcMode,
            surfaceFormat,
            bufferingMode);
}

DECL_FUNCTION(void,
              GX2GetCurrentScanBuffer,
              GX2ScanTarget scanTarget,
              GX2ColorBuffer *colorBuffer) {
    real_GX2GetCurrentScanBuffer(scanTarget, colorBuffer);

    if (colorBuffer == nullptr) {
        return;
    }

    if (scanTarget == GX2_SCAN_TARGET_TV) {
        std::memcpy(
                &gLastTVColorBuffer,
                colorBuffer,
                sizeof(GX2ColorBuffer));
        gHaveLastTV.store(true);
    } else if (scanTarget == GX2_SCAN_TARGET_DRC0) {
        std::memcpy(
                &gLastGamePadColorBuffer,
                colorBuffer,
                sizeof(GX2ColorBuffer));
        gHaveLastGamePad.store(true);
    }
}

DECL_FUNCTION(void,
              GX2MarkScanBufferCopied,
              GX2ScanTarget scanTarget) {
    if (scanTarget == GX2_SCAN_TARGET_TV &&
        gHaveLastTV.load()) {
        MaybeCapture(
                VideoSource::TV,
                &gLastTVColorBuffer,
                static_cast<GX2SurfaceFormat>(gTVSurfaceFormat.load()));
    } else if (scanTarget == GX2_SCAN_TARGET_DRC0 &&
               gHaveLastGamePad.load()) {
        MaybeCapture(
                VideoSource::GamePad,
                &gLastGamePadColorBuffer,
                static_cast<GX2SurfaceFormat>(gGamePadSurfaceFormat.load()));
    }

    real_GX2MarkScanBufferCopied(scanTarget);
}

WUPS_MUST_REPLACE(
        GX2CopyColorBufferToScanBuffer,
        WUPS_LOADER_LIBRARY_GX2,
        GX2CopyColorBufferToScanBuffer);

WUPS_MUST_REPLACE(
        GX2SetTVBuffer,
        WUPS_LOADER_LIBRARY_GX2,
        GX2SetTVBuffer);

WUPS_MUST_REPLACE(
        GX2SetDRCBuffer,
        WUPS_LOADER_LIBRARY_GX2,
        GX2SetDRCBuffer);

WUPS_MUST_REPLACE(
        GX2GetCurrentScanBuffer,
        WUPS_LOADER_LIBRARY_GX2,
        GX2GetCurrentScanBuffer);

WUPS_MUST_REPLACE(
        GX2MarkScanBufferCopied,
        WUPS_LOADER_LIBRARY_GX2,
        GX2MarkScanBufferCopied);

namespace Capture {

bool Start() {
    bool expected = false;
    if (!gRunning.compare_exchange_strong(expected, true)) {
        return true;
    }

    FrameStore::Clear();
    gLastGpuReadbackMs.store(0);

    gTVContext.busy.store(false);
    gTVContext.oneShotRequested.store(false);
    gTVContext.lastCaptureTime = 0;

    gGamePadContext.busy.store(false);
    gGamePadContext.oneShotRequested.store(false);
    gGamePadContext.lastCaptureTime = 0;

    if (!StartEncoderWorker()) {
        gRunning.store(false);
        OSReport("[WiiUWebStream] Failed to start encoder thread\n");
        return false;
    }

    OSReport("[WiiUWebStream] Capture/encoder started\n");
    return true;
}

void Stop() {
    if (!gRunning.exchange(false)) {
        return;
    }

    // A GX2 hook may already be copying a frame. Do not tear down its mapped
    // destination or encoder queue until that call has returned. New calls
    // immediately observe gRunning == false and leave without touching buffers.
    while (gActiveCaptureCalls.load() > 0) {
        OSSleepTicks(OSMillisecondsToTicks(1));
    }

    StopEncoderWorker();

    gTVContext.busy.store(false);
    gGamePadContext.busy.store(false);

    FreeLinearBuffer(gTVContext);
    FreeLinearBuffer(gGamePadContext);

    gHaveLastTV.store(false);
    gHaveLastGamePad.store(false);

    FrameStore::Clear();

    OSReport("[WiiUWebStream] Capture/encoder stopped\n");
}

bool IsRunning() {
    return gRunning.load();
}

void RequestOne(VideoSource source) {
    ContextFor(source).oneShotRequested.store(true);
}

} // namespace Capture
