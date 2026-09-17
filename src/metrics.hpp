#pragma once

#include "types.hpp"

#include <cstdint>

struct CaptureMetricsSnapshot {
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

namespace Metrics {

void Reset();
void CaptureAttempt(VideoSource source);
void Copied(VideoSource source);
void Encoded(VideoSource source, uint64_t jpegBytes, uint64_t scaleMicros, uint64_t encodeMicros);
void DroppedRate(VideoSource source);
void DroppedBusy(VideoSource source);
void CopyFailure(VideoSource source);
void QueueFailure(VideoSource source);
void EncodeFailure(VideoSource source);
void WatchdogStall(VideoSource source);
void WatchdogNudge(VideoSource source);
void SetEffectiveFps(VideoSource source, int fps);
CaptureMetricsSnapshot Snapshot(VideoSource source);

} // namespace Metrics
