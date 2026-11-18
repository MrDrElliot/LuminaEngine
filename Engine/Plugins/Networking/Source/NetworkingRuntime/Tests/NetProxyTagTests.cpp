#include <gtest/gtest.h>

#include "Components/NetworkComponent.h"
#include "Net/NetWorldState.h"
#include "Systems/NetworkSystem.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/EntityTags.h"

using namespace Lumina;

// A client tags what another peer drives, so local projectiles and movers leave its mirror alone, and drops the tag once it owns the entity.
TEST(NetProxyTag, FollowsTheLocalRole)
{
    ECS::FRegistry Registry;
    FNetWorldState State;
    State.LocalPeerId = 5;

    const ECS::FEntity Fireball = Registry.Create();
    Registry.Emplace<SNetworkComponent>(Fireball).OwningConnectionId = 0;

    NetworkSystemInternal::RefreshNetRoles(Registry, State, false);
    EXPECT_EQ(Registry.Get<SNetworkComponent>(Fireball).LocalRole, ENetRole::SimulatedProxy);
    EXPECT_TRUE(Registry.HasAny<SNetProxyTag>(Fireball)) << "the host drives it";
    EXPECT_TRUE(Registry.HasAny<SNetDrivenTransformTag>(Fireball)) << "and streams where it is";

    Registry.Get<SNetworkComponent>(Fireball).bReplicatesMovement = false;
    NetworkSystemInternal::RefreshNetRoles(Registry, State, false);
    EXPECT_TRUE(Registry.HasAny<SNetProxyTag>(Fireball));
    EXPECT_FALSE(Registry.HasAny<SNetDrivenTransformTag>(Fireball)) << "only its spawn replicated, so this client flies it";

    Registry.Get<SNetworkComponent>(Fireball).OwningConnectionId = 5;
    NetworkSystemInternal::RefreshNetRoles(Registry, State, false);
    EXPECT_EQ(Registry.Get<SNetworkComponent>(Fireball).LocalRole, ENetRole::AutonomousProxy);
    EXPECT_FALSE(Registry.HasAny<SNetProxyTag>(Fireball)) << "this client moves what it owns";
    EXPECT_FALSE(Registry.HasAny<SNetDrivenTransformTag>(Fireball));

    NetworkSystemInternal::RefreshNetRoles(Registry, State, true);
    EXPECT_FALSE(Registry.HasAny<SNetProxyTag>(Fireball)) << "the host mirrors nothing";
}
