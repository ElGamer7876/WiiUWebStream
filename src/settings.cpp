#include "settings.hpp"

#include <coreinit/debug.h>
#include <wups.h>

namespace Settings {

std::atomic_bool enabled{DEFAULT_ENABLED};
std::atomic_int webPort{DEFAULT_WEB_PORT};
std::atomic_int tvPort{DEFAULT_TV_PORT};
std::atomic_int gamepadPort{DEFAULT_GAMEPAD_PORT};
std::atomic_bool tvEnabled{DEFAULT_TV_ENABLED};
std::atomic_bool gamepadEnabled{DEFAULT_GAMEPAD_ENABLED};
std::atomic_int tvFps{DEFAULT_TV_FPS};
std::atomic_int gamepadFps{DEFAULT_GAMEPAD_FPS};
std::atomic_int jpegQuality{DEFAULT_JPEG_QUALITY};

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

} // namespace

void Load() {
    LoadValue(KEY_ENABLED, enabled, DEFAULT_ENABLED);
    LoadValue(KEY_WEB_PORT, webPort, DEFAULT_WEB_PORT);
    LoadValue(KEY_TV_PORT, tvPort, DEFAULT_TV_PORT);
    LoadValue(KEY_GAMEPAD_PORT, gamepadPort, DEFAULT_GAMEPAD_PORT);
    LoadValue(KEY_TV_ENABLED, tvEnabled, DEFAULT_TV_ENABLED);
    LoadValue(KEY_GAMEPAD_ENABLED, gamepadEnabled, DEFAULT_GAMEPAD_ENABLED);
    LoadValue(KEY_TV_FPS, tvFps, DEFAULT_TV_FPS);
    LoadValue(KEY_GAMEPAD_FPS, gamepadFps, DEFAULT_GAMEPAD_FPS);
    LoadValue(KEY_JPEG_QUALITY, jpegQuality, DEFAULT_JPEG_QUALITY);

    Save();
}

void Save() {
    const auto result = WUPSStorageAPI::SaveStorage();
    if (result != WUPS_STORAGE_ERROR_SUCCESS) {
        OSReport("[WiiUWebStream] Storage save failed: %d\n", result);
    }
}

void SetEnabled(bool value)             { StoreValue(KEY_ENABLED, enabled, value); }
void SetWebPort(int value)              { StoreValue(KEY_WEB_PORT, webPort, value); }
void SetTvPort(int value)               { StoreValue(KEY_TV_PORT, tvPort, value); }
void SetGamePadPort(int value)          { StoreValue(KEY_GAMEPAD_PORT, gamepadPort, value); }
void SetTvEnabled(bool value)           { StoreValue(KEY_TV_ENABLED, tvEnabled, value); }
void SetGamePadEnabled(bool value)      { StoreValue(KEY_GAMEPAD_ENABLED, gamepadEnabled, value); }
void SetTvFps(int value)                { StoreValue(KEY_TV_FPS, tvFps, value); }
void SetGamePadFps(int value)           { StoreValue(KEY_GAMEPAD_FPS, gamepadFps, value); }
void SetJpegQuality(int value)          { StoreValue(KEY_JPEG_QUALITY, jpegQuality, value); }

bool PortsAreValid() {
    const int web = webPort.load();
    const int tv = tvPort.load();
    const int gamepad = gamepadPort.load();

    const bool rangeOk =
            web >= 1024 && web <= 65535 &&
            tv >= 1024 && tv <= 65535 &&
            gamepad >= 1024 && gamepad <= 65535;

    return rangeOk && web != tv && web != gamepad && tv != gamepad;
}

} // namespace Settings
