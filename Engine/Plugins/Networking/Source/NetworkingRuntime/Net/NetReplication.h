#pragma once

#include "World/ECS/Registry.h"


#include "Platform/GenericPlatform.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/FunctionRef.h"
#include "Networking/NetworkTypes.h"

namespace Lumina
{
    class FNetArchive;
    class INetworkTransport;
    class CObject;
    class CEntityScript;
    struct FConnectionHandle;
    struct FNetObjectMap;
    struct FNetAssetMap;
    struct FNetNameMap;
    struct FNetWorldState;
    struct FReplTypeTable;
    struct FComponentRepState;
    enum class ESendMode : uint8;
    enum class ENetMessage : uint8;

    // Tag, this networked entity has replicated property changes pending, which the host sends on its next tick.
    struct FNetDirty {};

    namespace Net
    {
        // Max bytes for one framed message, since the length prefix is 16-bit and AppendFramedMessage drops anything larger.
        inline constexpr SIZE_T MaxFramedMessageSize = 0xFFFF;

        // Reflection-driven entity replication keyed by a compact type hash, carrying only Replicated properties.

        // Hash of the wire protocol
        NETWORKINGRUNTIME_API uint32 GetProtocolHash(const FReplTypeTable& Types);

        // Compact NetGUID codec that moves the dynamic flag to the LSB so both ranges varint to 1-2 bytes. Every wire NetGUID goes through this pair.
        void   WriteNetGuid(FNetArchive& Ar, uint32 Guid);
        uint32 ReadNetGuid (FNetArchive& Ar);

        // Block is the changed-field bitmask then each changed field's bytes in order, shared by every recipient, and empty for a removal.
        struct FComponentRepOut
        {
            uint32         WireIndex = 0;
            bool           bRemoved = false;
            TVector<uint8> Block;
        };

        // A baseline emits every component and field, otherwise only changed fields and removed components; a given DiffState is updated and Out's storage reused.
        NETWORKINGRUNTIME_API void CollectComponentFieldsInto(ECS::FRegistry& Registry, ECS::FEntity Entity, FNetWorldState& State,
            bool bBaseline, FComponentRepState* DiffState, TVector<FComponentRepOut>& Out);

        // True when Parent replicates to clients, so a child sends its local transform, otherwise world since the client has no parent to compose against.
        bool ParentReplicates(ECS::FRegistry& Registry, ECS::FEntity Parent);

        // Null Components writes an empty component block.
        NETWORKINGRUNTIME_API void WriteEntityComponents(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity,
            const TVector<FComponentRepOut>* Components = nullptr);

        // A spawn sends full state, since unreplicated configuration would otherwise arrive as defaults.
        void WriteSpawnComponents(FNetArchive& Ar, ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity);
        void ReadSpawnComponents(FNetArchive& Ar, ECS::FRegistry& Registry, const FNetWorldState& State, ECS::FEntity Entity);

        // Every script's full state, in manifest order, for the same reason.
        void WriteScriptStates(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity);
        void ReadScriptStates(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity, uint32 Count);

        // Client, recreate/refresh components on Entity, then apply the replicated attachment link.
        NETWORKINGRUNTIME_API void ReadEntityComponents(FNetArchive& Ar, ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity);

        // One script's diff, keyed by its index on the entity. Block has the same layout as a component's.
        struct FScriptRepOut
        {
            uint32         ScriptIndex = 0;
            TVector<uint8> Block;
        };

        // Server, diff every script on Entity that declares replicated fields. Returns true when a rate-limited field was held back, so the entity stays dirty.
        NETWORKINGRUNTIME_API bool CollectScriptFieldsInto(ECS::FRegistry& Registry, ECS::FEntity Entity, FNetWorldState& State,
            bool bBaseline, FComponentRepState* DiffState, TVector<FScriptRepOut>& Out);

        // The class of every script on Entity, so a client can build a spawned entity's scripts before their fields arrive.
        void WriteScriptManifest(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity);
        uint32 ReadScriptManifest(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity);

