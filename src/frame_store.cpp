#include "frame_store.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace {

struct Store {
    std::mutex mutex;
    std::condition_variable condition;
    std::shared_ptr<const JpegFrame> latest;
    uint64_t nextSequence = 1;
    double smoothedFps = 0.0;
    std::chrono::steady_clock::time_point lastPublish{};
    std::atomic_int clients{0};
};

Store gTV;
Store gGamePad;

Store &GetStore(VideoSource source) {
    return source == VideoSource::TV ? gTV : gGamePad;
}

void ClearStore(Store &store) {
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        store.latest.reset();
        store.nextSequence = 1;
        store.smoothedFps = 0.0;
        store.lastPublish = {};
    }
    store.clients.store(0);
    store.condition.notify_all();
}

} // namespace

namespace FrameStore {

void Publish(VideoSource source,
             std::vector<uint8_t> &&jpeg,
             uint32_t width,
             uint32_t height) {
    auto &store = GetStore(source);
    auto frame = std::make_shared<JpegFrame>();
    frame->bytes = std::move(jpeg);
    frame->width = width;
    frame->height = height;

    {
        std::lock_guard<std::mutex> lock(store.mutex);

        frame->sequence = store.nextSequence++;

        const auto now = std::chrono::steady_clock::now();
        if (store.lastPublish.time_since_epoch().count() != 0) {
            const double seconds =
                    std::chrono::duration<double>(now - store.lastPublish).count();
            if (seconds > 0.0001) {
                const double instantFps = 1.0 / seconds;
                if (store.smoothedFps <= 0.01) {
                    store.smoothedFps = instantFps;
                } else {
                    store.smoothedFps =
                            (store.smoothedFps * 0.80) + (instantFps * 0.20);
                }
            }
        }

        store.lastPublish = now;
        store.latest = std::move(frame);
    }

    store.condition.notify_all();
}

std::shared_ptr<const JpegFrame> Latest(VideoSource source) {
    auto &store = GetStore(source);
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.latest;
}

std::shared_ptr<const JpegFrame> WaitForNew(
        VideoSource source,
        uint64_t previousSequence,
        std::chrono::milliseconds timeout) {
    auto &store = GetStore(source);
    std::unique_lock<std::mutex> lock(store.mutex);

    store.condition.wait_for(lock, timeout, [&]() {
        return store.latest && store.latest->sequence != previousSequence;
    });

    if (store.latest && store.latest->sequence != previousSequence) {
        return store.latest;
    }

    return {};
}

uint64_t Sequence(VideoSource source) {
    auto frame = Latest(source);
    return frame ? frame->sequence : 0;
}

StreamStats Stats(VideoSource source) {
    auto &store = GetStore(source);
    StreamStats result;

    {
        std::lock_guard<std::mutex> lock(store.mutex);
        result.clients = store.clients.load();
        result.fps = store.smoothedFps;

        if (store.latest) {
            result.sequence = store.latest->sequence;
            result.width = store.latest->width;
            result.height = store.latest->height;
            result.hasFrame = true;
        }
    }

    return result;
}

void ClientConnected(VideoSource source) {
    GetStore(source).clients.fetch_add(1);
}

void ClientDisconnected(VideoSource source) {
    auto &clients = GetStore(source).clients;
    const int old = clients.fetch_sub(1);
    if (old <= 0) {
        clients.store(0);
    }
}

int ClientCount(VideoSource source) {
    return GetStore(source).clients.load();
}

void NotifyAll() {
    gTV.condition.notify_all();
    gGamePad.condition.notify_all();
}

void Clear() {
    ClearStore(gTV);
    ClearStore(gGamePad);
}

} // namespace FrameStore
