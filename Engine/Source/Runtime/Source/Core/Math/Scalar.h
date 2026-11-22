#pragma once

#include "Core/Templates/NumericLimits.h"
#include "Platform/GenericPlatform.h"
#include "Core/LuminaMacros.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <concepts>
#include <type_traits>

// Scalar math in Lumina::Math; component-wise vector overloads live in Vector/VectorMath.h.

namespace Lumina::Math
{
    template<typename T> [[nodiscard]] constexpr T Pi()       { return T(3.141592653589793238462643383279502884L); }
    template<typename T> [[nodiscard]] constexpr T TwoPi()    { return T(2) * Pi<T>(); }
    template<typename T> [[nodiscard]] constexpr T HalfPi()   { return Pi<T>() / T(2); }
    template<typename T> [[nodiscard]] constexpr T Epsilon()  { return TNumericLimits<T>::Epsilon(); }

    constexpr float kSmallNumber      = 1e-8f;
    constexpr float kKindaSmallNumber = 1e-4f;

    template<std::floating_point T> [[nodiscard]] constexpr T Radians(T Degrees) { return Degrees * (Pi<T>() / T(180)); }
    template<std::floating_point T> [[nodiscard]] constexpr T Degrees(T Radians) { return Radians * (T(180) / Pi<T>()); }

    // The constant-evaluation half of the wrappers below, in double so a float result rounds once. Run-time calls never reach it.
    namespace CompileTime
    {
        constexpr double kLn2 = 0.693147180559945309417232121458176568;
        constexpr double kPi = 3.141592653589793238462643383279502884;
        constexpr double kExactIntegerLimit = 4503599627370496.0;

        constexpr bool IsNaN(double X) { return X != X; }
        constexpr bool IsInf(double X) { return X == std::numeric_limits<double>::infinity() || X == -std::numeric_limits<double>::infinity(); }

        constexpr double Trunc(double X)
        {
            if (IsNaN(X) || IsInf(X) || (X < 0.0 ? -X : X) >= kExactIntegerLimit)
            {
                return X;
            }
            return static_cast<double>(static_cast<int64_t>(X));
        }

        constexpr double Floor(double X)
        {
            const double T = Trunc(X);
            return T > X ? T - 1.0 : T;
        }

        constexpr double Ceil(double X)
        {
            const double T = Trunc(X);
            return T < X ? T + 1.0 : T;
        }

        // Halves round away from zero, as std::round does.
        constexpr double Round(double X)
        {
            return X < 0.0 ? -Floor(-X + 0.5) : Floor(X + 0.5);
        }

