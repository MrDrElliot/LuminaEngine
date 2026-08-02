#pragma once

#include "Memory/SmartPtr.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Core/Math/Math.h"
#include "GUID/GUID.h"
#include "Assets/AssetRef.h"
#include "Networking/INetworkTransport.h"
#include "World/Net/NetGUID.h"
#include "World/Net/NetReplicationGraph.h"

namespace Lumina
{
    class CObject;

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
        PropertyUpdate    = 7, // server to clients, replicated components (reliable)
        ClientTransform   = 8, // client to server, owned-entity transforms
        Welcome           = 9, // server to client, which level to load
        ClientReady       = 10,// client to server, map loaded, request baseline
        ObjectExport      = 11,// CObject net-index map
        AssetExport       = 12,// FAssetRef net-index map
        NameExport        = 13,// FName net-index map
    };

    // CObject-ref net-index cache. A referenced object is sent as a compact varint index; the index/GUID
    // map is exported once over a reliable stream and replayed to late joiners. One struct, both directions.
    struct FNetObjectMap
    {
        THashMap<CObject*, uint32> ObjToIndex;     // outgoing object index
        THashMap<uint32, FGuid>    IndexToGuid;    // outgoing on assign, incoming from export
        THashMap<uint32, CObject*> IndexToObject;  // incoming resolved cache
        TVector<uint32>            PendingExports; // outgoing, assigned but not yet sent
        uint32                     NextIndex = 1;  // 0 is null
    };

    // FAssetRef net-index cache. Same export-once model as FNetObjectMap, keyed by GUID else Path.
    struct FNetAssetMap
    {
        THashMap<FName, uint32>     KeyToIndex;     // outgoing asset key index
        THashMap<uint32, FAssetRef> IndexToRef;     // outgoing on assign, incoming from export
        TVector<uint32>             PendingExports; // outgoing, assigned but not yet sent
        uint32                      NextIndex = 1;  // 0 is null
    };
    
    // FName net-index cache. FName IDs are an XXHash64 of the string (deterministic across peers), but a peer
    // can only resolve the text once it has interned it. So the string is exported once over a reliable stream,
    // keyed by a compact varint index, then every later reference is just that index. Same export-once model as
    // FNetObjectMap/FNetAssetMap. One struct, both directions.
    struct FNetNameMap
    {
        THashMap<FName, uint32> KeyToIndex;     // outgoing name -> index
        THashMap<uint32, FName> IndexToName;    // outgoing on assign, incoming from export
        TVector<uint32>         PendingExports; // outgoing, assigned but not yet sent
        uint32                  NextIndex = 1;  // 0 is NAME_None
    };

    // Replication-layer instrumentation for the network debug tool. Per-tick fields are refreshed by the
    // server send path each tick; Peak/OversizedSnapshotDrops are cumulative.
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
    };

    // Per-world networking state.
    struct FNetWorldState
    {
        TUniquePtr<INetworkTransport> Transport;

        // Client side, link to the server. Invalid until ConnectToServer succeeds.
        FConnectionHandle             ServerConnection;

        // Server side, connected clients.
        int32                         ConnectedClients = 0;
        TVector<uint32>               ConnectedClientIds;

        // This peer's unique id. ServerPeerId on the server, the assigned handle on a client.
        uint32                        LocalPeerId = ServerPeerId;

        // Server side, re-broadcast the ownership table next tick.
        bool                          bOwnershipDirty = true;

        // Client side, set once the link to the server is established.
        bool                          bClientConnected = false;

        // Server side, seconds since the last full keyframe. Periodically re-arms every client's
        // FNetClientView::bForceBaseline so a dropped delta self-heals.
        float                         TimeSinceKeyframe = 0.0f;

        // NetGUID to entity map for this world. Stable ids are adopted once at init.
        FNetGUIDTable                 GuidTable;

        // Dynamic-entity destruction is event-driven (on_destroy<SNetworkComponent> -> GuidTable.Unregister),
        // so there's no per-tick live-set to maintain -- MaintainDynamicLifetime only assigns GUIDs.

        // Server side, adopted stable NetGUIDs still live. Diffed each tick so a level entity destroyed at
        // runtime is despawned for clients.
        TVector<uint32>               KnownStableGuids;

        // Server side, stable NetGUIDs destroyed at runtime, replayed to late joiners so they remove copies.
        TVector<uint32>               DestroyedStableGuids;

        bool                          bInitialized = false;
        bool                          bStableEntitiesAdopted = false;

        // Client side, ClientReady sent to the server once the link is up.
        bool                          bClientReadySent = false;

        //~ Client snapshot interpolation for SimulatedProxy movement. Per-entity sample rings live on the
        //  entity's FRepTransform component; this state is the global render clock shared by all of them.
        double                        LatestServerTime   = 0.0;
        double                        ServerPlaybackTime = 0.0;
        double                        ClockOffset        = 0.0; // legacy (unused by the smooth clock)
        bool                          bClockInitialized  = false;

        // Net-index caches. Outgoing maps this peer mints and exports; incoming maps are kept per sender
        // connection id since the index space is sender-owned.
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
        
        // Per-connection relevancy + send-schedule. Created on connect, erased on disconnect. Never touches
        // GuidTable (which stays the global entity-lifetime map).
        THashMap<uint32, FNetClientView> ClientViews;

        // Incremented once per ServerReplicateRelevant tick. An FRelevantEntry is "relevant this tick" when
        // its RelevantTick == this, which replaces a full-map "reset every entry to not-relevant" pass.
        uint64                           RelevancyTick = 0;

        // Reused buffer for the per-tick transport Service() drain (avoids reallocating it every frame).
        TVector<FNetworkEvent>           ServiceEvents;

        // Per-tick scratch: reliable PropertyUpdate datagrams that must go to specific clients (entities with
        // owner-conditioned --@replicated fields). Built by ReplicateDirtyProperties, flushed by
        // ServerReplicateRelevant AFTER net-index exports so the recipient can resolve any referenced index.
        THashMap<uint32, TVector<uint8>> PendingClientReliable;

        // Client: replicated children whose parent hasn't spawned yet. Key = child entity (integral id, so the
        // map needs no entt hasher), value = desired parent NetGUID. Drained when that parent spawns
        // (ApplySpawnEntity), since relevancy spawn order isn't guaranteed.
        THashMap<uint32, uint32> PendingAttach;
    };
}
