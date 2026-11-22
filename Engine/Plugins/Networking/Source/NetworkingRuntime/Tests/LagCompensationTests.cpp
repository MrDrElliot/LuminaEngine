#include <gtest/gtest.h>

#include "Components/LagCompensationComponent.h"
#include "Core/Object/ObjectCore.h"
#include "Net/LagCompensation.h"
#include "Platform/Time/PlatformTime.h"
#include "Systems/LagCompensationSystem.h"
#include "World/World.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr double kTick = 1.0 / 60.0;

    // A standing capsule 1.8 m tall and 0.4 m wide, its foot at X on the ground.
    FLagCompensationSample Capsule(uint32 EntityIndex, float X)
    {
        FLagCompensationSample Sample;
        Sample.Entity = ECS::FEntity(EntityIndex, 0);
        Sample.A      = FVector3(X, 0.4f, 0.0f);
        Sample.B      = FVector3(X, 1.4f, 0.0f);
        Sample.Radius = 0.4f;
        return Sample;
    }

    // A runner moving one meter a tick along X, and a bystander standing still at X = 20.
    FLagCompensationHistory RecordRunner(int32 Ticks)
    {
        FLagCompensationHistory History;
        History.MaxRewindSeconds = 0.35;
        for (int32 Tick = 0; Tick < Ticks; ++Tick)
        {
            const FLagCompensationSample Samples[] = { Capsule(7, 20.0f), Capsule(3, (float)Tick) };
            History.Record(Tick * kTick, Samples);
        }
        return History;
    }

    // A shot fired along -Z at chest height, crossing X.
    SLagCompensatedHit ShootAt(const FLagCompensationHistory& History, double ViewTime, float X, float Thickness = 0.0f)
    {
        return LagCompensation::Raycast(History, ViewTime, FVector3(X, 1.0f, 10.0f), FVector3(0.0f, 0.0f, -1.0f), 50.0f, Thickness, ECS::NullEntity);
    }
}

TEST(LagCompensation, AShotLandsWhereTheTargetWasAtTheViewTime)
{
    const FLagCompensationHistory History = RecordRunner(30);
    const double ViewTime = 20.5 * kTick;

    const SLagCompensatedHit Hit = ShootAt(History, ViewTime, 20.5f);
    ASSERT_TRUE(Hit.bHit);
    EXPECT_EQ(Hit.Entity, ECS::FEntity(3, 0)) << "halfway between ticks 20 and 21 the runner stands at 20.5";
    EXPECT_NEAR(Hit.Distance, 9.6f, 1e-3f) << "the ray meets the front of the 0.4 m capsule";
    EXPECT_NEAR(Hit.Normal.z, 1.0f, 1e-3f);

    EXPECT_FALSE(ShootAt(History, ViewTime, 25.0f).bHit) << "where the runner is now is empty at the view time";
}

TEST(LagCompensation, ThicknessWidensAShotIntoASweptSphere)
{
    const FLagCompensationHistory History = RecordRunner(30);
    const double ViewTime = 25 * kTick;

    EXPECT_FALSE(ShootAt(History, ViewTime, 25.6f).bHit) << "0.6 m off center misses a 0.4 m capsule";
    EXPECT_TRUE(ShootAt(History, ViewTime, 25.6f, 0.3f).bHit) << "a 0.3 m sphere reaches it";
}

TEST(LagCompensation, TheShooterIsIgnoredAndTheNearestHitWins)
{
    FLagCompensationHistory History;
    const FLagCompensationSample Samples[] = { Capsule(1, 0.0f), Capsule(2, 0.0f) };
    History.Record(0.0, Samples);
    History.Record(kTick, Samples);

    // Both stand on the line of fire, the shooter at the muzzle and a target further down.
    FLagCompensationFrame& Frame = History.Frames[History.Newest];
    Frame.Samples[1].A.z = Frame.Samples[1].B.z = -5.0f;

    const SLagCompensatedHit Hit = LagCompensation::Raycast(History, kTick, FVector3(0.0f, 1.0f, 0.0f), FVector3(0.0f, 0.0f, -1.0f), 50.0f, 0.0f,
        ECS::FEntity(1, 0));
    ASSERT_TRUE(Hit.bHit);
    EXPECT_EQ(Hit.Entity, ECS::FEntity(2, 0));
}

TEST(LagCompensation, ViewTimesAreClampedToTheRewindWindow)
{
    const FLagCompensationHistory History = RecordRunner(120);
    const double Newest = 119 * kTick;

    EXPECT_DOUBLE_EQ(History.ClampViewTime(Newest + 1.0), Newest) << "a client cannot claim a time from the future";
    EXPECT_NEAR(History.ClampViewTime(0.0), Newest - 0.35, 1e-9) << "nor older than the window";
    EXPECT_LE(History.Count, 25) << "frames older than the window are let go";

    // An old view time is pulled to the window's edge, so the runner is found 21 ticks back rather than at tick 0.
    EXPECT_FALSE(ShootAt(History, 0.0, 0.0f).bHit);
    EXPECT_TRUE(ShootAt(History, 0.0, 98.0f).bHit);
}

