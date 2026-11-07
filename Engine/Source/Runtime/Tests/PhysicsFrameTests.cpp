#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "Containers/Algorithm.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsScene.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"

using namespace Lumina;

namespace
{
    void BindFrameTestTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        Transform.Bind(Registry, Entity);
        Transform.ResetDirtyState();
    }

    // A world driven through the same physics calls a frame makes, with the world's own pre-step hook installed.
    struct FPhysicsFrameWorld
    {
        CWorld* World = nullptr;
        TStrongObjectPtr<CWorld> WorldOwner;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }
        Physics::IPhysicsScene& Scene() const { return *World->GetPhysicsScene(); }

        FPhysicsFrameWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("PhysicsFrameTestWorld"), FGuid::New(), OF_Transient);
            WorldOwner = World;
            ECS::FRegistry& R = Registry();
            R.Ctx().Emplace<CWorld*>(World);
            R.GetSignals<STransformComponent>().OnConstruct.Connect<&BindFrameTestTransform>();
            if (Physics::GetPhysicsContext() == nullptr)
            {
                Physics::Initialize();
            }
            World->EnsurePhysicsScene();
        }

        ~FPhysicsFrameWorld()
        {
            WorldOwner.Reset();
        }

        ECS::FEntity Spawn(const FVector3& Location, ECS::FEntity Parent = ECS::NullEntity)
        {
            ECS::FRegistry& R = Registry();
            const ECS::FEntity Entity = R.Create();
            R.Emplace<STransformComponent>(Entity).SetLocation(Location);
            if (!Parent.IsNull())
            {
                ECS::Utils::ReparentEntity(R, Entity, Parent, false);
            }
            return Entity;
        }

        ECS::FEntity SpawnSphere(const FVector3& Location, EBodyType Type, bool bGravity, ECS::FEntity Parent = ECS::NullEntity)
        {
            const ECS::FEntity Entity = Spawn(Location, Parent);
            SRigidBodyComponent& Body = Registry().Emplace<SRigidBodyComponent>(Entity);
            Body.BodyType = Type;
            Body.bUseGravity = bGravity;
            Body.LinearDamping = 0.0f;
            Body.AngularDamping = 0.0f;
            Registry().Emplace<SSphereColliderComponent>(Entity).Radius = 0.5f;
            return Entity;
        }

        void SpawnFloor(float HalfExtent = 20.0f)
        {
            const ECS::FEntity Floor = Spawn(FVector3(0.0f, -0.5f, 0.0f));
            Registry().Emplace<SRigidBodyComponent>(Floor).BodyType = EBodyType::Static;
            Registry().Emplace<SBoxColliderComponent>(Floor).HalfExtent = FVector3(HalfExtent, 0.5f, HalfExtent);
        }

        FVector3 WorldLocation(ECS::FEntity Entity)
        {
            return ECS::Utils::ComputeWorldTransform(Registry(), Entity).GetLocation();
        }

        // One engine frame of physics, from the step through the display write, and the frame-end propagation after it.
        void Frame(double Dt)
        {
            ECS::Utils::ResolveAllDirtyTransforms(Registry());
            Scene().Update(Dt);
            Scene().DispatchPendingEvents();
            ECS::Utils::ResolveAllDirtyTransforms(Registry());
            World->FlushCommands();
            World->GetEvents().Advance();
        }
    };

    // Faster than the 60 Hz physics rate, so frames land between steps and the display pose is a real blend.
    constexpr double kRenderDt = 1.0 / 90.0;
}

TEST(PhysicsFrame, FixedStepCodeReadsTheSimulatedPose)
{
    FPhysicsFrameWorld W;
    const ECS::FEntity Ball = W.SpawnSphere(FVector3(0.0f, 3.0f, 0.0f), EBodyType::Dynamic, false);
    W.Frame(0.0);
    W.Scene().SetLinearVelocity(Ball, FVector3(3.0f, 0.0f, 0.0f));

    int32 Reads = 0;
    W.Scene().SetPreStepCallback([&](float)
    {
        ++Reads;
        EXPECT_NEAR(W.Registry().Get<STransformComponent>(Ball).GetLocation().x, W.Scene().GetBodyPosition(Ball).x, 1e-5f)
            << "a fixed step reads where the body is, not the blended display pose";
    }, [] { return true; });

    for (int32 Frame = 0; Frame < 90; ++Frame)
    {
        W.Frame(Frame % 7 == 0 ? 3.0 * kRenderDt : kRenderDt);
    }
    EXPECT_GT(Reads, 60);
}

