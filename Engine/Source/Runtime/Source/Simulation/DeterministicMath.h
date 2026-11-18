#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina::SimMath
{
    // Adds, multiplies and divides only, which IEEE rounds exactly, so no peer depends on its runtime library's sin or cos.

    inline constexpr float kPi     = 3.14159265358979323846f;
    inline constexpr float kHalfPi = 1.57079632679489661923f;
    inline constexpr float kTwoPi  = 6.28318530717958647692f;

    // Wraps to [-pi, pi] by subtracting whole turns, rounding the turn count the same way on every machine.
    NODISCARD inline float WrapAngle(float Radians)
    {
        const float Turns = Radians * (1.0f / kTwoPi);
        const float Whole = (float)(int64)(Turns + (Turns >= 0.0f ? 0.5f : -0.5f));
        return Radians - Whole * kTwoPi;
    }

    // Taylor series to the 11th power on [-pi/2, pi/2], with the rest of the circle folded onto it.
    NODISCARD inline float Sin(float Radians)
    {
        float X = WrapAngle(Radians);
        if (X > kHalfPi)
        {
            X = kPi - X;
        }
        else if (X < -kHalfPi)
        {
            X = -kPi - X;
        }
        const float X2 = X * X;
        return X * (1.0f + X2 * (-1.6666667e-1f + X2 * (8.3333333e-3f + X2 * (-1.9841270e-4f + X2 * (2.7557319e-6f + X2 * -2.5052108e-8f)))));
    }

    NODISCARD inline float Cos(float Radians)
    {
        return Sin(Radians + kHalfPi);
    }

    // Abramowitz and Stegun 4.4.49 on [0, 1], with larger arguments folded onto it, good to about 1e-5 radians.
    NODISCARD inline float Atan(float Value)
    {
        const bool  bNegative = Value < 0.0f;
        const float A = bNegative ? -Value : Value;
        const bool  bInverted = A > 1.0f;
        const float X = bInverted ? 1.0f / A : A;
        const float X2 = X * X;
        float Result = X * (0.9998660f + X2 * (-0.3302995f + X2 * (0.1801410f + X2 * (-0.0851330f + X2 * 0.0208351f))));
        if (bInverted)
        {
            Result = kHalfPi - Result;
        }
        return bNegative ? -Result : Result;
    }

    NODISCARD inline float Atan2(float Y, float X)
    {
        if (X > 0.0f)
        {
            return Atan(Y / X);
        }
        if (X < 0.0f)
        {
            return Atan(Y / X) + (Y >= 0.0f ? kPi : -kPi);
        }
        return Y > 0.0f ? kHalfPi : (Y < 0.0f ? -kHalfPi : 0.0f);
    }
}
