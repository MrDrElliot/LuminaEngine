#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Networking/NetworkTypes.h"
#include "entt/entt.hpp"

namespace Lumina
{
    class FNetArchive;
    class INetworkTransport;
    class CObject;
    struct FConnectionHandle;
    struct FNetObjectMap;
    struct FNetAssetMap;
    struct FNetNameMap;
    struct FNetWorldState;
    struct FComponentRepState;
    enum class ESendMode : uint8;

    // Tag, this networked entity has replicated property changes pending (set by World.Net:MarkDirty).
    // The server sends a reliable PropertyUpdate for it next tick, then clears the tag.
    struct FNetDirty {};

    namespace Net
    {
        // Max bytes for one framed message. The frame length prefix is 16-bit, so a single message (e.g. a
        // transform snapshot) can't exceed this; AppendFramedMessage drops anything larger. Single-sourced
        // here so the network debug tool can warn as a snapshot approaches the cap.
        inline constexpr SIZE_T MaxFramedMessageSize = 0xFFFF;

        // Reflection-driven entity replication. The same generic path the world serializer uses, keyed by a
        // compact type hash, carrying only Replicated properties. Works for any REFLECT(Component).

        // Hash of the wire protocol
        uint32 GetProtocolHash();

        // Compact NetGUID codec. Stable ids are small; dynamic (spawned) ids carry the high bit, which would
        // varint to 5 bytes, so the two ranges are interleaved (LSB = dynamic flag) and the remainder
        // varint-encoded, giving 1-2 bytes for both. Every NetGUID on the wire must go through this pair.
        void   WriteNetGuid(FNetArchive& Ar, uint32 Guid);
        uint32 ReadNetGuid (FNetArchive& Ar);

        // One replicated component's diff result, ready for the wire. Block = [changed-field bitmask
        // (ceil(NumRepFields/8) bytes)] ++ [each changed field's whole-byte serialization, in field order].
        // Recipient-independent (native components carry no per-client conditions), so one Block is reused
        // for every recipient. Built by CollectComponentFields; written verbatim by WriteEntityComponents.
        struct FComponentRepOut
        {
            uint32         WireIndex = 0;
            TVector<uint8> Block;
        };

        // Server, diff this entity's replicated native components against DiffState (per-field byte compare).
        // bBaseline emits ALL components + ALL fields (mask all-ones) and seeds DiffState; otherwise emits only
        // components with >=1 changed field, each carrying a changed-field bitmask, and updates DiffState.
        // Object/asset/name refs mint into State's outgoing maps.
        TVector<FComponentRepOut> CollectComponentFields(entt::registry& Registry, entt::entity Entity,
            FNetWorldState& State, bool bBaseline, FComponentRepState* DiffState);

        // True when Parent is an entity that actually replicates to clients (SNetworkComponent + bReplicates +
        // bNetLoadOnClient + a NetGUID). A child of such a parent sends its LOCAL transform + the parent's NetGUID
        // (client reparents + composes, rigid); a child of a non-replicated parent must send WORLD instead, since
        // the client has no parent to compose against. Both the transform extract and the attachment-link write
        // gate on this so they stay consistent.
        bool ParentReplicates(entt::registry& Registry, entt::entity Parent);

        // Server, write one entity's replicated component blocks (from Components, precomputed by
        // CollectComponentFields). Used for both Spawn (baseline) and PropertyUpdate (diff). Null Components
        // writes an empty component block. Recipient-independent.
        void WriteEntityComponents(FNetArchive& Ar, entt::registry& Registry, entt::entity Entity,
            const TVector<FComponentRepOut>* Components = nullptr);

        // Client, recreate/refresh components on Entity, then apply the replicated attachment link.
        void ReadEntityComponents(FNetArchive& Ar, entt::registry& Registry, entt::entity Entity);

        // Client, reparent any children that were deferred waiting on NewEntity (NetGUID NewGuid) to spawn.
        // Call right after registering a freshly-spawned entity's NetGUID in the GuidTable.
        void DrainPendingAttach(entt::registry& Registry, FNetWorldState& State, uint32 NewGuid, entt::entity NewEntity);

        //~ Packet batching. Many small messages per tick are concatenated into one length-prefixed datagram,
        //~ one ENet header and ack instead of N. Every packet on the wire is a batch.

        // Append one framed message to a batch buffer.
        void AppendFramedMessage(TVector<uint8>& Batch, const uint8* Msg, SIZE_T MsgSize);

        // Send a single message as a 1-message batch (RPC, peer-id handshake).
        void SendFramed(INetworkTransport& Transport, FConnectionHandle Connection, const uint8* Msg, SIZE_T MsgSize, uint8 Channel, ESendMode Mode);
        void BroadcastFramed(INetworkTransport& Transport, const uint8* Msg, SIZE_T MsgSize, uint8 Channel, ESendMode Mode);

        // Split a received batch into its framed messages.
        void ForEachFramedMessage(const uint8* Data, SIZE_T Size, const TFunction<void(const uint8*, SIZE_T)>& Fn);

        //~ Net-index caches. A CObject/FAssetRef reference is sent as a compact varint index; identity is
        //~ exported once and resolved in the sender's index space. Incoming maps are per-connection.

        // Bind both index caches on an archive. Writer uses the peer's outgoing maps, reader the sender's
        // incoming maps. Used for component + RPC-arg replication.
        void BindWriters(FNetArchive& Ar, FNetWorldState& State);
        void BindReaders(FNetArchive& Ar, FNetWorldState& State, uint32 SenderConn);

        // Build/apply the index-to-identity export messages (reliable). Apply ingests into the sender's map.
        void BuildObjectExport(const FNetObjectMap& Map, const TVector<uint32>& Indices, TVector<uint8>& OutMsg);
        void ApplyObjectExport(FNetObjectMap& Map, const uint8* Data, SIZE_T Size);
        void BuildAssetExport(const FNetAssetMap& Map, const TVector<uint32>& Indices, TVector<uint8>& OutMsg);
        void ApplyAssetExport(FNetAssetMap& Map, const uint8* Data, SIZE_T Size);
        void BuildNameExport(const FNetNameMap& Map, const TVector<uint32>& Indices, TVector<uint8>& OutMsg);
        void ApplyNameExport(FNetNameMap& Map, const uint8* Data, SIZE_T Size);
    }
}