TEST(PhysicsFrame, ABodyThatStopsRestsWhereItWasSimulated)
{
    FPhysicsFrameWorld W;
    W.SpawnFloor();
    const ECS::FEntity Dropped = W.SpawnSphere(FVector3(-3.0f, 2.0f, 0.0f), EBodyType::Dynamic, true);
    const ECS::FEntity Stopped = W.SpawnSphere(FVector3(3.0f, 5.0f, 0.0f), EBodyType::Dynamic, false);
    W.Frame(0.0);
    W.Scene().SetLinearVelocity(Stopped, FVector3(4.0f, 0.0f, 0.0f));
    for (int32 Frame = 0; Frame < 20; ++Frame)
    {
        W.Frame(kRenderDt);
    }

    // Put to sleep by hand mid-flight, which raises no move event for the step it stops in.
    W.Scene().DeactivateBody(Stopped);

    int32 Frame = 0;
    for (; Frame < 900 && W.Scene().IsBodyActive(Dropped); ++Frame)
    {
        W.Frame(kRenderDt);
    }
    ASSERT_FALSE(W.Scene().IsBodyActive(Dropped)) << "the dropped ball never fell asleep";
    ASSERT_FALSE(W.Scene().IsBodyActive(Stopped));

    for (int32 Settle = 0; Settle < 30; ++Settle)
    {
        W.Frame(kRenderDt);
        for (const ECS::FEntity Body : { Dropped, Stopped })
        {
            const FVector3 Shown = W.WorldLocation(Body);
            const FVector3 Simulated = W.Scene().GetBodyPosition(Body);
            EXPECT_NEAR(Shown.x, Simulated.x, 1e-5f) << "frame " << Settle;
            EXPECT_NEAR(Shown.y, Simulated.y, 1e-5f) << "frame " << Settle;
            EXPECT_NEAR(Shown.z, Simulated.z, 1e-5f) << "frame " << Settle;
        }
    }
}

TEST(PhysicsFrame, AScriptTeleportIsNotOverwritten)
{
    FPhysicsFrameWorld W;
    const ECS::FEntity Moving = W.SpawnSphere(FVector3(0.0f, 3.0f, 0.0f), EBodyType::Dynamic, false);
    const ECS::FEntity Sleeping = W.SpawnSphere(FVector3(0.0f, 3.0f, 5.0f), EBodyType::Dynamic, false);
    W.Frame(0.0);
    W.Scene().SetLinearVelocity(Moving, FVector3(3.0f, 0.0f, 0.0f));
    W.Scene().SetLinearVelocity(Sleeping, FVector3(3.0f, 0.0f, 0.0f));
    for (int32 Frame = 0; Frame < 20; ++Frame)
    {
        W.Frame(kRenderDt);
    }
    W.Scene().DeactivateBody(Sleeping);
    W.Frame(2.0 * kRenderDt);
    ASSERT_FALSE(W.Scene().IsBodyActive(Sleeping));

    const FVector3 MovingTarget(50.0f, 3.0f, 0.0f);
    const FVector3 SleepingTarget(-50.0f, 3.0f, 5.0f);
    W.Registry().Get<STransformComponent>(Moving).SetLocation(MovingTarget);
    W.Registry().Get<STransformComponent>(Sleeping).SetLocation(SleepingTarget);

    // Both a frame that steps and one that does not, since only a stepping frame clears the pending teleport.
    for (const double Dt : { 2.0 * kRenderDt, 0.25 * kRenderDt, kRenderDt })
    {
        W.Frame(Dt);
        EXPECT_NEAR(W.WorldLocation(Moving).x, MovingTarget.x, 0.2f) << "the body pose from before the teleport is not written back";
        EXPECT_NEAR(W.Scene().GetBodyPosition(Moving).x, MovingTarget.x, 0.2f);
        EXPECT_NEAR(W.WorldLocation(Sleeping).x, SleepingTarget.x, 0.2f);
        EXPECT_NEAR(W.Scene().GetBodyPosition(Sleeping).x, SleepingTarget.x, 0.2f);
    }
}

