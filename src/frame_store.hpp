#pragma once

#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

struct JpegFrame {
    std::vector<uint8_t> bytes;
    uint64_t sequence = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct StreamStats {
    uint64_t sequence = 0;
    double fps = 0.0;
    uint32_t width = 0;
    uint32_t height = 0;
    int clients = 0;
    bool hasFrame = false;
};

namespace FrameStore {

void Publish(VideoSource source,
             std::vector<uint8_t> &&jpeg,
             uint32_t width,
             uint32_t height);

std::shared_ptr<const JpegFrame> Latest(VideoSource source);

std::shared_ptr<const JpegFrame> WaitForNew(
        VideoSource source,
        uint64_t previousSequence,
        std::chrono::milliseconds timeout);

uint64_t Sequence(VideoSource source);
StreamStats Stats(VideoSource source);

void ClientConnected(VideoSource source);
void ClientDisconnected(VideoSource source);
int ClientCount(VideoSource source);

void NotifyAll();
void Clear();

} // namespace FrameStore
