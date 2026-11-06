#pragma once

#include <cfloat>
#include "AI/Navigation/NavClusterGraph.h"
#include "AI/Navigation/NavTypes.h"
#include "Core/Math/Random.h"
#include "Core/Threading/Sync.h"

class dtNavMesh;
class dtNavMeshQuery;

namespace Lumina
{
    struct SNavTileData;

    // World-space AABB of a loaded nav tile and its grid coords.
    struct FNavTileBounds
    {
        FVector3   Min = FVector3( FLT_MAX);
        FVector3   Max = FVector3(-FLT_MAX);
        int32       X = 0;
        int32       Y = 0;
    };

    // Snapshot of cached debug data sizes, cheap enough to call every frame.
    struct FNavDebugStats
    {
        int32 LoadedTiles    = 0;
        int32 Triangles      = 0;
        int32 BoundaryEdges  = 0;
        int32 OffMeshLinks   = 0;
    };

    // Runtime navmesh over dtNavMesh with an atomic-flag query pool, constructed on the main thread and queried from any.
    class RUNTIME_API FNavMesh
    {
    public:

        FNavMesh();
        ~FNavMesh();

        FNavMesh(const FNavMesh&) = delete;
        FNavMesh& operator=(const FNavMesh&) = delete;
        FNavMesh(FNavMesh&&) = delete;
        FNavMesh& operator=(FNavMesh&&) = delete;

        // Empty mesh sized for MaxResidentTiles, with tiles arriving through AddTile.
        bool Initialize(const FVector3& Origin, float TileWorldSize, int32 MaxResidentTiles, int32 MaxPolysPerTile);

        // Consumes Tiles and makes every non-empty one resident, so MaxResidentTiles must be at least that many.
        bool Initialize(const FVector3& Origin, float TileWorldSize, int32 MaxResidentTiles, int32 MaxPolysPerTile, TVector<SNavTileData>&& Tiles);

        void Shutdown();

        bool IsReady() const { return bReady; }

        bool ProjectPoint(const FVector3& World, const FVector3& Extents, const FNavQueryFilter& Filter, FVector3& Out) const;
        bool FindPath(const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FNavPath& Out) const;

        // Walks the surface from Start toward End, returning false when the query could not run, and Out.bHit says whether a wall stopped it.
        bool Raycast(const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, SNavRaycastResult& Out) const;

        bool FindRandomPoint(const FVector3& Center, float Radius, const FNavQueryFilter& Filter, FVector3& Out) const;

        // Consumes NewBlob, where empty removes the tile, and serializes against in-flight queries itself.
        bool RebuildTile(int32 TileX, int32 TileY, TVector<uint8>&& NewBlob);

        // One tile in a batched swap, where an empty blob removes the tile.
        struct FTileSwap
        {
            int32                 X = 0;
            int32                 Y = 0;
            const TVector<uint8>* Blob = nullptr;
        };

        // Every swap lands under one exclusive hold, so a batch waits on in-flight queries once. Returns how many landed.
        int32 RebuildTiles(const TVector<FTileSwap>& Swaps);

        // Pages a baked tile in, failing if the resident pool is full or the blob does not match this layout.
        bool AddTile(int32 TileX, int32 TileY, const TVector<uint8>& Blob);

        // Pages a tile out, returning true when the slot is free afterwards, including when it already was.
        bool RemoveTile(int32 TileX, int32 TileY);

        bool HasTile(int32 TileX, int32 TileY) const;

        int32 GetResidentTileCount() const;

        // Bumped whenever a tile is added or removed, so a follower holding an FNavPath can tell the ground moved under it.
        uint64 GetTopologyEpoch() const { return TopologyEpoch.load(std::memory_order_acquire); }

        // Starts a replacement mesh past the one it replaces, so a path or cache stamped against the old mesh sees a change.
        void ContinueTopologyEpochFrom(uint64 PreviousEpoch)
        {
            TopologyEpoch.store(Math::Max(TopologyEpoch.load(std::memory_order_acquire), PreviousEpoch + 1), std::memory_order_release);
        }

