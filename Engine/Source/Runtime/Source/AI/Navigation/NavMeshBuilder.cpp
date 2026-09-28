#include "RuntimePCH.h"
#include "NavMeshBuilder.h"

#include "Config/NavigationSettings.h"
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

            // Stamped after erosion so a Null volume carves a hole the agent radius cannot reopen.
            for (const FNavAreaVolume& Volume : In.AreaVolumes)
            {
                if (Volume.Hull.size() < 3)
                {
                    continue;
                }
                rcMarkConvexPolyArea(&Ctx, reinterpret_cast<const float*>(Volume.Hull.data()), (int)Volume.Hull.size(),
                                     Volume.MinY, Volume.MaxY, Volume.Area, *Compact);
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

            // Detour keeps only the links whose start point lands inside this tile, so pass them all.
            const int32 LinkCount = (int32)In.Links.size();
            TVector<float>  LinkVerts;  LinkVerts.resize((size_t)LinkCount * 6);
            TVector<float>  LinkRadii;  LinkRadii.resize(LinkCount);
            TVector<uint8>  LinkDirs;   LinkDirs.resize(LinkCount);
            TVector<uint8>  LinkAreas;  LinkAreas.resize(LinkCount);
            TVector<uint16> LinkFlags;  LinkFlags.resize(LinkCount);
            TVector<uint32> LinkIds;    LinkIds.resize(LinkCount);
            for (int32 i = 0; i < LinkCount; ++i)
            {
                const FNavOffMeshLink& Link = In.Links[i];
                LinkVerts[i * 6 + 0] = Link.Start.x;
                LinkVerts[i * 6 + 1] = Link.Start.y;
                LinkVerts[i * 6 + 2] = Link.Start.z;
                LinkVerts[i * 6 + 3] = Link.End.x;
                LinkVerts[i * 6 + 4] = Link.End.y;
                LinkVerts[i * 6 + 5] = Link.End.z;
                LinkRadii[i] = Link.Radius;
                LinkDirs[i]  = Link.bBidirectional ? (uint8)DT_OFFMESH_CON_BIDIR : (uint8)0;
                LinkAreas[i] = Link.Area;
                LinkFlags[i] = Link.Flags;
                LinkIds[i]   = Link.UserId;
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
            if (LinkCount > 0)
            {
                Params.offMeshConVerts  = LinkVerts.data();
                Params.offMeshConRad    = LinkRadii.data();
                Params.offMeshConDir    = LinkDirs.data();
                Params.offMeshConAreas  = LinkAreas.data();
                Params.offMeshConFlags  = LinkFlags.data();
                Params.offMeshConUserID = LinkIds.data();
                Params.offMeshConCount  = LinkCount;
            }

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
        const uint32 MaxBakeTiles = (uint32)Math::Max(1, GetDefault<CNavigationSettings>()->MaxBakeTiles);
        if (TileCount > MaxBakeTiles)
        {
            LOG_ERROR("NavMesh bake aborted: {} tiles ({}x{}) exceeds the {}-tile cap for TileWorldSize={:.1f}. "
                      "Raise Navigation settings MaxBakeTiles, raise Settings.CellSize or TileSizeVoxels, "
                      "or shrink the bounds / entity scale.",
                      TileCount, Grid.TilesX, Grid.TilesY, MaxBakeTiles, Grid.TileWorldSize);
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
        Handle.Output.TilesX = Grid.TilesX;
        Handle.Output.TilesY = Grid.TilesY;
        Handle.Output.MaxTiles = (int32)TileCount;
        Handle.Output.MaxPolysPerTile = 1 << 14;
        Handle.Output.Tiles.resize(TileCount);
        Handle.TilesScheduled = TileCount;

        // Bin triangles per tile once so each tile bakes only its overlapping geometry.
        FTileBins Bins;
        BuildTileBins(Input, Grid, Bins);

        // Aggregate per-tile failures into one log line.
        std::atomic<uint32> FailCount{ 0 };

        // Row-major order has workers scattered across unrelated bands at once, which reads as random
        // chunks appearing. Sorting by distance from the middle makes the mesh grow outwards instead, and
        // it costs one sort of the tile indices.
        TVector<uint32> Order((size_t)TileCount);
        {
            for (uint32 i = 0; i < TileCount; ++i)
            {
                Order[i] = i;
            }
            const float CenterX = (float)(Grid.TilesX - 1) * 0.5f;
            const float CenterY = (float)(Grid.TilesY - 1) * 0.5f;
            Algo::Sort(Order, [&](uint32 A, uint32 B)
            {
                const float AX = (float)(A % (uint32)Grid.TilesX) - CenterX;
                const float AY = (float)(A / (uint32)Grid.TilesX) - CenterY;
                const float BX = (float)(B % (uint32)Grid.TilesX) - CenterX;
                const float BY = (float)(B / (uint32)Grid.TilesX) - CenterY;
                return (AX * AX + AY * AY) < (BX * BX + BY * BY);
            });
        }

        Task::ParallelFor(TileCount, [&](uint32 Slot)
        {
            if (Handle.bCancelRequested.load(std::memory_order_acquire))
            {
                Handle.TilesCompleted.fetch_add(1, std::memory_order_release);
                return;
            }

            const uint32 Index = Order[Slot];
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

    TSharedPtr<FNavBakeHandle> Bake(FNavBuildInput Input)
    {
        TSharedPtr<FNavBakeHandle> Handle = MakeShared<FNavBakeHandle>();

        Task::AsyncTask(1, 1, [Handle, In = std::move(Input)](uint32, uint32, uint32) mutable
        {
            RunBake(In, *Handle);
        }, ETaskPriority::Background);

        return Handle;
    }

    TSharedPtr<FNavBakeHandle> Bake(FNavBuildInput Input, TFunction<void(FNavBuildInput&)> Prepare)
    {
        TSharedPtr<FNavBakeHandle> Handle = MakeShared<FNavBakeHandle>();

        Task::AsyncTask(1, 1, [Handle, In = std::move(Input), Prepare = Move(Prepare)](uint32, uint32, uint32) mutable
        {
            if (Prepare)
            {
                Prepare(In);
            }
            RunBake(In, *Handle);
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
    namespace
    {
        // The layout, never the live bounds, or moving the volume shifts the clamps in TriTileRange.
        FTileGrid GridFromLayout(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout)
        {
            FTileGrid Grid{};
            Grid.Origin = BaseLayout.Origin;
            Grid.TileWorldSize = BaseLayout.TileWorldSize;
            const int32 BorderVoxels = (int32)std::ceil(Input.Settings.AgentRadius / Input.Settings.CellSize) + 3;
            Grid.BorderSize = (float)BorderVoxels * Input.Settings.CellSize;
            Grid.TilesX = Math::Max(1, BaseLayout.TilesX);
            Grid.TilesY = Math::Max(1, BaseLayout.TilesY);
            return Grid;
        }
    }

    void BakeTiles(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout,
                   const TVector<FNavTileCoord>& Coords, TVector<FNavTileData>& Out)
    {
        LUMINA_PROFILE_SCOPE();

        Out.clear();
        Out.resize(Coords.size());
        if (Coords.empty())
        {
            return;
        }

        const FTileGrid Grid = GridFromLayout(Input, BaseLayout);

        THashMap<uint64, int32> SlotByKey;
        SlotByKey.reserve(Coords.size());
        for (int32 i = 0; i < (int32)Coords.size(); ++i)
        {
            SlotByKey[NavTile::PackKey(Coords[i].X, Coords[i].Y)] = i;
            Out[i].X = Coords[i].X;
            Out[i].Y = Coords[i].Y;
        }

        // One pass over the geometry fills every requested tile, instead of one pass per tile.
        TVector<TVector<int32>> Bins;
        Bins.resize(Coords.size());
        const int32 NumTris = (int32)(Input.Indices.size() / 3);
        for (int32 t = 0; t < NumTris; ++t)
        {
            int32 TX0, TY0, TX1, TY1;
            TriTileRange(Input, Grid, t, TX0, TY0, TX1, TY1);
            for (int32 ty = TY0; ty <= TY1; ++ty)
            {
                for (int32 tx = TX0; tx <= TX1; ++tx)
                {
                    auto It = SlotByKey.find(NavTile::PackKey(tx, ty));
                    if (It != SlotByKey.end())
                    {
                        Bins[It->second].push_back(t);
                    }
                }
            }
        }

        Task::ParallelFor((uint32)Coords.size(), [&](uint32 i)
        {
            const TVector<int32>& Tris = Bins[i];
            BakeTile(Input, Grid, Coords[i].X, Coords[i].Y, Tris.empty() ? nullptr : Tris.data(), (int32)Tris.size(), Out[i]);
        }, 1, ETaskPriority::Background);
    }

    bool BakeSingleTile(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout, int32 TX, int32 TY, FNavTileData& Out)
    {
        TVector<FNavTileCoord> Coords;
        Coords.push_back(FNavTileCoord{ TX, TY });

        TVector<FNavTileData> Baked;
        BakeTiles(Input, BaseLayout, Coords, Baked);
        if (Baked.empty())
        {
            return false;
        }
        Out = std::move(Baked[0]);
        return true;
    }
#else
    bool BakeSingleTile(const FNavBuildInput&, const FNavBuildOutput&, int32, int32, FNavTileData&)
    {
        return false;
    }

    void BakeTiles(const FNavBuildInput&, const FNavBuildOutput&, const TVector<FNavTileCoord>& Coords, TVector<FNavTileData>& Out)
    {
        Out.clear();
        Out.resize(Coords.size());
    }
#endif
}
