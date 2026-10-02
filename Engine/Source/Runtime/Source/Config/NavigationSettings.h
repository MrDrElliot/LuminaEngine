#pragma once

#include "AI/Navigation/NavTypes.h"
#include "Config/DeveloperSettings.h"
#include "NavigationSettings.generated.h"

namespace Lumina
{
    /** Navigation policy that applies to every world in the project, as opposed to the bake volume and
     *  agent parameters authored per SNavMeshComponent. Project-scoped rather than per-user: the tile
     *  budget a world is designed around is a shipping decision and travels with the content. */
    REFLECT(MinimalAPI, ConfigFile = "/Config/GameSettings.json", DisplayName = "Navigation", Category = "Engine")
    class CNavigationSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Tiles within this distance of a streaming focus point stay resident. Zero keeps the whole
         *  baked navmesh loaded, which is what a level that fits in the tile budget wants. */
        PROPERTY(Editable, Category = "Streaming", ClampMin = 0.0f)
        float StreamLoadRadius = 0.0f;

        /** Hard ceiling on tiles resident in Detour at once. Fixed when the navmesh hydrates, so a
         *  change needs a re-hydrate to take effect. */
        PROPERTY(Editable, Category = "Streaming", ClampMin = 1)
        int32 ResidentTileBudget = 4096;

        /** Resident tiles survive out to StreamLoadRadius times this, so a focus point sitting on a
         *  tile boundary does not page the same tile in and out every frame. */
        PROPERTY(Editable, Category = "Streaming", ClampMin = 1.0f)
        float KeepRadiusScale = 1.25f;

        /** Tile add/remove operations per tick. Each one briefly excludes navmesh queries, so this
         *  bounds the worst-case stall a streaming frame can impose. */
        PROPERTY(Editable, Category = "Streaming", ClampMin = 1)
        int32 MaxTileOpsPerTick = 8;

        /** Refuses a bake larger than this many tiles. A huge tile count is one Recast pipeline each
         *  and reads as a hang, so it fails with a message naming the knobs instead. */
        PROPERTY(Editable, Category = "Bake", ClampMin = 1)
        int32 MaxBakeTiles = 65536;

        /** Concurrent single-tile rebakes kicked per component per tick when geometry moves. */
        PROPERTY(Editable, Category = "Bake", ClampMin = 1)
        int32 MaxConcurrentTileRebakes = 8;

        // Region partition for every volume left on Project Default, where layers bake a tile about a quarter faster than watershed.
        PROPERTY(Editable, Category = "Bake")
        ENavRegionPartition RegionPartition = ENavRegionPartition::Layers;

        /** Longest polygon corridor a single FindPath may walk. Raising it costs stack in the query. */
        PROPERTY(Editable, Category = "Query", ClampMin = 16, ClampMax = 1024)
        int32 MaxPathPolys = 512;

        /** Corners a straightened path may return. A path needing more comes back flagged truncated. */
        PROPERTY(Editable, Category = "Query", ClampMin = 8, ClampMax = 512)
        int32 MaxPathCorners = 256;

        /** Search nodes each pooled query allocates. Bounds how far a single path may explore. */
        PROPERTY(Editable, Category = "Query", ClampMin = 64)
        int32 QueryNodePoolSize = 2048;

        /** Pooled dtNavMeshQuery objects beyond one per worker. Each costs its node pool, and running
         *  out makes a query fail rather than wait, so a little slack is cheaper than the alternative. */
        PROPERTY(Editable, Category = "Query", ClampMin = 0)
        int32 QueryPoolSlack = 2;
    };
}
