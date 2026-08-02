#pragma once

#include "AI/Navigation/NavTypes.h"

class dtNavMesh;
class dtNavMeshQuery;

namespace Lumina
{
    struct FNavTileData;

    /** World-space AABB of a loaded nav tile + its grid coords. */
    struct FNavTileBounds
    {
        FVector3   Min = FVector3( FLT_MAX);
        FVector3   Max = FVector3(-FLT_MAX);
        int32       X = 0;
        int32       Y = 0;
    };

    /** Snapshot of cached debug data sizes. Cheap; safe to call every frame. */
    struct FNavDebugStats
    {
        int32 LoadedTiles    = 0;
        int32 Triangles      = 0;
        int32 BoundaryEdges  = 0;
        int32 OffMeshLinks   = 0;
    };

    /** Runtime navmesh: dtNavMesh + atomic-flag CAS query pool. Construct on main thread; query from any. */
    class RUNTIME_API FNavMesh
    {
    public:

        FNavMesh();
        ~FNavMesh();

        FNavMesh(const FNavMesh&) = delete;
        FNavMesh& operator=(const FNavMesh&) = delete;

        /** Tiles is consumed; each blob is copied into Detour memory. */
        bool Initialize(const FVector3& Origin, float TileWorldSize, int32 MaxTiles, int32 MaxPolysPerTile, TVector<FNavTileData>&& Tiles);

        void Shutdown();

        bool IsReady() const { return bReady; }

        bool ProjectPoint(const FVector3& World, const FVector3& Extents, const FNavQueryFilter& Filter, FVector3& Out) const;
        bool FindPath(const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FNavPath& Out) const;
        bool Raycast(const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FVector3& HitOut) const;

        bool FindRandomPoint(const FVector3& Center, float Radius, const FNavQueryFilter& Filter, FVector3& Out) const;

        /** NewBlob is consumed; empty removes the tile. Caller must serialize against in-flight queries on this tile. */
        bool RebuildTile(int32 TileX, int32 TileY, TVector<uint8>&& NewBlob);

        /** Iterates the cached flat triangle list (skip the dtNavMesh traversal cost). */
        using FTriangleVisitor = TMoveOnlyFunction<void(const FVector3&, const FVector3&, const FVector3&, uint8)>;
        void ForEachTriangle(FTriangleVisitor Visitor) const;

        /** Visitor MUST be thread-safe; invoked concurrently. */
        using FParallelTriangleVisitor = TFunction<void(const FVector3&, const FVector3&, const FVector3&, uint8)>;
        void ParallelForEachTriangle(const FParallelTriangleVisitor& Visitor) const;

        /** Outer perimeter of each poly (neis[edge]==0). Excludes internal poly-to-poly edges. */
        using FBoundaryEdgeVisitor = TFunction<void(const FVector3& A, const FVector3& B, uint8 Area)>;
        void ForEachBoundaryEdge(const FBoundaryEdgeVisitor& Visitor) const;

        /** Off-mesh connection start/end pairs (jump links, teleporters, etc.). */
        using FOffMeshLinkVisitor = TFunction<void(const FVector3& Start, const FVector3& End)>;
        void ForEachOffMeshLink(const FOffMeshLinkVisitor& Visitor) const;

        /** Loaded tile world-AABBs from dtMeshTile::header. */
        using FTileBoundsVisitor = TFunction<void(const FNavTileBounds&)>;
        void ForEachLoadedTile(const FTileBoundsVisitor& Visitor) const;

        FNavDebugStats GetDebugStats() const;

        /** Auto-called by Initialize and RebuildTile. Read-only on dtNavMesh. */
        void RefreshTriangleCache();

    private:

        struct FQuerySlot
        {
            dtNavMeshQuery*     Query = nullptr;
            std::atomic<bool>   Busy{ false };
        };

        struct FAcquiredQuery
        {
            FQuerySlot* Slot = nullptr;
            ~FAcquiredQuery() { if (Slot) Slot->Busy.store(false, std::memory_order_release); }
            dtNavMeshQuery* Get() const { return Slot ? Slot->Query : nullptr; }
            explicit operator bool() const { return Slot != nullptr; }
        };

        FAcquiredQuery AcquireQuery() const;

    private:

        dtNavMesh*                          NavMesh = nullptr;

        // Mutable so const query API can flip Busy flags.
        mutable TVector<FQuerySlot>         QueryPool;

        // Flat cache: 3 vec3 per tri in Verts; 1 area byte per tri.
        TVector<FVector3>                  CachedTriVerts;
        TVector<uint8>                      CachedTriAreas;

        // 2 vec3 per edge in Verts; 1 area byte per edge. Boundary = poly outer perimeter.
        TVector<FVector3>                  CachedBoundaryVerts;
        TVector<uint8>                      CachedBoundaryAreas;

        // 2 vec3 per link (Start, End).
        TVector<FVector3>                  CachedOffMeshVerts;

        TVector<FNavTileBounds>             CachedTileBounds;

        FVector3                           Origin = FVector3(0.0f);
        float                               TileWorldSize = 0.0f;
        bool                                bReady = false;
    };
}
