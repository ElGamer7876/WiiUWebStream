#pragma once

#include <atomic>
#include <cstdint>

namespace Settings {

enum class Preset : int {
    Custom = 0,
    LowLatency = 1,
    Balanced = 2,
    Quality = 3,
    OBS = 4,
    Recovery = 5,
};

enum class Resolution : int {
    R426x240 = 0,
    R640x360 = 1,
    R854x480 = 2,
    R960x540 = 3,
    R1280x720 = 4,
    R1920x1080 = 5,
};

enum class LogLevel : int {
    Off = 0,
    Error = 1,
    Info = 2,
    Verbose = 3,
};

inline constexpr const char *STORAGE_ID = "wiiu_web_stream";
inline constexpr const char *KEY_ENABLED = "enabled";
inline constexpr const char *KEY_PRESET = "preset";
inline constexpr const char *KEY_WEB_PORT = "webPort";
inline constexpr const char *KEY_TV_PORT = "tvPort";
inline constexpr const char *KEY_GAMEPAD_PORT = "gamepadPort";
inline constexpr const char *KEY_TV_ENABLED = "tvEnabled";
inline constexpr const char *KEY_GAMEPAD_ENABLED = "gamepadEnabled";
inline constexpr const char *KEY_TV_FPS = "tvFps";
inline constexpr const char *KEY_GAMEPAD_FPS = "gamepadFps";
inline constexpr const char *KEY_JPEG_QUALITY = "jpegQuality";
inline constexpr const char *KEY_TV_RESOLUTION = "tvResolution";
inline constexpr const char *KEY_GAMEPAD_RESOLUTION = "gamepadResolution";
inline constexpr const char *KEY_ADAPTIVE_FPS = "adaptiveFps";
inline constexpr const char *KEY_AUTH_ENABLED = "authEnabled";
inline constexpr const char *KEY_AUTH_CODE = "authCode";
inline constexpr const char *KEY_WATCHDOG_ENABLED = "watchdogEnabled";
inline constexpr const char *KEY_LOG_LEVEL = "logLevel";
inline constexpr const char *KEY_HIGH_RISK_ENABLED = "highRiskEnabled";
inline constexpr const char *KEY_HIGH_RISK_ACCEPTED = "highRiskAccepted";
inline constexpr const char *KEY_AUDIO_STREAMING = "audioStreaming";
inline constexpr const char *KEY_CONTINUOUS_CAPTURE = "continuousCapture";
inline constexpr const char *KEY_SAFETY_GOVERNOR = "safetyGovernor";
inline constexpr const char *KEY_SAFETY_WARNING_ACCEPTED = "safetyWarningAccepted";

inline constexpr bool DEFAULT_ENABLED = true;
inline constexpr int DEFAULT_PRESET = static_cast<int>(Preset::Balanced);
inline constexpr int DEFAULT_WEB_PORT = 7770;
inline constexpr int DEFAULT_TV_PORT = 7771;
inline constexpr int DEFAULT_GAMEPAD_PORT = 7772;
inline constexpr bool DEFAULT_TV_ENABLED = true;
inline constexpr bool DEFAULT_GAMEPAD_ENABLED = true;
inline constexpr int DEFAULT_TV_FPS = 8;
inline constexpr int DEFAULT_GAMEPAD_FPS = 8;
inline constexpr int DEFAULT_JPEG_QUALITY = 70;
inline constexpr int DEFAULT_TV_RESOLUTION = static_cast<int>(Resolution::R640x360);
inline constexpr int DEFAULT_GAMEPAD_RESOLUTION = static_cast<int>(Resolution::R854x480);
inline constexpr bool DEFAULT_ADAPTIVE_FPS = true;
inline constexpr bool DEFAULT_AUTH_ENABLED = false;
inline constexpr int DEFAULT_AUTH_CODE = 777777;
inline constexpr bool DEFAULT_WATCHDOG_ENABLED = true;
inline constexpr int DEFAULT_LOG_LEVEL = static_cast<int>(LogLevel::Info);
inline constexpr bool DEFAULT_HIGH_RISK_ENABLED = false;
inline constexpr bool DEFAULT_HIGH_RISK_ACCEPTED = false;
inline constexpr bool DEFAULT_AUDIO_STREAMING = false;
inline constexpr bool DEFAULT_CONTINUOUS_CAPTURE = false;
inline constexpr bool DEFAULT_SAFETY_GOVERNOR = true;
inline constexpr bool DEFAULT_SAFETY_WARNING_ACCEPTED = false;

extern std::atomic_bool enabled;
extern std::atomic_int preset;
extern std::atomic_int webPort;
extern std::atomic_int tvPort;
extern std::atomic_int gamepadPort;
extern std::atomic_bool tvEnabled;
extern std::atomic_bool gamepadEnabled;
extern std::atomic_int tvFps;
extern std::atomic_int gamepadFps;
extern std::atomic_int jpegQuality;
extern std::atomic_int tvResolution;
extern std::atomic_int gamepadResolution;
extern std::atomic_bool adaptiveFps;
extern std::atomic_bool authEnabled;
extern std::atomic_int authCode;
extern std::atomic_bool watchdogEnabled;
extern std::atomic_int logLevel;
extern std::atomic_bool highRiskEnabled;
extern std::atomic_bool highRiskAccepted;
extern std::atomic_bool audioStreaming;
extern std::atomic_bool continuousCapture;
extern std::atomic_bool safetyGovernor;
extern std::atomic_bool safetyWarningAccepted;

void Load();
void Save();
void ApplyPreset(Preset value);

void SetEnabled(bool value);
void SetPreset(int value);
void SetWebPort(int value);
void SetTvPort(int value);
void SetGamePadPort(int value);
void SetTvEnabled(bool value);
void SetGamePadEnabled(bool value);
void SetTvFps(int value, bool markCustom = true);
void SetGamePadFps(int value, bool markCustom = true);
void SetJpegQuality(int value, bool markCustom = true);
void SetTvResolution(int value, bool markCustom = true);
void SetGamePadResolution(int value, bool markCustom = true);
void SetAdaptiveFps(bool value);
void SetAuthEnabled(bool value);
void SetAuthCode(int value);
void SetWatchdogEnabled(bool value);
void SetLogLevel(int value);
void SetHighRiskEnabled(bool value);
void SetHighRiskAccepted(bool value);
void SetAudioStreaming(bool value);
void SetContinuousCapture(bool value);
void SetSafetyGovernor(bool value);
void SetSafetyWarningAccepted(bool value);

bool HighRiskAccepted();
void EnforceSafeLimits();
bool PortsAreValid();
void OutputDimensions(bool gamePad, uint32_t &width, uint32_t &height);
const char *PresetName();
const char *ResolutionName(int value);

} // namespace Settings
