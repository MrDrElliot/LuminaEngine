#pragma once

#include "Networking/NetworkTypes.h"
#include "World/ECS/Registry.h"
#include "World/ECS/EventMeta.h"

#include "Core/Object/ObjectMacros.h"
#include "NetworkEvents.generated.h"

namespace Lumina
{
    // What happened to a session. The first two reach the host, the last two a client.
    REFLECT()
    enum class ENetSessionEventKind : uint8
    {
        ClientJoined = 0,
        ClientLeft   = 1,
        JoinedHost   = 2,
        LeftHost     = 3,
    };

    // The base of a C++ net message, a REFLECT(Event) struct a system sends with CNetSubsystem::Send and reads where it lands like any other event.
    REFLECT()
    struct RUNTIME_API SNetMessage
    {
        GENERATED_BODY()

        // The connection it came from, written on arrival, 0 for the host.
        PROPERTY(Hidden)
        uint32 Sender = 0;
    };

    // A connection joined or left, queued for systems and broadcast through CNetSubsystem's session delegates.
    REFLECT(Event)
    struct RUNTIME_API SNetSessionEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ENetSessionEventKind Kind = ENetSessionEventKind::ClientJoined;

        // The remote connection on the host, 0 on a client, whose only peer is the host.
        PROPERTY(Hidden)
        uint32 ConnectionId = 0;

        PROPERTY(Hidden)
        ENetLeaveReason Reason = ENetLeaveReason::None;
    };

    // A client created the local copy of a replicated entity, with its baseline already applied.
    REFLECT(Event)
    struct RUNTIME_API SNetEntitySpawnedEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY(Hidden)
        uint32 NetGUID = 0;

        PROPERTY(Hidden)
        uint32 OwningConnectionId = 0;
    };

    // An entity stopped being networked and lives on as a local entity, raised on the host and on every client that held it.
    REFLECT(Event)
    struct RUNTIME_API SNetEntityTornOffEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;
    };

    // A client destroyed its copy of a replicated entity, so Entity is already gone when this is read.
    REFLECT(Event)
    struct RUNTIME_API SNetEntityDespawnedEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY(Hidden)
        uint32 NetGUID = 0;
    };

    // A client heard back about a spawn it predicted, the host's copy arriving or the wait for it running out.
    REFLECT(Event)
    struct RUNTIME_API SNetPredictedSpawnEvent
    {
        GENERATED_BODY()

        // Already destroyed when the host refused, or when the host's copy replaced it.
        PROPERTY(Hidden)
        ECS::FEntity StandIn = ECS::NullEntity;

        // Null when the host refused.
        PROPERTY(Hidden)
        ECS::FEntity Authoritative = ECS::NullEntity;

        PROPERTY(Hidden)
        uint32 Key = 0;

        PROPERTY(Hidden)
        bool bConfirmed = false;
    };

    // A client applied replicated fields to an entity, one per entity per update however many components changed.
    REFLECT(Event)
    struct RUNTIME_API SNetReplicatedEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;
    };

    // An entity's owning connection changed, on the host when it reassigns one and on a client when it hears of it.
    REFLECT(Event)
    struct RUNTIME_API SNetOwnershipChangedEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY(Hidden)
        uint32 PreviousOwner = 0;

        PROPERTY(Hidden)
        uint32 OwningConnectionId = 0;
    };
}
