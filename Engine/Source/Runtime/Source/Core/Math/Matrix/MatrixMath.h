#pragma once

#include "Matrix.h"
#include "Core/Math/Quat/Quat.h"
#include <cmath>

// Matrix free fns + quat<->matrix conversions in Lumina::Math.
// Conventions: COLUMN-MAJOR, LEFT-HANDED, ZERO-TO-ONE clip depth. Do not "fix" these.

namespace Lumina::Math
{
    template<typename T, int C, int R>
    [[nodiscard]] constexpr TMat<T, R, C> Transpose(const TMat<T, C, R>& M)
    {
        TMat<T, R, C> Result(T(0));
        for (int c = 0; c < C; ++c)
        {
            for (int r = 0; r < R; ++r)
            {
                Result.Cols[r][c] = M.Cols[c][r];
            }
        }
        return Result;
    }

    // SIMD fast path: 4x4 float transpose via a single shuffle network.
    [[nodiscard]] inline TMat<float, 4, 4> Transpose(const TMat<float, 4, 4>& M)
    {
        using namespace SIMD;
        __m128 R0 = VFloat4::Load(&M.Cols[0][0]);
        __m128 R1 = VFloat4::Load(&M.Cols[1][0]);
        __m128 R2 = VFloat4::Load(&M.Cols[2][0]);
        __m128 R3 = VFloat4::Load(&M.Cols[3][0]);
        _MM_TRANSPOSE4_PS(R0, R1, R2, R3);

        TMat<float, 4, 4> Out;
        VFloat4(R0).Store(&Out.Cols[0][0]);
        VFloat4(R1).Store(&Out.Cols[1][0]);
        VFloat4(R2).Store(&Out.Cols[2][0]);
        VFloat4(R3).Store(&Out.Cols[3][0]);
        return Out;
    }

    template<typename T>
    [[nodiscard]] constexpr T Determinant(const TMat<T, 3, 3>& M)
    {
        return + M[0][0] * (M[1][1] * M[2][2] - M[2][1] * M[1][2])
               - M[1][0] * (M[0][1] * M[2][2] - M[2][1] * M[0][2])
               + M[2][0] * (M[0][1] * M[1][2] - M[1][1] * M[0][2]);
    }

    template<typename T>
    [[nodiscard]] constexpr T Determinant(const TMat<T, 4, 4>& M)
    {
        const T S0 = M[2][2] * M[3][3] - M[3][2] * M[2][3];
        const T S1 = M[2][1] * M[3][3] - M[3][1] * M[2][3];
        const T S2 = M[2][1] * M[3][2] - M[3][1] * M[2][2];
        const T S3 = M[2][0] * M[3][3] - M[3][0] * M[2][3];
        const T S4 = M[2][0] * M[3][2] - M[3][0] * M[2][2];
        const T S5 = M[2][0] * M[3][1] - M[3][0] * M[2][1];

        const T C0 = M[1][1] * S0 - M[1][2] * S1 + M[1][3] * S2;
        const T C1 = M[1][0] * S0 - M[1][2] * S3 + M[1][3] * S4;
        const T C2 = M[1][0] * S1 - M[1][1] * S3 + M[1][3] * S5;
        const T C3 = M[1][0] * S2 - M[1][1] * S4 + M[1][2] * S5;

        return M[0][0] * C0 - M[0][1] * C1 + M[0][2] * C2 - M[0][3] * C3;
    }

    template<typename T>
    [[nodiscard]] constexpr TMat<T, 3, 3> Inverse(const TMat<T, 3, 3>& M)
    {
        const T Det = Determinant(M);
        const T Inv = T(1) / Det;

        TMat<T, 3, 3> R(T(0));
        R[0][0] = +(M[1][1] * M[2][2] - M[2][1] * M[1][2]) * Inv;
        R[0][1] = -(M[0][1] * M[2][2] - M[2][1] * M[0][2]) * Inv;
        R[0][2] = +(M[0][1] * M[1][2] - M[1][1] * M[0][2]) * Inv;
        R[1][0] = -(M[1][0] * M[2][2] - M[2][0] * M[1][2]) * Inv;
        R[1][1] = +(M[0][0] * M[2][2] - M[2][0] * M[0][2]) * Inv;
        R[1][2] = -(M[0][0] * M[1][2] - M[1][0] * M[0][2]) * Inv;
        R[2][0] = +(M[1][0] * M[2][1] - M[2][0] * M[1][1]) * Inv;
        R[2][1] = -(M[0][0] * M[2][1] - M[2][0] * M[0][1]) * Inv;
        R[2][2] = +(M[0][0] * M[1][1] - M[1][0] * M[0][1]) * Inv;
        return R;
    }

