#include <gtest/gtest.h>

#include "Animation/AnimationGraphVM.h"

using namespace Lumina;

namespace
{
    constexpr int32 LastEasing = (int32)EAnimAlphaEasing::CircularInOut;

    const char* NameOf(int32 Value)
    {
        static const char* Names[] =
        {
            "Linear", "SmoothStep",
            "QuadraticIn", "QuadraticOut", "QuadraticInOut",
            "CubicIn", "CubicOut", "CubicInOut",
            "QuarticIn", "QuarticOut", "QuarticInOut",
            "QuinticIn", "QuinticOut", "QuinticInOut",
            "SinusoidalIn", "SinusoidalOut", "SinusoidalInOut",
            "ExponentialIn", "ExponentialOut", "ExponentialInOut",
            "CircularIn", "CircularOut", "CircularInOut",
        };
        return (Value >= 0 && Value < (int32)(sizeof(Names) / sizeof(Names[0]))) ? Names[Value] : "?";
    }
}

// Alpha 0 must stay pure pose A and alpha 1 pure pose B, whatever shape the curve takes between them.
TEST(AnimAlphaEasing, EveryCurvePreservesItsEndpoints)
{
    for (int32 i = 0; i <= LastEasing; ++i)
    {
        const EAnimAlphaEasing Easing = (EAnimAlphaEasing)i;
        EXPECT_NEAR(ApplyAlphaEasing(Easing, 0.0f), 0.0f, 1e-4f) << NameOf(i) << " must map 0 to 0";
        EXPECT_NEAR(ApplyAlphaEasing(Easing, 1.0f), 1.0f, 1e-4f) << NameOf(i) << " must map 1 to 1";
    }
}

// A blend that walked backwards mid-transition would read as a pop, so every curve has to rise.
TEST(AnimAlphaEasing, EveryCurveIsMonotonic)
{
    constexpr int32 Steps = 64;
    for (int32 i = 0; i <= LastEasing; ++i)
    {
        const EAnimAlphaEasing Easing = (EAnimAlphaEasing)i;

        float Previous = ApplyAlphaEasing(Easing, 0.0f);
        for (int32 Step = 1; Step <= Steps; ++Step)
        {
            const float T = (float)Step / (float)Steps;
            const float Value = ApplyAlphaEasing(Easing, T);

            EXPECT_GE(Value, Previous - 1e-4f) << NameOf(i) << " went backwards at t=" << T;
            EXPECT_GE(Value, -1e-4f) << NameOf(i) << " undershot at t=" << T;
            EXPECT_LE(Value, 1.0f + 1e-4f) << NameOf(i) << " overshot at t=" << T;
            Previous = Value;
        }
    }
}

// An alpha driven by a raw parameter can arrive unbounded; clamping is what keeps a blend in range.
TEST(AnimAlphaEasing, InputIsClampedToTheUnitRange)
{
    for (int32 i = 0; i <= LastEasing; ++i)
    {
        const EAnimAlphaEasing Easing = (EAnimAlphaEasing)i;
        EXPECT_NEAR(ApplyAlphaEasing(Easing, -5.0f), 0.0f, 1e-4f) << NameOf(i) << " must clamp below zero";
        EXPECT_NEAR(ApplyAlphaEasing(Easing, 7.5f), 1.0f, 1e-4f) << NameOf(i) << " must clamp above one";
    }
}

TEST(AnimAlphaEasing, LinearIsIdentityAndEasedCurvesActuallyBend)
{
    EXPECT_NEAR(ApplyAlphaEasing(EAnimAlphaEasing::Linear, 0.25f), 0.25f, 1e-5f);
    EXPECT_NEAR(ApplyAlphaEasing(EAnimAlphaEasing::Linear, 0.75f), 0.75f, 1e-5f);

    // An In curve starts slow and an Out curve starts fast; that is the whole point of choosing one.
    EXPECT_LT(ApplyAlphaEasing(EAnimAlphaEasing::CubicIn, 0.25f), 0.25f);
    EXPECT_GT(ApplyAlphaEasing(EAnimAlphaEasing::CubicOut, 0.25f), 0.25f);

    EXPECT_NEAR(ApplyAlphaEasing(EAnimAlphaEasing::CubicInOut, 0.5f), 0.5f, 1e-4f);
    EXPECT_NEAR(ApplyAlphaEasing(EAnimAlphaEasing::SmoothStep, 0.5f), 0.5f, 1e-4f);
}

// A symmetric curve eases in and out by the same shape, so f(t) and f(1-t) have to sum to one.
TEST(AnimAlphaEasing, InOutCurvesAreSymmetricAboutTheMidpoint)
{
    const EAnimAlphaEasing Symmetric[] =
    {
        EAnimAlphaEasing::Linear,
        EAnimAlphaEasing::SmoothStep,
        EAnimAlphaEasing::QuadraticInOut,
        EAnimAlphaEasing::CubicInOut,
        EAnimAlphaEasing::QuarticInOut,
        EAnimAlphaEasing::QuinticInOut,
        EAnimAlphaEasing::SinusoidalInOut,
        EAnimAlphaEasing::ExponentialInOut,
        EAnimAlphaEasing::CircularInOut,
    };

    for (EAnimAlphaEasing Easing : Symmetric)
    {
        for (int32 Step = 0; Step <= 16; ++Step)
        {
            const float T = (float)Step / 16.0f;
            const float Sum = ApplyAlphaEasing(Easing, T) + ApplyAlphaEasing(Easing, 1.0f - T);
            EXPECT_NEAR(Sum, 1.0f, 2e-3f) << NameOf((int32)Easing) << " is asymmetric at t=" << T;
        }
    }
}
