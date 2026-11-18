#include <gtest/gtest.h>

#include "Components/NetworkComponent.h"
#include "Net/NetPropertyDelivery.h"
#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"
#include "Net/ScriptRepState.h"
#include "Systems/NetworkSystem.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/ProjectileComponent.h"

using namespace Lumina;

namespace
{
    constexpr uint32 kGuid = NetGUID_DynamicStart + 5;

    // One host entity already spawned on one client, and the client's side of the property stream.
    struct FPropertyScene
    {
        ECS::FRegistry       Server;
        FNetWorldState       ServerState;
        ECS::FEntity         Source = ECS::NullEntity;
        ECS::FRegistry       Client;
        FNetWorldState*      ClientState = nullptr;
        FNetPropertyDelivery Delivery;

        FPropertyScene()
        {
            Source = Server.Create();
            Server.Emplace<SNetworkComponent>(Source).NetGUID = SNetGUID{ kGuid };
            Server.Emplace<SProjectileComponent>(Source);

            ClientState = &Client.Ctx().Emplace<FNetWorldState>();
            TVector<uint8> Spawn;
            NetworkSystemInternal::WriteSpawnMessage(Server, ServerState, Source, kGuid, 0, FVector3(0.0f), FQuat::Identity(), FVector3(1.0f), Spawn);
            NetworkSystemInternal::ApplySpawnEntity(Client, *ClientState, 1, Spawn.GetData(), static_cast<SIZE_T>(Spawn.Num()));
        }

        ECS::FEntity Mirror() const { return ClientState->GuidTable.Find(SNetGUID{ kGuid }); }

        // The host's current state of its entity, packed as the next property packet.
        TVector<uint8> NextPacket(uint32 Guid = kGuid)
        {
            TVector<uint8> Msg;
            NetworkSystemInternal::WritePropertyResync(Server, ServerState, Source, Guid, Msg);
            TVector<uint8> Framed;
            Net::AppendFramedMessage(Framed, Msg.GetData(), static_cast<SIZE_T>(Msg.Num()));
            NetProperties::Queue(Delivery, Guid, Framed.GetData(), static_cast<SIZE_T>(Framed.Num()));

            TVector<TVector<uint8>> Packets;
            int32 Count = 0;
            NetProperties::Pack(Delivery, 0.0, Packets, Count);
            return Count == 1 ? Packets[0] : TVector<uint8>();
        }

        void Deliver(const TVector<uint8>& Packet)
        {
            Net::ForEachFramedMessage(Packet.GetData(), static_cast<SIZE_T>(Packet.Num()), [&](const uint8* Msg, SIZE_T Size)
            {
                NetworkSystemInternal::ApplyPropertyPacket(Client, *ClientState, 1, Msg, Size);
            });
        }

        float ClientFrequency() const { return Client.Get<ECS::Read<SNetworkComponent>>(Mirror()).NetUpdateFrequency; }
    };
}

TEST(NetPropertyDelivery, AcksReportTheNewestAndTheGapsBeforeIt)
{
    FNetPropertyAcks Acks;
    for (uint32 Seq : { 1u, 2u, 4u, 5u })
    {
        NetProperties::NoteReceived(Acks, Seq);
    }
    EXPECT_EQ(Acks.Latest, 5u);
    EXPECT_EQ(Acks.Bits, 0b1101u) << "4 and 2 and 1 arrived, 3 did not";

    NetProperties::NoteReceived(Acks, 40);
    EXPECT_EQ(Acks.Bits, 0u) << "35 back is past the window";
}

TEST(NetPropertyDelivery, PacketsStayUnderADatagramAndRememberWhatTheyCarried)
{
    FNetPropertyDelivery Delivery;
    TVector<uint8> Message;
    Message.SetNum(400, 7);
    TVector<uint8> Framed;
    Net::AppendFramedMessage(Framed, Message.GetData(), static_cast<SIZE_T>(Message.Num()));
    for (uint32 Guid = 1; Guid <= 5; ++Guid)
    {
        NetProperties::Queue(Delivery, Guid, Framed.GetData(), static_cast<SIZE_T>(Framed.Num()));
    }

    TVector<TVector<uint8>> Packets;
    int32 Count = 0;
    NetProperties::Pack(Delivery, 0.0, Packets, Count);
    ASSERT_EQ(Count, 3) << "two 400 byte messages fit a packet, a third does not";
    for (int32 Index = 0; Index < Count; ++Index)
    {
        EXPECT_LE(Packets[Index].Num(), NetProperties::kPacketBudgetBytes + 2);
    }
    ASSERT_EQ(Delivery.InFlight.Num(), 3);
    EXPECT_EQ(Delivery.InFlight[2].Guids.Num(), 1);
    EXPECT_EQ(Delivery.InFlight[2].Guids[0], 5u);

    // The client got 1 and 3, so 2 is lost for good on a sequenced channel and its entities go again.
    EXPECT_EQ(NetProperties::ApplyAck(Delivery, 3, 0b10u), 1u);
    EXPECT_TRUE(Delivery.InFlight.IsEmpty());
    EXPECT_EQ(Delivery.Resends.Num(), 2);
    EXPECT_EQ(Delivery.Resends[0], 3u);
}

