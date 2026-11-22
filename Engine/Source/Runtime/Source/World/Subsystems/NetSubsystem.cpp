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
