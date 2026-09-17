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
#include <wups/config/WUPSConfigItemIntegerRange.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
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

constexpr WUPSConfigItemMultipleValues::ValuePair PRESET_VALUES[] = {
    {static_cast<uint32_t>(Settings::Preset::Custom), "Custom"},
    {static_cast<uint32_t>(Settings::Preset::LowLatency), "Low Latency"},
    {static_cast<uint32_t>(Settings::Preset::Balanced), "Balanced"},
    {static_cast<uint32_t>(Settings::Preset::Quality), "Quality"},
    {static_cast<uint32_t>(Settings::Preset::OBS), "OBS"},
};

constexpr WUPSConfigItemMultipleValues::ValuePair SAFE_RESOLUTION_VALUES[] = {
    {static_cast<uint32_t>(Settings::Resolution::R426x240), "426x240"},
    {static_cast<uint32_t>(Settings::Resolution::R640x360), "640x360"},
    {static_cast<uint32_t>(Settings::Resolution::R854x480), "854x480"},
    {static_cast<uint32_t>(Settings::Resolution::R960x540), "960x540"},
};

constexpr WUPSConfigItemMultipleValues::ValuePair HIGH_RISK_RESOLUTION_VALUES[] = {
    {static_cast<uint32_t>(Settings::Resolution::R426x240), "426x240"},
    {static_cast<uint32_t>(Settings::Resolution::R640x360), "640x360"},
    {static_cast<uint32_t>(Settings::Resolution::R854x480), "854x480"},
    {static_cast<uint32_t>(Settings::Resolution::R960x540), "960x540"},
    {static_cast<uint32_t>(Settings::Resolution::R1280x720), "1280x720 (HIGH RISK)"},
    {static_cast<uint32_t>(Settings::Resolution::R1920x1080), "1920x1080 (HIGH RISK)"},
};

constexpr WUPSConfigItemMultipleValues::ValuePair LOG_VALUES[] = {
    {static_cast<uint32_t>(Settings::LogLevel::Off), "Off"},
    {static_cast<uint32_t>(Settings::LogLevel::Error), "Errors"},
    {static_cast<uint32_t>(Settings::LogLevel::Info), "Info"},
    {static_cast<uint32_t>(Settings::LogLevel::Verbose), "Verbose"},
};

void ApplyRuntimeSettings() {
    if (!gApplicationRunning.load()) return;
    Audio::ApplySettings();
    Network::Reconfigure();
}

void EnabledChanged(ConfigItemBoolean *, bool v) { Settings::SetEnabled(v); }
void TvEnabledChanged(ConfigItemBoolean *, bool v) { Settings::SetTvEnabled(v); }
void GamePadEnabledChanged(ConfigItemBoolean *, bool v) { Settings::SetGamePadEnabled(v); }
void AdaptiveChanged(ConfigItemBoolean *, bool v) { Settings::SetAdaptiveFps(v); }
void AuthChanged(ConfigItemBoolean *, bool v) { Settings::SetAuthEnabled(v); }
void WatchdogChanged(ConfigItemBoolean *, bool v) { Settings::SetWatchdogEnabled(v); }
void HighRiskEnabledChanged(ConfigItemBoolean *, bool v) { Settings::SetHighRiskEnabled(v); }
void HighRiskAcceptedChanged(ConfigItemBoolean *, bool v) { Settings::SetHighRiskAccepted(v); }
void AudioStreamingChanged(ConfigItemBoolean *, bool v) { Settings::SetAudioStreaming(v); }
void ContinuousCaptureChanged(ConfigItemBoolean *, bool v) { Settings::SetContinuousCapture(v); }

void PortChanged(ConfigItemIntegerRange *item, int value) {
    const std::string id = item->identifier;
    if (id == Settings::KEY_WEB_PORT) Settings::SetWebPort(value);
    else if (id == Settings::KEY_TV_PORT) Settings::SetTvPort(value);
    else if (id == Settings::KEY_GAMEPAD_PORT) Settings::SetGamePadPort(value);
}

void FpsChanged(ConfigItemIntegerRange *item, int value) {
    const std::string id = item->identifier;
    if (id == Settings::KEY_TV_FPS || id == "highRiskTvFps") Settings::SetTvFps(value);
    else if (id == Settings::KEY_GAMEPAD_FPS || id == "highRiskGamePadFps") Settings::SetGamePadFps(value);
}

void QualityChanged(ConfigItemIntegerRange *, int value) { Settings::SetJpegQuality(value); }
void AuthCodeChanged(ConfigItemIntegerRange *, int value) { Settings::SetAuthCode(value); }
void PresetChanged(ConfigItemMultipleValues *, uint32_t value) { Settings::SetPreset(static_cast<int>(value)); }

