#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>

namespace SdRecord {
void Append(VideoSource source, const uint8_t *jpeg, size_t size);
void Close();
} // namespace SdRecord
