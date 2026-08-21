#include "RuntimePCH.h"
#include "NavMeshBuilder.h"

#include "Memory/SmartPtr.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "TaskSystem/TaskSystem.h"

#if defined(LUMINA_HAS_RECAST)
    #include <Recast.h>
    #include <RecastAlloc.h>
    #include <DetourNavMesh.h>
    #include <DetourNavMeshBuilder.h>
    #include <DetourAlloc.h>
#include "Log/Log.h"
#endif

namespace Lumina::NavMeshBuilder
{
    namespace
    {
#if defined(LUMINA_HAS_RECAST)
        // The Set functions only store function pointers, so a static initializer is safe.
        void* RecastAlloc(size_t Size, rcAllocHint) { LUMINA_MEMORY_SCOPE("Navigation"); return Memory::Malloc(Size); }
        void  RecastFree(void* Ptr)                 { if (Ptr) { Memory::Free(Ptr); } }
        void* DetourAlloc(size_t Size, dtAllocHint) { LUMINA_MEMORY_SCOPE("Navigation"); return Memory::Malloc(Size); }
        void  DetourFree(void* Ptr)                 { if (Ptr) { Memory::Free(Ptr); } }
        const bool GNavAllocatorsSet = []
        {
            rcAllocSetCustom(RecastAlloc, RecastFree);
            dtAllocSetCustom(DetourAlloc, DetourFree);
            return true;
        }();
#endif

        struct FTileGrid
        {
            FVector3   Origin;             // BoundsMin
            float       TileWorldSize;      // TileSizeVoxels * CellSize
            float       BorderSize;         // world units of per-tile expand for seam consistency
            int32       TilesX = 0;
            int32       TilesY = 0;         // nav-grid Y = world Z
        };

        FTileGrid ComputeGrid(const FNavBuildInput& In)
        {
            FTileGrid Grid{};
            Grid.Origin = In.BoundsMin;
            Grid.TileWorldSize = (float)In.Settings.TileSizeVoxels * In.Settings.CellSize;
            // AgentRadius/CellSize + 3 is the Recast convention; tiles need a voxel border for seam triangles.
            const int32 BorderVoxels = (int32)std::ceil(In.Settings.AgentRadius / In.Settings.CellSize) + 3;
            Grid.BorderSize = (float)BorderVoxels * In.Settings.CellSize;
            const FVector3 Span = In.BoundsMax - In.BoundsMin;
            Grid.TilesX = (int32)std::ceil(Math::Max(Span.x, 0.0f) / Grid.TileWorldSize);
            Grid.TilesY = (int32)std::ceil(Math::Max(Span.z, 0.0f) / Grid.TileWorldSize);
            Grid.TilesX = Math::Max(Grid.TilesX, 1);
            Grid.TilesY = Math::Max(Grid.TilesY, 1);
            return Grid;
        }

        // Rasterizing the full input per tile grinds to a halt, while binning makes each tile local.
        struct FTileBins
        {
            TVector<uint32> Offsets;     // size TileCount + 1; tile t owns [Offsets[t], Offsets[t+1])
            TVector<int32>  TriIndices;  // flattened global triangle indices
        };

        // Grown by BorderSize to match BakeTile's expanded bounds.
        void TriTileRange(const FNavBuildInput& In, const FTileGrid& Grid, int32 Tri, int32& TX0, int32& TY0, int32& TX1, int32& TY1)
        {
            const FVector3& A = In.Vertices[In.Indices[Tri * 3 + 0]];
            const FVector3& B = In.Vertices[In.Indices[Tri * 3 + 1]];
            const FVector3& C = In.Vertices[In.Indices[Tri * 3 + 2]];
            const float MinX = Math::Min(A.x, Math::Min(B.x, C.x)) - Grid.BorderSize;
            const float MaxX = Math::Max(A.x, Math::Max(B.x, C.x)) + Grid.BorderSize;
            const float MinZ = Math::Min(A.z, Math::Min(B.z, C.z)) - Grid.BorderSize;
            const float MaxZ = Math::Max(A.z, Math::Max(B.z, C.z)) + Grid.BorderSize;
            TX0 = Math::Clamp((int32)std::floor((MinX - Grid.Origin.x) / Grid.TileWorldSize), 0, Grid.TilesX - 1);
            TX1 = Math::Clamp((int32)std::floor((MaxX - Grid.Origin.x) / Grid.TileWorldSize), 0, Grid.TilesX - 1);
            TY0 = Math::Clamp((int32)std::floor((MinZ - Grid.Origin.z) / Grid.TileWorldSize), 0, Grid.TilesY - 1);
            TY1 = Math::Clamp((int32)std::floor((MaxZ - Grid.Origin.z) / Grid.TileWorldSize), 0, Grid.TilesY - 1);
        }

