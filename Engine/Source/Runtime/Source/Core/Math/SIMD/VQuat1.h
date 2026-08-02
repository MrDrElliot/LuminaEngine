#pragma once
#include "VFloat4.h"
#include "Core/Math/Quat/Quat.h"

// Single-quaternion SSE kernels (one __m128 = x,y,z,w in lanes 0..3), the AoS counterpart to the SoA
// VQuat4 batch path. Used by the SIMD FTransform so compose/rotate stay in registers. Quaternion math is
// shuffle-bound in AoS (the SoA VQuat4 is faster for 4-at-once), but this keeps a single transform's
// chain (Parent * Local, Rotation * Vector) off the scalar path. Lane-wise math matches scalar TQuat.

namespace Lumina::SIMD
{
    // 3D cross product on the xyz lanes; the w lane is 0 (the w terms cancel), so it's safe on quats too.
    [[nodiscard]] FORCEINLINE VFloat4 Cross(VFloat4 A, VFloat4 B)
    {
        // (Ay*Bz - Az*By, Az*Bx - Ax*Bz, Ax*By - Ay*Bx, 0)
        const VFloat4 A_yzx = Permute<1, 2, 0, 3>(A);
        const VFloat4 A_zxy = Permute<2, 0, 1, 3>(A);
        const VFloat4 B_yzx = Permute<1, 2, 0, 3>(B);
        const VFloat4 B_zxy = Permute<2, 0, 1, 3>(B);
        return A_yzx * B_zxy - A_zxy * B_yzx;
    }

    // Hamilton product A*B (applies B then A), matching the scalar TQuat operator* and the SoA VQuat4::Mul.
    // Built from 4 splat*permuted-B terms with per-lane sign flips; verified lane-for-lane against scalar.
    [[nodiscard]] FORCEINLINE VFloat4 QuatMul(VFloat4 A, VFloat4 B)
    {
        // x = aw*bx + ax*bw + ay*bz - az*by
        // y = aw*by - ax*bz + ay*bw + az*bx
        // z = aw*bz + ax*by - ay*bx + az*bw
        // w = aw*bw - ax*bx - ay*by - az*bz
        const VFloat4 S1(+1.0f, -1.0f, +1.0f, -1.0f);
        const VFloat4 S2(+1.0f, +1.0f, -1.0f, -1.0f);
        const VFloat4 S3(-1.0f, +1.0f, +1.0f, -1.0f);

        VFloat4 R = SplatW(A) * B;                                          // aw * (bx,by,bz,bw)
        R = MulAdd(SplatX(A), Permute<3, 2, 1, 0>(B) * S1, R);              // ax * (bw,-bz,by,-bx)
        R = MulAdd(SplatY(A), Permute<2, 3, 0, 1>(B) * S2, R);              // ay * (bz,bw,-bx,-by)
        R = MulAdd(SplatZ(A), Permute<1, 0, 3, 2>(B) * S3, R);              // az * (-by,bx,bw,-bz)
        return R;
    }

    [[nodiscard]] FORCEINLINE VFloat4 QuatConjugate(VFloat4 Q)
    {
        // Negate x,y,z; keep w.
        return Q * VFloat4(-1.0f, -1.0f, -1.0f, 1.0f);
    }

    // Rotate vector V (xyz; w ignored) by unit quaternion Q: V + 2w(QxV) + 2 Qx(QxV).
    [[nodiscard]] FORCEINLINE VFloat4 QuatRotate(VFloat4 Q, VFloat4 V)
    {
        const VFloat4 T = VFloat4(2.0f) * Cross(Q, V);
        return V + SplatW(Q) * T + Cross(Q, T);
    }

    [[nodiscard]] FORCEINLINE VFloat4 QuatNormalize(VFloat4 Q)
    {
        const float LenSq = Dot(Q, Q);
        if (LenSq <= 1e-12f)
        {
            return VFloat4(0.0f, 0.0f, 0.0f, 1.0f);
        }
        return Q * VFloat4::Broadcast(1.0f / std::sqrt(LenSq));
    }

