#pragma once

#include "types.hpp"

#include <cstdint>
#include <string>

namespace Safety {

struct Snapshot {
    bool governorEnabled = false;
    bool active = false;
    bool emergencyStopped = false;
    int level = 0;
    int tvFpsCap = 0;
    int gamepadFpsCap = 0;
    int jpegQualityCap = 0;
    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;
    uint64_t interventions = 0;
    std::string reason;
};

void Reset();
void Tick();

bool EmergencyStopped();
void EmergencyStop();
void ResumeStreaming();

int FpsCap(VideoSource source);
int JpegQualityCap();
void ClampDimensions(VideoSource source, uint32_t &width, uint32_t &height);

Snapshot GetSnapshot();

} // namespace Safety
