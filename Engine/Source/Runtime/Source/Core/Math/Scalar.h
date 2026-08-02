#pragma once

#include "Platform/GenericPlatform.h"
#include "Core/LuminaMacros.h"
#include <cmath>
#include <limits>
#include <concepts>
#include <type_traits>

// Scalar math in Lumina::Math; component-wise vector overloads live in Vector/VectorMath.h.

namespace Lumina::Math
{
    template<typename T> [[nodiscard]] constexpr T Pi()       { return T(3.141592653589793238462643383279502884L); }
    template<typename T> [[nodiscard]] constexpr T TwoPi()    { return T(2) * Pi<T>(); }
    template<typename T> [[nodiscard]] constexpr T HalfPi()   { return Pi<T>() / T(2); }
    template<typename T> [[nodiscard]] constexpr T Epsilon()  { return std::numeric_limits<T>::epsilon(); }

    template<std::floating_point T> [[nodiscard]] constexpr T Radians(T Degrees) { return Degrees * (Pi<T>() / T(180)); }
    template<std::floating_point T> [[nodiscard]] constexpr T Degrees(T Radians) { return Radians * (T(180) / Pi<T>()); }

    template<std::floating_point T> [[nodiscard]] T Sin(T V)        { return std::sin(V); }
    template<std::floating_point T> [[nodiscard]] T Cos(T V)        { return std::cos(V); }
    template<std::floating_point T> [[nodiscard]] T Tan(T V)        { return std::tan(V); }
    template<std::floating_point T> [[nodiscard]] T Asin(T V)       { return std::asin(V); }
    template<std::floating_point T> [[nodiscard]] T Acos(T V)       { return std::acos(V); }
    template<std::floating_point T> [[nodiscard]] T Atan(T V)       { return std::atan(V); }
    template<std::floating_point T> [[nodiscard]] T Atan2(T Y, T X) { return std::atan2(Y, X); }
    template<std::floating_point T> [[nodiscard]] T Exp(T V)        { return std::exp(V); }
    template<std::floating_point T> [[nodiscard]] T Exp2(T V)       { return std::exp2(V); }
    template<std::floating_point T> [[nodiscard]] T Log(T V)        { return std::log(V); }
    template<std::floating_point T> [[nodiscard]] T Log2(T V)       { return std::log2(V); }

    template<typename T> [[nodiscard]] T Sqrt(T V)        { return static_cast<T>(std::sqrt(V)); }
    template<typename T> [[nodiscard]] T InverseSqrt(T V) { return T(1) / static_cast<T>(std::sqrt(V)); }
    template<typename T> [[nodiscard]] T Pow(T Base, T Exp){ return static_cast<T>(std::pow(Base, Exp)); }

    template<std::floating_point T> [[nodiscard]] T Floor(T V) { return std::floor(V); }
    template<std::floating_point T> [[nodiscard]] T Ceil(T V)  { return std::ceil(V); }
    template<std::floating_point T> [[nodiscard]] T Round(T V) { return std::round(V); }
    template<std::floating_point T> [[nodiscard]] T Trunc(T V) { return std::trunc(V); }
    template<std::floating_point T> [[nodiscard]] T Fract(T V) { return V - std::floor(V); }
    template<std::floating_point T> [[nodiscard]] T Mod(T X, T Y) { return X - Y * std::floor(X / Y); }

    template<typename T> requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Max(T A, T B) { return A < B ? B : A; }

    template<typename T> requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Min(T A, T B) { return B < A ? B : A; }

    template<typename T> requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Clamp(T V, T Lo, T Hi) { return V < Lo ? Lo : (Hi < V ? Hi : V); }

    template<typename T> requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Abs(T V) { return V < T(0) ? -V : V; }

    template<typename T> requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr T Sign(T V) { return T((V > T(0)) - (V < T(0))); }

    template<std::floating_point T> [[nodiscard]] constexpr T Saturate(T V) { return Clamp(V, T(0), T(1)); }

    // Linear blend: a*(1-t) + b*t.
    template<typename T> [[nodiscard]] constexpr T Mix(T A, T B, T Alpha) { return A * (T(1) - Alpha) + B * Alpha; }

    // step(edge, x): 0 if x < edge, else 1.
    template<std::floating_point T> [[nodiscard]] constexpr T Step(T Edge, T X) { return X < Edge ? T(0) : T(1); }

    // Smooth Hermite interpolation between edge0 and edge1.
    template<std::floating_point T> [[nodiscard]] constexpr T SmoothStep(T Edge0, T Edge1, T X)
    {
        const T Tt = Clamp((X - Edge0) / (Edge1 - Edge0), T(0), T(1));
        return Tt * Tt * (T(3) - T(2) * Tt);
    }

    template<std::floating_point T> [[nodiscard]] T Fma(T A, T B, T C) { return std::fma(A, B, C); }

    // |a - b| <= eps.
    template<typename T> requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr bool EpsilonEqual(T A, T B, T Eps) { return Abs(A - B) <= Eps; }
}
