#pragma once

#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "NetSubsystem.generated.h"

namespace Lumina
{
    // Networked entities, ownership and connections.
    REFLECT()
    class RUNTIME_API CNetSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        // This peer's own connection, which is 0 on a host and in a standalone world.
        FUNCTION()
        uint32 GetLocalConnection();

        // The remote connections that finished joining, which only a host can see.
        FUNCTION()
        void GetConnections(TVector<uint32>& OutConnections);

        // Needs an SNetworkComponent on the entity.
        FUNCTION()
        bool IsEntityNetworked(ECS::FEntity Entity);

        FUNCTION()
        uint32 GetOwner(ECS::FEntity Entity);

        // Host only, a client asking is refused.
        FUNCTION()
        bool SetOwner(ECS::FEntity Entity, uint32 ConnectionId);

        // Resends the entity's replicated component fields.
        FUNCTION()
        void MarkEntityDirty(ECS::FEntity Entity);

        FUNCTION()
        uint32 EntityToNetId(ECS::FEntity Entity);

        FUNCTION()
        ECS::FEntity NetIdToEntity(uint32 NetId);

        // A host is always joined, and a client is once its handshake finished.
        FUNCTION()
        bool IsJoined();

        // The entity a connection owns, preferring one with a character controller.
        FUNCTION()
        ECS::FEntity GetOwnedPawn(uint32 ConnectionId);

        // Client, StandIn shows a spawn before the host makes it. Send the key with the request, 0 means a host or standalone world, which spawns for real.
        FUNCTION()
        uint32 PredictSpawn(ECS::FEntity StandIn, bool bKeepStandIn = true);

        // Host, call the frame Entity spawns, with the connection and key the request carried.
        FUNCTION()
        void ConfirmPredictedSpawn(ECS::FEntity Entity, uint32 ConnectionId, uint32 Key);
    };
}