    // Cofactor (coefficient) method; full general 4x4 inverse.
    template<typename T>
    [[nodiscard]] constexpr TMat<T, 4, 4> Inverse(const TMat<T, 4, 4>& M)
    {
        const T C00 = M[2][2] * M[3][3] - M[3][2] * M[2][3];
        const T C02 = M[1][2] * M[3][3] - M[3][2] * M[1][3];
        const T C03 = M[1][2] * M[2][3] - M[2][2] * M[1][3];

        const T C04 = M[2][1] * M[3][3] - M[3][1] * M[2][3];
        const T C06 = M[1][1] * M[3][3] - M[3][1] * M[1][3];
        const T C07 = M[1][1] * M[2][3] - M[2][1] * M[1][3];

        const T C08 = M[2][1] * M[3][2] - M[3][1] * M[2][2];
        const T C10 = M[1][1] * M[3][2] - M[3][1] * M[1][2];
        const T C11 = M[1][1] * M[2][2] - M[2][1] * M[1][2];

        const T C12 = M[2][0] * M[3][3] - M[3][0] * M[2][3];
        const T C14 = M[1][0] * M[3][3] - M[3][0] * M[1][3];
        const T C15 = M[1][0] * M[2][3] - M[2][0] * M[1][3];

        const T C16 = M[2][0] * M[3][2] - M[3][0] * M[2][2];
        const T C18 = M[1][0] * M[3][2] - M[3][0] * M[1][2];
        const T C19 = M[1][0] * M[2][2] - M[2][0] * M[1][2];

        const T C20 = M[2][0] * M[3][1] - M[3][0] * M[2][1];
        const T C22 = M[1][0] * M[3][1] - M[3][0] * M[1][1];
        const T C23 = M[1][0] * M[2][1] - M[2][0] * M[1][1];

        const TVec<T, 4> F0(C00, C00, C02, C03);
        const TVec<T, 4> F1(C04, C04, C06, C07);
        const TVec<T, 4> F2(C08, C08, C10, C11);
        const TVec<T, 4> F3(C12, C12, C14, C15);
        const TVec<T, 4> F4(C16, C16, C18, C19);
        const TVec<T, 4> F5(C20, C20, C22, C23);

        const TVec<T, 4> V0(M[1][0], M[0][0], M[0][0], M[0][0]);
        const TVec<T, 4> V1(M[1][1], M[0][1], M[0][1], M[0][1]);
        const TVec<T, 4> V2(M[1][2], M[0][2], M[0][2], M[0][2]);
        const TVec<T, 4> V3(M[1][3], M[0][3], M[0][3], M[0][3]);

        const TVec<T, 4> I0 = V1 * F0 - V2 * F1 + V3 * F2;
        const TVec<T, 4> I1 = V0 * F0 - V2 * F3 + V3 * F4;
        const TVec<T, 4> I2 = V0 * F1 - V1 * F3 + V3 * F5;
        const TVec<T, 4> I3 = V0 * F2 - V1 * F4 + V2 * F5;

        const TVec<T, 4> SignA(+T(1), -T(1), +T(1), -T(1));
        const TVec<T, 4> SignB(-T(1), +T(1), -T(1), +T(1));

        TMat<T, 4, 4> Inv(T(0));
        Inv[0] = I0 * SignA;
        Inv[1] = I1 * SignB;
        Inv[2] = I2 * SignA;
        Inv[3] = I3 * SignB;

        const TVec<T, 4> Row0(Inv[0][0], Inv[1][0], Inv[2][0], Inv[3][0]);
        const TVec<T, 4> Dot0 = M[0] * Row0;
        const T Dot1 = (Dot0.x + Dot0.y) + (Dot0.z + Dot0.w);
        const T OneOverDet = T(1) / Dot1;

        return Inv * OneOverDet;
    }