        void BuildTileBins(const FNavBuildInput& In, const FTileGrid& Grid, FTileBins& Out)
        {
            const int32 TileCount = Grid.TilesX * Grid.TilesY;
            const int32 NumTris   = (int32)(In.Indices.size() / 3);
            Out.Offsets.assign((size_t)TileCount + 1, 0u);
            if (NumTris == 0 || In.Vertices.empty())
            {
                return;
            }

            // A counting sort, so tally per tile, prefix-sum into offsets, then scatter.
            for (int32 t = 0; t < NumTris; ++t)
            {
                int32 TX0, TY0, TX1, TY1;
                TriTileRange(In, Grid, t, TX0, TY0, TX1, TY1);
                for (int32 ty = TY0; ty <= TY1; ++ty)
                {
                    for (int32 tx = TX0; tx <= TX1; ++tx)
                    {
                        ++Out.Offsets[(size_t)(ty * Grid.TilesX + tx) + 1];
                    }
                }
            }
            for (int32 i = 0; i < TileCount; ++i)
            {
                Out.Offsets[i + 1] += Out.Offsets[i];
            }

            Out.TriIndices.resize(Out.Offsets[TileCount]);
            TVector<uint32> Cursor(Out.Offsets.begin(), Out.Offsets.begin() + TileCount);
            for (int32 t = 0; t < NumTris; ++t)
            {
                int32 TX0, TY0, TX1, TY1;
                TriTileRange(In, Grid, t, TX0, TY0, TX1, TY1);
                for (int32 ty = TY0; ty <= TY1; ++ty)
                {
                    for (int32 tx = TX0; tx <= TX1; ++tx)
                    {
                        Out.TriIndices[Cursor[(size_t)(ty * Grid.TilesX + tx)]++] = t;
                    }
                }
            }
        }

#if defined(LUMINA_HAS_RECAST)
        // Pure over its inputs, so it is safe to call concurrently.
        bool BakeTile(const FNavBuildInput& In, const FTileGrid& Grid, int32 TX, int32 TY, const int32* TileTriIndices, int32 NumTileTris, FNavTileData& Out)
        {
            const FNavBuildSettings& S = In.Settings;
            Out.X = TX;
            Out.Y = TY;
            Out.Blob.clear();

            const float TileMinX = Grid.Origin.x + (float)TX * Grid.TileWorldSize;
            const float TileMinZ = Grid.Origin.z + (float)TY * Grid.TileWorldSize;
            const float TileMaxX = TileMinX + Grid.TileWorldSize;
            const float TileMaxZ = TileMinZ + Grid.TileWorldSize;

            rcConfig Cfg{};
            Cfg.cs = S.CellSize;
            Cfg.ch = S.CellHeight;
            Cfg.walkableSlopeAngle = S.AgentMaxSlopeDeg;
            Cfg.walkableHeight     = (int)std::ceil(S.AgentHeight / S.CellHeight);
            Cfg.walkableClimb      = (int)std::floor(S.AgentMaxClimb / S.CellHeight);
            Cfg.walkableRadius     = (int)std::ceil(S.AgentRadius / S.CellSize);
            Cfg.maxEdgeLen         = (int)(S.EdgeMaxLength / S.CellSize);
            Cfg.maxSimplificationError = S.EdgeMaxError;
            Cfg.minRegionArea      = S.RegionMinSize * S.RegionMinSize;
            Cfg.mergeRegionArea    = S.RegionMergeSize * S.RegionMergeSize;
            Cfg.maxVertsPerPoly    = S.VertsPerPoly;
            Cfg.detailSampleDist   = S.DetailSampleDist < 0.9f ? 0.0f : S.CellSize * S.DetailSampleDist;
            Cfg.detailSampleMaxError = S.CellHeight * S.DetailSampleMaxError;
            Cfg.tileSize           = S.TileSizeVoxels;
            Cfg.borderSize         = Cfg.walkableRadius + 3;
            Cfg.width              = Cfg.tileSize + Cfg.borderSize * 2;
            Cfg.height             = Cfg.tileSize + Cfg.borderSize * 2;

            // bmin/bmax include border so seam tris rasterize; rcBuildPolyMesh strips border polys.
            Cfg.bmin[0] = TileMinX - Grid.BorderSize;
            Cfg.bmin[1] = In.BoundsMin.y;
            Cfg.bmin[2] = TileMinZ - Grid.BorderSize;
            Cfg.bmax[0] = TileMaxX + Grid.BorderSize;
            Cfg.bmax[1] = In.BoundsMax.y;
            Cfg.bmax[2] = TileMaxZ + Grid.BorderSize;

            rcContext Ctx(false);

            rcHeightfield* Solid = rcAllocHeightfield();
            if (!Solid) return false;
            if (!rcCreateHeightfield(&Ctx, *Solid, Cfg.width, Cfg.height, Cfg.bmin, Cfg.bmax, Cfg.cs, Cfg.ch))
            {
                rcFreeHeightField(Solid);
                return false;
            }

            // Empty tile is valid (no overlapping geometry); skip straight to a clear blob.
            if (NumTileTris == 0)
            {
                rcFreeHeightField(Solid);
                return true;
            }

            const float* Verts    = reinterpret_cast<const float*>(In.Vertices.data());
            const int32  NumVerts  = (int32)In.Vertices.size();
            const int32  GlobalTris = (int32)(In.Indices.size() / 3);

            // Gather just this tile's triangles into a local index list (rcRasterizeonly walks these).
            TVector<int>   Tris;     Tris.resize((size_t)NumTileTris * 3);
            TVector<uint8> TriAreas(NumTileTris, 0);
            for (int32 i = 0; i < NumTileTris; ++i)
            {
                const int32 t = TileTriIndices[i];
                Tris[i * 3 + 0] = (int)In.Indices[t * 3 + 0];
                Tris[i * 3 + 1] = (int)In.Indices[t * 3 + 1];
                Tris[i * 3 + 2] = (int)In.Indices[t * 3 + 2];
            }

            rcMarkWalkableTriangles(&Ctx, Cfg.walkableSlopeAngle, Verts, NumVerts, Tris.data(), NumTileTris, TriAreas.data());

            if (!In.Areas.empty() && (int32)In.Areas.size() == GlobalTris)
            {
                for (int32 i = 0; i < NumTileTris; ++i)
                {
                    if (TriAreas[i] != 0) // keep unwalkable
                    {
                        TriAreas[i] = In.Areas[TileTriIndices[i]];
                    }
                }
            }

            if (!rcRasterizeTriangles(&Ctx, Verts, NumVerts, Tris.data(), TriAreas.data(), NumTileTris, *Solid, Cfg.walkableClimb))
            {
                rcFreeHeightField(Solid);
                return false;
            }

            rcFilterLowHangingWalkableObstacles(&Ctx, Cfg.walkableClimb, *Solid);
            rcFilterLedgeSpans(&Ctx, Cfg.walkableHeight, Cfg.walkableClimb, *Solid);
            rcFilterWalkableLowHeightSpans(&Ctx, Cfg.walkableHeight, *Solid);

            rcCompactHeightfield* Compact = rcAllocCompactHeightfield();
            if (!Compact)
            {
                rcFreeHeightField(Solid);
                return false;
            }
            if (!rcBuildCompactHeightfield(&Ctx, Cfg.walkableHeight, Cfg.walkableClimb, *Solid, *Compact))
            {
                rcFreeHeightField(Solid);
                rcFreeCompactHeightfield(Compact);
                return false;
            }
            rcFreeHeightField(Solid);
            Solid = nullptr;

            if (!rcErodeWalkableArea(&Ctx, Cfg.walkableRadius, *Compact))
            {
                rcFreeCompactHeightfield(Compact);
                return false;
            }

            // Watershed regions; monotone is faster but yields thin polys.
            if (!rcBuildDistanceField(&Ctx, *Compact) ||
                !rcBuildRegions(&Ctx, *Compact, Cfg.borderSize, Cfg.minRegionArea, Cfg.mergeRegionArea))
            {
                rcFreeCompactHeightfield(Compact);
                return false;
            }

            rcContourSet* CSet = rcAllocContourSet();
            if (!CSet ||
                !rcBuildContours(&Ctx, *Compact, Cfg.maxSimplificationError, Cfg.maxEdgeLen, *CSet))
            {
                rcFreeCompactHeightfield(Compact);
                if (CSet) rcFreeContourSet(CSet);
                return false;
            }
            if (CSet->nconts == 0)
            {
                // Empty tile is valid.
                rcFreeCompactHeightfield(Compact);
                rcFreeContourSet(CSet);
                return true;
            }

            rcPolyMesh* PMesh = rcAllocPolyMesh();
            if (!PMesh ||
                !rcBuildPolyMesh(&Ctx, *CSet, Cfg.maxVertsPerPoly, *PMesh))
            {
                rcFreeCompactHeightfield(Compact);
                rcFreeContourSet(CSet);
                if (PMesh) rcFreePolyMesh(PMesh);
                return false;
            }

            rcPolyMeshDetail* DMesh = rcAllocPolyMeshDetail();
            if (!DMesh ||
                !rcBuildPolyMeshDetail(&Ctx, *PMesh, *Compact, Cfg.detailSampleDist, Cfg.detailSampleMaxError, *DMesh))
            {
                rcFreeCompactHeightfield(Compact);
                rcFreeContourSet(CSet);
                rcFreePolyMesh(PMesh);
                if (DMesh)
                {
                    rcFreePolyMeshDetail(DMesh);
                }
                return false;
            }
            rcFreeCompactHeightfield(Compact);
            rcFreeContourSet(CSet);

            // Tag polys with Walk flag for default queries.
            for (int i = 0; i < PMesh->npolys; ++i)
            {
                if (PMesh->areas[i] == RC_WALKABLE_AREA)
                {
                    PMesh->areas[i] = (uint8)1; // ENavArea::Ground
                }
                if (PMesh->areas[i] != 0)
                {
                    PMesh->flags[i] = 1; // ENavPolyFlag::Walk
                }
            }

            dtNavMeshCreateParams Params{};
            Params.verts            = PMesh->verts;
            Params.vertCount        = PMesh->nverts;
            Params.polys            = PMesh->polys;
            Params.polyAreas        = PMesh->areas;
            Params.polyFlags        = PMesh->flags;
            Params.polyCount        = PMesh->npolys;
            Params.nvp              = PMesh->nvp;
            Params.detailMeshes     = DMesh->meshes;
            Params.detailVerts      = DMesh->verts;
            Params.detailVertsCount = DMesh->nverts;
            Params.detailTris       = DMesh->tris;
            Params.detailTriCount   = DMesh->ntris;
            Params.walkableHeight   = S.AgentHeight;
            Params.walkableRadius   = S.AgentRadius;
            Params.walkableClimb    = S.AgentMaxClimb;
            Params.tileX            = TX;
            Params.tileY            = TY;
            Params.tileLayer        = 0;
            rcVcopy(Params.bmin, PMesh->bmin);
            rcVcopy(Params.bmax, PMesh->bmax);
            Params.cs               = Cfg.cs;
            Params.ch               = Cfg.ch;
            Params.buildBvTree      = true;

            uint8* NavData = nullptr;
            int    NavDataSize = 0;
            const bool bCreated = dtCreateNavMeshData(&Params, &NavData, &NavDataSize);
            rcFreePolyMesh(PMesh);
            rcFreePolyMeshDetail(DMesh);

            if (!bCreated || !NavData)
            {
                return false;
            }

            // Copy out of Detour's allocator; runtime addTile copies again.
            Out.Blob.assign(NavData, NavData + NavDataSize);
            dtFree(NavData);
            return true;
        }
#endif
    }

