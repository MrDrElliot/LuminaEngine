#pragma once

#include "AI/Navigation/NavMesh.h"
#include "AI/Navigation/NavMeshBuilder.h"
#include "AI/Navigation/NavTileStreamer.h"
#include "AI/Navigation/NavTypes.h"
#include "Memory/SmartPtr.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "NavMeshComponent.generated.h"

namespace Lumina
{

    // The footprint of a nav source that moved, appeared, vanished or changed shape.
    struct FNavSourceBounds
    {
        FVector3 Min = FVector3( FLT_MAX);
        FVector3 Max = FVector3(-FLT_MAX);

        // False for area volumes and links, which leave the voxelized geometry and so a cached heightfield untouched.
        bool     bGeometry = true;
    };

    /** Per-tile rebake task in flight. */
    struct FNavTileRebake
    {
        int32                       TileX = 0;
        int32                       TileY = 0;
        uint32                      CacheGeneration = 0;
        bool                        bCachePinned = false;
        TVector<uint8>              ResultBlob;
        std::atomic<bool>           bDone{ false };
        std::atomic<bool>           bConsumed{ false };
    };

    /** Off-main FNavMesh init (dtNavMesh::init + addTile + per-worker query alloc). */
    struct FNavInitJob
    {
        TUniquePtr<FNavMesh>        ResultMesh;
        std::atomic<bool>           bDone{ false };
    };

    struct FNavMeshRuntime
    {
        TUniquePtr<FNavMesh>            Mesh;

        // The highest topology epoch any mesh this runtime dropped reached, which its next mesh continues from.
        uint64                          RetiredEpoch = 0;

        /** Shared with the bake worker, so dropping this mid-bake leaves the worker a live handle. */
        TSharedPtr<FNavBakeHandle>      ActiveBake;

        /** Async hydration job; bDone -> consume ResultMesh, State -> Ready. */
        TSharedPtr<FNavInitJob>         PendingInit;

        ENavBakeState                   State = ENavBakeState::Idle;

        bool                            bRuntimeDirty = true;   // Tiles changed; rebuild next tick.

        // Changed source footprints inside the volume, turned into dirty tiles at the next scan.
        TVector<FNavSourceBounds>               PendingSourceChanges;

        /** Tile coords waiting to be rebuilt. */
        THashSet<uint64>                        DirtyTiles;

        // Dirtied by the latest scan and held for one more, so a source that leaves and returns across two scans rebakes its tiles once.
        THashSet<uint64>                        SettlingTiles;

        /** Shared with async coordinator so Teardown can't dangle in-flight workers. */
        TVector<TSharedPtr<FNavTileRebake>>     PendingRebakes;

        /** Layout fed to BakeSingleTile so coords align with live mesh. */
        FNavBuildOutput                         LiveLayout;

        // Shared with rebakes in flight, and dropped with every full bake since its heightfields assume the old layout.
        TSharedPtr<FNavTileCache>               TileCache;

        /** Decides which baked tiles are resident in Mesh. Inert while StreamLoadRadius is 0. */
        FNavTileStreamer                        Streamer;

        // Whether Mesh hydrated empty for the streamer to fill, rather than seeded with every tile.
        bool                                    bStreamedInit = false;

        /** Entity world scale, mirrored each tick; multiplies Extents into the effective bake volume. */
        FVector3                                WorldScale = FVector3(1.0f);

        /** Auto-bake debounce: bake once bounds/settings settle and differ from what's baked. */
        FVector3                                AutoPrevCenter   = FVector3(FLT_MAX);
        FVector3                                AutoPrevExtents  = FVector3(FLT_MAX);
        SNavBuildSettings                       AutoPrevSettings;
        FVector3                                AutoBuiltCenter  = FVector3(FLT_MAX);
        FVector3                                AutoBuiltExtents = FVector3(FLT_MAX);
        SNavBuildSettings                       AutoBuiltSettings;
        float                                   AutoSettleTimer  = 0.0f;
        bool                                    bAutoBuiltValid  = false;

        // Throttles the geometry change scan to DynamicRebuildInterval.
        float                                   DynamicScanTimer = 0.0f;

        // Ticks since the exact island labels were last asked for, so a mesh that keeps changing relabels at a measured pace.
        uint32                                  TicksSinceRelabel = 0;

        /** The debug surface is hundreds of thousands of vertices, so it is rebuilt only when the mesh
         *  topology or its coloring changes rather than once per frame. */
        TVector<FSimpleElementVertex>           DebugSurface;
        uint64                                  DebugSurfaceEpoch = 0;
        float                                   DebugSurfaceAlpha = -1.0f;
        bool                                    bDebugSurfaceByArea = false;
        bool                                    bDebugSurfaceValid = false;
    };

    /** Bake volume (world AABB at Center +/- Extents); multiple components union at bake time. */
    REFLECT(Component, Category = "AI")
    struct RUNTIME_API SNavMeshComponent
    {
        GENERATED_BODY()

        SNavMeshComponent() = default;