TEST(PhysicsFrame, ParentedBodiesFollowTheirSimulatedParent)
{
    FPhysicsFrameWorld W;
    const FVector3 Offset(0.0f, 2.0f, 0.0f);
    const ECS::FEntity Parent = W.SpawnSphere(FVector3(0.0f, 3.0f, 0.0f), EBodyType::Dynamic, false);
    const ECS::FEntity Rider = W.Spawn(Offset, Parent);
    W.Registry().Emplace<SRigidBodyComponent>(Rider).BodyType = EBodyType::Kinematic;
    W.Registry().Emplace<SBoxColliderComponent>(Rider).HalfExtent = FVector3(0.25f);

    const ECS::FEntity Anchor = W.Spawn(FVector3(0.0f, 3.0f, 10.0f));
    const ECS::FEntity Child = W.SpawnSphere(FVector3(1.0f, 0.0f, 0.0f), EBodyType::Dynamic, false, Anchor);

    W.Frame(0.0);
    const float Speed = 3.0f;
    W.Scene().SetLinearVelocity(Parent, FVector3(Speed, 0.0f, 0.0f));
    W.Scene().SetLinearVelocity(Child, FVector3(0.0f, 0.0f, Speed));

    const float StepLag = Speed / W.World->GetDefaultWorldSettings().PhysicsHz;
    for (int32 Frame = 0; Frame < 120; ++Frame)
    {
        W.Frame(kRenderDt);

        const FVector3 ParentShown = W.WorldLocation(Parent);
        EXPECT_NEAR(Math::Length(W.WorldLocation(Rider) - (ParentShown + Offset)), 0.0f, 1e-4f) << "frame " << Frame;

        // The rider's body is driven toward the parent's simulated pose, so it trails the parent body by at most a step.
        const FVector3 RiderBody = W.Scene().GetBodyPosition(Rider);
        const FVector3 ParentBody = W.Scene().GetBodyPosition(Parent);
        EXPECT_NEAR(RiderBody.y, ParentBody.y + Offset.y, 1e-3f) << "frame " << Frame;
        EXPECT_LE(ParentBody.x - RiderBody.x, StepLag * 1.01f + 1e-3f) << "frame " << Frame;
        EXPECT_GE(ParentBody.x - RiderBody.x, -1e-3f) << "frame " << Frame;

        EXPECT_NEAR(Math::Length(W.WorldLocation(Child) - W.Scene().GetBodyPosition(Child)), 0.0f, StepLag + 1e-3f)
            << "a simulated child under a plain parent shows its own body, frame " << Frame;
    }
    EXPECT_GT(W.WorldLocation(Parent).x, 3.5f);
    EXPECT_GT(W.WorldLocation(Child).z, 13.5f);
}

TEST(PhysicsFrame, ABodyBelowTheKillHeightIsDestroyedWithItsChildren)
{
    FPhysicsFrameWorld W;
    const float Below = W.World->GetDefaultWorldSettings().WorldKillHeight - 10.0f;
    const ECS::FEntity Lost = W.SpawnSphere(FVector3(0.0f, Below, 0.0f), EBodyType::Dynamic, false);
    const ECS::FEntity LostChild = W.SpawnSphere(FVector3(3.0f, 0.0f, 0.0f), EBodyType::Dynamic, false, Lost);
    const ECS::FEntity Kept = W.SpawnSphere(FVector3(0.0f, 3.0f, 0.0f), EBodyType::Dynamic, false);
    W.Frame(0.0);
    W.Scene().SetLinearVelocity(Lost, FVector3(1.0f, 0.0f, 0.0f));
    W.Scene().SetLinearVelocity(LostChild, FVector3(1.0f, 0.0f, 0.0f));
    W.Scene().SetLinearVelocity(Kept, FVector3(3.0f, 0.0f, 0.0f));

    for (int32 Frame = 0; Frame < 10; ++Frame)
    {
        W.Frame(kRenderDt);
    }
    EXPECT_FALSE(W.Registry().IsValid(Lost));
    EXPECT_FALSE(W.Registry().IsValid(LostChild)) << "a staged child of a killed body is skipped once the destroy reaches it";
    ASSERT_TRUE(W.Registry().IsValid(Kept));
    EXPECT_GT(W.WorldLocation(Kept).x, 0.1f) << "the rest of the frame's poses are still written";
}