    // SIMD fast path for 4x4 float inverse (SSE cofactor/adjugate); same math as the
    // template above, exact-type overload replacing the scalar path for FMatrix4.
    [[nodiscard]] inline TMat<float, 4, 4> Inverse(const TMat<float, 4, 4>& M)
    {
        using namespace SIMD;

        const VFloat4 In0 = VFloat4::Load(&M.Cols[0][0]);
        const VFloat4 In1 = VFloat4::Load(&M.Cols[1][0]);
        const VFloat4 In2 = VFloat4::Load(&M.Cols[2][0]);
        const VFloat4 In3 = VFloat4::Load(&M.Cols[3][0]);

        // Each cofactor vector is C*A' - B'*D, where A',B' broadcast lane 2 into 0..2.
        auto Fac = [](VFloat4 A, VFloat4 B, VFloat4 C, VFloat4 D)
        {
            return C * Permute<0, 0, 0, 2>(A) - Permute<0, 0, 0, 2>(B) * D;
        };
        const VFloat4 Fac0 = Fac(Shuffle<3,3,3,3>(In3,In2), Shuffle<2,2,2,2>(In3,In2), Shuffle<2,2,2,2>(In2,In1), Shuffle<3,3,3,3>(In2,In1));
        const VFloat4 Fac1 = Fac(Shuffle<3,3,3,3>(In3,In2), Shuffle<1,1,1,1>(In3,In2), Shuffle<1,1,1,1>(In2,In1), Shuffle<3,3,3,3>(In2,In1));
        const VFloat4 Fac2 = Fac(Shuffle<2,2,2,2>(In3,In2), Shuffle<1,1,1,1>(In3,In2), Shuffle<1,1,1,1>(In2,In1), Shuffle<2,2,2,2>(In2,In1));
        const VFloat4 Fac3 = Fac(Shuffle<3,3,3,3>(In3,In2), Shuffle<0,0,0,0>(In3,In2), Shuffle<0,0,0,0>(In2,In1), Shuffle<3,3,3,3>(In2,In1));
        const VFloat4 Fac4 = Fac(Shuffle<2,2,2,2>(In3,In2), Shuffle<0,0,0,0>(In3,In2), Shuffle<0,0,0,0>(In2,In1), Shuffle<2,2,2,2>(In2,In1));
        const VFloat4 Fac5 = Fac(Shuffle<1,1,1,1>(In3,In2), Shuffle<0,0,0,0>(In3,In2), Shuffle<0,0,0,0>(In2,In1), Shuffle<1,1,1,1>(In2,In1));

        // Columns of M reorganized for the adjugate combine.
        const VFloat4 V0 = Permute<0,2,2,2>(Shuffle<0,0,0,0>(In1, In0));
        const VFloat4 V1 = Permute<0,2,2,2>(Shuffle<1,1,1,1>(In1, In0));
        const VFloat4 V2 = Permute<0,2,2,2>(Shuffle<2,2,2,2>(In1, In0));
        const VFloat4 V3 = Permute<0,2,2,2>(Shuffle<3,3,3,3>(In1, In0));

        // Signs match the scalar path: Inv columns 0/2 use (+,-,+,-), 1/3 use (-,+,-,+).
        const VFloat4 SignA( 1.0f, -1.0f,  1.0f, -1.0f);
        const VFloat4 SignB(-1.0f,  1.0f, -1.0f,  1.0f);

        const VFloat4 Inv0 = SignA * (V1 * Fac0 - V2 * Fac1 + V3 * Fac2);
        const VFloat4 Inv1 = SignB * (V0 * Fac0 - V2 * Fac3 + V3 * Fac4);
        const VFloat4 Inv2 = SignA * (V0 * Fac1 - V1 * Fac3 + V3 * Fac5);
        const VFloat4 Inv3 = SignB * (V0 * Fac2 - V1 * Fac4 + V2 * Fac5);

        // Determinant = dot(column 0, (Inv0[0], Inv1[0], Inv2[0], Inv3[0])).
        const VFloat4 Row = Shuffle<0,2,0,2>(Shuffle<0,0,0,0>(Inv0, Inv1), Shuffle<0,0,0,0>(Inv2, Inv3));
        const VFloat4 Rcp = VFloat4::Broadcast(1.0f / Dot(In0, Row));

        TMat<float, 4, 4> Out;
        (Inv0 * Rcp).Store(&Out.Cols[0][0]);
        (Inv1 * Rcp).Store(&Out.Cols[1][0]);
        (Inv2 * Rcp).Store(&Out.Cols[2][0]);
        (Inv3 * Rcp).Store(&Out.Cols[3][0]);
        return Out;
    }

