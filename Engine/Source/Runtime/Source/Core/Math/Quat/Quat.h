#pragma once

#include "Core/Math/Vector/Vector.h"
#include "Core/LuminaMacros.h"
#include <cmath>

// Lumina quaternion. Layout x,y,z,w but the 4-scalar ctor takes real FIRST: TQuat(w,x,y,z).
// Left-handed; plain members (no union) so offsetof on x/y/z/w is well-defined for reflection.

#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable: 4201)
#endif

namespace Lumina
{
    template<typename T>
    struct TQuat
    {
        using ScalarType = T;

        T x, y, z, w;

        // Trivial default ctor: uninitialized; keeps the type trivial
        // for unions / bulk serialization). Use TQuat::Identity() for identity.
        TQuat() = default;

        // Real part first: TQuat(w, x, y, z).
        constexpr TQuat(T InW, T InX, T InY, T InZ) : x(InX), y(InY), z(InZ), w(InW) {}

        // Scalar (real) + imaginary vector.
        constexpr TQuat(T InW, const TVec<T, 3>& V) : x(V.x), y(V.y), z(V.z), w(InW) {}

        // From euler angles in radians.
        explicit TQuat(const TVec<T, 3>& EulerRadians)
        {
            const TVec<T, 3> C(std::cos(EulerRadians.x * T(0.5)), std::cos(EulerRadians.y * T(0.5)), std::cos(EulerRadians.z * T(0.5)));
            const TVec<T, 3> S(std::sin(EulerRadians.x * T(0.5)), std::sin(EulerRadians.y * T(0.5)), std::sin(EulerRadians.z * T(0.5)));

            w = C.x * C.y * C.z + S.x * S.y * S.z;
            x = S.x * C.y * C.z - C.x * S.y * S.z;
            y = C.x * S.y * C.z + S.x * C.y * S.z;
            z = C.x * C.y * S.z - S.x * S.y * C.z;
        }

        template<typename U>
        explicit constexpr TQuat(const TQuat<U>& Q) : x(T(Q.x)), y(T(Q.y)), z(T(Q.z)), w(T(Q.w)) {}

        static constexpr TQuat Identity() { return TQuat(T(1), T(0), T(0), T(0)); }

        // Imaginary part.
        constexpr TVec<T, 3> Vector() const { return TVec<T, 3>(x, y, z); }
    };

    // Hamilton product (left-to-right composition: applies B then A).
    template<typename T>
    constexpr TQuat<T> operator*(const TQuat<T>& A, const TQuat<T>& B)
    {
        return TQuat<T>(
            A.w * B.w - A.x * B.x - A.y * B.y - A.z * B.z,
            A.w * B.x + A.x * B.w + A.y * B.z - A.z * B.y,
            A.w * B.y - A.x * B.z + A.y * B.w + A.z * B.x,
            A.w * B.z + A.x * B.y - A.y * B.x + A.z * B.w);
    }

    // Rotate a vector by a (unit) quaternion: v + 2w(q x v) + 2 q x (q x v).
    template<typename T>
    constexpr TVec<T, 3> operator*(const TQuat<T>& Q, const TVec<T, 3>& V)
    {
        const TVec<T, 3> U(Q.x, Q.y, Q.z);
        const TVec<T, 3> T1 = Math::Cross(U, V) * T(2);
        return V + T1 * Q.w + Math::Cross(U, T1);
    }

    template<typename T>
    constexpr TQuat<T> operator*(const TQuat<T>& Q, T S)
    {
        return TQuat<T>(Q.w * S, Q.x * S, Q.y * S, Q.z * S);
    }

    template<typename T>
    constexpr TQuat<T> operator+(const TQuat<T>& A, const TQuat<T>& B)
    {
        return TQuat<T>(A.w + B.w, A.x + B.x, A.y + B.y, A.z + B.z);
    }

    template<typename T>
    constexpr TQuat<T> operator-(const TQuat<T>& A, const TQuat<T>& B)
    {
        return TQuat<T>(A.w - B.w, A.x - B.x, A.y - B.y, A.z - B.z);
    }

    template<typename T>
    constexpr TQuat<T> operator-(const TQuat<T>& Q)
    {
        return TQuat<T>(-Q.w, -Q.x, -Q.y, -Q.z);
    }

    template<typename T>
    constexpr bool operator==(const TQuat<T>& A, const TQuat<T>& B)
    {
        return A.x == B.x && A.y == B.y && A.z == B.z && A.w == B.w;
    }

    template<typename T>
    constexpr bool operator!=(const TQuat<T>& A, const TQuat<T>& B)
    {
        return !(A == B);
    }

#ifndef REFLECTION_PARSER
    // The real FQuat is a template-alias the reflector can't walk; the
    // ManualStub shim below at parser-time gives reflection something to bite.
    using FQuat = TQuat<float>;

    // The shim describes this type field by field and nothing makes that description follow the
    // type, so drift is a compile error rather than a reflection that quietly lies. Note the
    // declaration order: the shim lists x,y,z,w, which is the storage order, NOT the constructor
    // order (TQuat takes w first).
    static_assert(sizeof(FQuat) == 16, "FQuat no longer matches its REFLECT(ManualStub) shim: size changed.");
    static_assert(std::is_same_v<FQuat::ScalarType, float>, "FQuat scalar type drifted from its shim.");
    static_assert(offsetof(FQuat, x) == 0 && offsetof(FQuat, y) == 4
        && offsetof(FQuat, z) == 8 && offsetof(FQuat, w) == 12,
        "FQuat member order drifted from its REFLECT(ManualStub) shim.");
#endif
    // Not reflected, always defined.
    using FDoubleQuat = TQuat<double>;
}