    static void RunBake(const FNavBuildInput& Input, FNavBakeHandle& Handle)
    {
#if !defined(LUMINA_HAS_RECAST)
        LOG_ERROR("NavMesh bake invoked but Recast/Detour is not vendored (LUMINA_HAS_RECAST undefined). All tiles will be empty stubs.");
#endif

        const FTileGrid Grid = ComputeGrid(Input);
        const uint32 TileCount = (uint32)(Grid.TilesX * Grid.TilesY);

        // A huge tile count is one Recast pipeline each and reads as stuck, so abort with a message.
        constexpr uint32 kMaxTiles = 8192;
        if (TileCount > kMaxTiles)
        {
            LOG_ERROR("NavMesh bake aborted: {} tiles ({}x{}) exceeds the {}-tile cap for TileWorldSize={:.1f}. "
                      "The bake volume is too large, raise Settings.CellSize or TileSizeVoxels, or shrink the bounds / entity scale.",
                      TileCount, Grid.TilesX, Grid.TilesY, kMaxTiles, Grid.TileWorldSize);
            Handle.Output.Tiles.clear();
            Handle.TilesScheduled = 0;
            Handle.bDone.store(true, std::memory_order_release);
            return;
        }

        if (TileCount > 2048)
        {
            LOG_WARN("NavMesh bake scheduling {} tiles ({}x{}); large for TileWorldSize={:.1f}. Raise "
                     "Settings.CellSize/TileSizeVoxels or shrink the bounds if the bake is slow.",
                     TileCount, Grid.TilesX, Grid.TilesY, Grid.TileWorldSize);
        }

        Handle.Output.Origin = Grid.Origin;
        Handle.Output.TileWorldSize = Grid.TileWorldSize;
        Handle.Output.MaxTiles = (int32)TileCount;
        Handle.Output.MaxPolysPerTile = 1 << 14;
        Handle.Output.Tiles.resize(TileCount);
        Handle.TilesScheduled = TileCount;

        // Bin triangles per tile once so each tile bakes only its overlapping geometry.
        FTileBins Bins;
        BuildTileBins(Input, Grid, Bins);

        // Aggregate per-tile failures into one log line.
        std::atomic<uint32> FailCount{ 0 };

        Task::ParallelFor(TileCount, [&](uint32 Index)
        {
            if (Handle.bCancelRequested.load(std::memory_order_acquire))
            {
                Handle.TilesCompleted.fetch_add(1, std::memory_order_release);
                return;
            }

            const int32 TX = (int32)(Index % (uint32)Grid.TilesX);
            const int32 TY = (int32)(Index / (uint32)Grid.TilesX);

#if defined(LUMINA_HAS_RECAST)
            const uint32 Begin = Bins.Offsets[Index];
            const uint32 End   = Bins.Offsets[Index + 1];
            const int32* TileTris = (End > Begin) ? &Bins.TriIndices[Begin] : nullptr;
            if (!BakeTile(Input, Grid, TX, TY, TileTris, (int32)(End - Begin), Handle.Output.Tiles[Index]))
            {
                FailCount.fetch_add(1, std::memory_order_relaxed);
            }
#else
            Handle.Output.Tiles[Index].X = TX;
            Handle.Output.Tiles[Index].Y = TY;
#endif
            Handle.TilesCompleted.fetch_add(1, std::memory_order_release);
        }, 1, ETaskPriority::Background);

        const uint32 Failed = FailCount.load(std::memory_order_relaxed);
        if (Failed > 0)
        {
            LOG_WARN("NavMesh bake: {}/{} tiles failed to bake (Recast pipeline returned false). Those tiles will be empty.", Failed, TileCount);
        }

        Handle.bDone.store(true, std::memory_order_release);
    }

