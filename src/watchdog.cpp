#include "watchdog.hpp"
#include "capture.hpp"
#include "frame_store.hpp"
#include "metrics.hpp"
#include "settings.hpp"
#include "log.hpp"
#include "network.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace {
std::atomic_bool gRunning{false};
std::thread gThread;

void Check(VideoSource source) {
    if (FrameStore::ClientCount(source) <= 0) return;
    const auto stats = FrameStore::Stats(source);
    if (!stats.hasFrame || stats.lastFrameAgeMs > 3500) {
        Metrics::WatchdogStall(source);
        Capture::RequestOne(source);
        Metrics::WatchdogNudge(source);
        Log::Verbose("watchdog nudge for %s, age=%llu ms", VideoSourceName(source), static_cast<unsigned long long>(stats.lastFrameAgeMs));
    }
}

void Loop() {
    unsigned networkTick = 0;
    while (gRunning.load()) {
        if (Settings::watchdogEnabled.load()) {
            if (Settings::tvEnabled.load()) Check(VideoSource::TV);
            if (Settings::gamepadEnabled.load()) Check(VideoSource::GamePad);
            if (++networkTick >= 5) { networkTick = 0; Network::EnsureListeners(); }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}
}

namespace Watchdog {
bool Start() {
    bool expected = false;
    if (!gRunning.compare_exchange_strong(expected, true)) return true;
    gThread = std::thread(Loop);
    return true;
}
void Stop() {
    if (!gRunning.exchange(false)) return;
    if (gThread.joinable()) gThread.join();
}
}
