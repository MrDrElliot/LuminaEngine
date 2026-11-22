#pragma once

#include "Memory/SmartPtr.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Math/Math.h"
#include "GUID/GUID.h"
#include "UI/UIDocumentRef.h"
#include "Networking/INetworkTransport.h"
#include "World/ECS/EventQueue.h"
#include "World/Net/NetGUID.h"
#include "Net/NetReplicationGraph.h"
#include "Net/NetReplication.h"
#include "Net/NetPrediction.h"
#include "Systems/NetworkSystem.h"
#include "Net/NetPropertyDelivery.h"

namespace Lumina
{
    class CObject;
    class CStruct;
    class CClass;
    class FProperty;
    class FNetCountingTransport;

    // Where a server tick's network time goes, for the -netstats breakdown.
    enum class ENetPhase : uint8
    {
        Extract, Schedule, Relevancy, Messages, Exports, Payloads, Send, Flush, Count
    };

    // The server's reserved peer id (authority, owns connection 0).
    inline constexpr uint32 ServerPeerId = 0;

    // Leading byte on every Data-channel packet so the receiver can route it.
    enum class ENetMessage : uint8
    {
        TransformSnapshot = 1,
        ScriptRpc         = 2,
        AssignPeerId      = 3, // server to client, assigns its peer id
        OwnershipUpdate   = 4, // server to clients, ownership table
        SpawnEntity       = 5, // server to clients, spawn with components
        DespawnEntity     = 6, // server to clients, despawn
        PropertyUpdate    = 7, // server to clients, replicated components and script fields of one entity
        ClientTransform   = 8, // client to server, owned-entity transforms
        Welcome           = 9, // server to client, which level to load
        ClientReady       = 10,// client to server, map loaded, request baseline
        ObjectExport      = 11,// CObject net-index map
        AssetExport       = 12,// SUIDocumentRef net-index map
        NameExport        = 13,// FName net-index map
        MoveCommands      = 14,// client to host, predicted character input
        MoveAck           = 15,// host to owner, the state a move produced
        SimStart          = 16,// host to client, a deterministic simulation session and the client's player slot
        SimInput          = 17,// either way, one player's inputs for consecutive simulation ticks
        SimChecksum       = 18,// either way, a digest of the simulation state at a tick
        PropertyPacket    = 19,// host to client, numbered unreliable PropertyUpdates
        PropertyAck       = 20,// client to host, which property packets arrived
        PropertyFence     = 21,// host to client, reliably after spawns, the first property packet that may touch them
        TearOffEntity     = 22,// host to clients, the entity stops being networked and stays as a local copy
        EventMessage      = 23,// either way, a net message raised as an event where it lands
    };

    // CObject-ref net-index cache, exported once over a reliable stream and replayed to late joiners.
    struct FNetObjectMap
    {
        THashMap<FGuid, uint32>    GuidToIndex;    // outgoing object index
        THashMap<uint32, FGuid>    IndexToGuid;    // outgoing on assign, incoming from export
        THashMap<uint32, TWeakObjectPtr<CObject>> IndexToObject;  // incoming resolved cache
        THashSet<uint32>           FailedIndices;  // incoming, GUIDs that did not load
        TVector<uint32>            PendingExports; // outgoing, assigned but not yet sent
        uint32                     NextIndex = 1;  // 0 is null
        bool                       bLoggedRefusal = false; // an out-of-range index was reported, incoming refusal or outgoing exhaustion
    };

    // SUIDocumentRef net-index cache. Same export-once model as FNetObjectMap, keyed by GUID else Path.
    struct FNetAssetMap
    {
        THashMap<FName, uint32>     KeyToIndex;     // outgoing asset key index
        THashMap<uint32, SUIDocumentRef> IndexToRef;     // outgoing on assign, incoming from export
        TVector<uint32>             PendingExports; // outgoing, assigned but not yet sent
        uint32                      NextIndex = 1;  // 0 is null
        bool                        bLoggedRefusal = false; // an out-of-range index was reported, incoming refusal or outgoing exhaustion
    };
    