        // Y is ignored because a tile spans the whole bake, and a ring overflow reports changed.
        bool HasTileChangedSince(uint64 SinceEpoch, const FVector3& Min, const FVector3& Max) const;

        // Iterates the cached flat triangle list, skipping the dtNavMesh traversal cost.
        using FTriangleVisitor = TMoveOnlyFunction<void(const FVector3&, const FVector3&, const FVector3&, uint8)>;
        void ForEachTriangle(FTriangleVisitor Visitor) const;

        // Invoked concurrently, so the visitor has to be thread-safe.
        using FParallelTriangleVisitor = TFunction<void(const FVector3&, const FVector3&, const FVector3&, uint8)>;
        void ParallelForEachTriangle(const FParallelTriangleVisitor& Visitor) const;

        // Outer perimeter of each poly, excluding internal poly-to-poly edges.
        using FBoundaryEdgeVisitor = TFunction<void(const FVector3& A, const FVector3& B, uint8 Area)>;
        void ForEachBoundaryEdge(const FBoundaryEdgeVisitor& Visitor) const;

        // Off-mesh connection start and end pairs, such as jump links and teleporters.
        using FOffMeshLinkVisitor = TFunction<void(const FVector3& Start, const FVector3& End)>;
        void ForEachOffMeshLink(const FOffMeshLinkVisitor& Visitor) const;

        // Loaded tile world AABBs from dtMeshTile::header.
        using FTileBoundsVisitor = TFunction<void(const FNavTileBounds&)>;
        void ForEachLoadedTile(const FTileBoundsVisitor& Visitor) const;

        FNavDebugStats GetDebugStats() const;

        // Exact island labels for the long path planner, computed off the game thread after swaps and adopted in a brief exclusive hold.
        bool TryBeginClusterRelabel();
        void RelabelClusters();
        void AdoptClusterLabels();

        // Marks the debug draw caches stale, and rebuilding them walks every poly, so it waits for a reader.
        void InvalidateDebugCache();

    private:

        struct FQuerySlot
        {
            dtNavMeshQuery*                  Query = nullptr;
            std::atomic<bool>                Busy{ false };
            FNavClusterGraph::FSearchScratch ClusterScratch;
            FRandomStream                    Rng;

            FQuerySlot() = default;
            ~FQuerySlot() = default;
            FQuerySlot(const FQuerySlot&) = delete;
            FQuerySlot& operator=(const FQuerySlot&) = delete;

            // Relocating a pool slot has to carry the flag by hand, because an atomic is not movable.
            FQuerySlot(FQuerySlot&& Other) noexcept
                : Query(Other.Query)
                , Busy(Other.Busy.load(std::memory_order_relaxed))
                , ClusterScratch(Move(Other.ClusterScratch))
                , Rng(Other.Rng)
            {
                Other.Query = nullptr;
            }

            FQuerySlot& operator=(FQuerySlot&& Other) noexcept
            {
                Query = Other.Query;
                Busy.store(Other.Busy.load(std::memory_order_relaxed), std::memory_order_relaxed);
                ClusterScratch = Move(Other.ClusterScratch);
                Rng = Other.Rng;
                Other.Query = nullptr;
                return *this;
            }
        };

        struct FAcquiredQuery
        {
            FQuerySlot* Slot = nullptr;
            FAcquiredQuery() = default;
            explicit FAcquiredQuery(FQuerySlot* InSlot) : Slot(InSlot) {}
            ~FAcquiredQuery() { if (Slot) Slot->Busy.store(false, std::memory_order_release); }
            FAcquiredQuery(const FAcquiredQuery&) = delete;
            FAcquiredQuery& operator=(const FAcquiredQuery&) = delete;
            FAcquiredQuery(FAcquiredQuery&&) = delete;
            FAcquiredQuery& operator=(FAcquiredQuery&&) = delete;
            dtNavMeshQuery* Get() const { return Slot ? Slot->Query : nullptr; }
            explicit operator bool() const { return Slot != nullptr; }
        };

