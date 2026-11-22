#pragma once
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Math.h"
#include "World/Net/NetGUID.h"
#include "World/Net/NetRole.h"
#include "NetworkComponent.generated.h"

namespace Lumina
{
    // Marks an entity for networking. Runtime net state is derived per peer by CNetworkSystem and never saved.
    REFLECT(Component, Category = "Networking")
    struct NETWORKINGRUNTIME_API SNetworkComponent
    {
        GENERATED_BODY()

        /** When false the entity has a stable identity but is not replicated. */
        PROPERTY(Editable, Category = "Networking")
        bool bReplicates = true;

        /** Relevant to every connection (skips relevancy culling once that exists). */
        PROPERTY(Editable, Category = "Networking")
        bool bAlwaysRelevant = false;

        /** When false the entity exists only on the server; clients strip it at world load. */
        PROPERTY(Editable, Category = "Networking")
        bool bNetLoadOnClient = true;

        /** Replicate this entity's transform. The server only sends it on frames where it actually moved. */
        PROPERTY(Editable, Category = "Networking")
        bool bReplicatesMovement = true;

        // Destroyed when its owning connection leaves, rather than handed back to the host.
        PROPERTY(Editable, Category = "Networking")
        bool bDestroyWithOwner = false;

        /** How many times per second this entity's movement is sent (caps the send rate; clients interpolate
         *  between updates). <= 0 sends every tick. Replicated so the owning client throttles its sends to match. */
        PROPERTY(Editable, Replicated, Category = "Networking")
        float NetUpdateFrequency = 60.0f;

        //~ Runtime net state (derived per-peer; not serialized). Read-only debug visibility.

        /** Stable network identity, resolved per peer. */
        PROPERTY(ReadOnly, NoSerialize, Category = "Networking|Debug")
        SNetGUID NetGUID;

        /** This peer's role. Authority on the server, proxy on clients. */
        PROPERTY(ReadOnly, NoSerialize, Category = "Networking|Debug")
        ENetRole LocalRole = ENetRole::None;

        /** The far end's role (Authority as seen from a client; the proxy role as seen from the server). */
        PROPERTY(ReadOnly, NoSerialize, Category = "Networking|Debug")
        ENetRole RemoteRole = ENetRole::None;

        /** Owning connection (FConnectionHandle::Value); 0 = server-owned / unowned. */
        PROPERTY(ReadOnly, NoSerialize, Category = "Networking|Debug")
        uint32 OwningConnectionId = 0;

        // The client that predicted this spawn and the key it sent, so that client pairs the host's copy with its stand-in.
        PROPERTY(ReadOnly, Replicated, Category = "Networking|Debug")
        uint32 PredictedBy = 0;

        PROPERTY(ReadOnly, Replicated, Category = "Networking|Debug")
        uint32 PredictionKey = 0;

        // Copied from the parent at runtime, so a hierarchy replicates as one unit without a component on every child.
        PROPERTY(ReadOnly, NoSerialize, Category = "Networking|Debug")
        bool bInherited = false;

        // Client-only latch set once a proxy's body is Kinematic, so the local sim stops fighting replicated transforms.
        bool bProxyPhysicsConfigured = false;
    };
}
