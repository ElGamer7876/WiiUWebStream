#include "safety.hpp"

#include "audio.hpp"
#include "capture.hpp"
#include "frame_store.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "network.hpp"
#include "settings.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace {

struct PreviousCounters {
    uint64_t encoded = 0;
    uint64_t encodeMicros = 0;
    uint64_t busyDrops = 0;
};

std::atomic_bool gEmergencyStopped{false};
std::atomic_int gLevel{0};
std::atomic_uint32_t gInterventions{0};
std::mutex gStateMutex;
std::string gReason;
PreviousCounters gTVPrevious;
PreviousCounters gGamePadPrevious;
int gOverloadTicks = 0;
int gHealthyTicks = 0;

PreviousCounters &Previous(VideoSource source) {
    return source == VideoSource::TV ? gTVPrevious : gGamePadPrevious;
}

int ConfiguredFps(VideoSource source) {
    return std::max(1, source == VideoSource::TV ? Settings::tvFps.load() : Settings::gamepadFps.load());
}

bool SourceUnderLoad(VideoSource source, bool &severe, std::string &reason) {
    severe = false;
    if (FrameStore::ClientCount(source) <= 0 &&
        !(Settings::HighRiskAccepted() && Settings::continuousCapture.load())) {
        Previous(source) = {
            Metrics::Snapshot(source).encodedFrames,
            Metrics::Snapshot(source).encodeMicrosTotal,
            Metrics::Snapshot(source).droppedEncoderBusy
        };
        return false;
    }

    const auto metrics = Metrics::Snapshot(source);
    const auto stream = FrameStore::Stats(source);
    auto &previous = Previous(source);

    const uint64_t deltaEncoded = metrics.encodedFrames - previous.encoded;
    const uint64_t deltaEncodeMicros = metrics.encodeMicrosTotal - previous.encodeMicros;
    const uint64_t deltaBusy = metrics.droppedEncoderBusy - previous.busyDrops;

    previous.encoded = metrics.encodedFrames;
    previous.encodeMicros = metrics.encodeMicrosTotal;
    previous.busyDrops = metrics.droppedEncoderBusy;

    const uint64_t recentEncodeUs = deltaEncoded ? deltaEncodeMicros / deltaEncoded : 0;
    const uint64_t frameBudgetUs = static_cast<uint64_t>(1000000 / ConfiguredFps(source));

    if (stream.hasFrame && stream.lastFrameAgeMs > 2500) {
        severe = true;
        reason = std::string(VideoSourceName(source)) + " frame age exceeded 2500 ms";
        return true;
    }
    if (deltaBusy >= 5) {
        severe = true;
        reason = std::string(VideoSourceName(source)) + " encoder busy drops increased rapidly";
        return true;
    }
    if (recentEncodeUs > 0 && recentEncodeUs > frameBudgetUs * 3 / 2) {
        severe = true;
        reason = std::string(VideoSourceName(source)) + " JPEG encoder exceeded 150% of frame budget";
        return true;
    }
    if (deltaBusy >= 2) {
        reason = std::string(VideoSourceName(source)) + " encoder busy drops detected";
        return true;
    }
    if (recentEncodeUs > 0 && recentEncodeUs > frameBudgetUs * 11 / 10) {
        reason = std::string(VideoSourceName(source)) + " JPEG encoder exceeded frame budget";
        return true;
    }
    if (stream.hasFrame && stream.lastFrameAgeMs > 1400) {
        reason = std::string(VideoSourceName(source)) + " frame delivery is falling behind";
        return true;
    }
    return false;
}

void SetReason(const std::string &reason) {
    std::lock_guard<std::mutex> lock(gStateMutex);
    gReason = reason;
}

void SetLevel(int value, const std::string &reason) {
    value = std::clamp(value, 0, 3);
    const int previous = gLevel.exchange(value);
    if (value > previous) {
        gInterventions.fetch_add(1);
        Log::Info("Safety Governor level %d: %s", value, reason.c_str());
    } else if (value < previous) {
        Log::Info("Safety Governor recovered to level %d", value);
    }
    SetReason(value == 0 ? "" : reason);
}

} // namespace

