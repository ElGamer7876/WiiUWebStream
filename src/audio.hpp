#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Audio {

enum class Source : uint8_t {
    TV = 0,
    GamePad = 1,
};

inline constexpr size_t MAX_FRAMES_PER_PACKET = 160;
inline constexpr size_t PCM_BYTES_PER_FRAME = 4; // stereo PCM16LE
inline constexpr size_t MAX_PCM_BYTES = MAX_FRAMES_PER_PACKET * PCM_BYTES_PER_FRAME;

struct Packet {
    uint64_t sequence = 0;
    uint32_t sampleRate = 0;
    uint16_t frames = 0;
    uint16_t channels = 2;
    size_t bytes = 0;
    std::array<uint8_t, MAX_PCM_BYTES> pcm{};
};

struct Stats {
    bool installed = false;
    uint64_t latestSequence = 0;
    uint64_t capturedPackets = 0;
    uint64_t droppedPackets = 0;
    uint32_t sampleRate = 0;
    int clients = 0;
};

bool Start();
void Stop();
void ApplySettings();
void EnsureCallbacks();
bool IsActive();

void ClientConnected(Source source);
void ClientDisconnected(Source source);
bool ReadAfter(Source source, uint64_t &lastSequence, Packet &out);
Stats GetStats(Source source);

} // namespace Audio