TEST(LagCompensation, OverlapFindsEveryHitboxInReach)
{
    const FLagCompensationHistory History = RecordRunner(30);
    TVector<ECS::FEntity> Found;
    LagCompensation::OverlapSphere(History, 20 * kTick, FVector3(20.0f, 1.0f, 0.0f), 0.5f, Found);
    EXPECT_EQ(Found.Num(), 2) << "the runner passes the bystander at tick 20";

    Found.Reset();
    LagCompensation::OverlapSphere(History, 25 * kTick, FVector3(20.0f, 1.0f, 0.0f), 0.5f, Found);
    ASSERT_EQ(Found.Num(), 1);
    EXPECT_EQ(Found[0], ECS::FEntity(7, 0));
}

// The system records each hitbox from its component and transform, but only in a world that hosts.
TEST(LagCompensation, TheSystemRecordsHitboxesOnAHost)
{
    RegisterPendingTypes();
    for (const ENetMode Mode : { ENetMode::ListenServer, ENetMode::Standalone })
    {
        CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        TStrongObjectPtr<CWorld> Owner = World;
        FWorldContext Context;
        Context.NetMode = Mode;
        FWorldTestAccess::SetOwningContext(*World, &Context);
        FWorldTestAccess::KeepOnlySystems(*World, { FName("CLagCompensationSystem") });

        ECS::FRegistry& Registry = World->GetEntityRegistry();
        const ECS::FEntity Soldier = Registry.Create();
        Registry.Emplace<STransformComponent>(Soldier).SetLocalLocation(FVector3(5.0f, 0.0f, 0.0f));
        Registry.Emplace<SLagCompensationComponent>(Soldier);

        FWorldTestAccess::TickStage(*World, EUpdateStage::FrameEnd, kTick);

        const FLagCompensationHistory* History = Registry.Ctx().Find<FLagCompensationHistory>();
        if (Mode == ENetMode::Standalone)
        {
            EXPECT_TRUE(History == nullptr || History->Count == 0) << "nothing to judge without a network";
        }
        else
        {
            ASSERT_NE(History, nullptr);
            ASSERT_EQ(History->Count, 1);
            const FLagCompensationSample& Sample = History->FrameAt(0).Samples[0];
            EXPECT_EQ(Sample.Entity, Soldier);
            EXPECT_NEAR(Sample.A.x, 5.0f, 1e-4f);
            EXPECT_NEAR(Sample.A.y, 0.4f, 1e-4f) << "center 0.9 minus half height 0.5";
            EXPECT_NEAR(Sample.B.y, 1.4f, 1e-4f);
        }

        EntitySystems::DestroyAll(SSystemContext(), World->Systems);
        FWorldTestAccess::SetOwningContext(*World, nullptr);
    }
}

// A crowded server, so the cost of a rewound shot is known at scale.
TEST(LagCompensation, RaycastCostWithThousandsOfHitboxes)
{
    constexpr int32 kHitboxes = 4000;
    FLagCompensationHistory History;
    TVector<FLagCompensationSample> Samples;
    for (int32 Tick = 0; Tick < 24; ++Tick)
    {
        Samples.Reset();
        for (int32 Index = 0; Index < kHitboxes; ++Index)
        {
            FLagCompensationSample Sample = Capsule((uint32)Index, (float)(Index % 64) * 3.0f + Tick * 0.1f);
            Sample.A.z = Sample.B.z = (float)(Index / 64) * 3.0f;
            Samples.Add(Sample);
        }
        History.Record(Tick * kTick, Samples);
    }

    constexpr int32 kShots = 2000;
    int32 Hits = 0;
    const uint64 Start = PlatformTime::Cycles();
    for (int32 Shot = 0; Shot < kShots; ++Shot)
    {
        const float X = (float)(Shot % 64) * 3.0f + 2.0f;
        Hits += LagCompensation::Raycast(History, 20.5 * kTick, FVector3(X, 1.0f, -5.0f), FVector3(0.0f, 0.0f, 1.0f), 300.0f, 0.0f, ECS::NullEntity).bHit ? 1 : 0;
    }
    const double Microseconds = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start) * 1000.0 / kShots;
    printf("  lag compensated raycast against %d hitboxes: %.1f us per shot\n", kHitboxes, Microseconds);
    EXPECT_EQ(Hits, kShots);
}

// A host drawing 300 frames a second keeps the same history a 60 Hz one does.
TEST(LagCompensation, RecordingIsCappedAtTheRecordRate)
{
    RegisterPendingTypes();
    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    TStrongObjectPtr<CWorld> Owner = World;
    FWorldContext Context;
    Context.NetMode = ENetMode::ListenServer;
    FWorldTestAccess::SetOwningContext(*World, &Context);
    FWorldTestAccess::KeepOnlySystems(*World, { FName("CLagCompensationSystem") });

    ECS::FRegistry& Registry = World->GetEntityRegistry();
    const ECS::FEntity Soldier = Registry.Create();
    Registry.Emplace<STransformComponent>(Soldier);
    Registry.Emplace<SLagCompensationComponent>(Soldier);

    for (int32 Frame = 0; Frame < 300; ++Frame)
    {
        FWorldTestAccess::TickStage(*World, EUpdateStage::FrameEnd, 1.0 / 300.0);
    }

    const FLagCompensationHistory* History = Registry.Ctx().Find<FLagCompensationHistory>();
    ASSERT_NE(History, nullptr);
    EXPECT_LE(History->Count, 24) << "a 0.35 s window at 60 Hz, not at 300";
    EXPECT_GE(History->Count, 20);
    EXPECT_GE(History->GetNewestTime() - History->GetOldestTime(), 0.33) << "the window is still full";

    EntitySystems::DestroyAll(SSystemContext(), World->Systems);
    FWorldTestAccess::SetOwningContext(*World, nullptr);
}