// Reflection-parser-only shim for FQuat; ManualStub tells codegen to skip StaticStruct().
// REFLECT/PROPERTY defined locally (not via ObjectMacros.h) to avoid an include cycle.
#ifdef REFLECTION_PARSER
#ifndef REFLECT
#define REFLECT(...)
#define PROPERTY(...)
#define FUNCTION(...)
#define GENERATED_BODY(...)
#endif
namespace Lumina
{
    REFLECT(ManualStub)
    struct FQuat
    {
        PROPERTY(Editable) float x;
        PROPERTY(Editable) float y;
        PROPERTY(Editable) float z;
        /** Real component. */
        PROPERTY(Editable) float w;
    };
}
#endif

namespace Lumina::Math
{
    template<typename T>
    [[nodiscard]] constexpr T Dot(const TQuat<T>& A, const TQuat<T>& B)
    {
        return A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w;
    }

    template<typename T>
    [[nodiscard]] constexpr T LengthSquared(const TQuat<T>& Q)
    {
        return Dot(Q, Q);
    }

    template<typename T>
    [[nodiscard]] T Length(const TQuat<T>& Q)
    {
        return static_cast<T>(std::sqrt(LengthSquared(Q)));
    }

    template<typename T>
    [[nodiscard]] TQuat<T> Normalize(const TQuat<T>& Q)
    {
        const T Len = Length(Q);
        return Len > T(0) ? Q * (T(1) / Len) : TQuat<T>::Identity();
    }

    template<typename T>
    [[nodiscard]] constexpr TQuat<T> Conjugate(const TQuat<T>& Q)
    {
        return TQuat<T>(Q.w, -Q.x, -Q.y, -Q.z);
    }

    template<typename T>
    [[nodiscard]] constexpr TQuat<T> Inverse(const TQuat<T>& Q)
    {
        const T LenSq = LengthSquared(Q);
        const TQuat<T> Conj = Conjugate(Q);
        return LenSq > T(0) ? Conj * (T(1) / LenSq) : TQuat<T>::Identity();
    }

    template<typename T>
    [[nodiscard]] TQuat<T> FromAxisAngle(const TVec<T, 3>& Axis, T AngleRadians)
    {
        const T Half = AngleRadians * T(0.5);
        const T S = static_cast<T>(std::sin(Half));
        const TVec<T, 3> A = Normalize(Axis) * S;
        return TQuat<T>(static_cast<T>(std::cos(Half)), A.x, A.y, A.z);
    }

    // Euler angles (radians) as (pitch=x, yaw=y, roll=z).
    template<typename T>
    [[nodiscard]] TVec<T, 3> EulerAngles(const TQuat<T>& Q)
    {
        const T Pitch = static_cast<T>(std::atan2(T(2) * (Q.y * Q.z + Q.w * Q.x), Q.w * Q.w - Q.x * Q.x - Q.y * Q.y + Q.z * Q.z));
        const T SinYaw = static_cast<T>(-2) * (Q.x * Q.z - Q.w * Q.y);
        const T Yaw = static_cast<T>(std::asin(SinYaw < T(-1) ? T(-1) : (SinYaw > T(1) ? T(1) : SinYaw)));
        const T Roll = static_cast<T>(std::atan2(T(2) * (Q.x * Q.y + Q.w * Q.z), Q.w * Q.w + Q.x * Q.x - Q.y * Q.y - Q.z * Q.z));
        return TVec<T, 3>(Pitch, Yaw, Roll);
    }

    template<typename T>
    [[nodiscard]] TQuat<T> Slerp(const TQuat<T>& A, const TQuat<T>& B, T Alpha)
    {
        T CosTheta = Dot(A, B);
        TQuat<T> End = B;

        // Take the shorter arc.
        if (CosTheta < T(0))
        {
            End = -B;
            CosTheta = -CosTheta;
        }

        // Nearly parallel: fall back to normalized lerp to avoid div-by-zero.
        if (CosTheta > T(1) - T(LE_KINDA_SMALL_NUMBER))
        {
            return Normalize(A + (End + (-A)) * Alpha);
        }

        const T Theta = static_cast<T>(std::acos(CosTheta));
        const T SinTheta = static_cast<T>(std::sin(Theta));
        const T WA = static_cast<T>(std::sin((T(1) - Alpha) * Theta)) / SinTheta;
        const T WB = static_cast<T>(std::sin(Alpha * Theta)) / SinTheta;
        return A * WA + End * WB;
    }

    template<typename T>
    [[nodiscard]] constexpr TVec<T, 3> Rotate(const TQuat<T>& Q, const TVec<T, 3>& V)
    {
        return Q * V;
    }

    // Pointer to contiguous x,y,z,w (GPU upload / interop).
    template<typename T> [[nodiscard]] const T* ValuePtr(const TQuat<T>& Q) { return &Q.x; }
    template<typename T> [[nodiscard]] T*       ValuePtr(TQuat<T>& Q)       { return &Q.x; }
}

#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
