#include "audio.hpp"
#include "log.hpp"
#include "settings.hpp"

#include <sndcore2/core.h>
#include <sndcore2/device.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace {

struct AXFinalMixParams {
    int32_t **data;
    uint16_t numChannelInput;
    uint16_t numSamples;
    uint16_t numDevices;
    uint16_t numChannelOutput;
};

struct AudioSlot {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    Audio::Packet packet{};
};

struct AudioState {
    std::array<AudioSlot, 8> slots{};
    std::atomic_uint32_t sequence{0};
    std::atomic_uint32_t captured{0};
    std::atomic_uint32_t dropped{0};
    std::atomic_int clients{0};
    std::atomic_uint32_t sampleRate{0};
};

std::atomic_bool gRunning{false};
std::atomic_bool gInstalled{false};
AudioState gTV;
AudioState gGamePad;
AXDeviceFinalMixCallback gPreviousTV = nullptr;
AXDeviceFinalMixCallback gPreviousDRC = nullptr;

AudioState &State(Audio::Source source) {
    return source == Audio::Source::TV ? gTV : gGamePad;
}

bool RiskEnabled() {
    return Settings::HighRiskAccepted() && Settings::audioStreaming.load();
}

int32_t Clamp24To16(int32_t sample) {
    sample >>= 8;
    return std::clamp(sample, -32768, 32767);
}

void Capture(Audio::Source source, void *raw) {
    if (!gRunning.load() || !RiskEnabled() || raw == nullptr) return;
    AudioState &state = State(source);
    if (state.clients.load() <= 0 && !Settings::continuousCapture.load()) return;

    auto *params = static_cast<AXFinalMixParams *>(raw);
    if (params->data == nullptr || params->numSamples == 0 || params->numChannelInput == 0) return;

    uint32_t next = state.sequence.load(std::memory_order_relaxed) + 1U;
    if (next == 0) next = 1;

    AudioSlot &slot = state.slots[next % state.slots.size()];
    if (slot.lock.test_and_set(std::memory_order_acquire)) {
        state.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint16_t frames = static_cast<uint16_t>(std::min<size_t>(params->numSamples, Audio::MAX_FRAMES_PER_PACKET));
    const uint16_t channels = params->numChannelInput;
    int32_t *left = params->data[0];
    int32_t *right = params->data[channels > 1 ? 1 : 0];
    if (left == nullptr || right == nullptr) {
        state.dropped.fetch_add(1, std::memory_order_relaxed);
        slot.lock.clear(std::memory_order_release);
        return;
    }

    Audio::Packet &packet = slot.packet;
    packet.sequence = next;
    packet.sampleRate = AXGetInputSamplesPerSec();
    packet.channels = 2;
    packet.frames = frames;
    packet.bytes = static_cast<size_t>(frames) * Audio::PCM_BYTES_PER_FRAME;

    for (uint16_t i = 0; i < frames; ++i) {
        const int16_t l = static_cast<int16_t>(Clamp24To16(left[i]));
        const int16_t r = static_cast<int16_t>(Clamp24To16(right[i]));
        const size_t o = static_cast<size_t>(i) * 4;
        packet.pcm[o + 0] = static_cast<uint8_t>(l & 0xFF);
        packet.pcm[o + 1] = static_cast<uint8_t>((static_cast<uint16_t>(l) >> 8) & 0xFF);
        packet.pcm[o + 2] = static_cast<uint8_t>(r & 0xFF);
        packet.pcm[o + 3] = static_cast<uint8_t>((static_cast<uint16_t>(r) >> 8) & 0xFF);
    }

    state.sampleRate.store(packet.sampleRate, std::memory_order_relaxed);
    state.captured.fetch_add(1, std::memory_order_relaxed);
    state.sequence.store(next, std::memory_order_release);
    slot.lock.clear(std::memory_order_release);
}

void TVCallback(void *raw) {
    Capture(Audio::Source::TV, raw);
    if (gPreviousTV && gPreviousTV != TVCallback) gPreviousTV(raw);
}

void DRCCallback(void *raw) {
    Capture(Audio::Source::GamePad, raw);
    if (gPreviousDRC && gPreviousDRC != DRCCallback) gPreviousDRC(raw);
}

void Install() {
    if (!gRunning.load() || !RiskEnabled()) return;
    AXDeviceFinalMixCallback currentTV = nullptr;
    AXDeviceFinalMixCallback currentDRC = nullptr;
    AXGetDeviceFinalMixCallback(AX_DEVICE_TYPE_TV, &currentTV);
    AXGetDeviceFinalMixCallback(AX_DEVICE_TYPE_DRC, &currentDRC);

    if (currentTV != TVCallback) {
        gPreviousTV = currentTV;
        AXRegisterDeviceFinalMixCallback(AX_DEVICE_TYPE_TV, TVCallback);
    }
    if (currentDRC != DRCCallback) {
        gPreviousDRC = currentDRC;
        AXRegisterDeviceFinalMixCallback(AX_DEVICE_TYPE_DRC, DRCCallback);
    }
    gInstalled.store(true);
}

void Restore() {
    if (!gInstalled.exchange(false)) return;
    AXDeviceFinalMixCallback currentTV = nullptr;
    AXDeviceFinalMixCallback currentDRC = nullptr;
    AXGetDeviceFinalMixCallback(AX_DEVICE_TYPE_TV, &currentTV);
    AXGetDeviceFinalMixCallback(AX_DEVICE_TYPE_DRC, &currentDRC);
    if (currentTV == TVCallback) AXRegisterDeviceFinalMixCallback(AX_DEVICE_TYPE_TV, gPreviousTV);
    if (currentDRC == DRCCallback) AXRegisterDeviceFinalMixCallback(AX_DEVICE_TYPE_DRC, gPreviousDRC);
    gPreviousTV = nullptr;
    gPreviousDRC = nullptr;
}

} // namespace