namespace Safety {

void Reset() {
    gEmergencyStopped.store(false);
    gLevel.store(0);
    gInterventions.store(0);
    gTVPrevious = {};
    gGamePadPrevious = {};
    gOverloadTicks = 0;
    gHealthyTicks = 0;
    SetReason("");
}

void Tick() {
    if (gEmergencyStopped.load()) return;

    if (!Settings::safetyGovernor.load()) {
        if (gLevel.load() != 0) SetLevel(0, "");
        gOverloadTicks = 0;
        gHealthyTicks = 0;
        return;
    }

    bool tvSevere = false;
    bool gpSevere = false;
    std::string tvReason;
    std::string gpReason;
    const bool tvOverload = Settings::tvEnabled.load() && SourceUnderLoad(VideoSource::TV, tvSevere, tvReason);
    const bool gpOverload = Settings::gamepadEnabled.load() && SourceUnderLoad(VideoSource::GamePad, gpSevere, gpReason);
    const bool overloaded = tvOverload || gpOverload;
    const bool severe = tvSevere || gpSevere;
    const std::string reason = tvOverload ? tvReason : gpReason;

    if (overloaded) {
        ++gOverloadTicks;
        gHealthyTicks = 0;
        const int threshold = severe ? 1 : 2;
        if (gOverloadTicks >= threshold) {
            SetLevel(std::min(3, gLevel.load() + 1), reason);
            gOverloadTicks = 0;
        }
        return;
    }

    gOverloadTicks = 0;
    if (gLevel.load() > 0) {
        ++gHealthyTicks;
        if (gHealthyTicks >= 8) {
            SetLevel(gLevel.load() - 1, "load recovered");
            gHealthyTicks = 0;
        }
    } else {
        gHealthyTicks = 0;
    }
}

bool EmergencyStopped() {
    return gEmergencyStopped.load();
}

void EmergencyStop() {
    if (gEmergencyStopped.exchange(true)) return;
    gLevel.store(3);
    gInterventions.fetch_add(1);
    SetReason("Emergency Stop requested by user");

    // Keep the web listener alive, but halt expensive producers and disconnect
    // TV/GamePad streaming clients. This leaves /settings available for recovery.
    Audio::Stop();
    Capture::Stop();
    Network::SuspendStreaming();
    Log::Info("Emergency Stop: capture/audio and stream listeners suspended");
}

void ResumeStreaming() {
    if (!gEmergencyStopped.exchange(false)) return;
    gLevel.store(0);
    SetReason("");
    gOverloadTicks = 0;
    gHealthyTicks = 0;
    gTVPrevious = {};
    gGamePadPrevious = {};

    if (!Capture::Start()) Log::Error("capture restart after Emergency Stop failed");
    Audio::Start();
    Audio::ApplySettings();
    Network::ResumeStreaming();
    Log::Info("streaming resumed after Emergency Stop");
}

int FpsCap(VideoSource source) {
    if (gEmergencyStopped.load()) return 1;
    switch (gLevel.load()) {
        case 1: return source == VideoSource::TV ? 8 : 8;
        case 2: return 6;
        case 3: return 5;
        default: return 0;
    }
}

int JpegQualityCap() {
    switch (gLevel.load()) {
        case 1: return 80;
        case 2: return 70;
        case 3: return 60;
        default: return 0;
    }
}

void ClampDimensions([[maybe_unused]] VideoSource source, uint32_t &width, uint32_t &height) {
    const int level = gLevel.load();
    if (level < 2) return;

    const uint32_t maxWidth = 640;
    const uint32_t maxHeight = 360;
    if (width <= maxWidth && height <= maxHeight) return;

    // All configured resolutions are 16:9. Clamp without adding another scaler
    // path or allocating larger temporary buffers.
    width = maxWidth;
    height = maxHeight;
}

Snapshot GetSnapshot() {
    Snapshot out;
    out.governorEnabled = Settings::safetyGovernor.load();
    out.level = gLevel.load();
    out.active = out.level > 0;
    out.emergencyStopped = gEmergencyStopped.load();
    out.interventions = gInterventions.load();
    out.tvFpsCap = FpsCap(VideoSource::TV);
    out.gamepadFpsCap = FpsCap(VideoSource::GamePad);
    out.jpegQualityCap = JpegQualityCap();
    if (out.level >= 2) {
        out.maxWidth = 640;
        out.maxHeight = 360;
    }
    {
        std::lock_guard<std::mutex> lock(gStateMutex);
        out.reason = gReason;
    }
    return out;
}

} // namespace Safety
