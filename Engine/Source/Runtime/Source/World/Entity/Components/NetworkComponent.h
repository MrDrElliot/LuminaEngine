#pragma once
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Math.h"
#include "World/Net/NetGUID.h"
#include "World/Net/NetRole.h"
#include "NetworkComponent.generated.h"

namespace Lumina
{
    // Marks an entity for networking. Authored fields serialize with the world; runtime net state is
    // derived per-peer by SNetworkSystem and shown ReadOnly for debugging but never saved.
    REFLECT(Component, Category = "Networking")
    struct RUNTIME_API SNetworkComponent
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

        /** How many times per second this entity's movement is sent (caps the send rate; clients interpolate
         *  between updates). <= 0 sends every tick. Replicated so the owning client throttles its sends to match. */
        PROPERTY(Editable, Replicated, Category = "Networking")
        float NetUpdateFrequency = 60.0f;

        //~ Runtime net state (derived per-peer; not serialized). Read-only debug visibility.

        /** Stable network identity, resolved per peer. */
        PROPERTY(ReadOnly, NoSerialize, Category = "Networking|Debug")
        FNetGUID NetGUID;

        /** This peer's role. Authority on the server, proxy on clients. */
        PROPERTY(ReadOnly, NoSerialize, Category = "Networking|Debug")
        ENetRole LocalRole = ENetRole::None;

        /** The far end's role (Authority as seen from a client; the proxy role as seen from the server). */
        PROPERTY(ReadOnly, NoSerialize, Category = "Networking|Debug")
        ENetRole RemoteRole = ENetRole::None;

        /** Owning connection (FConnectionHandle::Value); 0 = server-owned / unowned. */
        PROPERTY(ReadOnly, NoSerialize, Category = "Networking|Debug")
        uint32 OwningConnectionId = 0;

        // Client-only latch. A proxy's physics body switched to Kinematic so the local sim no longer
        // fights replicated transforms. Set once the body exists.
        bool bProxyPhysicsConfigured = false;
    };
}