namespace Audio {

bool Start() {
    bool expected = false;
    if (!gRunning.compare_exchange_strong(expected, true)) return true;
    ApplySettings();
    return true;
}

void Stop() {
    if (!gRunning.exchange(false)) return;
    Restore();
}

void ApplySettings() {
    if (!gRunning.load()) return;
    if (RiskEnabled()) Install(); else Restore();
}

void EnsureCallbacks() {
    if (!gRunning.load() || !RiskEnabled()) return;
    Install();
}

bool IsActive() {
    return gRunning.load() && gInstalled.load() && RiskEnabled();
}

void ClientConnected(Source source) {
    State(source).clients.fetch_add(1);
}

void ClientDisconnected(Source source) {
    auto &value = State(source).clients;
    int current = value.load();
    while (current > 0 && !value.compare_exchange_weak(current, current - 1)) {}
}

bool ReadAfter(Source source, uint64_t &lastSequence, Packet &out) {
    AudioState &state = State(source);
    const uint32_t latest = state.sequence.load(std::memory_order_acquire);
    if (latest == 0 || latest == static_cast<uint32_t>(lastSequence)) return false;

    AudioSlot &slot = state.slots[latest % state.slots.size()];
    if (slot.lock.test_and_set(std::memory_order_acquire)) return false;
    const bool valid = slot.packet.sequence == latest && slot.packet.bytes <= slot.packet.pcm.size();
    if (valid) out = slot.packet;
    slot.lock.clear(std::memory_order_release);
    if (!valid) return false;

    lastSequence = latest;
    return true;
}

Stats GetStats(Source source) {
    AudioState &state = State(source);
    Stats result{};
    result.installed = IsActive();
    result.latestSequence = state.sequence.load();
    result.capturedPackets = state.captured.load();
    result.droppedPackets = state.dropped.load();
    result.sampleRate = state.sampleRate.load();
    result.clients = state.clients.load();
    return result;
}

} // namespace Audio
