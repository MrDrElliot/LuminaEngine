#include "RuntimePCH.h"
#include "World/Subsystems/NetSubsystem.h"
#include "World/World.h"

#include "Networking/INetworkRuntime.h"

namespace Lumina
{
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
}
