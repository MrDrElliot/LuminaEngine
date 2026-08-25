#pragma once
#include "VFloat8.h"

// 8 quaternions as one VFloat8 per component, loaded from SoA pose streams with no transpose.

namespace Lumina::SIMD
{
    struct VQuat8
    {
        VFloat8 X, Y, Z, W;
    };

    [[nodiscard]] FORCEINLINE VQuat8 QuatIdentity8()
    {
        return { VFloat8::Zero(), VFloat8::Zero(), VFloat8::Zero(), VFloat8::Broadcast(1.0f) };
    }

    [[nodiscard]] FORCEINLINE VQuat8 LoadQuat8(const float* X, const float* Y, const float* Z, const float* W)
    {
        return { VFloat8::Load(X), VFloat8::Load(Y), VFloat8::Load(Z), VFloat8::Load(W) };
    }

    FORCEINLINE void StoreQuat8(float* X, float* Y, float* Z, float* W, const VQuat8& Q)
    {
        Q.X.Store(X);
        Q.Y.Store(Y);
        Q.Z.Store(Z);
        Q.W.Store(W);
    }

    [[nodiscard]] FORCEINLINE VFloat8 Dot(const VQuat8& A, const VQuat8& B)
    {
        return MulAdd(A.X, B.X, MulAdd(A.Y, B.Y, MulAdd(A.Z, B.Z, A.W * B.W)));
    }

    [[nodiscard]] FORCEINLINE VQuat8 Conjugate(const VQuat8& Q)
    {
        return { -Q.X, -Q.Y, -Q.Z, Q.W };
    }

    // Hamilton product, lane-wise; matches the scalar TQuat operator* (applies B then A).
    [[nodiscard]] FORCEINLINE VQuat8 Mul(const VQuat8& A, const VQuat8& B)
    {
        VQuat8 R;
        R.W = A.W * B.W - A.X * B.X - A.Y * B.Y - A.Z * B.Z;
        R.X = A.W * B.X + A.X * B.W + A.Y * B.Z - A.Z * B.Y;
        R.Y = A.W * B.Y - A.X * B.Z + A.Y * B.W + A.Z * B.X;
        R.Z = A.W * B.Z + A.X * B.Y - A.Y * B.X + A.Z * B.W;
        return R;
    }

    [[nodiscard]] FORCEINLINE VQuat8 Normalize(const VQuat8& Q)
    {
        const VFloat8 InvLen = InvSqrt(Max(Dot(Q, Q), VFloat8::Broadcast(1e-30f)));
        return { Q.X * InvLen, Q.Y * InvLen, Q.Z * InvLen, Q.W * InvLen };
    }

    namespace QuatDetail8
    {
        // sin(x) for x in [0, pi/2].
        [[nodiscard]] FORCEINLINE VFloat8 SinZeroHalfPI(VFloat8 A)
        {
            const VFloat8 S = A * A;
            VFloat8 T = VFloat8::Broadcast(-2.39e-08f);
            T = MulAdd(T, S, VFloat8::Broadcast(2.7526e-06f));
            T = MulAdd(T, S, VFloat8::Broadcast(-1.98409e-04f));
            T = MulAdd(T, S, VFloat8::Broadcast(8.3333315e-03f));
            T = MulAdd(T, S, VFloat8::Broadcast(-1.666666664e-01f));
            T = MulAdd(T, S, VFloat8::Broadcast(1.0f));
            return T * A;
        }

        // atan2(y, x) for y, x >= 0.
        [[nodiscard]] FORCEINLINE VFloat8 ATanPositive(VFloat8 Y, VFloat8 X)
        {
            const VFloat8 YGtX = CmpGt(Y, X);
            const VFloat8 A    = Select(YGtX, -(X / Y), Y / X);
            const VFloat8 D    = Select(YGtX, VFloat8::Broadcast(1.57079632679489662f), VFloat8::Zero());
            const VFloat8 S    = A * A;
            VFloat8 T = VFloat8::Broadcast(0.0028662257f);
            T = MulAdd(T, S, VFloat8::Broadcast(-0.0161657367f));
            T = MulAdd(T, S, VFloat8::Broadcast(0.0429096138f));
            T = MulAdd(T, S, VFloat8::Broadcast(-0.0752896400f));
            T = MulAdd(T, S, VFloat8::Broadcast(0.1065626393f));
            T = MulAdd(T, S, VFloat8::Broadcast(-0.1420889944f));
            T = MulAdd(T, S, VFloat8::Broadcast(0.1999355085f));
            T = MulAdd(T, S, VFloat8::Broadcast(-0.3333314528f));
            T = MulAdd(T, S, VFloat8::Broadcast(1.0f));
            return MulAdd(T, A, D);
        }
    }

