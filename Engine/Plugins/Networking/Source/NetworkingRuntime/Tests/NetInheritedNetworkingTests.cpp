#include <gtest/gtest.h>

#include "Components/NetworkComponent.h"
#include "Net/NetWorldState.h"
#include "Systems/NetworkSystem.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

namespace
{
    ECS::FEntity MakeChild(ECS::FRegistry& Registry, ECS::FEntity Parent)
    {
        const ECS::FEntity Child = Registry.Create();
        Registry.Emplace<STransformComponent>(Child);
        Registry.AttachChild(Child, Parent);
        return Child;
    }
}

// Only the root of a spawned prefab carries a network component, and its whole hierarchy still replicates.
TEST(NetInheritedNetworking, ASpawnedHierarchyReplicatesFromItsRoot)
{
    ECS::FRegistry Server;
    FNetWorldState ServerState;

    const ECS::FEntity Root = Server.Create();
    Server.Emplace<STransformComponent>(Root);
    Server.Emplace<SNetworkComponent>(Root).bAlwaysRelevant = true;
    const ECS::FEntity Staff = MakeChild(Server, Root);
    const ECS::FEntity Orb   = MakeChild(Server, Staff);

    // A child with its own settings keeps them, and they reach its own children.
    const ECS::FEntity Cape = MakeChild(Server, Root);
    Server.Emplace<SNetworkComponent>(Cape).bReplicatesMovement = false;
    const ECS::FEntity Clasp = MakeChild(Server, Cape);

    NetworkSystemInternal::MaintainDynamicLifetime(Server, ServerState);

    for (const ECS::FEntity Entity : { Root, Staff, Orb, Cape, Clasp })
    {
        const SNetworkComponent* Net = Server.TryGet<ECS::Read<SNetworkComponent>>(Entity);
        ASSERT_NE(Net, nullptr);
        EXPECT_NE(Net->NetGUID.Value, 0u) << "every entity spawns in the same tick as the root";
    }
    const SNetworkComponent& OrbNet = Server.Get<ECS::Read<SNetworkComponent>>(Orb);
    EXPECT_TRUE(OrbNet.bInherited);
    EXPECT_TRUE(OrbNet.bAlwaysRelevant) << "copied from the root through the staff";
    EXPECT_FALSE(Server.Get<ECS::Read<SNetworkComponent>>(Cape).bInherited);
    EXPECT_FALSE(Server.Get<ECS::Read<SNetworkComponent>>(Clasp).bReplicatesMovement) << "inherited from the cape, not the root";

    // The client builds the same tree from the spawns, children attached under their parents.
    ECS::FRegistry Client;
    FNetWorldState& ClientState = Client.Ctx().Emplace<FNetWorldState>();
    for (const ECS::FEntity Entity : { Root, Staff, Orb })
    {
        TVector<uint8> Msg;
        const uint32 Guid = Server.Get<ECS::Read<SNetworkComponent>>(Entity).NetGUID.Value;
        NetworkSystemInternal::WriteSpawnMessage(Server, ServerState, Entity, Guid, 0, FVector3(0.0f), FQuat::Identity(), FVector3(1.0f), Msg);
        NetworkSystemInternal::ApplySpawnEntity(Client, ClientState, 1, Msg.GetData(), static_cast<SIZE_T>(Msg.Num()));
    }
    auto OnClient = [&](ECS::FEntity Entity) { return ClientState.GuidTable.Find(Server.Get<ECS::Read<SNetworkComponent>>(Entity).NetGUID); };
    ASSERT_NE(OnClient(Orb), ECS::NullEntity);
    EXPECT_EQ(Client.GetHierarchy().GetParent(OnClient(Orb)), OnClient(Staff));
    EXPECT_EQ(Client.GetHierarchy().GetParent(OnClient(Staff)), OnClient(Root));
}

// A child inherits its parent's owner, and inheriting again changes nothing.
TEST(NetInheritedNetworking, InheritingCopiesTheOwnerOnce)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Root = Registry.Create();
    Registry.Emplace<SNetworkComponent>(Root).OwningConnectionId = 4;
    const ECS::FEntity Child = MakeChild(Registry, Root);

    TVector<ECS::FEntity> Added;
    NetworkSystemInternal::InheritNetworking(Registry, Root, &Added);
    ASSERT_EQ(Added.Num(), 1);
    EXPECT_EQ(Added[0], Child);
    EXPECT_EQ(Registry.Get<ECS::Read<SNetworkComponent>>(Child).OwningConnectionId, 4u) << "the owner's children are the owner's";

    Added.Reset();
    NetworkSystemInternal::InheritNetworking(Registry, Root, &Added);
    EXPECT_TRUE(Added.IsEmpty()) << "inheriting again changes nothing";
}