    template<typename T>
    [[nodiscard]] constexpr TMat<T, 4, 4> Translate(const TMat<T, 4, 4>& M, const TVec<T, 3>& V)
    {
        TMat<T, 4, 4> Result = M;
        Result[3] = M[0] * V.x + M[1] * V.y + M[2] * V.z + M[3];
        return Result;
    }

    template<typename T>
    [[nodiscard]] constexpr TMat<T, 4, 4> Scale(const TMat<T, 4, 4>& M, const TVec<T, 3>& V)
    {
        TMat<T, 4, 4> Result(T(0));
        Result[0] = M[0] * V.x;
        Result[1] = M[1] * V.y;
        Result[2] = M[2] * V.z;
        Result[3] = M[3];
        return Result;
    }

    // SIMD fast paths for FMatrix4 (exact-type overloads win over the templates above and
    // match their scalar result); the hot per-entity TRS path lives in VTransform/VQuat1.
    [[nodiscard]] inline TMat<float, 4, 4> Translate(const TMat<float, 4, 4>& M, const TVec<float, 3>& V)
    {
        using namespace SIMD;
        const VFloat4 C0 = VFloat4::Load(&M.Cols[0][0]);
        const VFloat4 C1 = VFloat4::Load(&M.Cols[1][0]);
        const VFloat4 C2 = VFloat4::Load(&M.Cols[2][0]);
        const VFloat4 C3 = VFloat4::Load(&M.Cols[3][0]);

        TMat<float, 4, 4> Out;
        C0.Store(&Out.Cols[0][0]);
        C1.Store(&Out.Cols[1][0]);
        C2.Store(&Out.Cols[2][0]);
        // Column 3 += M[0]*x + M[1]*y + M[2]*z; columns 0..2 pass through.
        MulAdd(C2, VFloat4::Broadcast(V.z),
        MulAdd(C1, VFloat4::Broadcast(V.y),
        MulAdd(C0, VFloat4::Broadcast(V.x), C3))).Store(&Out.Cols[3][0]);
        return Out;
    }

    [[nodiscard]] inline TMat<float, 4, 4> Scale(const TMat<float, 4, 4>& M, const TVec<float, 3>& V)
    {
        using namespace SIMD;
        TMat<float, 4, 4> Out;
        (VFloat4::Load(&M.Cols[0][0]) * VFloat4::Broadcast(V.x)).Store(&Out.Cols[0][0]);
        (VFloat4::Load(&M.Cols[1][0]) * VFloat4::Broadcast(V.y)).Store(&Out.Cols[1][0]);
        (VFloat4::Load(&M.Cols[2][0]) * VFloat4::Broadcast(V.z)).Store(&Out.Cols[2][0]);
        VFloat4::Load(&M.Cols[3][0]).Store(&Out.Cols[3][0]);
        return Out;
    }

    template<typename T>
    [[nodiscard]] TMat<T, 4, 4> Rotate(const TMat<T, 4, 4>& M, T AngleRadians, const TVec<T, 3>& Axis)
    {
        const T Cos = static_cast<T>(std::cos(AngleRadians));
        const T Sin = static_cast<T>(std::sin(AngleRadians));
        const TVec<T, 3> A = Normalize(Axis);
        const TVec<T, 3> Temp = A * (T(1) - Cos);

        TMat<T, 3, 3> Rot(T(0));
        Rot[0][0] = Cos + Temp.x * A.x;
        Rot[0][1] = Temp.x * A.y + Sin * A.z;
        Rot[0][2] = Temp.x * A.z - Sin * A.y;
        Rot[1][0] = Temp.y * A.x - Sin * A.z;
        Rot[1][1] = Cos + Temp.y * A.y;
        Rot[1][2] = Temp.y * A.z + Sin * A.x;
        Rot[2][0] = Temp.z * A.x + Sin * A.y;
        Rot[2][1] = Temp.z * A.y - Sin * A.x;
        Rot[2][2] = Cos + Temp.z * A.z;

        TMat<T, 4, 4> Result(T(0));
        Result[0] = M[0] * Rot[0][0] + M[1] * Rot[0][1] + M[2] * Rot[0][2];
        Result[1] = M[0] * Rot[1][0] + M[1] * Rot[1][1] + M[2] * Rot[1][2];
        Result[2] = M[0] * Rot[2][0] + M[1] * Rot[2][1] + M[2] * Rot[2][2];
        Result[3] = M[3];
        return Result;
    }