        FAcquiredQuery AcquireQuery() const;

        // A swap waiting on the topology turns new queries away until it lands, so a stream of them cannot hold it off.
        struct FQueryScope
        {
            const FNavMesh& Mesh;
            explicit FQueryScope(const FNavMesh& InMesh);
            ~FQueryScope();
            FQueryScope(const FQueryScope&) = delete;
            FQueryScope& operator=(const FQueryScope&) = delete;
            FQueryScope(FQueryScope&&) = delete;
            FQueryScope& operator=(FQueryScope&&) = delete;

            // True when a swap ran while the query stood aside, so every ref it held is suspect.
            bool YieldToSwap(uint64 Epoch) const;
        };

        struct FSwapScope
        {
            FNavMesh& Mesh;
            explicit FSwapScope(FNavMesh& InMesh);
            ~FSwapScope();
            FSwapScope(const FSwapScope&) = delete;
            FSwapScope& operator=(const FSwapScope&) = delete;
            FSwapScope(FSwapScope&&) = delete;
            FSwapScope& operator=(FSwapScope&&) = delete;
        };

        void WaitForSwaps() const;

        // All three assume TopologyLock is held exclusively, and Owned is a Detour allocation the mesh takes over.
        bool AddTileLocked(uint8* Owned, int32 Size);
        bool RemoveTileLocked(int32 TileX, int32 TileY);
        void RecordTileChangeLocked(int32 TileX, int32 TileY);

        // Rebuilds the debug caches if stale, on the main thread only like every reader of them.
        void EnsureDebugCache() const;

    private:

        dtNavMesh*                          NavMesh = nullptr;

        // Written only under the exclusive topology lock, alongside the tiles it describes.
        FNavClusterGraph                    Clusters;

        // addTile rewrites the links of up to eight neighbors, so tile mutation locks the whole mesh exclusively.
        mutable FSharedMutex                TopologyLock;
        std::atomic<uint32>                 PendingSwaps{ 0 };
        std::atomic<uint32>                 RelabelsInFlight{ 0 };

        std::atomic<uint64>                 TopologyEpoch{ 1 };

        // Keyed by epoch so a follower can ask whether a change touched its own corridor.
        struct FTileChange
        {
            uint64 Epoch = 0;
            int32  X = 0;
            int32  Y = 0;
        };
        static constexpr int32              ChangeRingSize = 256;
        FTileChange                         ChangeRing[ChangeRingSize] = {};

        // Mutable so const query API can flip Busy flags.
        mutable TVector<FQuerySlot>         QueryPool;

        // Debug draw only, so all of it is built on demand and mutable behind the const readers.
        mutable FMutex                      DebugCacheLock;
        mutable std::atomic<bool>           bDebugCacheDirty{ true };

        // Three verts and one area byte per triangle.
        mutable TVector<FVector3>          CachedTriVerts;
        mutable TVector<uint8>              CachedTriAreas;

        // Two verts and one area byte per poly outer perimeter edge.
        mutable TVector<FVector3>          CachedBoundaryVerts;
        mutable TVector<uint8>              CachedBoundaryAreas;

        // Start and End per link.
        mutable TVector<FVector3>          CachedOffMeshVerts;

        mutable TVector<FNavTileBounds>     CachedTileBounds;

        FVector3                           Origin = FVector3(0.0f);
        float                               TileWorldSize = 0.0f;
        bool                                bReady = false;
    };

    // Probes into the baked tile layout, so tests can assert it without including Detour.
    namespace NavMeshTesting
    {
        RUNTIME_API bool TileLinksAreAligned(const TVector<uint8>& Blob);
    }
}
