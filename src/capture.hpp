#pragma once

#include "types.hpp"

namespace Capture {

bool Start();
void Stop();
bool IsRunning();

void RequestOne(VideoSource source);

} // namespace Capture
