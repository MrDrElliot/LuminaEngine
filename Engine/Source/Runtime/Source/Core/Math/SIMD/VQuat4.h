#pragma once
#include "VFloat4.h"
#include "Core/Math/Quat/Quat.h"

// VQuat4: 4 quaternions in SoA form (one VFloat4 per component) for batch pose math.
// Loads/stores transpose from the engine's AoS FQuat (x,y,z,w) layout. Slerp uses
// sin/atan minimax polynomials (van Waveren), accurate to ~1e-6 over the full range.

namespace Lumina::SIMD
{
    struct VQuat4
    {
        VFloat4 X, Y, Z, W;
    };

    [[nodiscard]] FORCEINLINE VQuat4 QuatIdentity4()
    {
        return { VFloat4::Zero(), VFloat4::Zero(), VFloat4::Zero(), VFloat4::Broadcast(1.0f) };
    }

    // AoS -> SoA: 4 consecutive FQuats (16 floats) into component vectors.
    [[nodiscard]] FORCEINLINE VQuat4 LoadQuat4(const FQuat* P)
    {
        __m128 Q0 = _mm_loadu_ps(&P[0].x);
        __m128 Q1 = _mm_loadu_ps(&P[1].x);
        __m128 Q2 = _mm_loadu_ps(&P[2].x);
        __m128 Q3 = _mm_loadu_ps(&P[3].x);
        _MM_TRANSPOSE4_PS(Q0, Q1, Q2, Q3);
        return { Q0, Q1, Q2, Q3 };
    }

    FORCEINLINE void StoreQuat4(FQuat* P, const VQuat4& Q)
    {
        __m128 Q0 = Q.X.V;
        __m128 Q1 = Q.Y.V;
        __m128 Q2 = Q.Z.V;
        __m128 Q3 = Q.W.V;
        _MM_TRANSPOSE4_PS(Q0, Q1, Q2, Q3);
        _mm_storeu_ps(&P[0].x, Q0);
        _mm_storeu_ps(&P[1].x, Q1);
        _mm_storeu_ps(&P[2].x, Q2);
        _mm_storeu_ps(&P[3].x, Q3);
    }

    [[nodiscard]] FORCEINLINE VFloat4 Dot(const VQuat4& A, const VQuat4& B)
    {
        return MulAdd(A.X, B.X, MulAdd(A.Y, B.Y, MulAdd(A.Z, B.Z, A.W * B.W)));
    }

    [[nodiscard]] FORCEINLINE VQuat4 Conjugate(const VQuat4& Q)
    {
        return { -Q.X, -Q.Y, -Q.Z, Q.W };
    }

    // Hamilton product, lane-wise; matches the scalar TQuat operator* (applies B then A).
    [[nodiscard]] FORCEINLINE VQuat4 Mul(const VQuat4& A, const VQuat4& B)
    {
        VQuat4 R;
        R.W = A.W * B.W - A.X * B.X - A.Y * B.Y - A.Z * B.Z;
        R.X = A.W * B.X + A.X * B.W + A.Y * B.Z - A.Z * B.Y;
        R.Y = A.W * B.Y - A.X * B.Z + A.Y * B.W + A.Z * B.X;
        R.Z = A.W * B.Z + A.X * B.Y - A.Y * B.X + A.Z * B.W;
        return R;
    }

    [[nodiscard]] FORCEINLINE VQuat4 Normalize(const VQuat4& Q)
    {
        const VFloat4 InvLen = InvSqrt(Max(Dot(Q, Q), VFloat4::Broadcast(1e-30f)));
        return { Q.X * InvLen, Q.Y * InvLen, Q.Z * InvLen, Q.W * InvLen };
    }

    namespace QuatDetail
    {
        // sin(x) for x in [0, pi/2].
        [[nodiscard]] FORCEINLINE VFloat4 SinZeroHalfPI(VFloat4 A)
        {
            const VFloat4 S = A * A;
            VFloat4 T = VFloat4::Broadcast(-2.39e-08f);
            T = MulAdd(T, S, VFloat4::Broadcast(2.7526e-06f));
            T = MulAdd(T, S, VFloat4::Broadcast(-1.98409e-04f));
            T = MulAdd(T, S, VFloat4::Broadcast(8.3333315e-03f));
            T = MulAdd(T, S, VFloat4::Broadcast(-1.666666664e-01f));
            T = MulAdd(T, S, VFloat4::Broadcast(1.0f));
            return T * A;
        }

