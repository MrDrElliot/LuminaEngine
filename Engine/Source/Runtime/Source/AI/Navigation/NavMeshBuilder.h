#pragma once

#include "AI/Navigation/NavTypes.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    /** World-space triangle snapshot. Areas is one ENavArea per tri (empty = all Ground). */
    struct FNavBuildInput
    {
        TVector<FVector3>      Vertices;
        TVector<uint32>         Indices;
        TVector<uint8>          Areas;          // optional, size = NumTris

        FVector3               BoundsMin = FVector3( FLT_MAX);
        FVector3               BoundsMax = FVector3(-FLT_MAX);

        FNavBuildSettings       Settings;
    };

    /** Passed to FNavMesh::Initialize. */
    struct FNavBuildOutput
    {
        FVector3               Origin = FVector3(0.0f);
        float                   TileWorldSize = 0.0f;
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
        /** Tile-parallel async bake; non-blocking. Cancel via Handle->bCancelRequested. */
        RUNTIME_API TUniquePtr<FNavBakeHandle> Bake(FNavBuildInput Input);

        /** Blocking variant; still parallelizes tile work internally. */
        RUNTIME_API bool BakeSync(FNavBuildInput Input, FNavBuildOutput& Out);

        /** Single-tile rebake aligned to BaseLayout's grid (used for hot-swap). */
        RUNTIME_API bool BakeSingleTile(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout, int32 TX, int32 TY, FNavTileData& Out);
    }
}
