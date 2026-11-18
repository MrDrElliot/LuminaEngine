#include <gtest/gtest.h>
#include "Core/Object/ObjectCore.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/ProjectileComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"
#include "WorldTestAccess.h"

using namespace Lumina;

// A client flies a host projectile only its spawn replicated, and leaves alone one whose transform the host streams.
TEST(ProjectileNetProxy, ClientsFlySpawnOnlyMirrorsAndLeaveStreamedOnesAlone)
{
    RegisterPendingTypes();
    TStrongObjectPtr<CWorld> World(NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient));
    FWorldTestAccess::SetWorldType(*World, EWorldType::Game);
    FWorldTestAccess::KeepOnlySystems(*World, { FName("CProjectileSystem") });
    ASSERT_EQ(World->Systems.Num(), 1);

    ECS::FRegistry& Registry = World->GetEntityRegistry();
    auto Spawn = [&](bool bProxy, bool bStreamed)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<STransformComponent>(Entity);
        Registry.Emplace<SProjectileComponent>(Entity).Velocity = FVector3(10.0f, 0.0f, 0.0f);
        if (bProxy)
        {
            Registry.Emplace<SNetProxyTag>(Entity);
        }
        if (bStreamed)
        {
            Registry.Emplace<SNetDrivenTransformTag>(Entity);
        }
        return Entity;
    };
    const ECS::FEntity Owned    = Spawn(false, false);
    const ECS::FEntity Mirror   = Spawn(true, false);
    const ECS::FEntity Streamed = Spawn(true, true);

    FWorldTestAccess::TickStage(*World, EUpdateStage::PrePhysics, 0.1);

    auto X = [&](ECS::FEntity Entity) { return Registry.Get<ECS::Read<STransformComponent>>(Entity).GetLocalLocation().x; };
    EXPECT_NEAR(X(Owned), 1.0f, 1e-4f);
    EXPECT_NEAR(X(Mirror), 1.0f, 1e-4f) << "the mirror flies the same path the host does";
    EXPECT_NEAR(X(Streamed), 0.0f, 1e-4f) << "the host's snapshots move this one";
}
