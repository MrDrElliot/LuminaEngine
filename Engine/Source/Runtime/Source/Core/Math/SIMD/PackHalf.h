#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina::SIMD
{
    // Packs Count xy pairs (2 * Count floats) into Count uint32, x in the low half.
    RUNTIME_API void PackHalf2x16Array(const float* Src, uint32* Dst, uint32 Count);
}