        // atan2(y, x) for y, x >= 0.
        [[nodiscard]] FORCEINLINE VFloat4 ATanPositive(VFloat4 Y, VFloat4 X)
        {
            const VFloat4 YGtX = CmpGt(Y, X);
            const VFloat4 A    = Select(YGtX, -(X / Y), Y / X);
            const VFloat4 D    = Select(YGtX, VFloat4::Broadcast(1.57079632679489662f), VFloat4::Zero());
            const VFloat4 S    = A * A;
            VFloat4 T = VFloat4::Broadcast(0.0028662257f);
            T = MulAdd(T, S, VFloat4::Broadcast(-0.0161657367f));
            T = MulAdd(T, S, VFloat4::Broadcast(0.0429096138f));
            T = MulAdd(T, S, VFloat4::Broadcast(-0.0752896400f));
            T = MulAdd(T, S, VFloat4::Broadcast(0.1065626393f));
            T = MulAdd(T, S, VFloat4::Broadcast(-0.1420889944f));
            T = MulAdd(T, S, VFloat4::Broadcast(0.1999355085f));
            T = MulAdd(T, S, VFloat4::Broadcast(-0.3333314528f));
            T = MulAdd(T, S, VFloat4::Broadcast(1.0f));
            return MulAdd(T, A, D);
        }
    }

    // Shortest-arc slerp with per-lane alpha; near-parallel lanes fall back to lerp.
    // Result is normalized. Inputs are expected normalized (animation-pose quats are).
    [[nodiscard]] FORCEINLINE VQuat4 SlerpShortest(const VQuat4& A, const VQuat4& B, VFloat4 Alpha)
    {
        const VFloat4 One = VFloat4::Broadcast(1.0f);

        const VFloat4 Cosom    = Dot(A, B);
        const VFloat4 NegMask  = CmpLt(Cosom, VFloat4::Zero());
        const VFloat4 AbsCosom = Abs(Cosom);

        const VFloat4 SinSq = Max(One - AbsCosom * AbsCosom, VFloat4::Zero());
        const VFloat4 Sinom = Sqrt(SinSq);
        const VFloat4 Omega = QuatDetail::ATanPositive(Sinom, AbsCosom);

        // Unselected lanes may divide by zero below; the lerp fallback masks them out.
        const VFloat4 InvSinom = One / Sinom;
        VFloat4 Scale0 = QuatDetail::SinZeroHalfPI((One - Alpha) * Omega) * InvSinom;
        VFloat4 Scale1 = QuatDetail::SinZeroHalfPI(Alpha * Omega) * InvSinom;

        const VFloat4 LerpMask = CmpLt(SinSq, VFloat4::Broadcast(1e-6f));
        Scale0 = Select(LerpMask, One - Alpha, Scale0);
        Scale1 = Select(LerpMask, Alpha, Scale1);

        // Shortest path: fold the sign flip of B into its weight.
        Scale1 = Select(NegMask, -Scale1, Scale1);

        VQuat4 R;
        R.X = MulAdd(A.X, Scale0, B.X * Scale1);
        R.Y = MulAdd(A.Y, Scale0, B.Y * Scale1);
        R.Z = MulAdd(A.Z, Scale0, B.Z * Scale1);
        R.W = MulAdd(A.W, Scale0, B.W * Scale1);
        return Normalize(R);
    }

    namespace QuatDetail
    {
        template <typename TAlphaFn>
        FORCEINLINE void BlendQuatArrayImpl(FQuat* Out, const FQuat* A, const FQuat* B, int Count, TAlphaFn&& AlphaAt)
        {
            int i = 0;
            for (; i + 4 <= Count; i += 4)
            {
                StoreQuat4(Out + i, SlerpShortest(LoadQuat4(A + i), LoadQuat4(B + i), AlphaAt(i)));
            }

            if (i < Count)
            {
                FQuat Ta[4], Tb[4], To[4];
                for (int j = 0; j < 4; ++j)
                {
                    const int Src = i + j < Count ? i + j : Count - 1;
                    Ta[j] = A[Src];
                    Tb[j] = B[Src];
                }
                StoreQuat4(To, SlerpShortest(LoadQuat4(Ta), LoadQuat4(Tb), AlphaAt(i)));
                for (int j = 0; i + j < Count; ++j)
                {
                    Out[i + j] = To[j];
                }
            }
        }
    }

    // Out[i] = SlerpShortest(A[i], B[i], Alpha). Out may alias A or B.
    inline void BlendQuatArray(FQuat* Out, const FQuat* A, const FQuat* B, int Count, float Alpha)
    {
        const VFloat4 VAlpha = VFloat4::Broadcast(Alpha);
        QuatDetail::BlendQuatArrayImpl(Out, A, B, Count, [&](int) { return VAlpha; });
    }

    // Per-element alpha variant; Alphas must hold Count entries.
    inline void BlendQuatArrayVarAlpha(FQuat* Out, const FQuat* A, const FQuat* B, const float* Alphas, int Count)
    {
        QuatDetail::BlendQuatArrayImpl(Out, A, B, Count, [&](int i)
        {
            if (i + 4 <= Count)
            {
                return VFloat4::Load(Alphas + i);
            }
            LUMINA_SIMD_ALIGN16 float Tail[4] = {};
            for (int j = 0; i + j < Count; ++j)
            {
                Tail[j] = Alphas[i + j];
            }
            return VFloat4::LoadAligned(Tail);
        });
    }
}
