#pragma once

#include <atomic>
#include <cstdint>

namespace Settings {

inline constexpr const char *STORAGE_ID = "wiiu_web_stream";

inline constexpr const char *KEY_ENABLED         = "enabled";
inline constexpr const char *KEY_WEB_PORT        = "webPort";
inline constexpr const char *KEY_TV_PORT         = "tvPort";
inline constexpr const char *KEY_GAMEPAD_PORT    = "gamepadPort";
inline constexpr const char *KEY_TV_ENABLED      = "tvEnabled";
inline constexpr const char *KEY_GAMEPAD_ENABLED = "gamepadEnabled";
inline constexpr const char *KEY_TV_FPS          = "tvFps";
inline constexpr const char *KEY_GAMEPAD_FPS     = "gamepadFps";
inline constexpr const char *KEY_JPEG_QUALITY    = "jpegQuality";

inline constexpr bool DEFAULT_ENABLED         = true;
inline constexpr int DEFAULT_WEB_PORT         = 7770;
inline constexpr int DEFAULT_TV_PORT          = 7771;
inline constexpr int DEFAULT_GAMEPAD_PORT     = 7772;
inline constexpr bool DEFAULT_TV_ENABLED      = true;
inline constexpr bool DEFAULT_GAMEPAD_ENABLED = true;
inline constexpr int DEFAULT_TV_FPS           = 5;
inline constexpr int DEFAULT_GAMEPAD_FPS      = 5;
inline constexpr int DEFAULT_JPEG_QUALITY     = 70;

inline constexpr uint32_t TV_OUTPUT_WIDTH      = 640;
inline constexpr uint32_t TV_OUTPUT_HEIGHT     = 360;
inline constexpr uint32_t GAMEPAD_OUTPUT_WIDTH = 854;
inline constexpr uint32_t GAMEPAD_OUTPUT_HEIGHT = 480;

extern std::atomic_bool enabled;
extern std::atomic_int webPort;
extern std::atomic_int tvPort;
extern std::atomic_int gamepadPort;
extern std::atomic_bool tvEnabled;
extern std::atomic_bool gamepadEnabled;
extern std::atomic_int tvFps;
extern std::atomic_int gamepadFps;
extern std::atomic_int jpegQuality;

void Load();
void Save();

void SetEnabled(bool value);
void SetWebPort(int value);
void SetTvPort(int value);
void SetGamePadPort(int value);
void SetTvEnabled(bool value);
void SetGamePadEnabled(bool value);
void SetTvFps(int value);
void SetGamePadFps(int value);
void SetJpegQuality(int value);

bool PortsAreValid();

} // namespace Settings
