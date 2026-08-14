#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina::SIMD
{
    // F16C is a separate CPUID bit that the /arch:AVX baseline does not imply.
    RUNTIME_API bool HasF16C();

    // Packs Count xy pairs (2 * Count floats) into Count uint32, x in the low half.
    RUNTIME_API void PackHalf2x16Array(const float* Src, uint32* Dst, uint32 Count);
}