    template<typename T>
    [[nodiscard]] TMat<T, 4, 4> Perspective(T FovYRadians, T Aspect, T Near, T Far)
    {
        const T TanHalf = static_cast<T>(std::tan(FovYRadians / T(2)));
        TMat<T, 4, 4> Result(T(0));
        Result[0][0] = T(1) / (Aspect * TanHalf);
        Result[1][1] = T(1) / TanHalf;
        Result[2][2] = Far / (Far - Near);
        Result[2][3] = T(1);
        Result[3][2] = -(Far * Near) / (Far - Near);
        return Result;
    }

    template<typename T>
    [[nodiscard]] TMat<T, 4, 4> PerspectiveFov(T FovRadians, T Width, T Height, T Near, T Far)
    {
        const T Rad = FovRadians;
        const T H = static_cast<T>(std::cos(T(0.5) * Rad)) / static_cast<T>(std::sin(T(0.5) * Rad));
        const T W = H * Height / Width;
        TMat<T, 4, 4> Result(T(0));
        Result[0][0] = W;
        Result[1][1] = H;
        Result[2][2] = Far / (Far - Near);
        Result[2][3] = T(1);
        Result[3][2] = -(Far * Near) / (Far - Near);
        return Result;
    }

    template<typename T>
    [[nodiscard]] constexpr TMat<T, 4, 4> Ortho(T Left, T Right, T Bottom, T Top, T Near, T Far)
    {
        TMat<T, 4, 4> Result(T(1));
        Result[0][0] = T(2) / (Right - Left);
        Result[1][1] = T(2) / (Top - Bottom);
        Result[2][2] = T(1) / (Far - Near);
        Result[3][0] = -(Right + Left) / (Right - Left);
        Result[3][1] = -(Top + Bottom) / (Top - Bottom);
        Result[3][2] = -Near / (Far - Near);
        return Result;
    }

    template<typename T>
    [[nodiscard]] constexpr TMat<T, 4, 4> Ortho(T Left, T Right, T Bottom, T Top)
    {
        TMat<T, 4, 4> Result(T(1));
        Result[0][0] = T(2) / (Right - Left);
        Result[1][1] = T(2) / (Top - Bottom);
        Result[2][2] = -T(1);
        Result[3][0] = -(Right + Left) / (Right - Left);
        Result[3][1] = -(Top + Bottom) / (Top - Bottom);
        return Result;
    }

    template<typename T>
    [[nodiscard]] TMat<T, 4, 4> LookAt(const TVec<T, 3>& Eye, const TVec<T, 3>& Center, const TVec<T, 3>& Up)
    {
        const TVec<T, 3> F = Normalize(Center - Eye);
        const TVec<T, 3> S = Normalize(Cross(Up, F));
        const TVec<T, 3> U = Cross(F, S);

        TMat<T, 4, 4> Result(T(1));
        Result[0][0] = S.x;  Result[1][0] = S.y;  Result[2][0] = S.z;
        Result[0][1] = U.x;  Result[1][1] = U.y;  Result[2][1] = U.z;
        Result[0][2] = F.x;  Result[1][2] = F.y;  Result[2][2] = F.z;
        Result[3][0] = -Dot(S, Eye);
        Result[3][1] = -Dot(U, Eye);
        Result[3][2] = -Dot(F, Eye);
        return Result;
    }

    // Quaternion -> rotation matrix.
    template<typename T>
    [[nodiscard]] constexpr TMat<T, 3, 3> ToMatrix3(const TQuat<T>& Q)
    {
        const T XX = Q.x * Q.x; const T YY = Q.y * Q.y; const T ZZ = Q.z * Q.z;
        const T XZ = Q.x * Q.z; const T XY = Q.x * Q.y; const T YZ = Q.y * Q.z;
        const T WX = Q.w * Q.x; const T WY = Q.w * Q.y; const T WZ = Q.w * Q.z;

        TMat<T, 3, 3> R(T(0));
        R[0][0] = T(1) - T(2) * (YY + ZZ);
        R[0][1] = T(2) * (XY + WZ);
        R[0][2] = T(2) * (XZ - WY);
        R[1][0] = T(2) * (XY - WZ);
        R[1][1] = T(1) - T(2) * (XX + ZZ);
        R[1][2] = T(2) * (YZ + WX);
        R[2][0] = T(2) * (XZ + WY);
        R[2][1] = T(2) * (YZ - WX);
        R[2][2] = T(1) - T(2) * (XX + YY);
        return R;
    }

