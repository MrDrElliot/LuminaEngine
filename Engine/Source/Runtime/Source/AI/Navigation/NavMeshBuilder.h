#pragma once

#include <cfloat>
#include "AI/Navigation/NavTypes.h"
#include "Memory/SmartPtr.h"
#include "Containers/Function.h"
#include "Core/Threading/Sync.h"

struct rcCompactHeightfield;

namespace Lumina
{
    class FNavBuildPool;

    // Tiles rebaked lately, kept as their eroded heightfield, so a tile whose geometry has not changed only restamps its areas and links.
    class RUNTIME_API FNavTileCache
    {
    public:

        explicit FNavTileCache(size_t InBudgetBytes);
        ~FNavTileCache();

        FNavTileCache(const FNavTileCache&) = delete;
        FNavTileCache& operator=(const FNavTileCache&) = delete;
        FNavTileCache(FNavTileCache&&) = delete;
        FNavTileCache& operator=(FNavTileCache&&) = delete;

        // Geometry under the tile changed, so whatever is held for it no longer counts.
        void Invalidate(int32 X, int32 Y);
        uint32 GetGeneration(int32 X, int32 Y) const;

        // Held for a bake that gathers no geometry, so the budget cannot evict the entry before the bake reads it.
        bool Pin(int32 X, int32 Y, uint32 Generation);
        void Unpin(int32 X, int32 Y);

        // Out is a fresh copy owned by the caller, freed with rcFreeCompactHeightfield.
        bool Load(int32 X, int32 Y, uint32 Generation, rcCompactHeightfield*& Out);
        void Store(int32 X, int32 Y, uint32 Generation, const rcCompactHeightfield& Compact);

        int32 GetHits() const { return Hits.load(std::memory_order_relaxed); }
        int32 GetMisses() const { return Misses.load(std::memory_order_relaxed); }

    private:

        struct FEntry;

        mutable FMutex                       Lock;
        THashMap<uint64, TUniquePtr<FEntry>> Entries;
        THashMap<uint64, uint32>             Generations;
        size_t                               BudgetBytes = 0;
        size_t                               Bytes = 0;
        uint64                               Clock = 0;
        std::atomic<int32>                   Hits{ 0 };
        std::atomic<int32>                   Misses{ 0 };
    };

    // World-space triangle snapshot, with one ENavArea per tri in Areas or all Ground when it is empty.
    struct FNavBuildInput
    {
        TVector<FVector3>      Vertices;
        TVector<uint32>         Indices;
        TVector<uint8>          Areas;          // optional, size = NumTris

        // Stamped onto the eroded surface before region building, in order.
        TVector<FNavAreaVolume> AreaVolumes;

        // Each tile keeps the off-mesh links whose start point falls inside it.
        TVector<FNavOffMeshLink> Links;

        FVector3               BoundsMin = FVector3( FLT_MAX);
        FVector3               BoundsMax = FVector3(-FLT_MAX);

        SNavBuildSettings       Settings;
    };

    // Passed to FNavMesh::Initialize.
    struct FNavBuildOutput
    {
        FVector3               Origin = FVector3(0.0f);
        float                   TileWorldSize = 0.0f;

        // Grid the tile coords were baked against, since live bounds misplace a hot rebake once the volume moves or rescales.
        int32                   TilesX = 0;
        int32                   TilesY = 0;

        int32                   MaxTiles = 0;
        int32                   MaxPolysPerTile = 0;
        TVector<SNavTileData>   Tiles;
    };

    // Async bake handle with cooperative cancellation.
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
        // Tile-parallel async bake that never blocks, canceled via Handle->bCancelRequested, and the worker keeps its own reference.
        RUNTIME_API TSharedPtr<FNavBakeHandle> Bake(FNavBuildInput Input);

        // Prepare runs on the bake worker first, so geometry emission never holds the game thread.
        RUNTIME_API TSharedPtr<FNavBakeHandle> Bake(FNavBuildInput Input, TFunction<void(FNavBuildInput&)> Prepare);

        // Blocking variant that still parallelizes tile work internally.
        RUNTIME_API bool BakeSync(FNavBuildInput Input, FNavBuildOutput& Out);

        // Single-tile hot-swap rebake aligned to BaseLayout's grid.
        RUNTIME_API bool BakeSingleTile(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout, int32 TX, int32 TY, SNavTileData& Out);

        // One binning pass feeds every tile in Coords, on Pool's threads when given one, and a tile that fails keeps an empty blob.
        RUNTIME_API void BakeTiles(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout, const TVector<FNavTileCoord>& Coords,
                                   TVector<SNavTileData>& Out, FNavBuildPool* Pool = nullptr, FNavTileCache* Cache = nullptr);
    }
}
