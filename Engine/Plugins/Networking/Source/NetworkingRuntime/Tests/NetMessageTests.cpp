#include <gtest/gtest.h>

#include "Components/NetworkComponent.h"
#include "Core/Serialization/NetArchive.h"
#include "Memory/Memcpy.h"
#include "Net/NetMessages.h"
#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"
#include "SaveGame/SaveGameEvents.h"
#include "Systems/NetworkSystem.h"
#include "World/ECS/EventQueue.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

namespace
{
    // Laid out the way a C# struct with a Connection Sender, an FEntity and a uint is.
    struct FScriptPing
    {
        uint32 Sender  = 0;
        uint32 Target  = 0;
        uint32 Payload = 0;
    };

    constexpr uint32 kSenderOffset = offsetof(FScriptPing, Sender);
    constexpr uint32 kTargetOffset = offsetof(FScriptPing, Target);
    constexpr uint32 kClientConnection = 5u;

    uint32 GuidOf(const ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        return Registry.Get<ECS::Read<SNetworkComponent>>(Entity).NetGUID.Value;
    }

    TVector<FScriptPing> ReadPings(ECS::FEventQueues& Events, ECS::FComponentTypeID TypeID)
    {
        Events.Commit();
        uint64 Cursor = 0;
        const ECS::FEventRuns Runs = Events.Get(TypeID).Read(Cursor);
        TVector<FScriptPing> Out;
        for (const ECS::FEventRun& Run : { Runs.First, Runs.Second })
        {
            for (uint32 Index = 0; Index < Run.Count; ++Index)
            {
                FScriptPing Ping;
                Memory::Memcpy(&Ping, static_cast<const uint8*>(Run.Data) + Index * Runs.Stride, sizeof(FScriptPing));
                Out.Add(Ping);
            }
        }
        return Out;
    }
}

// A pure ECS game writes its replicated components like any others, and the host still finds what to resend.
TEST(NetReplicatedChanges, AWriteToAReplicatedComponentMarksItsEntityDirty)
{
    ECS::FRegistry Server;
    FNetWorldState State;
    const ECS::FEntity Networked = Server.Create();
    Server.Emplace<SNameComponent>(Networked);
    Server.Emplace<SNetworkComponent>(Networked);
    const ECS::FEntity LocalOnly = Server.Create();
    Server.Emplace<SNameComponent>(LocalOnly);

    NetworkSystemInternal::MarkReplicatedChanges(Server, State);
    EXPECT_TRUE(Server.HasAll<FNetDirty>(Networked)) << "the first scan sends what the pool already held";
    EXPECT_FALSE(Server.HasAll<FNetDirty>(LocalOnly)) << "an entity without SNetworkComponent never replicates";

    Server.Remove<FNetDirty>(Networked);
    NetworkSystemInternal::MarkReplicatedChanges(Server, State);
    EXPECT_FALSE(Server.HasAll<FNetDirty>(Networked)) << "nothing was written since the last scan";

    Server.Patch<SNameComponent>(Networked);
    NetworkSystemInternal::MarkReplicatedChanges(Server, State);
    EXPECT_TRUE(Server.HasAll<FNetDirty>(Networked));
}

// A C# message crosses as raw bytes, so its sender is rewritten and its entity travels as a net GUID.
TEST(NetMessages, AScriptMessageArrivesWithItsSenderAndTheReceiversEntity)
{
    const ECS::FComponentTypeID TypeID = ECS::AcquireScriptEventType(FName("cs:NetMessageTests.Ping"), sizeof(FScriptPing), alignof(FScriptPing));
    ASSERT_NE(TypeID, ECS::InvalidComponentTypeID);
    FScriptNetMessages Scripts;
    const uint32 EntityOffsets[] = { kTargetOffset };
    Scripts.Register(TypeID, kSenderOffset, TSpan<const uint32>(EntityOffsets, 1));

    ECS::FRegistry Server;
    FNetWorldState ServerState;
    ServerState.Registry = &Server;
    const ECS::FEntity Crate = Server.Create();
    Server.Emplace<STransformComponent>(Crate);
    Server.Emplace<SNetworkComponent>(Crate);
    NetworkSystemInternal::MaintainDynamicLifetime(Server, ServerState);

    ECS::FRegistry Client;
    FNetWorldState& ClientState = Client.Ctx().Emplace<FNetWorldState>();
    {
        TVector<uint8> Spawn;
        NetworkSystemInternal::WriteSpawnMessage(Server, ServerState, Crate, GuidOf(Server, Crate), 0, FVector3(0.0f), FQuat::Identity(), FVector3(1.0f), Spawn);
        NetworkSystemInternal::ApplySpawnEntity(Client, ClientState, 1, Spawn.GetData(), static_cast<SIZE_T>(Spawn.Num()));
    }
    const ECS::FEntity ClientCrate = ClientState.GuidTable.Find(SNetGUID{ GuidOf(Server, Crate) });
    ASSERT_NE(ClientCrate, ECS::NullEntity);

    const FScriptPing Sent{ .Sender = 99u, .Target = static_cast<uint32>(Crate.Value), .Payload = 0xC0FFEEu };
    TVector<uint8> Msg;
    ASSERT_TRUE(NetMessages::Write(ServerState, Scripts, TypeID, &Sent, Msg));

    ECS::FEventQueues Events(Client);
    ClientState.Events = &Events;
    NetMessages::Apply(ClientState, Scripts, kClientConnection, Msg.GetData(), static_cast<SIZE_T>(Msg.Num()));

    const TVector<FScriptPing> Arrived = ReadPings(Events, TypeID);
    ASSERT_EQ(Arrived.Num(), 1);
    EXPECT_EQ(Arrived[0].Sender, kClientConnection) << "the claimed sender is never trusted";
    EXPECT_EQ(Arrived[0].Target, static_cast<uint32>(ClientCrate.Value));
    EXPECT_EQ(Arrived[0].Payload, 0xC0FFEEu);
}

// A peer could name any event type by its hash, so only real messages may be raised on arrival.
TEST(NetMessages, AnEngineEventIsNeitherSentNorRaised)
{
    const ECS::FComponentTypeID EngineEvent = ECS::GetComponentTypeID<SSaveGameRequestEvent>();
    FScriptNetMessages Scripts;
    FNetWorldState State;
    SSaveGameRequestEvent Request;
    TVector<uint8> Msg;
    EXPECT_FALSE(NetMessages::Write(State, Scripts, EngineEvent, &Request, Msg));

    TVector<uint8> Forged;
    FNetArchive Writer(Forged);
    uint8 Type = static_cast<uint8>(ENetMessage::EventMessage);
    Writer << Type;
    uint32 Hash = static_cast<uint32>(ECS::FComponentTypeRegistry::Get().GetInfo(EngineEvent).Name.GetStableHash());
    Writer.SerializeBits(&Hash, 32);

    ECS::FRegistry Registry;
    ECS::FEventQueues Events(Registry);
    State.Events = &Events;
    NetMessages::Apply(State, Scripts, kClientConnection, Forged.GetData(), static_cast<SIZE_T>(Forged.Num()));
    Events.Commit();
    uint64 Cursor = 0;
    EXPECT_EQ(Events.Get(EngineEvent).Read(Cursor).Num(), 0u);
}
