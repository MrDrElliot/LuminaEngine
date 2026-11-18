#include <gtest/gtest.h>

#include "Components/NetworkComponent.h"
#include "Net/NetWorldState.h"
#include "Networking/NetworkEvents.h"
#include "Systems/NetworkSystem.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/EntityTags.h"

using namespace Lumina;

namespace
{
    constexpr uint32 kClientPeer = 5;
    constexpr uint32 kGuid = NetGUID_DynamicStart + 9;

    // A client that predicted one spawn, and a host that makes it with the key the client sent.
    struct FPredictedSpawnScene
    {
        ECS::FRegistry  Server;
        FNetWorldState  ServerState;
        ECS::FRegistry  Client;
        FNetWorldState* ClientState = nullptr;
        ECS::FEntity    StandIn = ECS::NullEntity;
        uint32          Key = 0;

        explicit FPredictedSpawnScene(bool bKeepStandIn)
        {
            ClientState = &Client.Ctx().Emplace<FNetWorldState>();
            ClientState->LocalPeerId = kClientPeer;
            StandIn = Client.Create();
            Key = NetworkSystemInternal::BeginPredictedSpawn(*ClientState, StandIn, bKeepStandIn, 10.0);
        }

        ECS::FEntity HostSpawns(uint32 PredictedBy, uint32 SentKey)
        {
            const ECS::FEntity Source = Server.Create();
            SNetworkComponent& Net = Server.Emplace<SNetworkComponent>(Source);
            Net.PredictedBy   = PredictedBy;
            Net.PredictionKey = SentKey;

            TVector<uint8> Msg;
            NetworkSystemInternal::WriteSpawnMessage(Server, ServerState, Source, kGuid, 0, FVector3(0.0f), FQuat::Identity(), FVector3(1.0f), Msg);
            NetworkSystemInternal::ApplySpawnEntity(Client, *ClientState, 1, Msg.GetData(), static_cast<SIZE_T>(Msg.Num()));
            return ClientState->GuidTable.Find(SNetGUID{ kGuid });
        }
    };
}

TEST(NetPredictedSpawn, AKeptStandInHidesTheHostCopyAndLeavesWithIt)
{
    FPredictedSpawnScene Scene(true);
    ASSERT_NE(Scene.Key, 0u);

    const ECS::FEntity Copy = Scene.HostSpawns(kClientPeer, Scene.Key);
    ASSERT_NE(Copy, ECS::NullEntity);
    EXPECT_EQ(Scene.Client.Get<ECS::Read<SNetworkComponent>>(Copy).PredictionKey, Scene.Key) << "the key rides in the spawn";
    EXPECT_TRUE(Scene.Client.HasAny<SDisabledTag>(Copy)) << "the stand-in is what this client sees";
    EXPECT_TRUE(Scene.Client.IsValid(Scene.StandIn));
    EXPECT_TRUE(Scene.ClientState->PredictedSpawns.IsEmpty());

    TVector<uint8> Despawn;
    NetworkSystemInternal::WriteDespawnMessage(kGuid, Despawn);
    NetworkSystemInternal::ApplyDespawnEntity(Scene.Client, *Scene.ClientState, Despawn.GetData(), static_cast<SIZE_T>(Despawn.Num()));
    EXPECT_FALSE(Scene.Client.IsValid(Copy));
    EXPECT_FALSE(Scene.Client.IsValid(Scene.StandIn)) << "the host ended the spawn, so the stand-in ends too";
}

TEST(NetPredictedSpawn, AReplacedStandInMakesWayForTheHostCopy)
{
    FPredictedSpawnScene Scene(false);
    const ECS::FEntity Copy = Scene.HostSpawns(kClientPeer, Scene.Key);
    ASSERT_NE(Copy, ECS::NullEntity);
    EXPECT_FALSE(Scene.Client.HasAny<SDisabledTag>(Copy));
    EXPECT_FALSE(Scene.Client.IsValid(Scene.StandIn));
}

TEST(NetPredictedSpawn, AnotherClientsKeyIsLeftAlone)
{
    FPredictedSpawnScene Scene(true);
    const ECS::FEntity Copy = Scene.HostSpawns(kClientPeer + 1, Scene.Key);
    ASSERT_NE(Copy, ECS::NullEntity);
    EXPECT_FALSE(Scene.Client.HasAny<SDisabledTag>(Copy));
    EXPECT_TRUE(Scene.Client.IsValid(Scene.StandIn));
    EXPECT_EQ(Scene.ClientState->PredictedSpawns.Num(), 1) << "still waiting on its own spawn";
}

TEST(NetPredictedSpawn, ARefusedSpawnDestroysTheStandInWhenTheWaitRunsOut)
{
    FPredictedSpawnScene Scene(true);
    NetworkSystemInternal::ExpirePredictedSpawns(Scene.Client, *Scene.ClientState, 9.0);
    EXPECT_TRUE(Scene.Client.IsValid(Scene.StandIn)) << "not yet due";

    NetworkSystemInternal::ExpirePredictedSpawns(Scene.Client, *Scene.ClientState, 10.5);
    EXPECT_FALSE(Scene.Client.IsValid(Scene.StandIn));
    EXPECT_TRUE(Scene.ClientState->PredictedSpawns.IsEmpty());
}