TEST(NetPropertyDelivery, APacketNoLaterOneRevealsIsGivenUpOnAfterTheTimeout)
{
    FNetPropertyDelivery Delivery;
    FNetPropertyPacket& Packet = Delivery.InFlight.EmplaceGetRef();
    Packet.Seq = 1;
    Packet.SentAt = 10.0;
    Packet.Guids.Add(42);

    EXPECT_EQ(NetProperties::Expire(Delivery, 10.5, 1.0), 0u);
    EXPECT_EQ(NetProperties::Expire(Delivery, 11.5, 1.0), 1u);
    ASSERT_EQ(Delivery.Resends.Num(), 1);
    EXPECT_EQ(Delivery.Resends[0], 42u);
}

// A lost packet is followed by the entity's current state, not the old bytes, and the client acknowledges what landed.
TEST(NetPropertyDelivery, ALostPacketIsRepairedWithTheCurrentState)
{
    FPropertyScene Scene;
    ASSERT_NE(Scene.Mirror(), ECS::NullEntity);

    Scene.Server.Get<SNetworkComponent>(Scene.Source).NetUpdateFrequency = 20.0f;
    Scene.NextPacket();
    Scene.Server.Get<SNetworkComponent>(Scene.Source).NetUpdateFrequency = 30.0f;
    Scene.Deliver(Scene.NextPacket());

    EXPECT_FLOAT_EQ(Scene.ClientFrequency(), 30.0f);
    EXPECT_EQ(Scene.ClientState->PropertyAcks.Latest, 2u);
    EXPECT_EQ(Scene.ClientState->PropertyAcks.Bits, 0u) << "packet 1 never came";
    EXPECT_EQ(NetProperties::ApplyAck(Scene.Delivery, Scene.ClientState->PropertyAcks.Latest, Scene.ClientState->PropertyAcks.Bits), 1u);
    ASSERT_EQ(Scene.Delivery.Resends.Num(), 1);

    // The host has moved on, so the resend carries what is true now.
    Scene.Server.Get<SNetworkComponent>(Scene.Source).NetUpdateFrequency = 45.0f;
    Scene.Deliver(Scene.NextPacket());
    EXPECT_FLOAT_EQ(Scene.ClientFrequency(), 45.0f);
}

TEST(NetPropertyDelivery, AResendRemovesComponentsTheEntityLost)
{
    FPropertyScene Scene;
    ASSERT_TRUE(Scene.Client.HasAll<SProjectileComponent>(Scene.Mirror()));

    // The delta that carried the removal was lost, so only the resend can say it.
    Scene.Server.Remove<SProjectileComponent>(Scene.Source);
    uint32 WireIndex = 0;
    for (int32 Index = 0; Index < Scene.ServerState.ReplTypes.ByIndex.Num(); ++Index)
    {
        WireIndex = Scene.ServerState.ReplTypes.ByIndex[Index].Struct == SProjectileComponent::StaticStruct() ? static_cast<uint32>(Index) : WireIndex;
    }
    Scene.Server.Emplace<FComponentRepState>(Scene.Source).RemovedWireIndices.Add(WireIndex);

    Scene.Deliver(Scene.NextPacket());
    EXPECT_FALSE(Scene.Client.HasAll<SProjectileComponent>(Scene.Mirror()));
}

TEST(NetPropertyDelivery, AnUpdateForAnEntityNotYetSpawnedIsLeftUnacknowledged)
{
    FPropertyScene Scene;
    Scene.Deliver(Scene.NextPacket(kGuid + 1));
    EXPECT_EQ(Scene.ClientState->PropertyAcks.Latest, 0u) << "the host sends it again once the spawn is here";

    Scene.Deliver(Scene.NextPacket());
    EXPECT_EQ(Scene.ClientState->PropertyAcks.Latest, 2u);
}

TEST(NetPropertyDelivery, PacketsOlderThanASpawnFenceAreIgnored)
{
    FPropertyScene Scene;
    Scene.Server.Get<SNetworkComponent>(Scene.Source).NetUpdateFrequency = 12.0f;
    const TVector<uint8> Stale = Scene.NextPacket();
    Scene.ClientState->PropertyAcks.Floor = 2;

    Scene.Deliver(Stale);
    EXPECT_EQ(Scene.ClientState->PropertyAcks.Latest, 0u);
    EXPECT_FLOAT_EQ(Scene.ClientFrequency(), 60.0f) << "the spawn's state stands";
}