    // Shortest-arc slerp with per-lane alpha; near-parallel lanes fall back to lerp. Result is normalized.
    [[nodiscard]] FORCEINLINE VQuat8 SlerpShortest(const VQuat8& A, const VQuat8& B, VFloat8 Alpha)
    {
        const VFloat8 One = VFloat8::Broadcast(1.0f);

        const VFloat8 Cosom    = Dot(A, B);
        const VFloat8 NegMask  = CmpLt(Cosom, VFloat8::Zero());
        const VFloat8 AbsCosom = Abs(Cosom);

        const VFloat8 SinSq = Max(One - AbsCosom * AbsCosom, VFloat8::Zero());
        const VFloat8 Sinom = Sqrt(SinSq);
        const VFloat8 Omega = QuatDetail8::ATanPositive(Sinom, AbsCosom);

        // Unselected lanes may divide by zero below; the lerp fallback masks them out.
        const VFloat8 InvSinom = One / Sinom;
        VFloat8 Scale0 = QuatDetail8::SinZeroHalfPI((One - Alpha) * Omega) * InvSinom;
        VFloat8 Scale1 = QuatDetail8::SinZeroHalfPI(Alpha * Omega) * InvSinom;

        const VFloat8 LerpMask = CmpLt(SinSq, VFloat8::Broadcast(1e-6f));
        Scale0 = Select(LerpMask, One - Alpha, Scale0);
        Scale1 = Select(LerpMask, Alpha, Scale1);

        // Taking the shortest path means folding the sign flip of B into its weight.
        Scale1 = Select(NegMask, -Scale1, Scale1);

        VQuat8 R;
        R.X = MulAdd(A.X, Scale0, B.X * Scale1);
        R.Y = MulAdd(A.Y, Scale0, B.Y * Scale1);
        R.Z = MulAdd(A.Z, Scale0, B.Z * Scale1);
        R.W = MulAdd(A.W, Scale0, B.W * Scale1);
        return Normalize(R);
    }

    // Count is always a multiple of 8 below, since pose rotation streams are padded to the vector width.

    struct FQuatStreams
    {
        float* X;
        float* Y;
        float* Z;
        float* W;
    };

    struct FConstQuatStreams
    {
        const float* X;
        const float* Y;
        const float* Z;
        const float* W;
    };

    [[nodiscard]] FORCEINLINE VQuat8 LoadAt(const FConstQuatStreams& S, int32 i)
    {
        return LoadQuat8(S.X + i, S.Y + i, S.Z + i, S.W + i);
    }

    FORCEINLINE void StoreAt(const FQuatStreams& S, int32 i, const VQuat8& Q)
    {
        StoreQuat8(S.X + i, S.Y + i, S.Z + i, S.W + i, Q);
    }

    // Out = SlerpShortest(A, B, Alpha). Out may alias A or B.
    inline void SlerpQuatStreams(const FQuatStreams& Out, const FConstQuatStreams& A, const FConstQuatStreams& B,
                                 int32 Count, float Alpha)
    {
        const VFloat8 VAlpha = VFloat8::Broadcast(Alpha);
        for (int32 i = 0; i < Count; i += 8)
        {
            StoreAt(Out, i, SlerpShortest(LoadAt(A, i), LoadAt(B, i), VAlpha));
        }
    }

    // Per-bone alpha variant; Alphas must hold Count entries.
    inline void SlerpQuatStreamsVarAlpha(const FQuatStreams& Out, const FConstQuatStreams& A, const FConstQuatStreams& B,
                                         const float* Alphas, int32 Count)
    {
        for (int32 i = 0; i < Count; i += 8)
        {
            StoreAt(Out, i, SlerpShortest(LoadAt(A, i), LoadAt(B, i), VFloat8::Load(Alphas + i)));
        }
    }

    // Out = A * Conjugate(B), which is the local-space additive rotation delta.
    inline void MulConjQuatStreams(const FQuatStreams& Out, const FConstQuatStreams& A, const FConstQuatStreams& B,
                                   int32 Count)
    {
        for (int32 i = 0; i < Count; i += 8)
        {
            StoreAt(Out, i, Mul(LoadAt(A, i), Conjugate(LoadAt(B, i))));
        }
    }
}
