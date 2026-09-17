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

    authCode.store(std::clamp(authCode.load(), 0, 999999));
    Save();
}

void Save() {
    const auto result = WUPSStorageAPI::SaveStorage();
    if (result != WUPS_STORAGE_ERROR_SUCCESS) {
        OSReport("[WiiUWebStream] Storage save failed: %d\n", result);
    }
}

void ApplyPreset(Preset value) {
    const int raw = static_cast<int>(value);
    StoreValue(KEY_PRESET, preset, raw);

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
        case Preset::Custom:
        default:
            break;
    }
}

void SetEnabled(bool value) { StoreValue(KEY_ENABLED, enabled, value); }
void SetPreset(int value) { ApplyPreset(static_cast<Preset>(std::clamp(value, 0, 4))); }
void SetWebPort(int value) { StoreValue(KEY_WEB_PORT, webPort, value); }
void SetTvPort(int value) { StoreValue(KEY_TV_PORT, tvPort, value); }
void SetGamePadPort(int value) { StoreValue(KEY_GAMEPAD_PORT, gamepadPort, value); }
void SetTvEnabled(bool value) { StoreValue(KEY_TV_ENABLED, tvEnabled, value); }
void SetGamePadEnabled(bool value) { StoreValue(KEY_GAMEPAD_ENABLED, gamepadEnabled, value); }
void SetTvFps(int value, bool markCustom) { StoreValue(KEY_TV_FPS, tvFps, std::clamp(value, 1, 15)); if (markCustom) MarkCustom(); }
void SetGamePadFps(int value, bool markCustom) { StoreValue(KEY_GAMEPAD_FPS, gamepadFps, std::clamp(value, 1, 15)); if (markCustom) MarkCustom(); }
void SetJpegQuality(int value, bool markCustom) { StoreValue(KEY_JPEG_QUALITY, jpegQuality, std::clamp(value, 35, 85)); if (markCustom) MarkCustom(); }
void SetTvResolution(int value, bool markCustom) { StoreValue(KEY_TV_RESOLUTION, tvResolution, std::clamp(value, 0, 3)); if (markCustom) MarkCustom(); }
void SetGamePadResolution(int value, bool markCustom) { StoreValue(KEY_GAMEPAD_RESOLUTION, gamepadResolution, std::clamp(value, 0, 3)); if (markCustom) MarkCustom(); }
void SetAdaptiveFps(bool value) { StoreValue(KEY_ADAPTIVE_FPS, adaptiveFps, value); }
void SetAuthEnabled(bool value) { StoreValue(KEY_AUTH_ENABLED, authEnabled, value); }
void SetAuthCode(int value) { StoreValue(KEY_AUTH_CODE, authCode, std::clamp(value, 0, 999999)); }
void SetWatchdogEnabled(bool value) { StoreValue(KEY_WATCHDOG_ENABLED, watchdogEnabled, value); }
void SetLogLevel(int value) { StoreValue(KEY_LOG_LEVEL, logLevel, std::clamp(value, 0, 3)); }

bool PortsAreValid() {
    const int web = webPort.load();
    const int tv = tvPort.load();
    const int gamepad = gamepadPort.load();
    const bool rangeOk = web >= 1024 && web <= 65535 && tv >= 1024 && tv <= 65535 && gamepad >= 1024 && gamepad <= 65535;
    return rangeOk && web != tv && web != gamepad && tv != gamepad;
}

void OutputDimensions(bool gamePad, uint32_t &width, uint32_t &height) {
    const int value = gamePad ? gamepadResolution.load() : tvResolution.load();
    switch (static_cast<Resolution>(std::clamp(value, 0, 3))) {
        case Resolution::R426x240: width = 426; height = 240; break;
        case Resolution::R640x360: width = 640; height = 360; break;
        case Resolution::R854x480: width = 854; height = 480; break;
        case Resolution::R960x540: width = 960; height = 540; break;
    }
}

const char *PresetName() {
    switch (static_cast<Preset>(std::clamp(preset.load(), 0, 4))) {
        case Preset::LowLatency: return "Low Latency";
        case Preset::Balanced: return "Balanced";
        case Preset::Quality: return "Quality";
        case Preset::OBS: return "OBS";
        case Preset::Custom: default: return "Custom";
    }
}

const char *ResolutionName(int value) {
    switch (static_cast<Resolution>(std::clamp(value, 0, 3))) {
        case Resolution::R426x240: return "426x240";
        case Resolution::R640x360: return "640x360";
        case Resolution::R854x480: return "854x480";
        case Resolution::R960x540: return "960x540";
    }
    return "unknown";
}

} // namespace Settings
