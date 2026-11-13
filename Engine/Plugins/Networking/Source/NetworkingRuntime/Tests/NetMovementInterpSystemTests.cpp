#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Systems/NetMovementInterpSystem.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/World.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr double FrameDt = 1.0 / 60.0;

    const FVector3 ProxyLocation(1.0f, 2.0f, 3.0f);

    // A bare client world running only CNetMovementInterpSystem, with one proxy since the system idles without any.
    struct FInterpWorld
    {
        TStrongObjectPtr<CWorld> World;
        ECS::FEntity             Proxy;

        explicit FInterpWorld(ENetRole Role = ENetRole::SimulatedProxy)
        {
            RegisterPendingTypes();
            World = TStrongObjectPtr<CWorld>(NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient));
            FWorldTestAccess::SetWorldType(*World, EWorldType::Game);
            Registry().Ctx().Emplace<CWorld*>(World.Get());
            FWorldTestAccess::KeepOnlySystems(*World, { "CNetMovementInterpSystem" });
            Clock().bNetworked = true;

            Proxy = Registry().Create();
            Registry().Emplace<STransformComponent>(Proxy);
            Registry().Emplace<SNetworkComponent>(Proxy).LocalRole = Role;
            Registry().Emplace<FRepTransform>(Proxy).Ring.Push(0.0, ProxyLocation, FQuat(1.0f, 0.0f, 0.0f, 0.0f));
        }

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }
        FNetInterpClock& Clock() const { return Registry().Ctx().Get<FNetInterpClock>(); }

        double Tick(double Dt = FrameDt) const
        {
            FWorldTestAccess::TickStage(*World, EUpdateStage::PostPhysics, Dt);
            return Clock().ServerPlaybackTime;
        }

        FVector3 ProxyLocalLocation() const { return Registry().Get<STransformComponent>(Proxy).GetLocalLocation(); }
        bool ProxyNeedsTransformUpdate() const { return Registry().HasAll<FNeedsTransformUpdate>(Proxy); }
    };
}

TEST(NetMovementInterpSystem, FirstTickSnapsPlaybackToLatestServerTime)
{
    FInterpWorld Fixture;
    ASSERT_EQ(Fixture.World->Systems.Num(), 1) << "CNetMovementInterpSystem was not found";
    Fixture.Clock().LatestServerTime = 0.3;

    EXPECT_DOUBLE_EQ(Fixture.Tick(), 0.3) << "a small first offset must snap, not ease in";
    EXPECT_TRUE(Fixture.Clock().bInitialized);
}

TEST(NetMovementInterpSystem, SmallDriftClosesAtFourTimesDeltaPerTick)
{
    FInterpWorld Fixture;
    Fixture.Clock().LatestServerTime = 1.0;
    Fixture.Tick();

    EXPECT_NEAR(Fixture.Tick(0.1), 1.1 - 0.1 * 0.4, 1e-9);
    EXPECT_NEAR(Fixture.Tick(0.3), 1.0, 1e-9) << "the closing rate clamps to the full error";
}

TEST(NetMovementInterpSystem, LargeServerJumpResyncsInsteadOfEasing)
{
    FInterpWorld Fixture;
    Fixture.Clock().LatestServerTime = 1.0;
    Fixture.Tick();

    Fixture.Clock().LatestServerTime = 2.0;
    EXPECT_DOUBLE_EQ(Fixture.Tick(), 2.0);

    Fixture.Clock().LatestServerTime = 2.4;
    EXPECT_LT(Fixture.Tick(), 2.1) << "a jump under half a second eases in";
}

TEST(NetMovementInterpSystem, SettledProxyIsNotMarkedForTransformUpdateAgain)
{
    FInterpWorld Fixture;

    Fixture.Tick();
    EXPECT_EQ(Fixture.ProxyLocalLocation(), ProxyLocation);
    ASSERT_TRUE(Fixture.ProxyNeedsTransformUpdate());

    Fixture.Registry().Remove<FNeedsTransformUpdate>(Fixture.Proxy);
    Fixture.Tick();
    EXPECT_FALSE(Fixture.ProxyNeedsTransformUpdate());
}

TEST(NetMovementInterpSystem, AutonomousProxyIsSkipped)
{
    FInterpWorld Fixture(ENetRole::AutonomousProxy);

    Fixture.Tick();
    EXPECT_EQ(Fixture.ProxyLocalLocation(), FVector3(0.0f));
    EXPECT_FALSE(Fixture.ProxyNeedsTransformUpdate());
}