    template<typename T>
    [[nodiscard]] constexpr TMat<T, 4, 4> ToMatrix4(const TQuat<T>& Q)
    {
        const TMat<T, 3, 3> R = ToMatrix3(Q);
        TMat<T, 4, 4> Result(T(1));
        Result[0] = TVec<T, 4>(R[0], T(0));
        Result[1] = TVec<T, 4>(R[1], T(0));
        Result[2] = TVec<T, 4>(R[2], T(0));
        return Result;
    }

    // Rotation matrix -> quaternion, branch-by-largest-component.
    template<typename T>
    [[nodiscard]] TQuat<T> ToQuat(const TMat<T, 3, 3>& M)
    {
        const T FourXSquaredMinus1 = M[0][0] - M[1][1] - M[2][2];
        const T FourYSquaredMinus1 = M[1][1] - M[0][0] - M[2][2];
        const T FourZSquaredMinus1 = M[2][2] - M[0][0] - M[1][1];
        const T FourWSquaredMinus1 = M[0][0] + M[1][1] + M[2][2];

        int BiggestIndex = 0;
        T FourBiggestSquaredMinus1 = FourWSquaredMinus1;
        if (FourXSquaredMinus1 > FourBiggestSquaredMinus1) { FourBiggestSquaredMinus1 = FourXSquaredMinus1; BiggestIndex = 1; }
        if (FourYSquaredMinus1 > FourBiggestSquaredMinus1) { FourBiggestSquaredMinus1 = FourYSquaredMinus1; BiggestIndex = 2; }
        if (FourZSquaredMinus1 > FourBiggestSquaredMinus1) { FourBiggestSquaredMinus1 = FourZSquaredMinus1; BiggestIndex = 3; }

        const T BiggestVal = static_cast<T>(std::sqrt(FourBiggestSquaredMinus1 + T(1))) * T(0.5);
        const T Mult = T(0.25) / BiggestVal;

        switch (BiggestIndex)
        {
        case 0: return TQuat<T>(BiggestVal, (M[1][2] - M[2][1]) * Mult, (M[2][0] - M[0][2]) * Mult, (M[0][1] - M[1][0]) * Mult);
        case 1: return TQuat<T>((M[1][2] - M[2][1]) * Mult, BiggestVal, (M[0][1] + M[1][0]) * Mult, (M[2][0] + M[0][2]) * Mult);
        case 2: return TQuat<T>((M[2][0] - M[0][2]) * Mult, (M[0][1] + M[1][0]) * Mult, BiggestVal, (M[1][2] + M[2][1]) * Mult);
        default: return TQuat<T>((M[0][1] - M[1][0]) * Mult, (M[2][0] + M[0][2]) * Mult, (M[1][2] + M[2][1]) * Mult, BiggestVal);
        }
    }

    template<typename T>
    [[nodiscard]] TQuat<T> ToQuat(const TMat<T, 4, 4>& M)
    {
        return ToQuat(TMat<T, 3, 3>(
            TVec<T, 3>(M[0].x, M[0].y, M[0].z),
            TVec<T, 3>(M[1].x, M[1].y, M[1].z),
            TVec<T, 3>(M[2].x, M[2].y, M[2].z)));
    }

    // Quaternion from an axis and angle (radians).
    template<typename T>
    [[nodiscard]] TQuat<T> AngleAxis(T AngleRadians, const TVec<T, 3>& Axis)
    {
        const T Half = AngleRadians * T(0.5);
        const T S = static_cast<T>(std::sin(Half));
        return TQuat<T>(static_cast<T>(std::cos(Half)), Axis.x * S, Axis.y * S, Axis.z * S);
    }

    // Look-rotation quaternion from a forward direction and up vector.
    template<typename T>
    [[nodiscard]] TQuat<T> QuatLookAt(const TVec<T, 3>& Direction, const TVec<T, 3>& Up)
    {
        TMat<T, 3, 3> Result(T(1));
        Result[2] = Direction;                          // left-handed forward
        const TVec<T, 3> Right = Cross(Up, Result[2]);
        Result[0] = Right * (T(1) / static_cast<T>(std::sqrt(Max(T(0.00001), Dot(Right, Right)))));
        Result[1] = Cross(Result[2], Result[0]);
        return ToQuat(Result);
    }