    TUniquePtr<FNavBakeHandle> Bake(FNavBuildInput Input)
    {
        TUniquePtr<FNavBakeHandle> Handle = MakeUnique<FNavBakeHandle>();
        FNavBakeHandle* Raw = Handle.get();

        Task::AsyncTask(1, 1, [Raw, In = std::move(Input)](uint32, uint32, uint32) mutable
        {
            RunBake(In, *Raw);
        }, ETaskPriority::Background);

        return Handle;
    }

    bool BakeSync(FNavBuildInput Input, FNavBuildOutput& Out)
    {
        FNavBakeHandle Handle;
        RunBake(Input, Handle);
        Out = std::move(Handle.Output);
        return Handle.bDone.load(std::memory_order_acquire);
    }

#if defined(LUMINA_HAS_RECAST)
    bool BakeSingleTile(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout, int32 TX, int32 TY, FNavTileData& Out)
    {
        // Reuse pipeline with BaseLayout's origin/tile size for byte-exact hot-swap.
        FTileGrid Grid{};
        Grid.Origin = BaseLayout.Origin;
        Grid.TileWorldSize = BaseLayout.TileWorldSize;
        const int32 BorderVoxels = (int32)std::ceil(Input.Settings.AgentRadius / Input.Settings.CellSize) + 3;
        Grid.BorderSize = (float)BorderVoxels * Input.Settings.CellSize;
        Grid.TilesX = Math::Max(1, (int32)std::ceil((Input.BoundsMax.x - Input.BoundsMin.x) / Grid.TileWorldSize));
        Grid.TilesY = Math::Max(1, (int32)std::ceil((Input.BoundsMax.z - Input.BoundsMin.z) / Grid.TileWorldSize));

        // A single tile culls the input to just this tile's overlapping triangles.
        const int32 NumTris = (int32)(Input.Indices.size() / 3);
        TVector<int32> TileTris;
        TileTris.reserve(64);
        for (int32 t = 0; t < NumTris; ++t)
        {
            int32 tx0, ty0, tx1, ty1;
            TriTileRange(Input, Grid, t, tx0, ty0, tx1, ty1);
            if (TX >= tx0 && TX <= tx1 && TY >= ty0 && TY <= ty1)
            {
                TileTris.push_back(t);
            }
        }
        return BakeTile(Input, Grid, TX, TY, TileTris.empty() ? nullptr : TileTris.data(), (int32)TileTris.size(), Out);
    }
#else
    bool BakeSingleTile(const FNavBuildInput&, const FNavBuildOutput&, int32, int32, FNavTileData&)
    {
        return false;
    }
#endif
}
