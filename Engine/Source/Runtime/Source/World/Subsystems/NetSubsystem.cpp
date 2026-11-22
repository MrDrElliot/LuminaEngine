#include "RuntimePCH.h"
#include "World/Subsystems/NetSubsystem.h"
#include "World/World.h"

#include "Networking/INetworkRuntime.h"
#include "Scripting/ScriptNet.h"

namespace Lumina
{
    void CNetSubsystem::DispatchSessionEvents()
    {
        CWorld* World = GetWorld();
        if (World == nullptr)
        {
            return;
        }

        // Read every frame, even with nothing bound, so a handler bound later never sees a stale join.
        for (const SNetSessionEvent& Event : ECS::ReadEvents(World->GetEvents(), SessionEvents))
        {
            switch (Event.Kind)
            {
                case ENetSessionEventKind::ClientJoined: OnClientJoined.Broadcast(Event.ConnectionId); break;
                case ENetSessionEventKind::ClientLeft:   OnClientLeft.Broadcast(Event.ConnectionId);   break;
                case ENetSessionEventKind::JoinedHost:   OnJoinedHost.Broadcast();                     break;
                case ENetSessionEventKind::LeftHost:     OnLeftHost.Broadcast(Event.Reason);           break;
            }
        }
    }

    bool CNetSubsystem::IsHost()
    {
        CWorld* World = GetWorld();
        return World == nullptr || World->GetNetMode() != ENetMode::Client;
    }

    bool CNetSubsystem::IsOwner(ECS::FEntity Entity)
    {
        return !IsEntityNetworked(Entity) || GetOwner(Entity) == GetLocalConnection();
    }

    bool CNetSubsystem::IsProxy(ECS::FEntity Entity)
    {
        return !IsOwner(Entity);
    }

    uint32 CNetSubsystem::GetRpcCaller()
    {
        return ScriptNet::IsReceiving() ? ScriptNet::ReceivingCaller() : GetLocalConnection();
    }

    uint32 CNetSubsystem::GetLocalConnection()
    {
        CWorld* World = GetWorld();
        INetworkRuntime* Net = GetNetworkRuntime();
        return Net != nullptr && World != nullptr ? Net->GetLocalConnectionId(World) : 0u;
    }

    void CNetSubsystem::GetConnections(TVector<uint32>& OutConnections)
    {
        CWorld* World = GetWorld();
        OutConnections.Reset();
        if (INetworkRuntime* Net = GetNetworkRuntime(); Net != nullptr && World != nullptr)
        {
            Net->GetConnectionIds(World, OutConnections);
        }
    }

    bool CNetSubsystem::IsEntityNetworked(ECS::FEntity Entity)
    {
        CWorld* World = GetWorld();
        INetworkRuntime* Net = GetNetworkRuntime();
        return Net != nullptr && World != nullptr && Net->IsEntityNetworked(World, Entity);
    }

    uint32 CNetSubsystem::GetOwner(ECS::FEntity Entity)
    {
        CWorld* World = GetWorld();
        INetworkRuntime* Net = GetNetworkRuntime();
        return Net != nullptr && World != nullptr ? Net->GetEntityOwner(World, Entity) : 0u;
    }

    bool CNetSubsystem::SetOwner(ECS::FEntity Entity, uint32 ConnectionId)
    {
        CWorld* World = GetWorld();
        INetworkRuntime* Net = GetNetworkRuntime();
        return Net != nullptr && World != nullptr && Net->SetEntityOwner(World, Entity, ConnectionId);
    }

    bool CNetSubsystem::SendScriptMessage(uint32 Target, uint32 Connection, uint32 TypeId, void* Message)
    {
        return Target <= static_cast<uint32>(ENetMessageTarget::Everyone)
            && SendMessageOfType(static_cast<ENetMessageTarget>(Target), Connection, static_cast<ECS::FComponentTypeID>(TypeId), Message);
    }

    void CNetSubsystem::RegisterScriptMessage(uint32 TypeId, uint32 SenderOffset, void* EntityOffsets, uint32 Count)
    {
        if (INetworkRuntime* Net = GetNetworkRuntime())
        {
            Net->RegisterScriptNetMessage(static_cast<ECS::FComponentTypeID>(TypeId), SenderOffset, TSpan<const uint32>(static_cast<const uint32*>(EntityOffsets), Count));
        }
    }

    bool CNetSubsystem::SendMessageOfType(ENetMessageTarget Target, uint32 Connection, ECS::FComponentTypeID TypeID, const void* Message)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || Message == nullptr)
        {
            return false;
        }
        if (INetworkRuntime* Net = GetNetworkRuntime())
        {
            return Net->SendNetMessage(World, Target, Connection, TypeID, Message);
        }
        // With no netcode loaded the world is its own host, so a message for the host or everyone simply lands here.
        const bool bHere = Target == ENetMessageTarget::Host || Target == ENetMessageTarget::Everyone || (Target == ENetMessageTarget::Client && Connection == 0);
        if (bHere)
        {
            World->GetEventQueues().Get(TypeID).Send(Message);
        }
        return bHere;
    }

    void CNetSubsystem::TearOff(ECS::FEntity Entity)
    {
        CWorld* World = GetWorld();
        if (INetworkRuntime* Net = GetNetworkRuntime(); Net != nullptr && World != nullptr)
        {
            Net->TearOff(World, Entity);
        }
    }

    void CNetSubsystem::MarkEntityDirty(ECS::FEntity Entity)
    {
        CWorld* World = GetWorld();
        if (INetworkRuntime* Net = GetNetworkRuntime(); Net != nullptr && World != nullptr)
        {
            Net->MarkEntityDirty(World, Entity);
        }
    }

    uint32 CNetSubsystem::EntityToNetId(ECS::FEntity Entity)
    {
        CWorld* World = GetWorld();
        INetworkRuntime* Net = GetNetworkRuntime();
        return Net != nullptr && World != nullptr ? Net->EntityToNetId(World, Entity) : 0u;
    }

    ECS::FEntity CNetSubsystem::NetIdToEntity(uint32 NetId)
    {
        CWorld* World = GetWorld();
        INetworkRuntime* Net = GetNetworkRuntime();
        return Net != nullptr && World != nullptr ? Net->NetIdToEntity(World, NetId) : ECS::NullEntity;
    }

    bool CNetSubsystem::IsJoined()
    {
        CWorld* World = GetWorld();
        INetworkRuntime* Net = GetNetworkRuntime();
        return Net != nullptr && World != nullptr && Net->IsJoined(World);
    }

    ECS::FEntity CNetSubsystem::GetOwnedPawn(uint32 ConnectionId)
    {
        CWorld* World = GetWorld();
        INetworkRuntime* Net = GetNetworkRuntime();
        return Net != nullptr && World != nullptr ? Net->FindOwnedPawn(World, ConnectionId) : ECS::NullEntity;
    }

    uint32 CNetSubsystem::PredictSpawn(ECS::FEntity StandIn, bool bKeepStandIn)
    {
        CWorld* World = GetWorld();
        INetworkRuntime* Net = GetNetworkRuntime();
        return Net != nullptr && World != nullptr ? Net->PredictSpawn(World, StandIn, bKeepStandIn) : 0;
    }

    void CNetSubsystem::ConfirmPredictedSpawn(ECS::FEntity Entity, uint32 ConnectionId, uint32 Key)
    {
        CWorld* World = GetWorld();
        if (INetworkRuntime* Net = GetNetworkRuntime(); Net != nullptr && World != nullptr)
        {
            Net->ConfirmPredictedSpawn(World, Entity, ConnectionId, Key);
        }
    }
}
