#include <gtest/gtest.h>

#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Scripting/InteropTestLibrary.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/TransformComponent.h"
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
        .Call([&Order] { Order.Add(1); })
        .Interval(0.5f)
        .Call([&Order] { Order.Add(2); });

    Advance(Manager, 1);
    ASSERT_EQ(Order.Num(), 1);
    EXPECT_EQ(Order[0], 1);

    Advance(Manager, 6);
    ASSERT_EQ(Order.Num(), 2);
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

namespace
{
    // A world registry for the transform and property tweeners to write through.
    struct FTweenWorld
    {
        ECS::FRegistry Registry;
        FTweenManager  Manager;

        FTweenWorld()
        {
            RegisterPendingTypes();
            Manager.SetWorldRegistry(&Registry);
        }

        ECS::FEntity Spawn(const FVector3& Location = FVector3(0.0f))
        {
            const ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<STransformComponent>(Entity).SetLocalLocation(Location);
            return Entity;
        }

        FVector3 Location(ECS::FEntity Entity) { return Registry.Get<STransformComponent>(Entity).GetLocalLocation(); }
    };
}

TEST(Tween, YoyoPlaysBackToTheStart)
{
    FTweenManager Manager;

    float Value = -1.0f;
    FTween Tween = Manager.Create();
    Tween.To(0.0f, 10.0f, 1.0f, [&Value](const float& V) { Value = V; }).Trans(EEaseTransition::Linear).SetLoops(2, ETweenLoopMode::Yoyo);

    Advance(Manager, 10);
    EXPECT_NEAR(Value, 10.0f, 1e-3f);

    Advance(Manager, 5);
    EXPECT_NEAR(Value, 5.0f, 0.51f) << "the second pass runs backward";

    Advance(Manager, 6);
    EXPECT_NEAR(Value, 0.0f, 1e-3f);
    EXPECT_FALSE(Tween.IsRunning());
}

// A pass that ends mid-frame hands the rest of that frame to the next pass, so loops do not drift late.
TEST(Tween, LoopCarriesLeftoverTimeIntoTheNextPass)
{
    FTweenManager Manager;

    float Value = 0.0f;
    FTween Tween = Manager.Create();
    Tween.To(0.0f, 1.0f, 0.25f, [&Value](const float& V) { Value = V; }).Trans(EEaseTransition::Linear).SetLoops(0);

    Advance(Manager, 3);
    EXPECT_EQ(Tween.GetCompletedLoops(), 1);
    EXPECT_NEAR(Value, 0.2f, 1e-3f);
}

TEST(Tween, CompleteJumpsToTheEndAndFinishes)
{
    FTweenManager Manager;

    float Value = 0.0f;
    int32 Finished = 0;
    int32 Calls = 0;
    FTween Tween = Manager.Create();
    Tween.To(0.0f, 5.0f, 10.0f, [&Value](const float& V) { Value = V; })
        .Call([&Calls] { ++Calls; })
        .SetLoops(0)
        .OnFinished([&Finished] { ++Finished; });

    Advance(Manager, 2);
    Tween.Complete();

    EXPECT_NEAR(Value, 5.0f, 1e-4f);
    EXPECT_EQ(Calls, 1) << "every step left in the pass runs";
    EXPECT_EQ(Finished, 1);
    EXPECT_FALSE(Tween.IsRunning());
}

TEST(Tween, IgnoreTimeScaleRunsOnRealTime)
{
    FTweenManager Manager;

    float Scaled = 0.0f;
    float Real = 0.0f;
    Manager.Create().To(0.0f, 1.0f, 1.0f, [&Scaled](const float& V) { Scaled = V; }).Trans(EEaseTransition::Linear);
    Manager.Create().To(0.0f, 1.0f, 1.0f, [&Real](const float& V) { Real = V; }).Trans(EEaseTransition::Linear).SetIgnoreTimeScale(true);

    // A paused world hands scaled tweens no time at all.
    for (int32 i = 0; i < 5; ++i)
    {
        Manager.Tick(0.0f, kStep);
    }
    EXPECT_NEAR(Scaled, 0.0f, 1e-4f);
    EXPECT_NEAR(Real, 0.5f, 1e-3f);
}

TEST(Tween, ProgressReportsTimeIntoThePass)
{
    FTweenManager Manager;

    FTween Tween = Manager.Create();
    Tween.Interval(0.5f).To(0.0f, 1.0f, 0.5f, [](const float&) {}).Delay(0.25f);

    EXPECT_NEAR(Tween.GetDuration(), 1.25f, 1e-4f);
    Advance(Manager, 3);
    EXPECT_NEAR(Tween.GetElapsed(), 0.3f, 1e-3f);
    EXPECT_NEAR(Tween.GetProgress(), 0.3f / 1.25f, 1e-3f);
}

TEST(Tween, StepAndLoopEventsFireInOrder)
{
    FTweenManager Manager;

    TVector<int32> Steps;
    TVector<int32> Loops;
    Manager.Create()
        .Interval(0.1f)
        .Interval(0.1f)
        .SetLoops(2)
        .OnStep([&Steps](int32 Step) { Steps.Add(Step); })
        .OnLoop([&Loops](int32 Completed) { Loops.Add(Completed); });

    Advance(Manager, 10);
    ASSERT_EQ(Steps.Num(), 4);
    EXPECT_EQ(Steps[0], 0);
    EXPECT_EQ(Steps[1], 1);
    EXPECT_EQ(Steps[2], 0);
    ASSERT_EQ(Loops.Num(), 1) << "the last pass ends in OnFinished, not OnLoop";
    EXPECT_EQ(Loops[0], 1);
}

