#include <gtest/gtest.h>

#include "Components/RepTransformComponent.h"

using namespace Lumina;

namespace
{
    constexpr double Interval         = 0.05;
    constexpr double MaxExtrapolation = 0.25;
    constexpr float  TeleportDistance = 3.0f;

    FNetInterpState MakeRing(std::initializer_list<float> Xs)
    {
        FNetInterpState Ring;
        double Time = 0.0;
        for (float X : Xs)
        {
            Ring.Push(Time, FVector3(X, 0.0f, 0.0f), FQuat(1.0f, 0.0f, 0.0f, 0.0f));
            Time += Interval;
        }
        return Ring;
    }

    float EvaluateX(const FNetInterpState& Ring, double RenderTime, float Teleport = TeleportDistance)
    {
        FVector3 Pos(-1.0f);
        FQuat Rot;
        Ring.Evaluate(RenderTime, Pos, Rot, true, MaxExtrapolation, Teleport);
        return Pos.x;
    }
}

TEST(NetInterpState, StallAfterTeleportHoldsNewestSample)
{
    const FNetInterpState Ring = MakeRing({ 0.0f, 100.0f });
    EXPECT_FLOAT_EQ(EvaluateX(Ring, Interval + 0.2), 100.0f);
}

TEST(NetInterpState, LargeJumpStillExtrapolatesWithoutTeleportDistance)
{
    const FNetInterpState Ring = MakeRing({ 0.0f, 100.0f });
    EXPECT_NEAR(EvaluateX(Ring, Interval + 0.05, 0.0f), 200.0f, 1e-3f);
}

TEST(NetInterpState, NormalSpeedExtrapolatesAndCapsAtMaxExtrapolation)
{
    const FNetInterpState Ring = MakeRing({ 0.0f, 0.1f });
    EXPECT_NEAR(EvaluateX(Ring, Interval + 0.05), 0.2f, 1e-5f);
    EXPECT_NEAR(EvaluateX(Ring, Interval + 1.0), 0.1f + 0.1f * static_cast<float>(MaxExtrapolation / Interval), 1e-5f);
}

TEST(NetInterpState, BracketingTeleportHoldsEarlierSample)
{
    const FNetInterpState Ring = MakeRing({ 0.0f, 100.0f, 100.1f });
    EXPECT_FLOAT_EQ(EvaluateX(Ring, Interval * 0.5), 0.0f);
    EXPECT_NEAR(EvaluateX(Ring, Interval * 1.5), 100.05f, 1e-3f);
}

TEST(NetInterpState, EmptyRingLeavesOutputAndSingleSampleHolds)
{
    EXPECT_FLOAT_EQ(EvaluateX(FNetInterpState(), 1.0), -1.0f);
    EXPECT_FLOAT_EQ(EvaluateX(MakeRing({ 7.0f }), 1.0), 7.0f);
}