    // FName net-index cache, exporting the string once since a peer can only resolve text it has interned.
    struct FNetNameMap
    {
        THashMap<FName, uint32> KeyToIndex;     // outgoing name -> index
        THashMap<uint32, FName> IndexToName;    // outgoing on assign, incoming from export
        TVector<uint32>         PendingExports; // outgoing, assigned but not yet sent
        uint32                  NextIndex = 1;  // 0 is NAME_None
        bool                    bLoggedRefusal = false; // an out-of-range index was reported, incoming refusal or outgoing exhaustion
    };

    // Replication instrumentation for the network debug tool, where per-tick fields refresh each tick and Peak and OversizedSnapshotDrops are cumulative.
    struct FNetReplicationStats
    {
        uint32 MovementFrameBytes      = 0; // size of the LARGEST TransformSnapshot frame this tick (vs cap)
        uint32 MovementTotalBytes      = 0; // total transform bytes this tick across all chunked frames
        uint32 MovementEntityCount     = 0; // total entities sent this tick
        uint16 MovementChunks          = 0; // number of snapshot frames the entities were split across
        uint32 PeakMovementFrameBytes  = 0; // high-water mark of the largest single frame
        uint32 OversizedSnapshotDrops  = 0; // frames that still exceeded the cap (should stay 0 with chunking)
        uint32 ReliableBatchBytes      = 0; // last tick's reliable datagram size
        uint32 UnreliableBatchBytes    = 0; // last tick's unreliable datagram size
        uint16 SpawnsSent              = 0; // last tick (summed across clients)
        uint16 DespawnsSent            = 0; // last tick (summed across clients)
        uint16 PropertyUpdatesSent     = 0; // last tick
        bool   bKeyframeThisTick       = false;
        uint32 RelevantAvg             = 0; // avg relevant entities per client (interest management)
        uint32 RelevantMax             = 0; // max relevant entities on any one client
        uint32 PropertyPacketsLost     = 0; // cumulative, property packets a client missed and whose entities went again
    };

    // The replicated component types, built with the session so a type a script reload rebinds reaches the next session.
    struct NETWORKINGRUNTIME_API FReplTypeTable
    {
        FReplTypeTable();
        ~FReplTypeTable();
        FReplTypeTable(const FReplTypeTable&) = delete;
        FReplTypeTable& operator = (const FReplTypeTable&) = delete;

        // The default instance every spawn of this type diffs against, made on first use.
        void* DefaultFor(uint32 WireIndex);

        struct FEntry
        {
            uint32   Hash    = 0;
            CStruct* Struct  = nullptr;
            void*    Default = nullptr;
        };

        // Sorted by name hash, so the same build gives the same order and the wire carries an index, not the hash.
        TVector<FEntry> ByIndex;
        TVector<int32>  TypeIdToIndex;
    };

    // Ownership-table entry, as sent in an OwnershipUpdate message.
    struct FNetOwnerRecord
    {
        uint32 Guid  = 0;
        uint32 Owner = 0;
    };

    // One grid-gathered candidate for a client's relevancy pass, copied out of the grid's cell-ordered arrays.
    struct FNetGatheredRecord
    {
        uint32      Rec   = 0;
        uint32      Guid  = 0;
        uint8       Flags = 0;
        ENetLODTier Tier  = ENetLODTier::Near;
        float       Score = 0.0f;
    };

    // A transform record waiting on the per-client send budget, ranked by Priority.
    struct FNetDueRecord
    {
        NetworkSystemInternal::FTransformSendRecord Record;
        float                                       Priority = 0.0f;
        FNetChangeSeq                               Seq;
    };

    // Reused by the serial replication and prediction paths, which never overlap in a tick.
    struct FNetReplicationScratch
    {
        TVector<Net::FComponentRepOut> Comps;
        TVector<Net::FScriptRepOut>    Scripts;
        TVector<uint8>                 Buffer;
        TVector<uint8>                 HookBytes;
        TVector<uint8>                 FieldBytes;
        TVector<uint32>                FieldOffsets;
        TVector<uint8>                 Mask;
        TVector<CStruct*>              AppliedTypes;
        TVector<FProperty*>            Fields;
        TVector<uint8>                 Held;
        TVector<uint8>                 Merged;
        TVector<uint32>                MergedOffsets;
        TVector<char>                  Names;
        TVector<FNetMoveCommand>       MoveCommands;
    };

