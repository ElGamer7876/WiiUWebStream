#include "settings.hpp"

#include <algorithm>
#include <coreinit/debug.h>
#include <wups.h>

namespace Settings {

std::atomic_bool enabled{DEFAULT_ENABLED};
std::atomic_int preset{DEFAULT_PRESET};
std::atomic_int webPort{DEFAULT_WEB_PORT};
std::atomic_int tvPort{DEFAULT_TV_PORT};
std::atomic_int gamepadPort{DEFAULT_GAMEPAD_PORT};
std::atomic_bool tvEnabled{DEFAULT_TV_ENABLED};
std::atomic_bool gamepadEnabled{DEFAULT_GAMEPAD_ENABLED};
std::atomic_int tvFps{DEFAULT_TV_FPS};
std::atomic_int gamepadFps{DEFAULT_GAMEPAD_FPS};
std::atomic_int jpegQuality{DEFAULT_JPEG_QUALITY};
std::atomic_int tvResolution{DEFAULT_TV_RESOLUTION};
std::atomic_int gamepadResolution{DEFAULT_GAMEPAD_RESOLUTION};
std::atomic_bool adaptiveFps{DEFAULT_ADAPTIVE_FPS};
std::atomic_bool authEnabled{DEFAULT_AUTH_ENABLED};
std::atomic_int authCode{DEFAULT_AUTH_CODE};
std::atomic_bool watchdogEnabled{DEFAULT_WATCHDOG_ENABLED};
std::atomic_int logLevel{DEFAULT_LOG_LEVEL};
std::atomic_bool highRiskEnabled{DEFAULT_HIGH_RISK_ENABLED};
std::atomic_bool highRiskAccepted{DEFAULT_HIGH_RISK_ACCEPTED};
std::atomic_bool audioStreaming{DEFAULT_AUDIO_STREAMING};
std::atomic_bool continuousCapture{DEFAULT_CONTINUOUS_CAPTURE};
std::atomic_bool safetyGovernor{DEFAULT_SAFETY_GOVERNOR};
std::atomic_bool safetyWarningAccepted{DEFAULT_SAFETY_WARNING_ACCEPTED};

namespace {

template<typename T>
void LoadValue(const char *key, std::atomic<T> &target, T defaultValue) {
    T value = defaultValue;
    const auto result = WUPSStorageAPI::GetOrStoreDefault(key, value, defaultValue);
    if (result != WUPS_STORAGE_ERROR_SUCCESS) {
        OSReport("[WiiUWebStream] Storage read failed for %s: %d\n", key, result);
        value = defaultValue;
    }
    target.store(value);
}

template<typename T>
void StoreValue(const char *key, std::atomic<T> &target, T value) {
    target.store(value);
    const auto result = WUPSStorageAPI::Store(key, value);
    if (result != WUPS_STORAGE_ERROR_SUCCESS) {
        OSReport("[WiiUWebStream] Storage write failed for %s: %d\n", key, result);
    }
}

void MarkCustom() {
    if (preset.load() != static_cast<int>(Preset::Custom)) {
        StoreValue(KEY_PRESET, preset, static_cast<int>(Preset::Custom));
    }
}

} // namespace

bool HighRiskAccepted() {
    return highRiskEnabled.load() && highRiskAccepted.load();
}

void EnforceSafeLimits() {
    if (HighRiskAccepted()) return;
    if (tvFps.load() > 15) StoreValue(KEY_TV_FPS, tvFps, 15);
    if (gamepadFps.load() > 15) StoreValue(KEY_GAMEPAD_FPS, gamepadFps, 15);
    if (jpegQuality.load() > 85) StoreValue(KEY_JPEG_QUALITY, jpegQuality, 85);
    if (tvResolution.load() > static_cast<int>(Resolution::R960x540)) StoreValue(KEY_TV_RESOLUTION, tvResolution, static_cast<int>(Resolution::R960x540));
    if (gamepadResolution.load() > static_cast<int>(Resolution::R854x480)) StoreValue(KEY_GAMEPAD_RESOLUTION, gamepadResolution, static_cast<int>(Resolution::R854x480));
    if (audioStreaming.load()) StoreValue(KEY_AUDIO_STREAMING, audioStreaming, false);
    if (continuousCapture.load()) StoreValue(KEY_CONTINUOUS_CAPTURE, continuousCapture, false);
}

void Load() {
    LoadValue(KEY_ENABLED, enabled, DEFAULT_ENABLED);
    LoadValue(KEY_PRESET, preset, DEFAULT_PRESET);
    LoadValue(KEY_WEB_PORT, webPort, DEFAULT_WEB_PORT);
    LoadValue(KEY_TV_PORT, tvPort, DEFAULT_TV_PORT);
    LoadValue(KEY_GAMEPAD_PORT, gamepadPort, DEFAULT_GAMEPAD_PORT);
    LoadValue(KEY_TV_ENABLED, tvEnabled, DEFAULT_TV_ENABLED);
    LoadValue(KEY_GAMEPAD_ENABLED, gamepadEnabled, DEFAULT_GAMEPAD_ENABLED);
    LoadValue(KEY_TV_FPS, tvFps, DEFAULT_TV_FPS);
    LoadValue(KEY_GAMEPAD_FPS, gamepadFps, DEFAULT_GAMEPAD_FPS);
    LoadValue(KEY_JPEG_QUALITY, jpegQuality, DEFAULT_JPEG_QUALITY);
    LoadValue(KEY_TV_RESOLUTION, tvResolution, DEFAULT_TV_RESOLUTION);
    LoadValue(KEY_GAMEPAD_RESOLUTION, gamepadResolution, DEFAULT_GAMEPAD_RESOLUTION);
    LoadValue(KEY_ADAPTIVE_FPS, adaptiveFps, DEFAULT_ADAPTIVE_FPS);
    LoadValue(KEY_AUTH_ENABLED, authEnabled, DEFAULT_AUTH_ENABLED);
    LoadValue(KEY_AUTH_CODE, authCode, DEFAULT_AUTH_CODE);
    LoadValue(KEY_WATCHDOG_ENABLED, watchdogEnabled, DEFAULT_WATCHDOG_ENABLED);
    LoadValue(KEY_LOG_LEVEL, logLevel, DEFAULT_LOG_LEVEL);
    LoadValue(KEY_HIGH_RISK_ENABLED, highRiskEnabled, DEFAULT_HIGH_RISK_ENABLED);
    LoadValue(KEY_HIGH_RISK_ACCEPTED, highRiskAccepted, DEFAULT_HIGH_RISK_ACCEPTED);
    LoadValue(KEY_AUDIO_STREAMING, audioStreaming, DEFAULT_AUDIO_STREAMING);
    LoadValue(KEY_CONTINUOUS_CAPTURE, continuousCapture, DEFAULT_CONTINUOUS_CAPTURE);
    LoadValue(KEY_SAFETY_GOVERNOR, safetyGovernor, DEFAULT_SAFETY_GOVERNOR);
    LoadValue(KEY_SAFETY_WARNING_ACCEPTED, safetyWarningAccepted, DEFAULT_SAFETY_WARNING_ACCEPTED);

    authCode.store(std::clamp(authCode.load(), 0, 999999));
    EnforceSafeLimits();
    Save();
}

void Save() {
    const auto result = WUPSStorageAPI::SaveStorage();
    if (result != WUPS_STORAGE_ERROR_SUCCESS) {
        OSReport("[WiiUWebStream] Storage save failed: %d\n", result);
    }
}

void ApplyPreset(Preset value) {
    StoreValue(KEY_PRESET, preset, static_cast<int>(value));
    switch (value) {
        case Preset::LowLatency:
            StoreValue(KEY_TV_FPS, tvFps, 12);
            StoreValue(KEY_GAMEPAD_FPS, gamepadFps, 10);
            StoreValue(KEY_JPEG_QUALITY, jpegQuality, 55);
            StoreValue(KEY_TV_RESOLUTION, tvResolution, static_cast<int>(Resolution::R640x360));
            StoreValue(KEY_GAMEPAD_RESOLUTION, gamepadResolution, static_cast<int>(Resolution::R640x360));
            StoreValue(KEY_ADAPTIVE_FPS, adaptiveFps, true);
            break;
        case Preset::Balanced:
            StoreValue(KEY_TV_FPS, tvFps, 8);
            StoreValue(KEY_GAMEPAD_FPS, gamepadFps, 8);
            StoreValue(KEY_JPEG_QUALITY, jpegQuality, 70);
            StoreValue(KEY_TV_RESOLUTION, tvResolution, static_cast<int>(Resolution::R640x360));
            StoreValue(KEY_GAMEPAD_RESOLUTION, gamepadResolution, static_cast<int>(Resolution::R854x480));
            StoreValue(KEY_ADAPTIVE_FPS, adaptiveFps, true);
            break;
        case Preset::Quality:
            StoreValue(KEY_TV_FPS, tvFps, 5);
            StoreValue(KEY_GAMEPAD_FPS, gamepadFps, 5);
            StoreValue(KEY_JPEG_QUALITY, jpegQuality, 85);
            StoreValue(KEY_TV_RESOLUTION, tvResolution, static_cast<int>(Resolution::R960x540));
            StoreValue(KEY_GAMEPAD_RESOLUTION, gamepadResolution, static_cast<int>(Resolution::R854x480));
            StoreValue(KEY_ADAPTIVE_FPS, adaptiveFps, true);
            break;
        case Preset::OBS:
            StoreValue(KEY_TV_FPS, tvFps, 10);
            StoreValue(KEY_GAMEPAD_FPS, gamepadFps, 8);
            StoreValue(KEY_JPEG_QUALITY, jpegQuality, 72);
            StoreValue(KEY_TV_RESOLUTION, tvResolution, static_cast<int>(Resolution::R960x540));
            StoreValue(KEY_GAMEPAD_RESOLUTION, gamepadResolution, static_cast<int>(Resolution::R854x480));
            StoreValue(KEY_ADAPTIVE_FPS, adaptiveFps, true);
            break;
        case Preset::Recovery:
            StoreValue(KEY_TV_FPS, tvFps, 5);
            StoreValue(KEY_GAMEPAD_FPS, gamepadFps, 5);
            StoreValue(KEY_JPEG_QUALITY, jpegQuality, 60);
            StoreValue(KEY_TV_RESOLUTION, tvResolution, static_cast<int>(Resolution::R640x360));
            StoreValue(KEY_GAMEPAD_RESOLUTION, gamepadResolution, static_cast<int>(Resolution::R640x360));
            StoreValue(KEY_ADAPTIVE_FPS, adaptiveFps, true);
            StoreValue(KEY_HIGH_RISK_ENABLED, highRiskEnabled, false);
            StoreValue(KEY_HIGH_RISK_ACCEPTED, highRiskAccepted, false);
            StoreValue(KEY_AUDIO_STREAMING, audioStreaming, false);
            StoreValue(KEY_CONTINUOUS_CAPTURE, continuousCapture, false);
            break;
        case Preset::Custom:
        default:
            break;
    }
}

void SetEnabled(bool value) { StoreValue(KEY_ENABLED, enabled, value); }
void SetPreset(int value) { ApplyPreset(static_cast<Preset>(std::clamp(value, 0, 5))); }
void SetWebPort(int value) { StoreValue(KEY_WEB_PORT, webPort, value); }
void SetTvPort(int value) { StoreValue(KEY_TV_PORT, tvPort, value); }
void SetGamePadPort(int value) { StoreValue(KEY_GAMEPAD_PORT, gamepadPort, value); }
void SetTvEnabled(bool value) { StoreValue(KEY_TV_ENABLED, tvEnabled, value); }
void SetGamePadEnabled(bool value) { StoreValue(KEY_GAMEPAD_ENABLED, gamepadEnabled, value); }
void SetTvFps(int value, bool markCustom) { StoreValue(KEY_TV_FPS, tvFps, std::clamp(value, 1, HighRiskAccepted() ? 60 : 15)); if (markCustom) MarkCustom(); }
void SetGamePadFps(int value, bool markCustom) { StoreValue(KEY_GAMEPAD_FPS, gamepadFps, std::clamp(value, 1, HighRiskAccepted() ? 60 : 15)); if (markCustom) MarkCustom(); }
void SetJpegQuality(int value, bool markCustom) { StoreValue(KEY_JPEG_QUALITY, jpegQuality, std::clamp(value, 35, HighRiskAccepted() ? 95 : 85)); if (markCustom) MarkCustom(); }
void SetTvResolution(int value, bool markCustom) { StoreValue(KEY_TV_RESOLUTION, tvResolution, std::clamp(value, 0, HighRiskAccepted() ? 5 : 3)); if (markCustom) MarkCustom(); }
void SetGamePadResolution(int value, bool markCustom) { StoreValue(KEY_GAMEPAD_RESOLUTION, gamepadResolution, std::clamp(value, 0, HighRiskAccepted() ? 5 : 2)); if (markCustom) MarkCustom(); }
void SetAdaptiveFps(bool value) { StoreValue(KEY_ADAPTIVE_FPS, adaptiveFps, value); }
void SetAuthEnabled(bool value) { StoreValue(KEY_AUTH_ENABLED, authEnabled, value); }
void SetAuthCode(int value) { StoreValue(KEY_AUTH_CODE, authCode, std::clamp(value, 0, 999999)); }
void SetWatchdogEnabled(bool value) { StoreValue(KEY_WATCHDOG_ENABLED, watchdogEnabled, value); }
void SetLogLevel(int value) { StoreValue(KEY_LOG_LEVEL, logLevel, std::clamp(value, 0, 3)); }
void SetHighRiskEnabled(bool value) { StoreValue(KEY_HIGH_RISK_ENABLED, highRiskEnabled, value); if (!value) { StoreValue(KEY_HIGH_RISK_ACCEPTED, highRiskAccepted, false); EnforceSafeLimits(); } }
void SetHighRiskAccepted(bool value) { StoreValue(KEY_HIGH_RISK_ACCEPTED, highRiskAccepted, value); if (!value) EnforceSafeLimits(); }
void SetAudioStreaming(bool value) { StoreValue(KEY_AUDIO_STREAMING, audioStreaming, HighRiskAccepted() && value); }
void SetContinuousCapture(bool value) { StoreValue(KEY_CONTINUOUS_CAPTURE, continuousCapture, HighRiskAccepted() && value); }
void SetSafetyGovernor(bool value) { StoreValue(KEY_SAFETY_GOVERNOR, safetyGovernor, value); }
void SetSafetyWarningAccepted(bool value) { StoreValue(KEY_SAFETY_WARNING_ACCEPTED, safetyWarningAccepted, value); }

bool PortsAreValid() {
    const int web = webPort.load();
    const int tv = tvPort.load();
    const int gamepad = gamepadPort.load();
    const bool rangeOk = web >= 1024 && web <= 65535 && tv >= 1024 && tv <= 65535 && gamepad >= 1024 && gamepad <= 65535;
    return rangeOk && web != tv && web != gamepad && tv != gamepad;
}

void OutputDimensions(bool gamePad, uint32_t &width, uint32_t &height) {
    const int maxValue = HighRiskAccepted() ? 5 : (gamePad ? 2 : 3);
    const int value = std::clamp(gamePad ? gamepadResolution.load() : tvResolution.load(), 0, maxValue);
    switch (static_cast<Resolution>(value)) {
        case Resolution::R426x240: width = 426; height = 240; break;
        case Resolution::R640x360: width = 640; height = 360; break;
        case Resolution::R854x480: width = 854; height = 480; break;
        case Resolution::R960x540: width = 960; height = 540; break;
        case Resolution::R1280x720: width = 1280; height = 720; break;
        case Resolution::R1920x1080: width = 1920; height = 1080; break;
    }
}

const char *PresetName() {
    switch (static_cast<Preset>(std::clamp(preset.load(), 0, 5))) {
        case Preset::LowLatency: return "Low Latency";
        case Preset::Balanced: return "Balanced";
        case Preset::Quality: return "Quality";
        case Preset::OBS: return "OBS";
        case Preset::Recovery: return "Recovery";
        case Preset::Custom: default: return "Custom";
    }
}

const char *ResolutionName(int value) {
    switch (static_cast<Resolution>(std::clamp(value, 0, 5))) {
        case Resolution::R426x240: return "426x240";
        case Resolution::R640x360: return "640x360";
        case Resolution::R854x480: return "854x480";
        case Resolution::R960x540: return "960x540";
        case Resolution::R1280x720: return "1280x720 (HIGH RISK)";
        case Resolution::R1920x1080: return "1920x1080 (HIGH RISK)";
    }
    return "unknown";
}

} // namespace Settings
