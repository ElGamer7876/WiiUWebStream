#include "metrics.hpp"

#include <mutex>

namespace {
struct Counters {
    uint64_t captureAttempts = 0;
    uint64_t copiedFrames = 0;
    uint64_t encodedFrames = 0;
    uint64_t droppedRateLimit = 0;
    uint64_t droppedEncoderBusy = 0;
    uint64_t copyFailures = 0;
    uint64_t queueFailures = 0;
    uint64_t encodeFailures = 0;
    uint64_t jpegBytesTotal = 0;
    uint64_t encodeMicrosTotal = 0;
    uint64_t scaleMicrosTotal = 0;
    uint64_t watchdogStalls = 0;
    uint64_t watchdogNudges = 0;
    int effectiveFps = 0;
};

std::mutex gMutex;
Counters gTV;
Counters gGamePad;
Counters &Get(VideoSource source) { return source == VideoSource::TV ? gTV : gGamePad; }
}

namespace Metrics {
void Reset() {
    std::lock_guard<std::mutex> lock(gMutex);
    gTV = {};
    gGamePad = {};
}
void CaptureAttempt(VideoSource s) { std::lock_guard<std::mutex> lock(gMutex); ++Get(s).captureAttempts; }
void Copied(VideoSource s) { std::lock_guard<std::mutex> lock(gMutex); ++Get(s).copiedFrames; }
void Encoded(VideoSource s, uint64_t bytes, uint64_t scaleUs, uint64_t encodeUs) {
    std::lock_guard<std::mutex> lock(gMutex);
    auto &c = Get(s);
    ++c.encodedFrames;
    c.jpegBytesTotal += bytes;
    c.scaleMicrosTotal += scaleUs;
    c.encodeMicrosTotal += encodeUs;
}
void DroppedRate(VideoSource s) { std::lock_guard<std::mutex> lock(gMutex); ++Get(s).droppedRateLimit; }
void DroppedBusy(VideoSource s) { std::lock_guard<std::mutex> lock(gMutex); ++Get(s).droppedEncoderBusy; }
void CopyFailure(VideoSource s) { std::lock_guard<std::mutex> lock(gMutex); ++Get(s).copyFailures; }
void QueueFailure(VideoSource s) { std::lock_guard<std::mutex> lock(gMutex); ++Get(s).queueFailures; }
void EncodeFailure(VideoSource s) { std::lock_guard<std::mutex> lock(gMutex); ++Get(s).encodeFailures; }
void WatchdogStall(VideoSource s) { std::lock_guard<std::mutex> lock(gMutex); ++Get(s).watchdogStalls; }
void WatchdogNudge(VideoSource s) { std::lock_guard<std::mutex> lock(gMutex); ++Get(s).watchdogNudges; }
void SetEffectiveFps(VideoSource s, int fps) { std::lock_guard<std::mutex> lock(gMutex); Get(s).effectiveFps = fps; }
CaptureMetricsSnapshot Snapshot(VideoSource s) {
    std::lock_guard<std::mutex> lock(gMutex);
    const auto &c = Get(s);
    CaptureMetricsSnapshot out;
    out.captureAttempts = c.captureAttempts;
    out.copiedFrames = c.copiedFrames;
    out.encodedFrames = c.encodedFrames;
    out.droppedRateLimit = c.droppedRateLimit;
    out.droppedEncoderBusy = c.droppedEncoderBusy;
    out.copyFailures = c.copyFailures;
    out.queueFailures = c.queueFailures;
    out.encodeFailures = c.encodeFailures;
    out.jpegBytesTotal = c.jpegBytesTotal;
    out.encodeMicrosTotal = c.encodeMicrosTotal;
    out.scaleMicrosTotal = c.scaleMicrosTotal;
    out.watchdogStalls = c.watchdogStalls;
    out.watchdogNudges = c.watchdogNudges;
    out.effectiveFps = c.effectiveFps;
    return out;
}
}