        void WriteEntityScripts(FNetArchive& Ar, const TVector<FScriptRepOut>& Scripts);

        // Client, applies each script's changed fields and tells it which ones changed.
        void ReadEntityScripts(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity);

        // Scripts are addressed by position on the entity, which both peers build in the same order.
        int32 FindScriptIndex(ECS::FRegistry& Registry, ECS::FEntity Entity, const CEntityScript* Script);
        CEntityScript* GetScriptAt(ECS::FRegistry& Registry, ECS::FEntity Entity, uint32 Index);

        // One framed ScriptRpc message. Caller is the connection that issued it, which the server rewrites on relay.
        void AppendScriptRpc(TVector<uint8>& Batch, uint32 Guid, uint32 ScriptIndex, uint32 RpcId, ERpcTarget Target,
            uint8 Flags, uint32 Caller, const uint8* Payload, uint32 PayloadSize);

        // Client, reparent children deferred waiting on NewEntity, called right after its NetGUID is registered.
        void DrainPendingAttach(ECS::FRegistry& Registry, FNetWorldState& State, uint32 NewGuid, ECS::FEntity NewEntity);

        //~ Packet batching concatenates many small messages per tick into one datagram, and every packet on the wire is a batch.

        // Append one framed message to a batch buffer.
        NETWORKINGRUNTIME_API void AppendFramedMessage(TVector<uint8>& Batch, const uint8* Msg, SIZE_T MsgSize);

        // Append NumEntries as framed "Type, uint16 Count, entries" messages, split so each fits the frame. WriteEntry returns false to skip one.
        void AppendCountedMessages(TVector<uint8>& Batch, ENetMessage Type, int32 NumEntries, TFunctionRef<bool(FNetArchive&, int32)> WriteEntry);

        // Send a single message as a 1-message batch (RPC, peer-id handshake).
        void SendFramed(INetworkTransport& Transport, FConnectionHandle Connection, const uint8* Msg, SIZE_T MsgSize, uint8 Channel, ESendMode Mode);

        // Split a received batch into its framed messages.
        NETWORKINGRUNTIME_API void ForEachFramedMessage(const uint8* Data, SIZE_T Size, const TFunction<void(const uint8*, SIZE_T)>& Fn);

        //~ Net-index caches send references as a varint index, with identity exported once and resolved in the sender's index space.

        // Bind both index caches on an archive, the writer using the peer's outgoing maps and the reader the sender's incoming maps.
        NETWORKINGRUNTIME_API void BindWriters(FNetArchive& Ar, FNetWorldState& State);
        NETWORKINGRUNTIME_API void BindReaders(FNetArchive& Ar, FNetWorldState& State, uint32 SenderConn);

        // Entity ids alone, which every peer shares, for a payload that may be relayed past the peer that wrote it.
        void BindEntityIds(FNetArchive& Ar, FNetWorldState& State);

        // Apply refuses indices at or past this, so a peer can never grow another's incoming maps or name table without bound.
        inline constexpr uint32 MaxIncomingNetIndex = 1u << 16;

        // Build/apply the index-to-identity export messages (reliable). Build appends framed messages to a batch; Apply ingests one message into the sender's map.
        NETWORKINGRUNTIME_API void BuildObjectExport(const FNetObjectMap& Map, const TVector<uint32>& Indices, TVector<uint8>& Batch);
        NETWORKINGRUNTIME_API void ApplyObjectExport(FNetObjectMap& Map, const uint8* Data, SIZE_T Size);
        NETWORKINGRUNTIME_API void BuildAssetExport(const FNetAssetMap& Map, const TVector<uint32>& Indices, TVector<uint8>& Batch);
        NETWORKINGRUNTIME_API void ApplyAssetExport(FNetAssetMap& Map, const uint8* Data, SIZE_T Size);
        NETWORKINGRUNTIME_API void BuildNameExport(const FNetNameMap& Map, const TVector<uint32>& Indices, TVector<uint8>& Batch);
        NETWORKINGRUNTIME_API void ApplyNameExport(FNetNameMap& Map, const uint8* Data, SIZE_T Size);
    }
}