namespace
{
    int32 BenchEnvInt(const char* Name, int32 Default)
    {
        const char* Value = std::getenv(Name);
        return Value != nullptr ? std::atoi(Value) : Default;
    }
}

// Run with GTEST_ALSO_RUN_DISABLED_TESTS=1, and LUMINA_PHYSICS_BENCH_WAIT holds that many seconds before measuring so a Tracy capture can attach.
TEST(PhysicsFrameBench, DISABLED_FrameCostAndSmoothness)
{
    FPhysicsFrameWorld W;
    ECS::FRegistry& R = W.Registry();
    W.SpawnFloor(200.0f);

    const float Speed = 2.0f;
    TVector<ECS::FEntity> Moving;
    for (int32 Index = 0; Index < 3000; ++Index)
    {
        Moving.Add(W.SpawnSphere(FVector3(-150.0f + 2.0f * (float)(Index % 60), 3.0f + 2.0f * (float)(Index / 1500), -100.0f + 2.0f * (float)((Index / 60) % 25)), EBodyType::Dynamic, false));
    }
    for (int32 Index = 0; Index < 1000; ++Index)
    {
        W.SpawnSphere(FVector3(50.0f + 1.5f * (float)(Index % 10), 1.0f + 1.1f * (float)(Index / 100), 50.0f + 1.5f * (float)((Index / 10) % 10)), EBodyType::Dynamic, true);
    }
    for (int32 Index = 0; Index < 100; ++Index)
    {
        const ECS::FEntity Character = W.Spawn(FVector3(-50.0f + 3.0f * (float)(Index % 10), 1.0f, 100.0f + 3.0f * (float)(Index / 10)));
        R.Emplace<SCharacterPhysicsComponent>(Character);
        R.Emplace<SCharacterMovementComponent>(Character);
    }
    TVector<ECS::FEntity> ParentedChildren;
    for (int32 Index = 0; Index < 200; ++Index)
    {
        const ECS::FEntity Anchor = W.Spawn(FVector3(100.0f + 2.0f * (float)(Index % 20), 6.0f, -100.0f + 2.0f * (float)(Index / 20)));
        ParentedChildren.Add(W.SpawnSphere(FVector3(0.0f), EBodyType::Dynamic, false, Anchor));
    }
    TVector<ECS::FEntity> CarrierParents;
    for (int32 Index = 0; Index < 100; ++Index)
    {
        const ECS::FEntity Carrier = W.SpawnSphere(FVector3(-150.0f + 3.0f * (float)(Index % 20), 20.0f, -100.0f + 4.0f * (float)(Index / 20)), EBodyType::Dynamic, false);
        const ECS::FEntity Rider = W.Spawn(FVector3(0.0f, 2.0f, 0.0f), Carrier);
        R.Emplace<SRigidBodyComponent>(Rider).BodyType = EBodyType::Kinematic;
        R.Emplace<SBoxColliderComponent>(Rider).HalfExtent = FVector3(0.25f);
        CarrierParents.Add(Carrier);
    }

    W.Frame(0.0);
    for (const ECS::FEntity Entity : Moving)         { W.Scene().SetLinearVelocity(Entity, FVector3(Speed, 0.0f, 0.0f)); }
    for (const ECS::FEntity Entity : ParentedChildren) { W.Scene().SetLinearVelocity(Entity, FVector3(Speed, 0.0f, 0.0f)); }
    for (const ECS::FEntity Entity : CarrierParents)  { W.Scene().SetLinearVelocity(Entity, FVector3(Speed, 0.0f, 0.0f)); }

    // Blocks of frames alternate between a fixed-step reader and none, so both pre-step passes run under the same machine load.
    constexpr int32 kModeBlock = 30;
    bool bReaders = false;
    W.Scene().SetPreStepCallback([](float) {}, [&] { return bReaders; });

    for (int32 Frame = 0; Frame < 120; ++Frame)
    {
        W.Frame(kRenderDt);
    }
    if (const int32 WaitSeconds = BenchEnvInt("LUMINA_PHYSICS_BENCH_WAIT", 0); WaitSeconds > 0)
    {
        std::this_thread::sleep_for(std::chrono::seconds(WaitSeconds));
    }

    const int32 Frames = BenchEnvInt("LUMINA_PHYSICS_BENCH_FRAMES", 900);
    const ECS::FEntity Tracked = Moving[0];
    const ECS::FEntity TrackedChild = ParentedChildren[0];
    TVector<double> UpdateUs[2], DispatchUs[2], FrameUs[2];
    double SumSq = 0.0, SumSqChild = 0.0, WorstRatio = 0.0;
    FVector3 Last = W.WorldLocation(Tracked);
    FVector3 LastChild = W.WorldLocation(TrackedChild);
    for (int32 Frame = 0; Frame < Frames; ++Frame)
    {
        bReaders = (Frame / kModeBlock) % 2 == 1;
        // A render rate that wanders around 90 Hz, so the blend alpha never repeats a short cycle.
        const double Dt = kRenderDt * (1.0 + 0.25 * std::sin(0.37 * Frame));
        ECS::Utils::ResolveAllDirtyTransforms(R);
        const auto T0 = std::chrono::high_resolution_clock::now();
        W.Scene().Update(Dt);
        const auto T1 = std::chrono::high_resolution_clock::now();
        W.Scene().DispatchPendingEvents();
        const auto T2 = std::chrono::high_resolution_clock::now();
        ECS::Utils::ResolveAllDirtyTransforms(R);
        W.World->FlushCommands();
        W.World->GetEvents().Advance();
        UpdateUs[bReaders].Add(std::chrono::duration<double, std::micro>(T1 - T0).count());
        DispatchUs[bReaders].Add(std::chrono::duration<double, std::micro>(T2 - T1).count());
        FrameUs[bReaders].Add(std::chrono::duration<double, std::micro>(T2 - T0).count());

        const FVector3 Now = W.WorldLocation(Tracked);
        const FVector3 NowChild = W.WorldLocation(TrackedChild);
        const double Ratio = (double)(Now.x - Last.x) / (Speed * Dt);
        const double RatioChild = (double)(NowChild.x - LastChild.x) / (Speed * Dt);
        SumSq += (Ratio - 1.0) * (Ratio - 1.0);
        SumSqChild += (RatioChild - 1.0) * (RatioChild - 1.0);
        WorstRatio = Math::Max(WorstRatio, Math::Abs(Ratio - 1.0));
        if (Frame < 24)
        {
            std::fprintf(stderr, "frame %3d dt %.5f ms delta %.6f m speed %.4f m/s (child %.4f)\n", Frame, Dt * 1000.0, Now.x - Last.x, (Now.x - Last.x) / Dt, (NowChild.x - LastChild.x) / Dt);
        }
        Last = Now;
        LastChild = NowChild;
    }

    auto Report = [](const char* Name, TVector<double>& Samples)
    {
        Algo::Sort(Samples);
        double Sum = 0.0;
        for (const double Sample : Samples) { Sum += Sample; }
        std::fprintf(stderr, "%-24s mean %8.1f us  median %8.1f us  min %8.1f us  p95 %8.1f us\n", Name, Sum / Samples.Num(),
            Samples[Samples.Num() / 2], Samples[0], Samples[(Samples.Num() * 95) / 100]);
    };
    std::fprintf(stderr, "%d frames in blocks of %d, awake bodies at end %u\n", Frames, kModeBlock, W.Scene().GetAwakeBodyCount());
    for (const int32 Mode : { 0, 1 })
    {
        std::fprintf(stderr, "%s\n", Mode ? "with a fixed-step reader:" : "without a fixed-step reader:");
        Report("  Update", UpdateUs[Mode]);
        Report("  DispatchPendingEvents", DispatchUs[Mode]);
        Report("  both", FrameUs[Mode]);
    }
    std::fprintf(stderr, "smoothness: rms(delta/(v*dt) - 1) %.2e, worst %.2e, parented child rms %.2e\n",
        std::sqrt(SumSq / Frames), WorstRatio, std::sqrt(SumSqChild / Frames));
    std::fflush(stderr);
}
