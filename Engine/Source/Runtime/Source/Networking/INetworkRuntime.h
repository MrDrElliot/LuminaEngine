#pragma once

#include "World/ECS/Registry.h"


#include "ModuleAPI.h"
#include "Memory/SmartPtr.h"
#include "Containers/Vector.h"
#include "Networking/NetworkTypes.h"

// The ECS registry is reached through its own header, which is already in the Runtime PCH.

namespace Lumina
{
    class CWorld;
    class CStruct;
    class CEntityScript;
    class INetworkTransport;
    class FNetArchive;

    // Every method has an empty default, so with no implementation installed the engine runs as if networking were disabled.
    class INetworkRuntime
    {
    public:

        virtual ~INetworkRuntime() = default;

        // Process lifecycle, driven by FEngine.
        virtual void Initialize() {}
        virtual void Shutdown() {}
        virtual void Update() {}

        // Host only. Gives every ready client a player slot and starts the world's deterministic simulation on all of them.
        virtual bool StartSimulationSession(CWorld* World, const CStruct* InputStruct, uint64 Seed) { return false; }

        // Clients currently connected to this world, for anything that reports it. Zero without netcode.
        virtual int32 GetConnectedClientCount(const CWorld* World) const { return 0; }

        // The engine owns the transport across a map change so a seamless travel does not reconnect, and false means nothing to carry.
        virtual bool TakeClientConnection(
            CWorld* OldWorld,
            TUniquePtr<INetworkTransport>& OutTransport,
            FConnectionHandle& OutConnection,
            uint32& OutLocalPeerId)
        {
            return false;
        }

        // A client drops server-owned entities here, and the netcode decides which those are.
        virtual void OnWorldEntitiesLoaded(CWorld* World) {}

        // An entity's parent changed, which replication may need to observe.
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

        // Sends a message of event type TypeID where Target says, raised there as that event with its sender written in. False when it cannot go.
        virtual bool SendNetMessage(CWorld* World, ENetMessageTarget Target, uint32 Connection, ECS::FComponentTypeID TypeID, const void* Message) { return false; }

        // A C# message has no reflection, so it crosses as its bytes, with the sender written at SenderOffset and each entity id at EntityOffsets translated.
        virtual void RegisterScriptNetMessage(ECS::FComponentTypeID TypeID, uint32 SenderOffset, TSpan<const uint32> EntityOffsets) {}

        // Server only. Ends networking for Entity and the networked entities under it, leaving every peer its own local copy.
        virtual void TearOff(CWorld* World, ECS::FEntity Entity) {}

        // The id both peers know an entity by, 0 when it has none.
        virtual uint32 EntityToNetId(const CWorld* World, ECS::FEntity Entity) const { return 0; }
        virtual ECS::FEntity NetIdToEntity(const CWorld* World, uint32 NetId) const { return ECS::NullEntity; }

        // Queues an RPC whose arguments the script already serialized. False when it could not be routed.
        virtual bool SendScriptRpc(CEntityScript* Script, uint32 RpcId, ERpcTarget Target, uint8 Flags,
            const uint8* Payload, uint32 PayloadSize) { return false; }

        // Client, the session with the host is up and this world matches the host's map.
        virtual bool IsJoined(const CWorld* World) const { return false; }

        // An entity ConnectionId owns, preferring one with a character controller, or the null entity.
        virtual ECS::FEntity FindOwnedPawn(const CWorld* World, uint32 ConnectionId) const { return ECS::NullEntity; }

        // Client, marks a local stand-in for a spawn it is asking the host for and returns the key to send along, 0 on a host.
        virtual uint32 PredictSpawn(CWorld* World, ECS::FEntity StandIn, bool bKeepStandIn) { return 0; }

        // Host, ties a new spawn to the key a client sent, set the frame it spawns so the client pairs it with its stand-in.
        virtual void ConfirmPredictedSpawn(CWorld* World, ECS::FEntity Entity, uint32 ConnectionId, uint32 Key) {}

        // Binds the entity ids every peer shares onto Ar, so a reflected value carrying an entity reads alike on every peer.
        virtual void BindEntityIds(const CWorld* World, FNetArchive& Ar) const {}
    };

    // Null until something installs one, which is a supported state and not an error.
    RUNTIME_API void SetNetworkRuntime(INetworkRuntime* Runtime);
    RUNTIME_API INetworkRuntime* GetNetworkRuntime();
}
