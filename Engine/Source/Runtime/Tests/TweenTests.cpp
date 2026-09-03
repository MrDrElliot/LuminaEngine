#include <gtest/gtest.h>

#include "World/Subsystems/TweenManager.h"

using namespace Lumina;

namespace
{
    // One step of 0.1s at a time, so a one-second tween is exactly ten ticks.
    constexpr float kStep = 0.1f;

    void Advance(FTweenManager& Manager, int32 Steps, float Step = kStep)
    {
        for (int32 i = 0; i < Steps; ++i)
        {
            Manager.Tick(Step);
        }
    }
}

TEST(Tween, ValueTweenReachesItsTarget)
{
    FTweenManager Manager;

    float Value = 0.0f;
    Manager.Create().To(0.0f, 10.0f, 1.0f, [&Value](const float& V) { Value = V; })
        .Trans(EEaseTransition::Linear).Ease(EEaseType::In);

    Advance(Manager, 5);
    EXPECT_NEAR(Value, 5.0f, 0.51f);

    Advance(Manager, 6);
    EXPECT_NEAR(Value, 10.0f, 1e-3f);
}

TEST(Tween, StepsRunInSequence)
{
    FTweenManager Manager;

    TVector<int32> Order;
    Manager.Create()
        .Call([&Order] { Order.push_back(1); })
        .Interval(0.5f)
        .Call([&Order] { Order.push_back(2); });

    Advance(Manager, 1);
    ASSERT_EQ(Order.size(), 1u);
    EXPECT_EQ(Order[0], 1);

    Advance(Manager, 6);
    ASSERT_EQ(Order.size(), 2u);
    EXPECT_EQ(Order[1], 2);
}

TEST(Tween, ParallelPutsTweenersInOneStep)
{
    FTweenManager Manager;

    float A = 0.0f;
    float B = 0.0f;
    Manager.Create()
        .To(0.0f, 1.0f, 1.0f, [&A](const float& V) { A = V; }).Trans(EEaseTransition::Linear).Ease(EEaseType::In)
        .Parallel()
        .To(0.0f, 1.0f, 1.0f, [&B](const float& V) { B = V; }).Trans(EEaseTransition::Linear).Ease(EEaseType::In);

    Advance(Manager, 5);

    // Sequential would have left B untouched while A ran.
    EXPECT_GT(A, 0.0f);
    EXPECT_GT(B, 0.0f);
    EXPECT_NEAR(A, B, 1e-4f);
}

TEST(Tween, DelayHoldsTheTweenerBack)
{
    FTweenManager Manager;

    float Value = 0.0f;
    Manager.Create().To(0.0f, 1.0f, 0.5f, [&Value](const float& V) { Value = V; })
        .Trans(EEaseTransition::Linear).Ease(EEaseType::In).Delay(0.5f);

    Advance(Manager, 4);
    EXPECT_NEAR(Value, 0.0f, 1e-4f);

    Advance(Manager, 7);
    EXPECT_NEAR(Value, 1.0f, 1e-3f);
}

TEST(Tween, FinishedCallbackFiresOnceAndTheTweenEnds)
{
    FTweenManager Manager;

    int32 Finished = 0;
    FTween Tween = Manager.Create();
    Tween.To(0.0f, 1.0f, 0.2f, [](const float&) {}).OnFinished([&Finished] { ++Finished; });

    EXPECT_TRUE(Tween.IsRunning());

    Advance(Manager, 10);
    EXPECT_EQ(Finished, 1);
    EXPECT_FALSE(Tween.IsRunning());
}

TEST(Tween, LoopsRepeatTheWholeSequence)
{
    FTweenManager Manager;

    int32 Calls = 0;
    Manager.Create().Call([&Calls] { ++Calls; }).Interval(0.2f).SetLoops(3);

    Advance(Manager, 20);
    EXPECT_EQ(Calls, 3);
}

TEST(Tween, ZeroLoopsRunForever)
{
    FTweenManager Manager;

    int32 Calls = 0;
    FTween Tween = Manager.Create();
    Tween.Call([&Calls] { ++Calls; }).Interval(0.2f).SetLoops(0);

    Advance(Manager, 30);
    EXPECT_GT(Calls, 5);
    EXPECT_TRUE(Tween.IsRunning());

    Tween.Kill();
    Advance(Manager, 1);
    EXPECT_FALSE(Tween.IsRunning());
}

TEST(Tween, SpeedScaleChangesTheRate)
{
    FTweenManager Manager;

    float Slow = 0.0f;
    float Fast = 0.0f;
    Manager.Create().To(0.0f, 1.0f, 1.0f, [&Slow](const float& V) { Slow = V; })
        .Trans(EEaseTransition::Linear).Ease(EEaseType::In).SetSpeedScale(1.0f);
    Manager.Create().To(0.0f, 1.0f, 1.0f, [&Fast](const float& V) { Fast = V; })
        .Trans(EEaseTransition::Linear).Ease(EEaseType::In).SetSpeedScale(2.0f);

    Advance(Manager, 3);
    EXPECT_GT(Fast, Slow);
}

TEST(Tween, PauseFreezesProgress)
{
    FTweenManager Manager;

    float Value = 0.0f;
    FTween Tween = Manager.Create();
    Tween.To(0.0f, 1.0f, 1.0f, [&Value](const float& V) { Value = V; })
        .Trans(EEaseTransition::Linear).Ease(EEaseType::In);

    Advance(Manager, 3);
    const float Held = Value;

    Tween.SetPaused(true);
    Advance(Manager, 5);
    EXPECT_NEAR(Value, Held, 1e-4f);

    Tween.SetPaused(false);
    Advance(Manager, 3);
    EXPECT_GT(Value, Held);
}

TEST(Tween, KillStopsFurtherApplies)
{
    FTweenManager Manager;

    float Value = 0.0f;
    FTween Tween = Manager.Create();
    Tween.To(0.0f, 1.0f, 1.0f, [&Value](const float& V) { Value = V; })
        .Trans(EEaseTransition::Linear).Ease(EEaseType::In);

    Advance(Manager, 2);
    const float Held = Value;

    Tween.Kill();
    Advance(Manager, 5);
    EXPECT_NEAR(Value, Held, 1e-4f);
}

// A stale handle must answer questions rather than reach into a recycled slot.
TEST(Tween, StaleHandleIsNotRunning)
{
    FTweenManager Manager;

    FTween Tween = Manager.Create();
    Tween.Interval(0.1f);

    Advance(Manager, 5);
    EXPECT_FALSE(Tween.IsRunning());

    Tween.Kill();
    Tween.SetPaused(true);
    EXPECT_FALSE(Tween.IsRunning());
}
