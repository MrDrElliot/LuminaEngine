#pragma once
#include "Platform/Platform.h"
#include <immintrin.h>

// Shared config for Lumina::SIMD. Baseline is /arch:AVX (VFloat8 + SSE/AVX float always
// available); 256-bit integer ops and FMA are AVX2, guarded below to avoid #UD on AVX-only.

// MSVC defines __AVX2__ with /arch:AVX2 (which implies FMA3); clang/gcc define
// __FMA__ directly. Under the AVX baseline neither is set.
#if defined(__AVX2__) || defined(__FMA__)
    #define LUMINA_SIMD_HAS_FMA 1
#else
    #define LUMINA_SIMD_HAS_FMA 0
#endif

#define LUMINA_SIMD_ALIGN16 alignas(16)
#define LUMINA_SIMD_ALIGN32 alignas(32)

namespace Lumina::SIMD
{
    // Natural alignment for the widest register type (256-bit). Use for buffers
    // fed to LoadAligned/StoreAligned.
    inline constexpr int kAlignment = 32;
}
