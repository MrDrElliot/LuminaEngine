#include "ArrayOps.h"

#include "SIMDConfig.h"
#include "VFloat8.h"

namespace Lumina::SIMD
{
    void LerpArray(float* Out, const float* A, const float* B, int32 Count, float Alpha)
    {
        const VFloat8 VAlpha    = VFloat8::Broadcast(Alpha);
        const VFloat8 VOneMinus = VFloat8::Broadcast(1.0f - Alpha);

        int32 i = 0;
        for (; i + 8 <= Count; i += 8)
        {
            MulAdd(VFloat8::Load(A + i), VOneMinus, VFloat8::Load(B + i) * VAlpha).Store(Out + i);
        }

        const float OneMinus = 1.0f - Alpha;
        for (; i < Count; ++i)
        {
            Out[i] = A[i] * OneMinus + B[i] * Alpha;
        }
    }

    void LerpArrayVarAlpha(float* Out, const float* A, const float* B, const float* Alphas, int32 Count)
    {
        const VFloat8 One = VFloat8::Broadcast(1.0f);

        int32 i = 0;
        for (; i + 8 <= Count; i += 8)
        {
            const VFloat8 Vt = VFloat8::Load(Alphas + i);
            MulAdd(VFloat8::Load(A + i), One - Vt, VFloat8::Load(B + i) * Vt).Store(Out + i);
        }

        for (; i < Count; ++i)
        {
            Out[i] = A[i] * (1.0f - Alphas[i]) + B[i] * Alphas[i];
        }
    }

    void AddScaledArray(float* Out, const float* A, const float* B, float S, int32 Count)
    {
        const VFloat8 Vs = VFloat8::Broadcast(S);

        int32 i = 0;
        for (; i + 8 <= Count; i += 8)
        {
            MulAdd(VFloat8::Load(B + i), Vs, VFloat8::Load(A + i)).Store(Out + i);
        }

        for (; i < Count; ++i)
        {
            Out[i] = A[i] + B[i] * S;
        }
    }

    void MulAddArray(float* Out, const float* Base, const float* Dir, const float* Scale, int32 Count)
    {
        int32 i = 0;
        for (; i + 8 <= Count; i += 8)
        {
            MulAdd(VFloat8::Load(Dir + i), VFloat8::Load(Scale + i), VFloat8::Load(Base + i)).Store(Out + i);
        }

        for (; i < Count; ++i)
        {
            Out[i] = Base[i] + Dir[i] * Scale[i];
        }
    }

    void MulLerpOneArray(float* Out, const float* A, const float* B, float S, int32 Count)
    {
        const VFloat8 Vs  = VFloat8::Broadcast(S);
        const VFloat8 One = VFloat8::Broadcast(1.0f);

        int32 i = 0;
        for (; i + 8 <= Count; i += 8)
        {
            const VFloat8 Factor = MulAdd(VFloat8::Load(B + i) - One, Vs, One);
            (VFloat8::Load(A + i) * Factor).Store(Out + i);
        }

        for (; i < Count; ++i)
        {
            Out[i] = A[i] * (1.0f + S * (B[i] - 1.0f));
        }
    }
}