        // Truncated, with the sign of X, as std::fmod is.
        constexpr double FMod(double X, double Y)
        {
            if (IsNaN(X) || IsNaN(Y) || IsInf(X) || Y == 0.0)
            {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return X - Y * Trunc(X / Y);
        }

        constexpr double Sqrt(double X)
        {
            if (IsNaN(X) || X < 0.0)
            {
                return std::numeric_limits<double>::quiet_NaN();
            }
            if (X == 0.0 || IsInf(X))
            {
                return X;
            }
            double Guess = X < 1.0 ? 1.0 : X;
            for (int Step = 0; Step < 1100; ++Step)
            {
                const double Next = 0.5 * (Guess + X / Guess);
                if (Next == Guess)
                {
                    break;
                }
                Guess = Next;
            }
            return Guess;
        }

        constexpr double PowerOfTwo(int Exponent)
        {
            double Result = 1.0;
            const double Factor = Exponent < 0 ? 0.5 : 2.0;
            for (int Step = 0; Step < (Exponent < 0 ? -Exponent : Exponent); ++Step)
            {
                Result *= Factor;
            }
            return Result;
        }

        constexpr double Exp(double X)
        {
            if (IsNaN(X))
            {
                return X;
            }
            if (X > 709.8)
            {
                return std::numeric_limits<double>::infinity();
            }
            if (X < -745.2)
            {
                return 0.0;
            }
            const double K = Round(X / kLn2);
            const double R = X - K * kLn2;
            double Term = 1.0;
            double Sum = 1.0;
            for (int N = 1; N < 30; ++N)
            {
                Term *= R / N;
                Sum += Term;
            }
            return Sum * PowerOfTwo(static_cast<int>(K));
        }

        constexpr double Log(double X)
        {
            if (IsNaN(X) || X < 0.0)
            {
                return std::numeric_limits<double>::quiet_NaN();
            }
            if (X == 0.0)
            {
                return -std::numeric_limits<double>::infinity();
            }
            if (IsInf(X))
            {
                return X;
            }
            int Exponent = 0;
            while (X >= 2.0)
            {
                X *= 0.5;
                ++Exponent;
            }
            while (X < 1.0)
            {
                X *= 2.0;
                --Exponent;
            }
            // log(m) = 2 atanh((m - 1) / (m + 1)), whose series converges fast for m in [1, 2).
            const double Z = (X - 1.0) / (X + 1.0);
            const double Z2 = Z * Z;
            double Power = Z;
            double Sum = 0.0;
            for (int N = 1; N < 80; N += 2)
            {
                Sum += Power / N;
                Power *= Z2;
            }
            return 2.0 * Sum + Exponent * kLn2;
        }

        constexpr double Pow(double Base, double Exponent)
        {
            if (Exponent == 0.0)
            {
                return 1.0;
            }
            const bool bIntegral = Trunc(Exponent) == Exponent && (Exponent < 0.0 ? -Exponent : Exponent) < 2147483648.0;
            if (bIntegral)
            {
                int64_t Remaining = static_cast<int64_t>(Exponent < 0.0 ? -Exponent : Exponent);
                double Result = 1.0;
                double Square = Base;
                while (Remaining > 0)
                {
                    if ((Remaining & 1) != 0)
                    {
                        Result *= Square;
                    }
                    Square *= Square;
                    Remaining >>= 1;
                }
                return Exponent < 0.0 ? 1.0 / Result : Result;
            }
            if (Base < 0.0)
            {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return Base == 0.0 ? 0.0 : Exp(Exponent * Log(Base));
        }

        // Taylor series on [-pi/4, pi/4], where a dozen terms reach double precision.
        constexpr double SinKernel(double X)
        {
            const double X2 = X * X;
            double Term = X;
            double Sum = X;
            for (int N = 1; N < 14; ++N)
            {
                Term *= -X2 / ((2 * N) * (2 * N + 1));
                Sum += Term;
            }
            return Sum;
        }

        constexpr double CosKernel(double X)
        {
            const double X2 = X * X;
            double Term = 1.0;
            double Sum = 1.0;
            for (int N = 1; N < 14; ++N)
            {
                Term *= -X2 / ((2 * N - 1) * (2 * N));
                Sum += Term;
            }
            return Sum;
        }

        // pi/2 split in two, so subtracting a multiple of it keeps the bits a single double of pi/2 would round away.
        constexpr double kHalfPiHigh = 1.5707963267948966;
        constexpr double kHalfPiLow = 6.123233995736766e-17;

        // X less its nearest multiple of pi/2, with that multiple's quadrant, so the kernels only see [-pi/4, pi/4].
        constexpr double ReduceQuarter(double X, int& OutQuadrant)
        {
            const double K = Round(X / kHalfPiHigh);
            OutQuadrant = static_cast<int>(static_cast<int64_t>(FMod(K, 4.0) + 4.0) % 4);
            return (X - K * kHalfPiHigh) - K * kHalfPiLow;
        }

        constexpr double Sin(double X)
        {
            if (IsNaN(X) || IsInf(X))
            {
                return std::numeric_limits<double>::quiet_NaN();
            }
            int Quadrant = 0;
            const double R = ReduceQuarter(X, Quadrant);
            switch (Quadrant)
            {
            case 0: return SinKernel(R);
            case 1: return CosKernel(R);
            case 2: return -SinKernel(R);
            default: return -CosKernel(R);
            }
        }

        constexpr double Cos(double X)
        {
            if (IsNaN(X) || IsInf(X))
            {
                return std::numeric_limits<double>::quiet_NaN();
            }
            int Quadrant = 0;
            const double R = ReduceQuarter(X, Quadrant);
            switch (Quadrant)
            {
            case 0: return CosKernel(R);
            case 1: return -SinKernel(R);
            case 2: return -CosKernel(R);
            default: return SinKernel(R);
            }
        }

        // Halved until small with atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))), then the series.
        constexpr double Atan(double X)
        {
            if (IsNaN(X))
            {
                return X;
            }
            if (IsInf(X))
            {
                return X > 0.0 ? kPi / 2.0 : -kPi / 2.0;
            }
            if (X > 1.0 || X < -1.0)
            {
                return (X > 0.0 ? kPi / 2.0 : -kPi / 2.0) - Atan(1.0 / X);
            }
            double Scale = 1.0;
            while ((X < 0.0 ? -X : X) > 0.125)
            {
                X = X / (1.0 + Sqrt(1.0 + X * X));
                Scale *= 2.0;
            }
            const double X2 = X * X;
            double Power = X;
            double Sum = 0.0;
            for (int N = 0; N < 30; ++N)
            {
                Sum += (N % 2 == 0 ? Power : -Power) / (2 * N + 1);
                Power *= X2;
            }
            return Scale * Sum;
        }

        constexpr double Atan2(double Y, double X)
        {
            if (IsNaN(X) || IsNaN(Y))
            {
                return std::numeric_limits<double>::quiet_NaN();
            }
            if (X > 0.0)
            {
                return Atan(Y / X);
            }
            if (X < 0.0)
            {
                return Y >= 0.0 ? Atan(Y / X) + kPi : Atan(Y / X) - kPi;
            }
            return Y > 0.0 ? kPi / 2.0 : (Y < 0.0 ? -kPi / 2.0 : 0.0);
        }

        constexpr double Asin(double X)
        {
            return (X > 1.0 || X < -1.0) ? std::numeric_limits<double>::quiet_NaN() : Atan2(X, Sqrt(1.0 - X * X));
        }

        constexpr double Acos(double X)
        {
            return (X > 1.0 || X < -1.0) ? std::numeric_limits<double>::quiet_NaN() : Atan2(Sqrt(1.0 - X * X), X);
        }
    }

    // Each wrapper below calls the standard library at run time and its CompileTime twin during constant evaluation.
    #define LUMINA_MATH_UNARY(Name, Std)                                                                     \
        template<std::floating_point T> [[nodiscard]] constexpr T Name(T V)                                 \
        {                                                                                                    \
            if consteval { return static_cast<T>(CompileTime::Name(static_cast<double>(V))); }              \
            else { return Std(V); }                                                                          \
        }

    LUMINA_MATH_UNARY(Sin, std::sin)
    LUMINA_MATH_UNARY(Cos, std::cos)
    LUMINA_MATH_UNARY(Asin, std::asin)
    LUMINA_MATH_UNARY(Acos, std::acos)
    LUMINA_MATH_UNARY(Atan, std::atan)
    LUMINA_MATH_UNARY(Exp, std::exp)
    LUMINA_MATH_UNARY(Log, std::log)
    LUMINA_MATH_UNARY(Floor, std::floor)
    LUMINA_MATH_UNARY(Ceil, std::ceil)
    LUMINA_MATH_UNARY(Round, std::round)
    LUMINA_MATH_UNARY(Trunc, std::trunc)
    #undef LUMINA_MATH_UNARY

    template<std::floating_point T> [[nodiscard]] constexpr T Tan(T V)
    {
        if consteval { return static_cast<T>(CompileTime::Sin(V) / CompileTime::Cos(V)); }
        else { return std::tan(V); }
    }

    template<std::floating_point T> [[nodiscard]] constexpr T Atan2(T Y, T X)
    {
        if consteval { return static_cast<T>(CompileTime::Atan2(Y, X)); }
        else { return std::atan2(Y, X); }
    }

    template<std::floating_point T> [[nodiscard]] constexpr T Exp2(T V)
    {
        if consteval { return static_cast<T>(CompileTime::Exp(V * CompileTime::kLn2)); }
        else { return std::exp2(V); }
    }

    template<std::floating_point T> [[nodiscard]] constexpr T Log2(T V)
    {
        if consteval { return static_cast<T>(CompileTime::Log(V) / CompileTime::kLn2); }
        else { return std::log2(V); }
    }

    // Truncated with the sign of X, as std::fmod is, unlike Mod which floors.
    template<std::floating_point T> [[nodiscard]] constexpr T FMod(T X, T Y)
    {
        if consteval { return static_cast<T>(CompileTime::FMod(X, Y)); }
        else { return std::fmod(X, Y); }
    }

    template<typename T> [[nodiscard]] constexpr T Sqrt(T V)
    {
        if consteval { return static_cast<T>(CompileTime::Sqrt(static_cast<double>(V))); }
        else { return static_cast<T>(std::sqrt(V)); }
    }

    template<typename T> [[nodiscard]] constexpr T InverseSqrt(T V) { return T(1) / Sqrt(V); }

    template<typename T> [[nodiscard]] constexpr T Pow(T Base, T Exp)
    {
        if consteval { return static_cast<T>(CompileTime::Pow(static_cast<double>(Base), static_cast<double>(Exp))); }
        else { return static_cast<T>(std::pow(Base, Exp)); }
    }

    template<std::floating_point T> [[nodiscard]] constexpr T Fract(T V) { return V - Floor(V); }
    template<std::floating_point T> [[nodiscard]] constexpr T Mod(T X, T Y) { return X - Y * Floor(X / Y); }

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

    // Fused at run time; during constant evaluation a float product is exact in double, so only a double operand rounds twice.
    template<std::floating_point T> [[nodiscard]] constexpr T Fma(T A, T B, T C)
    {
        if consteval { return static_cast<T>(static_cast<double>(A) * static_cast<double>(B) + static_cast<double>(C)); }
        else { return std::fma(A, B, C); }
    }

    // |a - b| <= eps.
    template<typename T> requires std::is_arithmetic_v<T>
    [[nodiscard]] constexpr bool EpsilonEqual(T A, T B, T Eps) { return Abs(A - B) <= Eps; }
}
