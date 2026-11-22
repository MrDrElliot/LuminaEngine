#include <gtest/gtest.h>

#include "Components/NetworkComponent.h"
#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"
#include "Systems/NetworkSystem.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

namespace
{
    constexpr uint32 kClientConnection = 7u;

    uint32 GuidOf(const ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        return Registry.Get<ECS::Read<SNetworkComponent>>(Entity).NetGUID.Value;
    }
}

// A body left to ragdoll stops replicating, and the host and the client each keep their own copy rather than despawning it.
TEST(NetTearOff, ATornOffHierarchyLivesOnLocallyOnEveryPeer)
{
    ECS::FRegistry Server;
    FNetWorldState ServerState;

    const ECS::FEntity Body = Server.Create();
    Server.Emplace<STransformComponent>(Body);
    Server.Emplace<SNetworkComponent>(Body);
    const ECS::FEntity Weapon = Server.Create();
    Server.Emplace<STransformComponent>(Weapon);
    Server.AttachChild(Weapon, Body);
    NetworkSystemInternal::MaintainDynamicLifetime(Server, ServerState);

    const uint32 BodyGuid   = GuidOf(Server, Body);
    const uint32 WeaponGuid = GuidOf(Server, Weapon);
    ASSERT_NE(BodyGuid, 0u);
    ASSERT_NE(WeaponGuid, 0u);

    ECS::FRegistry Client;
    FNetWorldState& ClientState = Client.Ctx().Emplace<FNetWorldState>();
    FNetClientView& View = ServerState.ClientViews[kClientConnection];
    for (const ECS::FEntity Entity : { Body, Weapon })
    {
        TVector<uint8> Msg;
        NetworkSystemInternal::WriteSpawnMessage(Server, ServerState, Entity, GuidOf(Server, Entity), 0, FVector3(0.0f), FQuat::Identity(), FVector3(1.0f), Msg);
        NetworkSystemInternal::ApplySpawnEntity(Client, ClientState, 1, Msg.GetData(), static_cast<SIZE_T>(Msg.Num()));
        View.Relevant[GuidOf(Server, Entity)] = FRelevantEntry{ .bDynamic = true };
    }
    const ECS::FEntity ClientBody   = ClientState.GuidTable.Find(SNetGUID{ BodyGuid });
    const ECS::FEntity ClientWeapon = ClientState.GuidTable.Find(SNetGUID{ WeaponGuid });
    ASSERT_NE(ClientBody, ECS::NullEntity);
    ASSERT_NE(ClientWeapon, ECS::NullEntity);

    ServerState.PendingTearOffs.Add(Body);
    NetworkSystemInternal::ProcessTearOffs(nullptr, Server, ServerState);

    for (const ECS::FEntity Entity : { Body, Weapon })
    {
        EXPECT_TRUE(Server.IsValid(Entity)) << "the host keeps its copy";
        EXPECT_FALSE(Server.HasAll<SNetworkComponent>(Entity));
    }
    EXPECT_EQ(ServerState.GuidTable.Find(SNetGUID{ BodyGuid }), ECS::NullEntity);
    EXPECT_TRUE(View.Relevant.IsEmpty()) << "the client view forgets them without owing a despawn";
    ASSERT_FALSE(View.PendingReliable.IsEmpty());

    int32 Messages = 0;
    Net::ForEachFramedMessage(View.PendingReliable.GetData(), static_cast<SIZE_T>(View.PendingReliable.Num()), [&](const uint8* Msg, SIZE_T Size)
    {
        ++Messages;
        NetworkSystemInternal::ApplyTearOff(Client, nullptr, ClientState, Msg, Size);
    });
    EXPECT_EQ(Messages, 2) << "one for the body and one for what it carried";

    for (const ECS::FEntity Entity : { ClientBody, ClientWeapon })
    {
        EXPECT_TRUE(Client.IsValid(Entity)) << "the client keeps simulating its copy";
        EXPECT_FALSE(Client.HasAll<SNetworkComponent>(Entity));
    }
    EXPECT_EQ(ClientState.GuidTable.Find(SNetGUID{ BodyGuid }), ECS::NullEntity);
    EXPECT_EQ(Client.GetHierarchy().GetParent(ClientWeapon), ClientBody) << "the hierarchy survives the tear-off";
}

// The step runs every host tick, so without a request it must leave every networked entity as it was.
TEST(NetTearOff, NothingPendingTouchesNothing)
{
    ECS::FRegistry Server;
    FNetWorldState ServerState;
    const ECS::FEntity Crate = Server.Create();
    Server.Emplace<SNetworkComponent>(Crate);
    NetworkSystemInternal::MaintainDynamicLifetime(Server, ServerState);

    NetworkSystemInternal::ProcessTearOffs(nullptr, Server, ServerState);
    EXPECT_TRUE(Server.HasAll<SNetworkComponent>(Crate));
    EXPECT_NE(ServerState.GuidTable.Find(SNetGUID{ GuidOf(Server, Crate) }), ECS::NullEntity);
}
