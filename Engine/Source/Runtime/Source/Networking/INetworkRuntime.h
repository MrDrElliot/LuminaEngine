#pragma once

#include "World/ECS/Registry.h"


#include "ModuleAPI.h"
#include "Memory/SmartPtr.h"
#include "Containers/Vector.h"
#include "Networking/NetworkTypes.h"

// The ECS registry is reached through its own header, and it is already in the
// Runtime PCH, so this costs nothing here.

namespace Lumina
{
    class CWorld;
    class CEntityScript;
    class INetworkTransport;

    // Everything the engine asks of networking, and the whole of it. The engine calls through this
    // and never names an implementation, so the netcode behind it can be replaced or removed
    // outright without the engine knowing which one it got.
    //
    // Every method has an empty default: with no implementation installed the engine runs exactly as
    // it does with networking disabled, and an implementation overrides only what it cares about.
    class INetworkRuntime
    {
    public:

        virtual ~INetworkRuntime() = default;

        // Process lifecycle, driven by FEngine.
        virtual void Initialize() {}
        virtual void Shutdown() {}
        virtual void Update() {}

        // Clients currently connected to this world, for anything that reports it. Zero without netcode.
        virtual int32 GetConnectedClientCount(const CWorld* World) const { return 0; }

        // Hand the live client connection over to the engine so it survives a map change, leaving the
        // old world without one. False when there is nothing to carry, which is the usual answer.
        //
        // The engine owns the transport across the gap and hands it back after the new world exists,
        // so a seamless travel does not reconnect. Only the netcode knows where the connection was
        // kept, which is why this is a handover rather than the engine reaching in for it.
        virtual bool TakeClientConnection(
            CWorld* OldWorld,
            TUniquePtr<INetworkTransport>& OutTransport,
            FConnectionHandle& OutConnection,
            uint32& OutLocalPeerId)
        {
            return false;
        }

        // A world has finished materializing its serialized entities. A client drops the entities the
        // server owns exclusively here; the decision of which those are belongs to the netcode, so
        // the engine reports the event rather than asking for the outcome.
        virtual void OnWorldEntitiesLoaded(CWorld* World) {}

        // An entity's parent changed. Replication may need to observe it; the engine does not know
        // whether it does.
        virtual void OnEntityAttachmentChanged(CWorld* World, ECS::FEntity Entity) {}

        //~ The gameplay surface scripts reach through. A world with no netcode answers as standalone.

        // This peer's connection id, 0 on the host and on a standalone world.
        virtual uint32 GetLocalConnectionId(const CWorld* World) const { return 0; }

        // Server, the remote connections that finished joining, in join order.
        virtual void GetConnectionIds(const CWorld* World, TVector<uint32>& OutIds) const {}

        virtual bool IsEntityNetworked(const CWorld* World, ECS::FEntity Entity) const { return false; }

        // The connection that controls Entity, 0 when the host does.
        virtual uint32 GetEntityOwner(const CWorld* World, ECS::FEntity Entity) const { return 0; }

        // Server only, so a client asking is refused rather than silently diverging.
        virtual bool SetEntityOwner(CWorld* World, ECS::FEntity Entity, uint32 ConnectionId) { return false; }

        // A replicated field on Entity changed, so the server diffs it on the next send.
        virtual void MarkEntityDirty(CWorld* World, ECS::FEntity Entity) {}

        // The id both peers know an entity by, 0 when it has none.
        virtual uint32 EntityToNetId(const CWorld* World, ECS::FEntity Entity) const { return 0; }
        virtual ECS::FEntity NetIdToEntity(const CWorld* World, uint32 NetId) const { return ECS::NullEntity; }

        // Queues an RPC whose arguments the script already serialized. False when it could not be routed.
        virtual bool SendScriptRpc(CEntityScript* Script, uint32 RpcId, ERpcTarget Target, uint8 Flags,
            const uint8* Payload, uint32 PayloadSize) { return false; }
    };

    // Null until something installs one, which is a supported state and not an error.
    RUNTIME_API void SetNetworkRuntime(INetworkRuntime* Runtime);
    RUNTIME_API INetworkRuntime* GetNetworkRuntime();
}
