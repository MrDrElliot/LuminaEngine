#pragma once

// Lumina::SIMD -- thin x86 intrinsic wrappers (VFloat4/VFloat8) for hand-vectorized hot
// loops; not a TVec replacement. LoadAligned/StoreAligned buffers must be kAlignment-aligned.

#include "SIMDConfig.h"
#include "VFloat4.h"
#include "VFloat8.h"
#include "VQuat4.h"

namespace Lumina::SIMD
{
    // Lerp over Count floats, bit-identical to Math::Mix under /arch:AVX.
    // Out may alias A or B (element-wise); tail handled scalar.
    inline void LerpArray(float* Out, const float* A, const float* B, int Count, float Alpha)
    {
        const VFloat8 VAlpha    = VFloat8::Broadcast(Alpha);
        const VFloat8 VOneMinus = VFloat8::Broadcast(1.0f - Alpha);

        int i = 0;
        for (; i + 8 <= Count; i += 8)
        {
            const VFloat8 Va = VFloat8::Load(A + i);
            const VFloat8 Vb = VFloat8::Load(B + i);
            MulAdd(Va, VOneMinus, Vb * VAlpha).Store(Out + i);
        }

        const float OneMinus = 1.0f - Alpha;
        for (; i < Count; ++i)
        {
            Out[i] = A[i] * OneMinus + B[i] * Alpha;
        }
    }

    // Per-element alpha lerp; Alphas holds Count entries. Out may alias A or B.
    inline void LerpArrayVarAlpha(float* Out, const float* A, const float* B, const float* Alphas, int Count)
    {
        const VFloat8 One = VFloat8::Broadcast(1.0f);

        int i = 0;
        for (; i + 8 <= Count; i += 8)
        {
            const VFloat8 Va = VFloat8::Load(A + i);
            const VFloat8 Vb = VFloat8::Load(B + i);
            const VFloat8 Vt = VFloat8::Load(Alphas + i);
            MulAdd(Va, One - Vt, Vb * Vt).Store(Out + i);
        }

        for (; i < Count; ++i)
        {
            Out[i] = A[i] * (1.0f - Alphas[i]) + B[i] * Alphas[i];
        }
    }
}
