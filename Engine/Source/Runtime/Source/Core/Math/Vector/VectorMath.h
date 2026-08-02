#pragma once

#include "VectorTypes.h"
#include "Core/Math/Scalar.h"
#include "Core/LuminaMacros.h"
#include <cmath>

namespace Lumina::Math
{
    template<typename T, int N>
    [[nodiscard]] constexpr T Dot(const TVec<T, N>& A, const TVec<T, N>& B)
    {
        T Sum = T(0);
        for (int i = 0; i < N; ++i) { Sum += A[i] * B[i]; }
        return Sum;
    }

    template<typename T>
    [[nodiscard]] constexpr TVec<T, 3> Cross(const TVec<T, 3>& A, const TVec<T, 3>& B)
    {
        return TVec<T, 3>(
            A.y * B.z - A.z * B.y,
            A.z * B.x - A.x * B.z,
            A.x * B.y - A.y * B.x);
    }

    template<typename T, int N>
    [[nodiscard]] constexpr T LengthSquared(const TVec<T, N>& V)
    {
        return Dot(V, V);
    }

    template<typename T, int N>
    [[nodiscard]] T Length(const TVec<T, N>& V)
    {
        return static_cast<T>(std::sqrt(LengthSquared(V)));
    }

    template<typename T, int N>
    [[nodiscard]] constexpr T DistanceSquared(const TVec<T, N>& A, const TVec<T, N>& B)
    {
        return LengthSquared(B - A);
    }

    template<typename T, int N>
    [[nodiscard]] T Distance(const TVec<T, N>& A, const TVec<T, N>& B)
    {
        return Length(B - A);
    }

    template<typename T, int N>
    [[nodiscard]] TVec<T, N> Normalize(const TVec<T, N>& V)
    {
        const T Len = Length(V);
        return Len > T(0) ? V * (T(1) / Len) : TVec<T, N>{};
    }

    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Lerp(const TVec<T, N>& A, const TVec<T, N>& B, T Alpha)
    {
        return A + (B - A) * Alpha;
    }

    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Reflect(const TVec<T, N>& Incident, const TVec<T, N>& Normal)
    {
        return Incident - Normal * (T(2) * Dot(Incident, Normal));
    }


    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Min(const TVec<T, N>& A, const TVec<T, N>& B)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = A[i] < B[i] ? A[i] : B[i]; }
        return R;
    }

    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Max(const TVec<T, N>& A, const TVec<T, N>& B)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = A[i] > B[i] ? A[i] : B[i]; }
        return R;
    }

    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Clamp(const TVec<T, N>& V, const TVec<T, N>& Lo, const TVec<T, N>& Hi)
    {
        return Min(Max(V, Lo), Hi);
    }

    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Abs(const TVec<T, N>& V)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = V[i] < T(0) ? -V[i] : V[i]; }
        return R;
    }

    // Horizontal min/max over the components.
    template<typename T, int N>
    [[nodiscard]] constexpr T MinComponent(const TVec<T, N>& V)
    {
        T Result = V[0];
        for (int i = 1; i < N; ++i) { if (V[i] < Result) { Result = V[i]; } }
        return Result;
    }

    template<typename T, int N>
    [[nodiscard]] constexpr T MaxComponent(const TVec<T, N>& V)
    {
        T Result = V[0];
        for (int i = 1; i < N; ++i) { if (V[i] > Result) { Result = V[i]; } }
        return Result;
    }

    template<typename T, int N>
    [[nodiscard]] constexpr bool IsNearlyEqual(const TVec<T, N>& A, const TVec<T, N>& B, T Epsilon = T(LE_KINDA_SMALL_NUMBER))
    {
        for (int i = 0; i < N; ++i)
        {
            const T Diff = A[i] - B[i];
            if ((Diff < T(0) ? -Diff : Diff) > Epsilon) { return false; }
        }
        return true;
    }

    #define LUMINA_VEC_MAP1(Name)                                                          \
        template<typename T, int N> [[nodiscard]] TVec<T, N> Name(const TVec<T, N>& V)      \
        { TVec<T, N> R{}; for (int i = 0; i < N; ++i) { R[i] = Name(V[i]); } return R; }

    LUMINA_VEC_MAP1(Floor)
    LUMINA_VEC_MAP1(Ceil)
    LUMINA_VEC_MAP1(Round)
    LUMINA_VEC_MAP1(Fract)
    LUMINA_VEC_MAP1(Sign)
    LUMINA_VEC_MAP1(Sqrt)
    LUMINA_VEC_MAP1(InverseSqrt)
    LUMINA_VEC_MAP1(Sin)
    LUMINA_VEC_MAP1(Cos)
    LUMINA_VEC_MAP1(Tan)
    LUMINA_VEC_MAP1(Asin)
    LUMINA_VEC_MAP1(Acos)
    LUMINA_VEC_MAP1(Atan)
    LUMINA_VEC_MAP1(Exp)
    LUMINA_VEC_MAP1(Log)
    LUMINA_VEC_MAP1(Radians)
    LUMINA_VEC_MAP1(Degrees)
    LUMINA_VEC_MAP1(Saturate)
    #undef LUMINA_VEC_MAP1

    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Max(const TVec<T, N>& V, std::type_identity_t<T> S)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = V[i] < S ? S : V[i]; }
        return R;
    }

    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Min(const TVec<T, N>& V, std::type_identity_t<T> S)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = S < V[i] ? S : V[i]; }
        return R;
    }

    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Clamp(const TVec<T, N>& V, std::type_identity_t<T> Lo, std::type_identity_t<T> Hi)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = V[i] < Lo ? Lo : (Hi < V[i] ? Hi : V[i]); }
        return R;
    }

    template<typename T, int N>
    [[nodiscard]] TVec<T, N> Pow(const TVec<T, N>& Base, const TVec<T, N>& Exp)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = Pow(Base[i], Exp[i]); }
        return R;
    }

    template<typename T, int N>
    [[nodiscard]] TVec<T, N> Mod(const TVec<T, N>& V, std::type_identity_t<T> S)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = Mod(V[i], S); }
        return R;
    }

    // Linear blend (mix), scalar and per-component interpolant.
    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Mix(const TVec<T, N>& A, const TVec<T, N>& B, T Alpha)
    {
        return A * (T(1) - Alpha) + B * Alpha;
    }

    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Mix(const TVec<T, N>& A, const TVec<T, N>& B, const TVec<T, N>& Alpha)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = A[i] * (T(1) - Alpha[i]) + B[i] * Alpha[i]; }
        return R;
    }

    template<typename T, int N>
    [[nodiscard]] constexpr TVec<T, N> Step(std::type_identity_t<T> Edge, const TVec<T, N>& V)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = V[i] < Edge ? T(0) : T(1); }
        return R;
    }

    // Refract a normalized incident I about normal N with index ratio eta.
    template<typename T, int N>
    [[nodiscard]] TVec<T, N> Refract(const TVec<T, N>& I, const TVec<T, N>& Nrm, T Eta)
    {
        const T DotNI = Dot(Nrm, I);
        const T K = T(1) - Eta * Eta * (T(1) - DotNI * DotNI);
        if (K < T(0)) { return TVec<T, N>{}; }
        return I * Eta - Nrm * (Eta * DotNI + Sqrt(K));
    }

    // Pointer to contiguous components (GPU upload / interop).
    template<typename T, int N> [[nodiscard]] const T* ValuePtr(const TVec<T, N>& V) { return &V[0]; }
    template<typename T, int N> [[nodiscard]] T*       ValuePtr(TVec<T, N>& V)       { return &V[0]; }
}
