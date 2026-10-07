#pragma once
#include "Platform/Platform.h"
#include <immintrin.h>

// Lumina::SIMD assumes AVX2 with FMA and F16C throughout, so a build below that level is an error rather than a slow path.
#if (defined(_M_X64) || defined(__x86_64__)) && !defined(__AVX2__)
    #error "Lumina targets AVX2. Set VectorExtensions to AVX2 for this target."
#endif

#define LUMINA_SIMD_ALIGN16 alignas(16)
#define LUMINA_SIMD_ALIGN32 alignas(32)

namespace Lumina::SIMD
{
    // Natural alignment for the widest register type (256-bit). Use for buffers
    // fed to LoadAligned/StoreAligned.
    inline constexpr int kAlignment = 32;
}
