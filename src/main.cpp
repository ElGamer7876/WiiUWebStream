#include "audio.hpp"
#include "capture.hpp"
#include "log.hpp"
#include "network.hpp"
#include "settings.hpp"
#include "watchdog.hpp"

#include <coreinit/debug.h>
#include <nn/ac.h>
#include <wups.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <wups/config_api.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <string>

WUPS_PLUGIN_NAME("Wii U Web Stream");
WUPS_PLUGIN_DESCRIPTION("TV, GamePad and optional audio streaming over LAN");
WUPS_PLUGIN_VERSION("v0.2.0-dev");
WUPS_PLUGIN_AUTHOR("ElGamer7876");
WUPS_PLUGIN_LICENSE("GPL-3.0-or-later");
WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("wiiu_web_stream");

namespace {
std::atomic_bool gApplicationRunning{false};
std::atomic_bool gPausedForConfig{false};

void EnabledChanged(ConfigItemBoolean *, bool value) {
    Settings::SetEnabled(value);
}

void PauseHeavyRuntimeForConfig() {
    if (!gApplicationRunning.load()) return;
    bool expected = false;
    if (!gPausedForConfig.compare_exchange_strong(expected, true)) return;

    // The full configuration UI lives on port 7770. Keep the network listeners
    // alive, but release capture/audio working memory while Aroma renders WUPS.
    Audio::Stop();
    Capture::Stop();
    Log::Info("capture/audio paused for minimal WUPS config menu");
}

void ResumeHeavyRuntimeAfterConfig() {
    if (!gApplicationRunning.load() || !gPausedForConfig.exchange(false)) return;
    if (!Capture::Start()) Log::Error("capture restart after config failed");
    Audio::Start();
    Audio::ApplySettings();
    Network::Reconfigure();
    Log::Info("capture/audio resumed after WUPS config menu");
}

WUPSConfigAPICallbackStatus ConfigMenuOpened(WUPSConfigCategoryHandle rootHandle) {
    PauseHeavyRuntimeForConfig();
    try {
        WUPSConfigCategory root(rootHandle);
        root.add(WUPSConfigItemBoolean::Create(
            Settings::KEY_ENABLED,
            "Enable server",
            Settings::DEFAULT_ENABLED,
            Settings::enabled.load(),
            EnabledChanged));

        root.add(WUPSConfigItemStub::Create("Configuration moved to the Web Settings panel to reduce WUPS memory use."));

        char text[192];
        const std::string ip = Network::ConsoleIpAddress();
        std::snprintf(text, sizeof(text), "Settings: http://%s:%d/settings%s",
                      ip.c_str(), Settings::webPort.load(),
                      Settings::authEnabled.load() ? "?key=CODE" : "");
        root.add(WUPSConfigItemStub::Create(text));

        std::snprintf(text, sizeof(text), "Status: %s", Network::ListenerStatusSummary().c_str());
        root.add(WUPSConfigItemStub::Create(text));

        if (Settings::HighRiskAccepted()) {
            root.add(WUPSConfigItemStub::Create("HIGH RISK mode is active. Disable it from Web Settings if recovery is needed."));
        }
    } catch (const std::exception &e) {
        OSReport("[WiiUWebStream] Config menu error: %s\n", e.what());
        ResumeHeavyRuntimeAfterConfig();
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosed() {
    Settings::EnforceSafeLimits();
    Settings::Save();
    Network::Reconfigure();
    ResumeHeavyRuntimeAfterConfig();
}

void StartRuntime() {
    bool expected = false;
    if (!gApplicationRunning.compare_exchange_strong(expected, true)) return;
    nn::ac::Initialize();
    nn::ac::ConnectAsync();
    if (!Capture::Start()) Log::Error("capture startup failed");
    Audio::Start();
    Watchdog::Start();
    if (Settings::enabled.load()) Network::Start();
}

void StopRuntime() {
    if (!gApplicationRunning.exchange(false)) return;
    gPausedForConfig.store(false);
    Watchdog::Stop();
    Network::Stop();
    Audio::Stop();
    Capture::Stop();
}
}

INITIALIZE_PLUGIN() {
    Settings::Load();
    Log::Info("INITIALIZE_PLUGIN v0.2.0-dev");
    WUPSConfigAPIOptionsV1 options{.name = "Wii U Web Stream"};
    const auto result = WUPSConfigAPI_Init(options, ConfigMenuOpened, ConfigMenuClosed);
    if (result != WUPSCONFIG_API_RESULT_SUCCESS) OSReport("[WiiUWebStream] WUPS config init failed: %d\n", result);
}

DEINITIALIZE_PLUGIN() { StopRuntime(); }
ON_APPLICATION_START() { StartRuntime(); }
ON_APPLICATION_REQUESTS_EXIT() { StopRuntime(); }
ON_APPLICATION_ENDS() { StopRuntime(); }
