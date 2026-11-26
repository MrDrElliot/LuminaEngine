#include <gtest/gtest.h>

#include "Config/NetworkSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Systems/NetMovementInterpSystem.h"
#include "Containers/Algorithm.h"
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
        uint32                   MovedConsumer = Constants::kIndexNoneU32;

        explicit FInterpWorld(ENetRole Role = ENetRole::SimulatedProxy)
        {
            RegisterPendingTypes();
            World = TStrongObjectPtr<CWorld>(NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient));
            FWorldTestAccess::SetWorldType(*World, EWorldType::Game);
            Registry().Ctx().Emplace<CWorld*>(World.Get());
            FWorldTestAccess::KeepOnlySystems(*World, { "CNetMovementInterpSystem" });
            Clock().bNetworked = true;

            Proxy = Registry().Create();
            // Bound by hand, since a world whose systems never started has no construct hook to do it.
            Registry().Emplace<STransformComponent>(Proxy).Bind(Registry(), Proxy);
            Registry().Emplace<SNetworkComponent>(Proxy).LocalRole = Role;
            Registry().Emplace<FRepTransform>(Proxy).Ring.Push(0.0, ProxyLocation, FQuat(1.0f, 0.0f, 0.0f, 0.0f));
            MovedConsumer = ECS::Utils::AddMovedTransformConsumer(Registry());
        }

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }
        FNetInterpClock& Clock() const { return Registry().Ctx().Get<FNetInterpClock>(); }

        double Tick(double Dt = FrameDt) const
        {
            FWorldTestAccess::TickStage(*World, EUpdateStage::PostPhysics, Dt);
            return Clock().ServerPlaybackTime;
        }

        FVector3 ProxyLocalLocation() const { return Registry().Get<STransformComponent>(Proxy).GetLocalLocation(); }
        bool ProxyMoved() const
        {
            TVector<ECS::FEntity> Moved;
            ECS::Utils::DrainMovedTransforms(Registry(), MovedConsumer, Moved);
            return Algo::Contains(Moved, Proxy);
        }
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

TEST(NetMovementInterpSystem, SettledProxyIsNotMovedAgain)
{
    FInterpWorld Fixture;

    Fixture.Tick();
    EXPECT_EQ(Fixture.ProxyLocalLocation(), ProxyLocation);
    ASSERT_TRUE(Fixture.ProxyMoved());

    Fixture.Tick();
    EXPECT_FALSE(Fixture.ProxyMoved());
}

TEST(NetMovementInterpSystem, AutonomousProxyIsSkipped)
{
    FInterpWorld Fixture(ENetRole::AutonomousProxy);

    Fixture.Tick();
    EXPECT_EQ(Fixture.ProxyLocalLocation(), FVector3(0.0f));
    EXPECT_FALSE(Fixture.ProxyMoved());
}

TEST(NetMovementInterpSystem, NullFallbackConstantsMatchSettingsDefaults)
{
    TStrongObjectPtr<CNetworkSettings> Settings(NewObject<CNetworkSettings>());

    EXPECT_FLOAT_EQ(Settings->TeleportDistance, CNetworkSettings::kDefaultTeleportDistance);
    EXPECT_FLOAT_EQ(Settings->MaxExtrapolation, CNetworkSettings::kDefaultMaxExtrapolation);
    EXPECT_FLOAT_EQ(Settings->InterpBufferIntervals, CNetworkSettings::kDefaultInterpBufferIntervals);
}