void ResolutionChanged(ConfigItemMultipleValues *item, uint32_t value) {
    const std::string id = item->identifier;
    if (id == Settings::KEY_TV_RESOLUTION || id == "highRiskTvResolution") Settings::SetTvResolution(static_cast<int>(value));
    else if (id == Settings::KEY_GAMEPAD_RESOLUTION || id == "highRiskGamePadResolution") Settings::SetGamePadResolution(static_cast<int>(value));
}

void LogLevelChanged(ConfigItemMultipleValues *, uint32_t value) { Settings::SetLogLevel(static_cast<int>(value)); }

WUPSConfigAPICallbackStatus ConfigMenuOpened(WUPSConfigCategoryHandle rootHandle) {
    try {
        WUPSConfigCategory root(rootHandle);
        const bool highRisk = Settings::HighRiskAccepted();

        root.add(WUPSConfigItemBoolean::Create(Settings::KEY_ENABLED, "Enable server", Settings::DEFAULT_ENABLED, Settings::enabled.load(), EnabledChanged));
        root.add(WUPSConfigItemMultipleValues::CreateFromValue(Settings::KEY_PRESET, "Preset", Settings::DEFAULT_PRESET, Settings::preset.load(), PRESET_VALUES, PresetChanged));

        root.add(WUPSConfigItemStub::Create("Network - LAN only; no UPnP / port forwarding"));
        root.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_WEB_PORT, "Web / combined port", Settings::DEFAULT_WEB_PORT, Settings::webPort.load(), 1024, 65535, PortChanged));
        root.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_TV_PORT, "TV MJPEG port", Settings::DEFAULT_TV_PORT, Settings::tvPort.load(), 1024, 65535, PortChanged));
        root.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_GAMEPAD_PORT, "GamePad MJPEG port", Settings::DEFAULT_GAMEPAD_PORT, Settings::gamepadPort.load(), 1024, 65535, PortChanged));

        root.add(WUPSConfigItemStub::Create("TV"));
        root.add(WUPSConfigItemBoolean::Create(Settings::KEY_TV_ENABLED, "Enable TV capture", Settings::DEFAULT_TV_ENABLED, Settings::tvEnabled.load(), TvEnabledChanged));
        if (!highRisk) {
            root.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_TV_FPS, "TV target FPS", Settings::DEFAULT_TV_FPS, Settings::tvFps.load(), 1, 15, FpsChanged));
            root.add(WUPSConfigItemMultipleValues::CreateFromValue(Settings::KEY_TV_RESOLUTION, "TV output resolution", Settings::DEFAULT_TV_RESOLUTION, Settings::tvResolution.load(), SAFE_RESOLUTION_VALUES, ResolutionChanged));
        }

        root.add(WUPSConfigItemStub::Create("GamePad"));
        root.add(WUPSConfigItemBoolean::Create(Settings::KEY_GAMEPAD_ENABLED, "Enable GamePad capture", Settings::DEFAULT_GAMEPAD_ENABLED, Settings::gamepadEnabled.load(), GamePadEnabledChanged));
        if (!highRisk) {
            root.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_GAMEPAD_FPS, "GamePad target FPS", Settings::DEFAULT_GAMEPAD_FPS, Settings::gamepadFps.load(), 1, 15, FpsChanged));
            root.add(WUPSConfigItemMultipleValues::CreateFromValue(Settings::KEY_GAMEPAD_RESOLUTION, "GamePad output resolution", Settings::DEFAULT_GAMEPAD_RESOLUTION, Settings::gamepadResolution.load(), SAFE_RESOLUTION_VALUES, ResolutionChanged));
        }

        root.add(WUPSConfigItemStub::Create("Performance"));
        if (!highRisk) {
            root.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_JPEG_QUALITY, "JPEG quality", Settings::DEFAULT_JPEG_QUALITY, Settings::jpegQuality.load(), 35, 85, QualityChanged));
        }
        root.add(WUPSConfigItemBoolean::Create(Settings::KEY_ADAPTIVE_FPS, "Adaptive FPS", Settings::DEFAULT_ADAPTIVE_FPS, Settings::adaptiveFps.load(), AdaptiveChanged));
        root.add(WUPSConfigItemBoolean::Create(Settings::KEY_WATCHDOG_ENABLED, "Health watchdog", Settings::DEFAULT_WATCHDOG_ENABLED, Settings::watchdogEnabled.load(), WatchdogChanged));

        root.add(WUPSConfigItemStub::Create("Security"));
        root.add(WUPSConfigItemBoolean::Create(Settings::KEY_AUTH_ENABLED, "Require URL access code", Settings::DEFAULT_AUTH_ENABLED, Settings::authEnabled.load(), AuthChanged));
        root.add(WUPSConfigItemIntegerRange::Create(Settings::KEY_AUTH_CODE, "Access code (0-999999)", Settings::DEFAULT_AUTH_CODE, Settings::authCode.load(), 0, 999999, AuthCodeChanged));
        root.add(WUPSConfigItemStub::Create("When enabled use ?key=CODE. This is LAN access control, not encryption."));
        root.add(WUPSConfigItemMultipleValues::CreateFromValue(Settings::KEY_LOG_LEVEL, "Log level", Settings::DEFAULT_LOG_LEVEL, Settings::logLevel.load(), LOG_VALUES, LogLevelChanged));

        root.add(WUPSConfigItemStub::Create("Advanced / Experimental"));
        root.add(WUPSConfigItemBoolean::Create(Settings::KEY_HIGH_RISK_ENABLED, "Enable high-risk actions", Settings::DEFAULT_HIGH_RISK_ENABLED, Settings::highRiskEnabled.load(), HighRiskEnabledChanged));
        root.add(WUPSConfigItemBoolean::Create(Settings::KEY_HIGH_RISK_ACCEPTED, "I understand and accept the risk", Settings::DEFAULT_HIGH_RISK_ACCEPTED, Settings::highRiskAccepted.load(), HighRiskAcceptedChanged));
        root.add(WUPSConfigItemStub::Create("WARNING: High-risk actions may increase CPU/GPU/memory/network/SD load and may freeze or crash the console."));

        if (highRisk) {
            root.add(WUPSConfigItemStub::Create("High-risk overrides (HIGH RISK)"));
            root.add(WUPSConfigItemIntegerRange::Create("highRiskTvFps", "TV target FPS override (HIGH RISK)", Settings::DEFAULT_TV_FPS, Settings::tvFps.load(), 1, 60, FpsChanged));
            root.add(WUPSConfigItemIntegerRange::Create("highRiskGamePadFps", "GamePad target FPS override (HIGH RISK)", Settings::DEFAULT_GAMEPAD_FPS, Settings::gamepadFps.load(), 1, 60, FpsChanged));
            root.add(WUPSConfigItemIntegerRange::Create("highRiskJpegQuality", "JPEG quality override (HIGH RISK)", Settings::DEFAULT_JPEG_QUALITY, Settings::jpegQuality.load(), 35, 95, QualityChanged));
            root.add(WUPSConfigItemMultipleValues::CreateFromValue("highRiskTvResolution", "TV output resolution override (HIGH RISK)", Settings::DEFAULT_TV_RESOLUTION, Settings::tvResolution.load(), HIGH_RISK_RESOLUTION_VALUES, ResolutionChanged));
            root.add(WUPSConfigItemMultipleValues::CreateFromValue("highRiskGamePadResolution", "GamePad output resolution override (HIGH RISK)", Settings::DEFAULT_GAMEPAD_RESOLUTION, Settings::gamepadResolution.load(), HIGH_RISK_RESOLUTION_VALUES, ResolutionChanged));
            root.add(WUPSConfigItemBoolean::Create(Settings::KEY_AUDIO_STREAMING, "Audio streaming (HIGH RISK)", Settings::DEFAULT_AUDIO_STREAMING, Settings::audioStreaming.load(), AudioStreamingChanged));
            root.add(WUPSConfigItemBoolean::Create(Settings::KEY_CONTINUOUS_CAPTURE, "Continuous capture without viewers (HIGH RISK)", Settings::DEFAULT_CONTINUOUS_CAPTURE, Settings::continuousCapture.load(), ContinuousCaptureChanged));
            root.add(WUPSConfigItemStub::Create("H.264 streaming (HIGH RISK) - Not available in this build."));
            root.add(WUPSConfigItemStub::Create("mDNS discovery (HIGH RISK) - Not available in this build."));
        } else {
            root.add(WUPSConfigItemStub::Create("High-risk options remain hidden until both switches above are enabled."));
        }
        root.add(WUPSConfigItemStub::Create("Close and reopen this menu after changing high-risk acceptance."));

        char text[160];
        const std::string ip = Network::ConsoleIpAddress();
        std::snprintf(text, sizeof(text), "Web: http://%s:%d%s", ip.c_str(), Settings::webPort.load(), Settings::authEnabled.load() ? "/?key=CODE" : "/"); root.add(WUPSConfigItemStub::Create(text));
        std::snprintf(text, sizeof(text), "TV: http://%s:%d/", ip.c_str(), Settings::tvPort.load()); root.add(WUPSConfigItemStub::Create(text));
        std::snprintf(text, sizeof(text), "GamePad: http://%s:%d/", ip.c_str(), Settings::gamepadPort.load()); root.add(WUPSConfigItemStub::Create(text));
        root.add(WUPSConfigItemStub::Create(Network::ListenerStatusSummary()));
        if (!Settings::PortsAreValid()) root.add(WUPSConfigItemStub::Create("ERROR: the three ports must be unique and >=1024."));
    } catch (const std::exception &e) {
        OSReport("[WiiUWebStream] Config menu error: %s\n", e.what());
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosed() {
    Settings::EnforceSafeLimits();
    Settings::Save();
    ApplyRuntimeSettings();
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
