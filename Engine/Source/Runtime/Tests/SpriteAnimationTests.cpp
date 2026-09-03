#include <gtest/gtest.h>

#include "Assets/AssetTypes/SpriteSheet/SpriteSheet.h"

using namespace Lumina;

namespace
{
    // One frame per step at 10 fps.
    constexpr float kFrameDt = 0.1f;
    constexpr float kFPS     = 10.0f;
}

TEST(SpriteAnimation, AdvancesOneFramePerPeriod)
{
    FSpritePlayback Playback;

    SpriteAnimation::Advance(Playback, 4, kFPS, true, 1.0f, kFrameDt);
    EXPECT_EQ(Playback.Frame, 1);

    SpriteAnimation::Advance(Playback, 4, kFPS, true, 1.0f, kFrameDt);
    EXPECT_EQ(Playback.Frame, 2);
}

TEST(SpriteAnimation, PartialStepsAccumulate)
{
    FSpritePlayback Playback;

    for (int32 Step = 0; Step < 4; ++Step)
    {
        SpriteAnimation::Advance(Playback, 4, kFPS, true, 1.0f, kFrameDt * 0.25f);
    }
    EXPECT_EQ(Playback.Frame, 1);
    EXPECT_FALSE(Playback.bFinished);
}

TEST(SpriteAnimation, LoopsBackToTheFirstFrame)
{
    FSpritePlayback Playback;

    for (int32 Step = 0; Step < 3; ++Step)
    {
        SpriteAnimation::Advance(Playback, 3, kFPS, true, 1.0f, kFrameDt);
    }
    EXPECT_EQ(Playback.Frame, 0);
    EXPECT_FALSE(Playback.bFinished);
}

TEST(SpriteAnimation, NonLoopingHoldsTheLastFrame)
{
    FSpritePlayback Playback;

    for (int32 Step = 0; Step < 8; ++Step)
    {
        SpriteAnimation::Advance(Playback, 3, kFPS, false, 1.0f, kFrameDt);
    }
    EXPECT_EQ(Playback.Frame, 2);
    EXPECT_TRUE(Playback.bFinished);
}

// A long frame must not be swallowed; it should skip through the frames it covers.
TEST(SpriteAnimation, LargeDeltaSkipsMultipleFrames)
{
    FSpritePlayback Playback;

    SpriteAnimation::Advance(Playback, 8, kFPS, true, 1.0f, kFrameDt * 3.0f);
    EXPECT_EQ(Playback.Frame, 3);
}

TEST(SpriteAnimation, SpeedScaleMultipliesTheRate)
{
    FSpritePlayback Playback;

    SpriteAnimation::Advance(Playback, 8, kFPS, true, 2.0f, kFrameDt);
    EXPECT_EQ(Playback.Frame, 2);
}

TEST(SpriteAnimation, ZeroRateDoesNotAdvance)
{
    FSpritePlayback Playback;

    SpriteAnimation::Advance(Playback, 4, 0.0f, true, 1.0f, kFrameDt);
    EXPECT_EQ(Playback.Frame, 0);

    SpriteAnimation::Advance(Playback, 4, kFPS, true, 0.0f, kFrameDt);
    EXPECT_EQ(Playback.Frame, 0);
}

TEST(SpriteAnimation, FinishedClipStaysPut)
{
    FSpritePlayback Playback;
    Playback.Frame     = 2;
    Playback.bFinished = true;

    SpriteAnimation::Advance(Playback, 3, kFPS, false, 1.0f, kFrameDt * 10.0f);
    EXPECT_EQ(Playback.Frame, 2);
}

TEST(SpriteAnimation, FrameIsClampedIntoTheClip)
{
    FSpritePlayback Playback;
    Playback.Frame = 99;

    SpriteAnimation::Advance(Playback, 3, kFPS, true, 1.0f, 0.0f);
    EXPECT_EQ(Playback.Frame, 2);
}

TEST(SpriteSheet, FindAnimationFallsBackToTheFirst)
{
    CSpriteSheet* Sheet = NewObject<CSpriteSheet>();
    ASSERT_NE(Sheet, nullptr);

    SSpriteAnimation& Idle = Sheet->Animations.emplace_back();
    Idle.Name = "Idle";
    SSpriteAnimation& Walk = Sheet->Animations.emplace_back();
    Walk.Name = "Walk";

    EXPECT_EQ(Sheet->FindAnimation("Walk")->Name, FName("Walk"));
    EXPECT_EQ(Sheet->FindAnimation(FName())->Name, FName("Idle"));
    EXPECT_EQ(Sheet->FindAnimation("Missing"), nullptr);
}