TEST(Tween, RelativeMovesStartFromWhereTheEntityIs)
{
    FTweenWorld World;
    const ECS::FEntity Entity = World.Spawn(FVector3(1.0f, 0.0f, 0.0f));

    World.Manager.Create()
        .MoveBy(Entity, FVector3(2.0f, 0.0f, 0.0f), 0.2f)
        .MoveBy(Entity, FVector3(0.0f, 0.0f, 3.0f), 0.2f)
        .ScaleBy(Entity, FVector3(2.0f), 0.2f);

    Advance(World.Manager, 10);
    const FVector3 Location = World.Location(Entity);
    EXPECT_NEAR(Location.x, 3.0f, 1e-4f);
    EXPECT_NEAR(Location.z, 3.0f, 1e-4f);
    EXPECT_NEAR(World.Registry.Get<STransformComponent>(Entity).GetLocalScale().y, 2.0f, 1e-4f);
}

TEST(Tween, PunchAndShakeSettleBackToRest)
{
    FTweenWorld World;
    const ECS::FEntity Punched = World.Spawn(FVector3(5.0f, 0.0f, 0.0f));
    const ECS::FEntity Shaken = World.Spawn(FVector3(0.0f, 2.0f, 0.0f));

    World.Manager.Create().Punch(Punched, ETweenTransformChannel::Location, FVector3(0.0f, 1.0f, 0.0f), 1.0f);
    World.Manager.Create().Shake(Shaken, ETweenTransformChannel::Location, FVector3(0.5f), 1.0f);

    Advance(World.Manager, 1, 0.05f);
    EXPECT_GT(World.Location(Punched).y, 0.1f) << "the punch kicks toward its direction";

    float Largest = 0.0f;
    for (int32 i = 0; i < 10; ++i)
    {
        Advance(World.Manager, 1, 0.03f);
        Largest = Math::Max(Largest, Math::Length(World.Location(Shaken) - FVector3(0.0f, 2.0f, 0.0f)));
    }
    EXPECT_GT(Largest, 0.05f) << "the shake moves the entity";

    Advance(World.Manager, 40);
    EXPECT_NEAR(Math::Length(World.Location(Punched) - FVector3(5.0f, 0.0f, 0.0f)), 0.0f, 1e-4f);
    EXPECT_NEAR(Math::Length(World.Location(Shaken) - FVector3(0.0f, 2.0f, 0.0f)), 0.0f, 1e-4f);
}

TEST(Tween, CurveEasingFollowsTheCurveShape)
{
    FTweenManager Manager;

    SKeyedCurve Curve;
    Curve.AddKey(0.0f, 0.0f);
    Curve.AddKey(0.5f, 1.0f);
    Curve.AddKey(1.0f, 0.0f);

    float Value = 0.0f;
    Manager.Create().To(0.0f, 10.0f, 1.0f, [&Value](const float& V) { Value = V; }).EaseCurve(Curve);

    Advance(Manager, 5);
    EXPECT_NEAR(Value, 10.0f, 1e-3f);

    Advance(Manager, 6);
    EXPECT_NEAR(Value, 0.0f, 1e-3f);
}

TEST(Tween, PropertyTweensDriveReflectedComponentFields)
{
    FTweenWorld World;
    const ECS::FEntity Entity = World.Spawn();
    World.Registry.Emplace<SInteropBenchComponent>(Entity).Inner.Value = 1.0f;
    SPointLightComponent& Light = World.Registry.Emplace<SPointLightComponent>(Entity);
    Light.LightColor = FVector3(0.0f);

    int32 Updates = 0;
    World.Registry.GetSignals<SPointLightComponent>().OnUpdate.Connect([](void* Count, ECS::FRegistry&, ECS::FEntity) { ++*static_cast<int32*>(Count); }, &Updates);

    World.Manager.Create()
        .PropertyTo<SInteropBenchComponent>(Entity, "Inner.Value", 3.0f, 0.4f).Trans(EEaseTransition::Linear)
        .Parallel()
        .PropertyTo<SPointLightComponent>(Entity, "LightColor", FVector3(1.0f, 0.5f, 0.25f), 0.4f).Trans(EEaseTransition::Linear);

    Advance(World.Manager, 2);
    EXPECT_NEAR(World.Registry.Get<SInteropBenchComponent>(Entity).Inner.Value, 2.0f, 1e-3f);
    EXPECT_GT(Updates, 0) << "each write goes through the component's update signal";

    Advance(World.Manager, 3);
    EXPECT_NEAR(World.Registry.Get<SInteropBenchComponent>(Entity).Inner.Value, 3.0f, 1e-4f);
    EXPECT_NEAR(World.Registry.Get<SPointLightComponent>(Entity).LightColor.y, 0.5f, 1e-4f);
}

// A path that names nothing keeps the step's time, so the rest of the sequence stays on schedule.
TEST(Tween, AnUnknownPropertyHoldsItsStepsTime)
{
    FTweenWorld World;
    const ECS::FEntity Entity = World.Spawn();

    int32 Calls = 0;
    World.Manager.Create()
        .PropertyTo<SPointLightComponent>(Entity, "NoSuchField", 1.0f, 0.5f)
        .Call([&Calls] { ++Calls; });

    Advance(World.Manager, 4);
    EXPECT_EQ(Calls, 0);
    Advance(World.Manager, 2);
    EXPECT_EQ(Calls, 1);
}