    // AoS FQuat (x,y,z,w) <-> VFloat4 lane mapping is identity (same order), so a plain load/store.
    [[nodiscard]] FORCEINLINE VFloat4 LoadQuat(const FQuat& Q)  { return VFloat4::Load(&Q.x); }
    FORCEINLINE void                  StoreQuat(FQuat& Q, VFloat4 V) { V.Store(&Q.x); }

    // Quaternion from a unit axis (xyz lanes, w ignored) and an angle in radians, built in-register.
    // Avoids the scalar FQuat round-trip (and its store-to-load-forwarding stall) on the rotate-compose path.
    [[nodiscard]] FORCEINLINE VFloat4 QuatAngleAxis(VFloat4 Axis, float Radians)
    {
        const float Half = Radians * 0.5f;
        const VFloat4 XYZ   = Axis * VFloat4::Broadcast(std::sin(Half));   // (sin*ax, sin*ay, sin*az, -)
        const VFloat4 LaneW = _mm_castsi128_ps(_mm_setr_epi32(0, 0, 0, -1));
        return Select(LaneW, VFloat4::Broadcast(std::cos(Half)), XYZ);     // w = cos(Half)
    }

    // Unit quaternion -> the 3 columns of its rotation matrix (column-major, each xyz + 0 in w), matching
    // Math::ToMatrix3 lane-for-lane. Stays entirely in registers (no FQuat/FMatrix round-trip) so it's the
    // SIMD core of VTransform::GetMatrix. Validated against scalar in the SIMDQuat test.
    FORCEINLINE void QuatToColumns(VFloat4 Q, VFloat4& Col0, VFloat4& Col1, VFloat4& Col2)
    {
        const VFloat4 Two = VFloat4::Broadcast(2.0f);

        // Diagonal: (1-2(yy+zz), 1-2(xx+zz), 1-2(xx+yy), -).
        const VFloat4 Qsq  = Q * Q;                                  // (xx, yy, zz, ww)
        const VFloat4 D1   = Permute<1, 0, 0, 3>(Qsq);              // (yy, xx, xx, ww)
        const VFloat4 D2   = Permute<2, 2, 1, 3>(Qsq);              // (zz, zz, yy, ww)
        const VFloat4 Diag = MulAdd(-Two, D1 + D2, VFloat4(1.0f, 1.0f, 1.0f, 0.0f));

        // Symmetric products *2: (2xy, 2xz, 2yz, -).
        const VFloat4 Sym = (Permute<0, 0, 1, 3>(Q) * Permute<1, 2, 2, 3>(Q)) * Two;   // (x,x,y)*(y,z,z)
        // w-cross products *2: (2wz, 2wy, 2wx, -).
        const VFloat4 WC  = (SplatW(Q) * Permute<2, 1, 0, 3>(Q)) * Two;                // w*(z,y,x)

        const VFloat4 P = Sym + WC;   // (2xy+2wz, 2xz+2wy, 2yz+2wx, -)
        const VFloat4 M = Sym - WC;   // (2xy-2wz, 2xz-2wy, 2yz-2wx, -)

        // Pick lanes 1 and 3 from the second arg (Select: high-bit lane -> first arg). Builds each column
        // as (diag, plus/minus, minus/plus, 0) per Math::ToMatrix3's R[col][row] layout.
        const VFloat4 BlendYW = _mm_castsi128_ps(_mm_setr_epi32(0, -1, 0, -1));
        const VFloat4 Zero    = VFloat4::Zero();

        Col0 = Select(BlendYW, Shuffle<0, 0, 0, 0>(P, Zero),    Shuffle<0, 0, 1, 1>(Diag, M));   // (d0, p0, m1, 0)
        Col1 = Select(BlendYW, Shuffle<1, 1, 0, 0>(Diag, Zero), Shuffle<0, 0, 2, 2>(M, P));      // (m0, d1, p2, 0)
        Col2 = Select(BlendYW, Shuffle<2, 2, 0, 0>(M, Zero),    Shuffle<1, 1, 2, 2>(P, Diag));   // (p1, m2, d2, 0)
    }
}