        ~SNavMeshComponent() = default;
        SNavMeshComponent(const SNavMeshComponent& Other)
            : Agent(Other.Agent)
            , Settings(Other.Settings)
            , bAutoBake(Other.bAutoBake)
            , bDynamicRebuild(Other.bDynamicRebuild)
            , DynamicRebuildInterval(Other.DynamicRebuildInterval)
            , Center(Other.Center)
            , Extents(Other.Extents)
            , Tiles(Other.Tiles)
            , Origin(Other.Origin)
            , TileWorldSize(Other.TileWorldSize)
            , TilesX(Other.TilesX)
            , TilesY(Other.TilesY)
            , MaxPolysPerTile(Other.MaxPolysPerTile)
        {
        }

        SNavMeshComponent& operator=(const SNavMeshComponent& Other)
        {
            if (this != &Other)
            {
                Agent                  = Other.Agent;
                Settings               = Other.Settings;
                bAutoBake              = Other.bAutoBake;
                bDynamicRebuild        = Other.bDynamicRebuild;
                DynamicRebuildInterval = Other.DynamicRebuildInterval;
                Center                 = Other.Center;
                Extents                = Other.Extents;
                Tiles                  = Other.Tiles;
                Origin                 = Other.Origin;
                TileWorldSize          = Other.TileWorldSize;
                TilesX                 = Other.TilesX;
                TilesY                 = Other.TilesY;
                MaxPolysPerTile        = Other.MaxPolysPerTile;
                Runtime                = FNavMeshRuntime{};
            }
            return *this;
        }

        SNavMeshComponent(SNavMeshComponent&&) noexcept            = default;
        SNavMeshComponent& operator=(SNavMeshComponent&&) noexcept = default;

        // Which agents path on this volume, so a vehicle mesh baked wider sits beside the infantry one and queries naming it find it.
        PROPERTY(Editable, Category = "NavMesh")
        FName Agent;

        /** Voxelization, agent, and tiling parameters fed to Recast. */
        PROPERTY(Editable, Category = "NavMesh|Build")
        SNavBuildSettings Settings;

        /** Re-bake automatically when the bounds/settings change (e.g. placed or moved in the editor). */
        PROPERTY(Editable, Category = "NavMesh|Build")
        bool bAutoBake = true;

        // Rebakes only the tiles under source geometry that moved, changed shape, appeared or disappeared.
        PROPERTY(Editable, Category = "NavMesh|Dynamic")
        bool bDynamicRebuild = true;

        // Seconds between geometry change scans, where zero scans every tick and walks every collider.
        PROPERTY(Editable, Category = "NavMesh|Dynamic", Units = "s", ClampMin = 0.0f)
        float DynamicRebuildInterval = 0.25f;

        /** World-space center of the bake volume. */
        PROPERTY(Editable, Category = "NavMesh|Bounds")
        FVector3 Center = FVector3(0.0f);

        /** Half-extents of the bake volume. */
        PROPERTY(Editable, Category = "NavMesh|Bounds")
        FVector3 Extents = FVector3(64.0f, 16.0f, 64.0f);

        /** Editor "Bake" button flag; consumed next tick by SNavMeshSystem. */
        bool bBakeRequested = false;

        /** Baked tile blobs (empty = non-walkable tile). Serialized. */
        PROPERTY(Hidden, Category = "NavMesh|Baked")
        TVector<SNavTileData> Tiles;

        /** Bake origin (= bounds min). */
        PROPERTY(Hidden, Category = "NavMesh|Baked")
        FVector3 Origin = FVector3(0.0f);

        /** Tile size in world units. */
        PROPERTY(Hidden, Category = "NavMesh|Baked")
        float TileWorldSize = 0.0f;

        /** Grid the baked tile coords index into. Serialized so a hot rebake still lines up after the
         *  volume is moved or rescaled without a full re-bake. */
        PROPERTY(Hidden, Category = "NavMesh|Baked")
        int32 TilesX = 0;

        PROPERTY(Hidden, Category = "NavMesh|Baked")
        int32 TilesY = 0;

        /** Cap fed to dtNavMeshParams. */
        PROPERTY(Hidden, Category = "NavMesh|Baked")
        int32 MaxPolysPerTile = 0;

        /** Transient; not serialized. */
        FNavMeshRuntime Runtime;

        /** Request an async rebuild of this volume; consumed next tick by SNavMeshSystem. Script entry point
         *  for rebuilding navigation after spawning/moving geometry at runtime. */
        FUNCTION()
        void RequestRebuild() { bBakeRequested = true; }

        /** True once a baked navmesh has finished hydrating and can answer queries. */
        FUNCTION()
        bool IsNavMeshReady() const { return Runtime.Mesh != nullptr && Runtime.Mesh->IsReady(); }

        /** True if baked tile data is present (independent of runtime hydration). */
        FUNCTION()
        bool HasNavData() const { return HasBakedData(); }

        bool HasBakedData() const { return !Tiles.IsEmpty() && TileWorldSize > 0.0f; }

        /** Effective world half-extents: authored Extents scaled by the entity transform. */
        FVector3 GetWorldExtents() const { return Extents * Runtime.WorldScale; }
    };
}