    // Per-world networking state.
    struct FNetWorldState
    {
        FReplTypeTable                ReplTypes;

        TUniquePtr<INetworkTransport> Transport;

        // The same transport when -netstats wrapped it in a byte counter.
        FNetCountingTransport*        CountingTransport = nullptr;

        // The world's own registry, which entity references need to find an entity's NetGUID.
        ECS::FRegistry*               Registry = nullptr;

        // The world's event queues, null when a test drives replication without a world.
        ECS::FEventQueues*            Events = nullptr;

        // Server, connections that left last tick, whose entities are released this tick so leave readers still find them.
        TVector<uint32>               DepartedConnections;

        // Client side, link to the server. Invalid until ConnectToServer succeeds.
        FConnectionHandle             ServerConnection;

        // Server side, connected clients.
        int32                         ConnectedClients = 0;
        TVector<uint32>               ConnectedClientIds;

        // Server side, clients that sent ClientReady, in join order. Gameplay only ever sees these.
        TVector<uint32>               ReadyClientIds;

        // Host, each client's slot in the deterministic simulation, so a relayed message carries its sender's real slot.
        THashMap<uint32, uint32>      SimPlayerOfConnection;

        // Outgoing script RPCs keyed by destination connection, flushed after the frame's replication.
        THashMap<uint32, TVector<uint8>> PendingRpcReliable;
        THashMap<uint32, TVector<uint8>> PendingRpcUnreliable;

        // Periodic stats line, on when the process was started with -netstats=Seconds.
        float                         StatsTimer = 0.0f;
        double                        StatsTickStart = 0.0;
        float                         StatsFrameMsMax = 0.0f;
        double                        StatsFrameMsSum = 0.0;
        double                        StatsReceiveMsSum = 0.0;
        double                        PhaseMsSum[static_cast<uint32>(ENetPhase::Count)] = {};
        double                        PhaseMsMax[static_cast<uint32>(ENetPhase::Count)] = {};
        double                        StatsSendMsSum = 0.0;
        float                         StatsSendMsMax = 0.0f;
        uint32                        StatsFrames = 0;
        uint64                        StatsLastSent = 0;
        uint64                        StatsLastReceived = 0;

        // Server scratch, the extract records every client receives regardless of distance.
        TVector<uint32>               AlwaysRelevantRecords;

        // This peer's unique id. ServerPeerId on the server, the assigned handle on a client.
        uint32                        LocalPeerId = ServerPeerId;

        FNetPropertyAcks              PropertyAcks;

        // Client, a local stand-in for a spawn this peer asked the host for, waiting on the host's copy.
        struct FPredictedSpawn
        {
            ECS::FEntity StandIn = ECS::NullEntity;
            double       Expire = 0.0;
            bool         bKeepStandIn = true;
        };
        THashMap<uint32, FPredictedSpawn> PredictedSpawns;
        uint32                        NextPredictionKey = 1;

        // Server, entities whose owner changed, sent only to clients that hold them and to the owner.
        TVector<ECS::FEntity>         OwnershipChanged;

        // Server, joiners still owed the owners of map entities, which no spawn message carries.
        TVector<uint32>               OwnershipJoiners;

        // Server, this tick's property deltas, each sent only to clients that hold the entity.
        struct FPendingProperty
        {
            uint32 Guid   = 0;
            uint32 Offset = 0;
            uint32 Size   = 0;
        };
        TVector<FPendingProperty>     PendingProperties;
        TVector<uint8>                PendingPropertyBytes;

        // Client side, set once the link to the server is established.
        bool                          bClientConnected = false;

        // Server side, seconds since the last full keyframe, which periodically re-arms bForceBaseline so a dropped delta self-heals.
        float                         TimeSinceKeyframe = 0.0f;

        // NetGUID to entity map for this world. Stable ids are adopted once at init.
        FNetGUIDTable                 GuidTable;

        // Dynamic-entity destruction is event-driven, so MaintainDynamicLifetime only assigns GUIDs.

