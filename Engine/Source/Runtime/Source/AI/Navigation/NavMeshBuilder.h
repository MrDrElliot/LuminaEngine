#pragma once

#include <cfloat>
#include "AI/Navigation/NavTypes.h"
#include "Memory/SmartPtr.h"
#include "Containers/Function.h"

namespace Lumina
{
    /** World-space triangle snapshot. Areas is one ENavArea per tri (empty = all Ground). */
    struct FNavBuildInput
    {
        TVector<FVector3>      Vertices;
        TVector<uint32>         Indices;
        TVector<uint8>          Areas;          // optional, size = NumTris

        /** Stamped onto the eroded surface before region building, in order. */
        TVector<FNavAreaVolume> AreaVolumes;

        /** Off-mesh connections; each tile keeps the ones whose start point falls inside it. */
        TVector<FNavOffMeshLink> Links;

        FVector3               BoundsMin = FVector3( FLT_MAX);
        FVector3               BoundsMax = FVector3(-FLT_MAX);

        FNavBuildSettings       Settings;
    };

    /** Passed to FNavMesh::Initialize. */
    struct FNavBuildOutput
    {
        FVector3               Origin = FVector3(0.0f);
        float                   TileWorldSize = 0.0f;

        /** Grid the tile coords were baked against. Recomputing these from live bounds misplaces a
         *  hot rebake as soon as the volume is moved or rescaled without a full re-bake. */
        int32                   TilesX = 0;
        int32                   TilesY = 0;

        int32                   MaxTiles = 0;
        int32                   MaxPolysPerTile = 0;
        TVector<FNavTileData>   Tiles;
    };

    /** Async bake handle; cancellation is cooperative. */
    struct FNavBakeHandle
    {
        std::atomic<uint32>     TilesCompleted{ 0 };
        uint32                  TilesScheduled = 0;
        std::atomic<bool>       bDone{ false };
        std::atomic<bool>       bCancelRequested{ false };
        FNavBuildOutput         Output;

        float Progress() const
        {
            if (TilesScheduled == 0) return 1.0f;
            return (float)TilesCompleted.load(std::memory_order_acquire) / (float)TilesScheduled;
        }
    };

    namespace NavMeshBuilder
    {
        /** Tile-parallel async bake; non-blocking. Cancel via Handle->bCancelRequested. The worker keeps its own reference. */
        RUNTIME_API TSharedPtr<FNavBakeHandle> Bake(FNavBuildInput Input);

        // Prepare runs on the bake worker first, so geometry emission never holds the game thread.
        RUNTIME_API TSharedPtr<FNavBakeHandle> Bake(FNavBuildInput Input, TFunction<void(FNavBuildInput&)> Prepare);

        /** Blocking variant; still parallelizes tile work internally. */
        RUNTIME_API bool BakeSync(FNavBuildInput Input, FNavBuildOutput& Out);

        /** Single-tile rebake aligned to BaseLayout's grid (used for hot-swap). */
        RUNTIME_API bool BakeSingleTile(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout, int32 TX, int32 TY, FNavTileData& Out);

        /** Batch rebake aligned to BaseLayout's grid. Bins the input once and bakes the tiles in
         *  parallel, so a batch costs one pass over the geometry rather than one per tile.
         *  Out is resized to Coords; a tile that failed to bake is left with an empty blob. */
        RUNTIME_API void BakeTiles(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout,
                                   const TVector<FNavTileCoord>& Coords, TVector<FNavTileData>& Out);
    }
}
