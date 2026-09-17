#pragma once

#include <cstdint>

enum class VideoSource : uint8_t {
    TV = 0,
    GamePad = 1,
};

inline const char *VideoSourceName(VideoSource source) {
    return source == VideoSource::TV ? "TV" : "GamePad";
}
