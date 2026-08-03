#pragma once

#include "ModuleAPI.h"
#include "Memory/SmartPtr.h"
#include "Networking/NetworkTypes.h"

// entt ships as one amalgamated header with no forward-declaration header, and it is already in the
// Runtime PCH, so this costs nothing here.
#include <entt/entt.hpp>

namespace Lumina
{
    class CWorld;
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
        virtual void OnEntityAttachmentChanged(CWorld* World, entt::entity Entity) {}
    };

    // Null until something installs one, which is a supported state and not an error.
    RUNTIME_API void SetNetworkRuntime(INetworkRuntime* Runtime);
    RUNTIME_API INetworkRuntime* GetNetworkRuntime();
}