    // Affine TRS decompose (for non-skewed, non-projective
    // transforms, which is all the engine builds). Returns false on singular.
    template<typename T>
    bool Decompose(const TMat<T, 4, 4>& M, TVec<T, 3>& OutScale, TQuat<T>& OutRotation, TVec<T, 3>& OutTranslation)
    {
        TMat<T, 4, 4> Local = M;
        if (std::abs(Local[3][3]) < T(LE_SMALL_NUMBER))
        {
            return false;
        }

        OutTranslation = TVec<T, 3>(Local[3].x, Local[3].y, Local[3].z);

        TVec<T, 3> Rows[3];
        for (int i = 0; i < 3; ++i)
        {
            Rows[i] = TVec<T, 3>(Local[i].x, Local[i].y, Local[i].z);
        }

        OutScale.x = Length(Rows[0]);
        OutScale.y = Length(Rows[1]);
        OutScale.z = Length(Rows[2]);

        if (OutScale.x > T(0)) { Rows[0] = Rows[0] * (T(1) / OutScale.x); }
        if (OutScale.y > T(0)) { Rows[1] = Rows[1] * (T(1) / OutScale.y); }
        if (OutScale.z > T(0)) { Rows[2] = Rows[2] * (T(1) / OutScale.z); }

        // Negative determinant -> one axis is mirrored.
        if (Dot(Cross(Rows[1], Rows[2]), Rows[0]) < T(0))
        {
            OutScale.x = -OutScale.x;
            Rows[0] = -Rows[0];
        }

        OutRotation = ToQuat(TMat<T, 3, 3>(Rows[0], Rows[1], Rows[2]));
        return true;
    }

    // Full decompose signature (skew/perspective are filled for affine transforms).
    template<typename T>
    bool Decompose(const TMat<T, 4, 4>& M, TVec<T, 3>& OutScale, TQuat<T>& OutRotation,
                   TVec<T, 3>& OutTranslation, TVec<T, 3>& OutSkew, TVec<T, 4>& OutPerspective)
    {
        OutSkew = TVec<T, 3>(T(0));
        OutPerspective = TVec<T, 4>(T(0), T(0), T(0), T(1));
        return Decompose(M, OutScale, OutRotation, OutTranslation);
    }

    // Shortest-arc quaternion rotating From onto To
    // (both expected normalized).
    template<typename T>
    [[nodiscard]] TQuat<T> RotationBetween(const TVec<T, 3>& From, const TVec<T, 3>& To)
    {
        const T CosTheta = Dot(From, To);

        if (CosTheta >= T(1) - Epsilon<T>())
        {
            return TQuat<T>::Identity();
        }
        if (CosTheta < T(-1) + Epsilon<T>())
        {
            TVec<T, 3> Axis = Cross(TVec<T, 3>(0, 0, 1), From);
            if (LengthSquared(Axis) < Epsilon<T>())
            {
                Axis = Cross(TVec<T, 3>(1, 0, 0), From);
            }
            return AngleAxis(Pi<T>(), Normalize(Axis));
        }

        const TVec<T, 3> Axis = Cross(From, To);
        const T S = static_cast<T>(std::sqrt((T(1) + CosTheta) * T(2)));
        const T InvS = T(1) / S;
        return TQuat<T>(S * T(0.5), Axis.x * InvS, Axis.y * InvS, Axis.z * InvS);
    }

    // Build a 4x4 matrix from 16 column-major scalars.
    template<typename T>
    [[nodiscard]] TMat<T, 4, 4> MakeMat4(const T* Ptr)
    {
        TMat<T, 4, 4> Result(T(0));
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                Result[c][r] = Ptr[c * 4 + r];
            }
        }
        return Result;
    }

    // Pointer to contiguous column-major scalars (GPU upload / interop).
    template<typename T, int C, int R> [[nodiscard]] const T* ValuePtr(const TMat<T, C, R>& M) { return M.GetData(); }
    template<typename T, int C, int R> [[nodiscard]] T*       ValuePtr(TMat<T, C, R>& M)       { return M.GetData(); }
}
