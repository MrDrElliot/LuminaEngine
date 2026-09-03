#include <gtest/gtest.h>

#include "Core/Math/Easing.h"

using namespace Lumina;

namespace
{
    constexpr EEaseTransition kAllTransitions[] =
    {
        EEaseTransition::Linear, EEaseTransition::Sine,  EEaseTransition::Quad,
        EEaseTransition::Cubic,  EEaseTransition::Quart, EEaseTransition::Quint,
        EEaseTransition::Expo,   EEaseTransition::Circ,  EEaseTransition::Back,
        EEaseTransition::Elastic, EEaseTransition::Bounce, EEaseTransition::Spring,
    };

    constexpr EEaseType kAllEases[] =
    {
        EEaseType::In, EEaseType::Out, EEaseType::InOut, EEaseType::OutIn,
    };

    constexpr bool NearlyEqual(float A, float B, float Tolerance = 1e-4f)
    {
        const float Diff = A - B;
        return (Diff < 0.0f ? -Diff : Diff) <= Tolerance;
    }
}

//~ The polynomial curves fold at compile time; these asserts are the proof, not the runtime cases below.

static_assert(Easing::Evaluate(EEaseTransition::Linear, EEaseType::In, 0.25f) == 0.25f);
static_assert(Easing::Evaluate(EEaseTransition::Quad, EEaseType::In, 0.5f) == 0.25f);
static_assert(Easing::Evaluate(EEaseTransition::Cubic, EEaseType::In, 0.5f) == 0.125f);
static_assert(NearlyEqual(Easing::Evaluate(EEaseTransition::Quart, EEaseType::In, 0.5f), 0.0625f));
static_assert(NearlyEqual(Easing::Evaluate(EEaseTransition::Quint, EEaseType::In, 0.5f), 0.03125f));

// Out is the In curve mirrored, so it has to be faster than linear at the start.
static_assert(Easing::Evaluate(EEaseTransition::Cubic, EEaseType::Out, 0.25f) > 0.25f);

// Back overshoots below zero on the way in; that is the whole point of it.
static_assert(Easing::Evaluate(EEaseTransition::Back, EEaseType::In, 0.2f) < 0.0f);

// Endpoints survive every polynomial curve and every direction.
static_assert(NearlyEqual(Easing::Evaluate(EEaseTransition::Quint, EEaseType::InOut, 0.0f), 0.0f));
static_assert(NearlyEqual(Easing::Evaluate(EEaseTransition::Quint, EEaseType::InOut, 1.0f), 1.0f));
static_assert(NearlyEqual(Easing::Evaluate(EEaseTransition::Back, EEaseType::OutIn, 1.0f), 1.0f));

// Input is clamped before shaping, so an unbounded alpha cannot run off the curve.
static_assert(Easing::Evaluate(EEaseTransition::Quad, EEaseType::In, -3.0f) == 0.0f);
static_assert(NearlyEqual(Easing::Evaluate(EEaseTransition::Quad, EEaseType::In, 4.0f), 1.0f));

static_assert(Easing::IsConstantEvaluable(EEaseTransition::Cubic));
static_assert(!Easing::IsConstantEvaluable(EEaseTransition::Sine));

TEST(Easing, EveryCurvePreservesItsEndpoints)
{
    for (EEaseTransition Transition : kAllTransitions)
    {
        for (EEaseType Ease : kAllEases)
        {
            EXPECT_NEAR(Easing::Evaluate(Transition, Ease, 0.0f), 0.0f, 1e-3f)
                << Easing::ToString(Transition) << " " << Easing::ToString(Ease) << " must map 0 to 0";
            EXPECT_NEAR(Easing::Evaluate(Transition, Ease, 1.0f), 1.0f, 1e-3f)
                << Easing::ToString(Transition) << " " << Easing::ToString(Ease) << " must map 1 to 1";
        }
    }
}

TEST(Easing, InputIsClampedToTheUnitRange)
{
    for (EEaseTransition Transition : kAllTransitions)
    {
        for (EEaseType Ease : kAllEases)
        {
            EXPECT_NEAR(Easing::Evaluate(Transition, Ease, -5.0f),
                        Easing::Evaluate(Transition, Ease, 0.0f), 1e-5f);
            EXPECT_NEAR(Easing::Evaluate(Transition, Ease, 7.5f),
                        Easing::Evaluate(Transition, Ease, 1.0f), 1e-5f);
        }
    }
}

TEST(Easing, LinearIsIdentity)
{
    for (int32 Step = 0; Step <= 10; ++Step)
    {
        const float T = (float)Step / 10.0f;
        for (EEaseType Ease : kAllEases)
        {
            EXPECT_NEAR(Easing::Evaluate(EEaseTransition::Linear, Ease, T), T, 1e-5f);
        }
    }
}

// The non-overshooting families must stay inside the range, which is what makes them safe for blends.
TEST(Easing, SettledCurvesStayInRange)
{
    const EEaseTransition Settled[] =
    {
        EEaseTransition::Linear, EEaseTransition::Sine, EEaseTransition::Quad, EEaseTransition::Cubic,
        EEaseTransition::Quart, EEaseTransition::Quint, EEaseTransition::Expo, EEaseTransition::Circ,
    };

    for (EEaseTransition Transition : Settled)
    {
        for (EEaseType Ease : kAllEases)
        {
            for (int32 Step = 0; Step <= 64; ++Step)
            {
                const float T = (float)Step / 64.0f;
                const float V = Easing::Evaluate(Transition, Ease, T);

                EXPECT_GE(V, -1e-3f) << Easing::ToString(Transition) << " undershot at " << T;
                EXPECT_LE(V, 1.0f + 1e-3f) << Easing::ToString(Transition) << " overshot at " << T;
            }
        }
    }
}

// Back, Elastic, Bounce and Spring earn their names by leaving the range, so assert they actually do.
TEST(Easing, OvershootingCurvesActuallyOvershoot)
{
    bool bBackUndershoots = false;
    bool bElasticUndershoots = false;

    for (int32 Step = 0; Step <= 64; ++Step)
    {
        const float T = (float)Step / 64.0f;
        bBackUndershoots    |= Easing::Evaluate(EEaseTransition::Back, EEaseType::In, T) < -1e-3f;
        bElasticUndershoots |= Easing::Evaluate(EEaseTransition::Elastic, EEaseType::In, T) < -1e-3f;
    }

    EXPECT_TRUE(bBackUndershoots);
    EXPECT_TRUE(bElasticUndershoots);
}
