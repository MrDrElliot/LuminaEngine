#pragma once

#include "AI/Navigation/NavMesh.h"
#include "AI/Navigation/NavMeshBuilder.h"
#include "AI/Navigation/NavTypes.h"
#include "Memory/SmartPtr.h"
#include "NavMeshComponent.generated.h"

namespace Lumina
{

    /** Cached collider AABB for change detection; key = (entity << 8 | colliderType). */
    struct FNavSourceEntity
    {
        FVector3   AABBMin = FVector3( FLT_MAX);
        FVector3   AABBMax = FVector3(-FLT_MAX);
    };

    /** Per-tile rebake task in flight. */
    struct FNavTileRebake
    {
        int32                       TileX = 0;
        int32                       TileY = 0;
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
        TUniquePtr<FNavBakeHandle>      ActiveBake;

        /** Async hydration job; bDone -> consume ResultMesh, State -> Ready. */
        TSharedPtr<FNavInitJob>         PendingInit;

        ENavBakeState                   State = ENavBakeState::Idle;

        bool                            bRuntimeDirty = true;   // Tiles changed; rebuild next tick.

        /** Cached per-collider AABBs from previous tick. */
        THashMap<uint64, FNavSourceEntity>      EntityAABBs;

        /** Tile coords waiting to be rebuilt. */
        THashSet<uint64>                        DirtyTiles;

        /** Shared with async coordinator so Teardown can't dangle in-flight workers. */
        TVector<TSharedPtr<FNavTileRebake>>     PendingRebakes;

        /** Layout fed to BakeSingleTile so coords align with live mesh. */
        FNavBuildOutput                         LiveLayout;

        int32                                   TilesX = 0;
        int32                                   TilesY = 0;

        /** Entity world scale, mirrored each tick; multiplies Extents into the effective bake volume. */
        FVector3                                WorldScale = FVector3(1.0f);

        /** Auto-bake debounce: bake once bounds/settings settle and differ from what's baked. */
        FVector3                                AutoPrevCenter   = FVector3(FLT_MAX);
        FVector3                                AutoPrevExtents  = FVector3(FLT_MAX);
        FNavBuildSettings                       AutoPrevSettings;
        FVector3                                AutoBuiltCenter  = FVector3(FLT_MAX);
        FVector3                                AutoBuiltExtents = FVector3(FLT_MAX);
        FNavBuildSettings                       AutoBuiltSettings;
        float                                   AutoSettleTimer  = 0.0f;
        bool                                    bAutoBuiltValid  = false;
    };

    /** Bake volume (world AABB at Center +/- Extents); multiple components union at bake time. */
    REFLECT(Component, Category = "AI")
    struct RUNTIME_API SNavMeshComponent
    {
        GENERATED_BODY()

        SNavMeshComponent() = default;

        SNavMeshComponent(const SNavMeshComponent& Other)
            : Settings(Other.Settings)
            , bAutoBake(Other.bAutoBake)
            , Center(Other.Center)
            , Extents(Other.Extents)
            , Tiles(Other.Tiles)
            , Origin(Other.Origin)
            , TileWorldSize(Other.TileWorldSize)
            , MaxPolysPerTile(Other.MaxPolysPerTile)
        {
        }

        SNavMeshComponent& operator=(const SNavMeshComponent& Other)
        {
            if (this != &Other)
            {
                Settings        = Other.Settings;
                bAutoBake       = Other.bAutoBake;
                Center          = Other.Center;
                Extents         = Other.Extents;
                Tiles           = Other.Tiles;
                Origin          = Other.Origin;
                TileWorldSize   = Other.TileWorldSize;
                MaxPolysPerTile = Other.MaxPolysPerTile;
                Runtime         = FNavMeshRuntime{};
            }
            return *this;
        }

        SNavMeshComponent(SNavMeshComponent&&) noexcept            = default;
        SNavMeshComponent& operator=(SNavMeshComponent&&) noexcept = default;

        /** Voxelization, agent, and tiling parameters fed to Recast. */
        PROPERTY(Editable, Category = "NavMesh|Build")
        FNavBuildSettings Settings;

        /** Re-bake automatically when the bounds/settings change (e.g. placed or moved in the editor). */
        PROPERTY(Editable, Category = "NavMesh|Build")
        bool bAutoBake = true;

        /** World-space center of the bake volume. */
        PROPERTY(Editable, Category = "NavMesh|Bounds")
        FVector3 Center = FVector3(0.0f);

        /** Half-extents of the bake volume. */
        PROPERTY(Editable, Category = "NavMesh|Bounds")
        FVector3 Extents = FVector3(64.0f, 16.0f, 64.0f);

        /** Editor "Bake" button flag; consumed next tick by SNavMeshSystem. */
        bool bBakeRequested = false;

        /** Baked tile blobs (empty = non-walkable tile). Serialized. */
        PROPERTY(Category = "NavMesh|Baked")
        TVector<FNavTileData> Tiles;

        /** Bake origin (= bounds min). */
        PROPERTY(Category = "NavMesh|Baked")
        FVector3 Origin = FVector3(0.0f);

        /** Tile size in world units. */
        PROPERTY(Category = "NavMesh|Baked")
        float TileWorldSize = 0.0f;

        /** Cap fed to dtNavMeshParams. */
        PROPERTY(Category = "NavMesh|Baked")
        int32 MaxPolysPerTile = 0;

        /** Transient; not serialized. */
        FNavMeshRuntime Runtime;

        /** Request an async rebuild of this volume; consumed next tick by SNavMeshSystem. Script entry point
         *  for rebuilding navigation after spawning/moving geometry at runtime. */
        FUNCTION(Script)
        void RequestRebuild() { bBakeRequested = true; }

        /** True once a baked navmesh has finished hydrating and can answer queries. */
        FUNCTION(Script)
        bool IsNavMeshReady() const { return Runtime.Mesh != nullptr && Runtime.Mesh->IsReady(); }

        /** True if baked tile data is present (independent of runtime hydration). */
        FUNCTION(Script)
        bool HasNavData() const { return HasBakedData(); }

        bool HasBakedData() const { return !Tiles.empty() && TileWorldSize > 0.0f; }

        /** Effective world half-extents: authored Extents scaled by the entity transform. */
        FVector3 GetWorldExtents() const { return Extents * Runtime.WorldScale; }
    };
}