        // Server side, adopted stable NetGUIDs still live, diffed each tick so a destroyed level entity despawns for clients.
        TVector<uint32>               KnownStableGuids;

        // Net messages queued per destination connection, sent with the RPCs. Locked, since a parallel system may send.
        FMutex                           MessageLock;
        THashMap<uint32, TVector<uint8>> PendingMessages;

        // The event type each arriving message names, found once by scanning the registered types.
        THashMap<uint32, ECS::FComponentTypeID> MessageTypes;

        // Server, entities torn off since the last send, whose networking ends before anything replicates this tick.
        TVector<ECS::FEntity>         PendingTearOffs;

        // Server side, stable NetGUIDs destroyed at runtime, replayed to late joiners so they remove copies.
        TVector<uint32>               DestroyedStableGuids;

        bool                          bInitialized = false;
        bool                          bStableEntitiesAdopted = false;

        // Client side, ClientReady sent to the server once the link is up.
        bool                          bClientReadySent = false;

        // Client side, this world already matches the host's map, so joining finishes here rather than after travel.
        bool                          bWelcomed = false;

        // Client side, each session event reaches this world's scripts once.
        bool                          bJoinDispatched = false;
        bool                          bLeaveDispatched = false;
        ENetLeaveReason               PendingLeaveReason = ENetLeaveReason::None;
        double                        ConnectStartTime = -1.0;

        // Newest server time received, mirrored into FNetInterpClock for the render clock.
        double                        LatestServerTime   = 0.0;

        // Net-index caches, with incoming maps kept per sender connection since the index space is sender-owned.
        FNetObjectMap                    OutObjects;
        FNetAssetMap                     OutAssets;
        FNetNameMap                      OutNames;
        THashMap<uint32, FNetObjectMap>  InObjects;
        THashMap<uint32, FNetAssetMap>   InAssets;
        THashMap<uint32, FNetNameMap>    InNames;

        // Replication-layer instrumentation surfaced by the network debug tool.
        FNetReplicationStats             Stats;

        //~ Interest management (server). Built once per tick, reused across clients.

        // Flat SoA snapshot of all movement-replicating entities this tick.
        FNetExtract                      Extract;
        
        // CSR spatial grid over the extract (XZ counting sort).
        FNetGrid                         Grid;
        
        // connId -> record index of the entity that connection owns (its pawn), for O(1) viewpoint lookup.
        THashMap<uint32, uint32>         OwnerToRecord;
        
        // Per-connection relevancy and send schedule, created on connect and erased on disconnect without touching GuidTable.
        THashMap<uint32, FNetClientView> ClientViews;

        // Host, the change tick of the last scan for writes to replicated component fields, and each pool's write epoch then, so an untouched pool is skipped.
        uint32                           ReplicatedChangeTick = 0;
        TVector<uint64>                  ReplicatedWriteEpochs;
        TVector<ECS::FEntity>            ReplicatedChangeScratch;

        // Map entities exist on every client, while a spawned one only on clients it is relevant to.
        bool HoldsEntity(uint32 Connection, uint32 Guid) const
        {
            if (Guid < NetGUID_DynamicStart)
            {
                return true;
            }
            const auto View = ClientViews.Find(Connection);
            return View != nullptr && View->Relevant.Contains(Guid);
        }

        // An FRelevantEntry is relevant this tick when its RelevantTick matches, which replaces a full-map reset pass.
        uint64                           RelevancyTick = 0;

        // Reused buffer for the per-tick transport Service() drain (avoids reallocating it every frame).
        TVector<FNetworkEvent>           ServiceEvents;

        // Client, replicated children keyed by entity whose parent has not spawned yet, since relevancy spawn order is not guaranteed.
        THashMap<uint32, uint32> PendingAttach;

        FNetReplicationScratch ReplicationScratch;

        // One slot per Jobs::GetThreadSlot(), sized to Jobs::GetNumThreadSlots() before each client-parallel pass.
        TVector<TVector<FNetGatheredRecord>>      GatheredScratch;
        TVector<TVector<FNetDueRecord>>           DueScratch;
        mutable TVector<TVector<FNetOwnerRecord>> OwnershipPickedScratch;
    };
}
