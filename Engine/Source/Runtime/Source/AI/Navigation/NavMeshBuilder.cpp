#include "RuntimePCH.h"
#include "NavMeshBuilder.h"

#include "Config/NavigationSettings.h"
#include "Memory/SmartPtr.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "TaskSystem/TaskSystem.h"
#include "AI/Navigation/NavBuildPool.h"
#include "tracy/TracyC.h"
#include "Core/Console/ConsoleVariable.h"
#include <array>

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

        TConsoleVar<int32> CVarNavRegionPartition("Nav.RegionPartition", 0,
            "Overrides every volume's region partition for A/B testing, 0 uses the settings, 1 Watershed, 2 Monotone, 3 Layers.");

        ENavRegionPartition ResolvePartition(ENavRegionPartition Setting)
        {
            const int32 Override = CVarNavRegionPartition.GetValue();
            if (Override > (int32)ENavRegionPartition::ProjectDefault && Override <= (int32)ENavRegionPartition::Layers)
            {
                return (ENavRegionPartition)Override;
            }
            if (Setting != ENavRegionPartition::ProjectDefault)
            {
                return Setting;
            }
            const CNavigationSettings* Project = GetDefault<CNavigationSettings>();
            const ENavRegionPartition ProjectPartition = Project != nullptr ? Project->RegionPartition : ENavRegionPartition::Layers;
            return ProjectPartition == ENavRegionPartition::ProjectDefault ? ENavRegionPartition::Layers : ProjectPartition;
        }
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

        // The tiles a bin pass fills, as a rectangle of the grid mapped to bin slots, with kIndexNone for tiles nobody asked for.
        struct FBinTarget
        {
            int32         X0 = 0;
            int32         Y0 = 0;
            int32         Width = 0;
            int32         Height = 0;
            int32         NumSlots = 0;
            TVector<int32> SlotOf;
        };

        FBinTarget WholeGrid(const FTileGrid& Grid)
        {
            FBinTarget Target{ 0, 0, Grid.TilesX, Grid.TilesY, Grid.TilesX * Grid.TilesY };
            Target.SlotOf.resize((size_t)Target.NumSlots);
            Algo::Iota(Target.SlotOf.begin(), Target.SlotOf.end(), 0);
            return Target;
        }

        template <typename TVisit>
        FORCEINLINE void ForEachTriSlot(const FNavBuildInput& In, const FTileGrid& Grid, const FBinTarget& Target, int32 Tri, TVisit&& Visit)
        {
            int32 TX0, TY0, TX1, TY1;
            TriTileRange(In, Grid, Tri, TX0, TY0, TX1, TY1);
            TX0 = Math::Max(TX0, Target.X0);
            TY0 = Math::Max(TY0, Target.Y0);
            TX1 = Math::Min(TX1, Target.X0 + Target.Width - 1);
            TY1 = Math::Min(TY1, Target.Y0 + Target.Height - 1);
            for (int32 ty = TY0; ty <= TY1; ++ty)
            {
                for (int32 tx = TX0; tx <= TX1; ++tx)
                {
                    const int32 Slot = Target.SlotOf[(size_t)((ty - Target.Y0) * Target.Width + (tx - Target.X0))];
                    if (Slot != Constants::kIndexNone)
                    {
                        Visit(Slot);
                    }
                }
            }
        }

        // A counting sort split into chunks of triangles, each with its own tally row, so both passes run in parallel and keep the serial order.
        void BuildTileBins(const FNavBuildInput& In, const FTileGrid& Grid, const FBinTarget& Target, FTileBins& Out, FNavBuildPool* Pool = nullptr)
        {
            constexpr uint32 kMinChunkTris = 16384;
            const uint32 NumTris = (uint32)(In.Indices.size() / 3);
            const uint32 NumSlots = (uint32)Target.NumSlots;
            Out.Offsets.assign((size_t)NumSlots + 1, 0u);
            Out.TriIndices.clear();
            if (NumTris == 0 || NumSlots == 0 || In.Vertices.empty())
            {
                return;
            }

            // Capped by worker count rather than triangle count, since every chunk carries a tally for every slot.
            const uint32 MaxChunks = Math::Max(1u, Jobs::GetNumWorkers() * 4);
            const uint32 Chunks = Math::Clamp((NumTris + kMinChunkTris - 1) / kMinChunkTris, 1u, MaxChunks);
            const uint32 ChunkTris = (NumTris + Chunks - 1) / Chunks;
            TVector<uint32> Cursors((size_t)Chunks * NumSlots, 0u);

            NavParallelFor(Pool, Chunks, [&](uint32 Chunk)
            {
                uint32* Tally = &Cursors[(size_t)Chunk * NumSlots];
                const uint32 End = Math::Min(NumTris, (Chunk + 1) * ChunkTris);
                for (uint32 t = Chunk * ChunkTris; t < End; ++t)
                {
                    ForEachTriSlot(In, Grid, Target, (int32)t, [Tally](int32 Slot) { ++Tally[Slot]; });
                }
            }, 1, ETaskPriority::Background);

            uint32 Running = 0;
            for (uint32 Slot = 0; Slot < NumSlots; ++Slot)
            {
                Out.Offsets[Slot] = Running;
                for (uint32 Chunk = 0; Chunk < Chunks; ++Chunk)
                {
                    uint32& Cursor = Cursors[(size_t)Chunk * NumSlots + Slot];
                    const uint32 Count = Cursor;
                    Cursor = Running;
                    Running += Count;
                }
            }
            Out.Offsets[NumSlots] = Running;
            Out.TriIndices.resize(Running);

            NavParallelFor(Pool, Chunks, [&](uint32 Chunk)
            {
                uint32* Cursor = &Cursors[(size_t)Chunk * NumSlots];
                const uint32 End = Math::Min(NumTris, (Chunk + 1) * ChunkTris);
                for (uint32 t = Chunk * ChunkTris; t < End; ++t)
                {
                    ForEachTriSlot(In, Grid, Target, (int32)t, [&Out, Cursor, t](int32 Slot) { Out.TriIndices[Cursor[Slot]++] = (int32)t; });
                }
            }, 1, ETaskPriority::Background);
        }

#if defined(LUMINA_HAS_RECAST)
        // Pure over its inputs, so it is safe to call concurrently.
        #if defined(TRACY_ENABLE)
        // Recast already brackets each stage with a timer, so forwarding those to Tracy breaks a tile bake down by stage.
        class FProfiledRecastContext final : public rcContext
        {
        public:
            FProfiledRecastContext()
                : rcContext(TracyCIsConnected != 0)
            {
                enableLog(false);
            }

        protected:
            void doStartTimer(const rcTimerLabel Label) override
            {
                Zones[Label] = ___tracy_emit_zone_begin(&SourceLocation(Label), 1);
            }

            void doStopTimer(const rcTimerLabel Label) override
            {
                ___tracy_emit_zone_end(Zones[Label]);
            }

        private:
            static const ___tracy_source_location_data& SourceLocation(rcTimerLabel Label)
            {
                static const char* const Names[RC_MAX_TIMERS] = {
                    "Recast Total", "Recast Temp", "Recast Rasterize", "Recast Compact Heightfield", "Recast Contours",
                    "Recast Contours Trace", "Recast Contours Simplify", "Recast Filter Border", "Recast Filter Walkable",
                    "Recast Median Area", "Recast Filter Low Obstacles", "Recast Poly Mesh", "Recast Merge Poly Mesh",
                    "Recast Erode", "Recast Mark Box", "Recast Mark Cylinder", "Recast Mark Convex", "Recast Distance Field",
                    "Recast Distance Field Dist", "Recast Distance Field Blur", "Recast Regions", "Recast Regions Watershed",
                    "Recast Regions Expand", "Recast Regions Flood", "Recast Regions Filter", "Recast Layers",
                    "Recast Detail Mesh", "Recast Merge Detail Mesh",
                };
                static const auto Locations = []
                {
                    std::array<___tracy_source_location_data, RC_MAX_TIMERS> Out{};
                    for (int32 i = 0; i < RC_MAX_TIMERS; ++i)
                    {
                        Out[i] = ___tracy_source_location_data{ Names[i], "BakeTile", __FILE__, (uint32_t)__LINE__, 0 };
                    }
                    return Out;
                }();
                return Locations[Label];
            }

            TracyCZoneCtx Zones[RC_MAX_TIMERS] = {};
        };
        #else
        class FProfiledRecastContext final : public rcContext
        {
        public:
            FProfiledRecastContext()
                : rcContext(false)
            {
            }
        };
        #endif

        // Rasterize, filter, compact and erode, everything a cached tile gets to skip.
        // The ledge filter's steep slope test spans the cells on both sides of a span, so it needs the rise of the steepest walkable slope across them.
        int LedgeFilterClimb(const rcConfig& Cfg)
        {
            const float SlopeRise = 2.0f * Cfg.cs * std::tan(Math::Radians(Cfg.walkableSlopeAngle));
            return Math::Max(Cfg.walkableClimb, (int)std::ceil(SlopeRise / Cfg.ch));
        }

        bool VoxelizeTile(rcContext& Ctx, const rcConfig& Cfg, const FNavBuildInput& In, const int32* TileTriIndices, int32 NumTileTris, rcCompactHeightfield*& OutCompact)
        {
            OutCompact = nullptr;
            rcHeightfield* Solid = rcAllocHeightfield();
            if (!Solid) return false;
            if (!rcCreateHeightfield(&Ctx, *Solid, Cfg.width, Cfg.height, Cfg.bmin, Cfg.bmax, Cfg.cs, Cfg.ch))
            {
                rcFreeHeightField(Solid);
                return false;
            }

            // An empty tile is valid and comes back without a heightfield.
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
            rcFilterLedgeSpans(&Ctx, Cfg.walkableHeight, LedgeFilterClimb(Cfg), *Solid);
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

            OutCompact = Compact;
            return true;
        }

        // A tile is wider than a small volume, so the ground it covers past the volume's edges is made unwalkable rather than baked.
        void ClipToVolume(rcContext& Ctx, const FNavBuildInput& In, rcCompactHeightfield& Compact)
        {
            const float MinX = Compact.bmin[0];
            const float MinZ = Compact.bmin[2];
            const float MaxX = Compact.bmax[0];
            const float MaxZ = Compact.bmax[2];

            // Each box only where the edge falls inside the heightfield, since Recast truncates a negative cell index to zero.
            const float Outside[4][4] =
            {
                { MinX,           MinZ,           In.BoundsMin.x, MaxZ },
                { In.BoundsMax.x, MinZ,           MaxX,           MaxZ },
                { MinX,           MinZ,           MaxX,           In.BoundsMin.z },
                { MinX,           In.BoundsMax.z, MaxX,           MaxZ },
            };
            for (const float (&Box)[4] : Outside)
            {
                if (Box[2] <= Box[0] || Box[3] <= Box[1] || Box[0] >= MaxX || Box[2] <= MinX || Box[1] >= MaxZ || Box[3] <= MinZ)
                {
                    continue;
                }
                const float BMin[3] = { Box[0], Compact.bmin[1], Box[1] };
                const float BMax[3] = { Box[2], Compact.bmax[1], Box[3] };
                rcMarkBoxArea(&Ctx, BMin, BMax, RC_NULL_AREA, Compact);
            }
        }

        bool BakeTile(const FNavBuildInput& In, const FTileGrid& Grid, int32 TX, int32 TY, const int32* TileTriIndices, int32 NumTileTris, SNavTileData& Out,
                      FNavTileCache* Cache = nullptr, uint32 Generation = 0)
        {
            const SNavBuildSettings& S = In.Settings;
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

            FProfiledRecastContext Ctx;

            // A tile whose geometry has not changed since its last bake starts from the heightfield that bake left, skipping voxelization.
            rcCompactHeightfield* Compact = nullptr;
            if (Cache == nullptr || !Cache->Load(TX, TY, Generation, Compact))
            {
                if (!VoxelizeTile(Ctx, Cfg, In, TileTriIndices, NumTileTris, Compact))
                {
                    return false;
                }
                if (Compact == nullptr)
                {
                    return true;
                }
                if (Cache != nullptr)
                {
                    Cache->Store(TX, TY, Generation, *Compact);
                }
            }

            ClipToVolume(Ctx, In, *Compact);

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

            bool bRegionsBuilt = false;
            switch (ResolvePartition(S.Partition))
            {
            case ENavRegionPartition::Monotone:
                bRegionsBuilt = rcBuildRegionsMonotone(&Ctx, *Compact, Cfg.borderSize, Cfg.minRegionArea, Cfg.mergeRegionArea);
                break;
            case ENavRegionPartition::Layers:
                bRegionsBuilt = rcBuildLayerRegions(&Ctx, *Compact, Cfg.borderSize, Cfg.minRegionArea);
                break;
            case ENavRegionPartition::Watershed:
            default:
                bRegionsBuilt = rcBuildDistanceField(&Ctx, *Compact)
                             && rcBuildRegions(&Ctx, *Compact, Cfg.borderSize, Cfg.minRegionArea, Cfg.mergeRegionArea);
                break;
            }
            if (!bRegionsBuilt)
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

        // Neighboring cells on a slope connect only while the slope rises no more than the climb across one cell.
        const SNavBuildSettings& Settings = Input.Settings;
        const float ConnectableSlope = Math::Degrees(std::atan(Settings.AgentMaxClimb / Math::Max(Settings.CellSize, 0.001f)));
        if (Settings.AgentMaxSlopeDeg > ConnectableSlope)
        {
            LOG_WARN("NavMesh bake: AgentMaxSlopeDeg {:.0f} is steeper than the {:.0f} degrees a {:.2f} climb can connect at CellSize {:.2f}, "
                     "so slopes past that split the mesh. Lower CellSize or raise AgentMaxClimb.",
                     Settings.AgentMaxSlopeDeg, ConnectableSlope, Settings.AgentMaxClimb, Settings.CellSize);
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
        BuildTileBins(Input, Grid, WholeGrid(Grid), Bins);

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

    // One worker in this many may rebake tiles while a game runs.
    constexpr uint32 kRebakeWorkerShare = 4;

    void BakeTiles(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout, const TVector<FNavTileCoord>& Coords,
                   TVector<SNavTileData>& Out, FNavBuildPool* Pool, FNavTileCache* Cache)
    {
        LUMINA_PROFILE_SCOPE();

        Out.clear();
        Out.resize(Coords.size());
        if (Coords.empty())
        {
            return;
        }

        const FTileGrid Grid = GridFromLayout(Input, BaseLayout);

        // Only the rectangle around the requested tiles gets slots, so a triangle anywhere else costs its bounds and nothing more.
        FBinTarget Target;
        Target.X0 = Grid.TilesX;
        Target.Y0 = Grid.TilesY;
        int32 X1 = -1, Y1 = -1;
        for (int32 i = 0; i < (int32)Coords.size(); ++i)
        {
            Out[i].X = Coords[i].X;
            Out[i].Y = Coords[i].Y;
            if (Coords[i].X >= 0 && Coords[i].X < Grid.TilesX && Coords[i].Y >= 0 && Coords[i].Y < Grid.TilesY)
            {
                Target.X0 = Math::Min(Target.X0, Coords[i].X);
                Target.Y0 = Math::Min(Target.Y0, Coords[i].Y);
                X1 = Math::Max(X1, Coords[i].X);
                Y1 = Math::Max(Y1, Coords[i].Y);
            }
        }
        Target.Width = Math::Max(0, X1 - Target.X0 + 1);
        Target.Height = Math::Max(0, Y1 - Target.Y0 + 1);
        Target.NumSlots = (int32)Coords.size();
        Target.SlotOf.assign((size_t)Target.Width * Target.Height, Constants::kIndexNone);
        for (int32 i = 0; i < (int32)Coords.size(); ++i)
        {
            if (Coords[i].X >= Target.X0 && Coords[i].X <= X1 && Coords[i].Y >= Target.Y0 && Coords[i].Y <= Y1)
            {
                Target.SlotOf[(size_t)((Coords[i].Y - Target.Y0) * Target.Width + (Coords[i].X - Target.X0))] = i;
            }
        }

        FTileBins Bins;
        BuildTileBins(Input, Grid, Target, Bins, Pool);

        auto BakeSlot = [&](uint32 i)
        {
            const uint32 Begin = Bins.Offsets[i];
            const uint32 End = Bins.Offsets[i + 1];
            BakeTile(Input, Grid, Coords[i].X, Coords[i].Y, End > Begin ? &Bins.TriIndices[Begin] : nullptr, (int32)(End - Begin), Out[i], Cache, Coords[i].CacheGeneration);
        };
        if (Pool != nullptr)
        {
            Pool->ParallelFor((uint32)Coords.size(), BakeSlot);
            return;
        }

        // A tile is several milliseconds no one can preempt, so a runtime rebake takes a few lanes rather than every worker the frame needs.
        const uint32 Lanes = Math::Clamp(Jobs::GetNumWorkers() / kRebakeWorkerShare, 1u, (uint32)Coords.size());
        std::atomic<uint32> NextTile{0};
        Task::ParallelFor(Lanes, [&](uint32)
        {
            for (uint32 i = NextTile.fetch_add(1, std::memory_order_relaxed); i < (uint32)Coords.size();
                 i = NextTile.fetch_add(1, std::memory_order_relaxed))
            {
                BakeSlot(i);
            }
        }, 1, ETaskPriority::Background);
    }

    bool BakeSingleTile(const FNavBuildInput& Input, const FNavBuildOutput& BaseLayout, int32 TX, int32 TY, SNavTileData& Out)
    {
        TVector<FNavTileCoord> Coords;
        Coords.push_back(FNavTileCoord{ TX, TY });

        TVector<SNavTileData> Baked;
        BakeTiles(Input, BaseLayout, Coords, Baked);
        if (Baked.empty())
        {
            return false;
        }
        Out = std::move(Baked[0]);
        return true;
    }
#else
    bool BakeSingleTile(const FNavBuildInput&, const FNavBuildOutput&, int32, int32, SNavTileData&)
    {
        return false;
    }

    void BakeTiles(const FNavBuildInput&, const FNavBuildOutput&, const TVector<FNavTileCoord>& Coords, TVector<SNavTileData>& Out, FNavBuildPool*, FNavTileCache*)
    {
        Out.clear();
        Out.resize(Coords.size());
    }
#endif
}

namespace Lumina
{
#if defined(LUMINA_HAS_RECAST)
    struct FNavTileCache::FEntry
    {
        uint32                  Generation = 0;
        uint32                  Pins = 0;
        uint64                  LastUse = 0;
        int32                   Width = 0;
        int32                   Height = 0;
        int32                   WalkableHeight = 0;
        int32                   WalkableClimb = 0;
        int32                   BorderSize = 0;
        uint16                  MaxDistance = 0;
        uint16                  MaxRegions = 0;
        float                   BMin[3] = {};
        float                   BMax[3] = {};
        float                   CellSize = 0.0f;
        float                   CellHeight = 0.0f;
        TVector<rcCompactCell>  Cells;
        TVector<rcCompactSpan>  Spans;
        TVector<uint8>          Areas;

        size_t GetBytes() const { return Cells.size() * sizeof(rcCompactCell) + Spans.size() * sizeof(rcCompactSpan) + Areas.size(); }
    };
#else
    struct FNavTileCache::FEntry
    {
        uint32 Generation = 0;
        uint32 Pins = 0;
        uint64 LastUse = 0;
        size_t GetBytes() const { return 0; }
    };
#endif

    FNavTileCache::FNavTileCache(size_t InBudgetBytes)
        : BudgetBytes(InBudgetBytes)
    {
    }

    FNavTileCache::~FNavTileCache() = default;

    void FNavTileCache::Invalidate(int32 X, int32 Y)
    {
        const uint64 Key = NavTile::PackKey(X, Y);
        FScopeLock Guard(Lock);
        ++Generations[Key];
        auto It = Entries.find(Key);
        if (It != Entries.end() && It->second->Pins == 0)
        {
            Bytes -= It->second->GetBytes();
            Entries.erase(It);
        }
    }

    uint32 FNavTileCache::GetGeneration(int32 X, int32 Y) const
    {
        FScopeLock Guard(Lock);
        auto It = Generations.find(NavTile::PackKey(X, Y));
        return It != Generations.end() ? It->second : 0;
    }

    bool FNavTileCache::Pin(int32 X, int32 Y, uint32 Generation)
    {
        FScopeLock Guard(Lock);
        auto It = Entries.find(NavTile::PackKey(X, Y));
        if (It == Entries.end() || It->second->Generation != Generation)
        {
            return false;
        }
        ++It->second->Pins;
        return true;
    }

    void FNavTileCache::Unpin(int32 X, int32 Y)
    {
        FScopeLock Guard(Lock);
        auto It = Entries.find(NavTile::PackKey(X, Y));
        if (It != Entries.end() && It->second->Pins > 0)
        {
            --It->second->Pins;
        }
    }

#if defined(LUMINA_HAS_RECAST)
    bool FNavTileCache::Load(int32 X, int32 Y, uint32 Generation, rcCompactHeightfield*& Out)
    {
        FScopeLock Guard(Lock);
        auto It = Entries.find(NavTile::PackKey(X, Y));
        if (It == Entries.end() || It->second->Generation != Generation)
        {
            Misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const FEntry& Entry = *It->second;
        rcCompactHeightfield* Compact = rcAllocCompactHeightfield();
        if (Compact == nullptr)
        {
            return false;
        }
        Compact->width = Entry.Width;
        Compact->height = Entry.Height;
        Compact->spanCount = (int)Entry.Spans.size();
        Compact->walkableHeight = Entry.WalkableHeight;
        Compact->walkableClimb = Entry.WalkableClimb;
        Compact->borderSize = Entry.BorderSize;
        Compact->maxDistance = Entry.MaxDistance;
        Compact->maxRegions = Entry.MaxRegions;
        rcVcopy(Compact->bmin, Entry.BMin);
        rcVcopy(Compact->bmax, Entry.BMax);
        Compact->cs = Entry.CellSize;
        Compact->ch = Entry.CellHeight;
        Compact->cells = (rcCompactCell*)rcAlloc(sizeof(rcCompactCell) * Entry.Cells.size(), RC_ALLOC_PERM);
        Compact->spans = (rcCompactSpan*)rcAlloc(sizeof(rcCompactSpan) * Entry.Spans.size(), RC_ALLOC_PERM);
        Compact->areas = (unsigned char*)rcAlloc(Entry.Areas.size(), RC_ALLOC_PERM);
        if (Compact->cells == nullptr || Compact->spans == nullptr || Compact->areas == nullptr)
        {
            rcFreeCompactHeightfield(Compact);
            return false;
        }
        memcpy(Compact->cells, Entry.Cells.data(), sizeof(rcCompactCell) * Entry.Cells.size());
        memcpy(Compact->spans, Entry.Spans.data(), sizeof(rcCompactSpan) * Entry.Spans.size());
        memcpy(Compact->areas, Entry.Areas.data(), Entry.Areas.size());
        It->second->LastUse = ++Clock;
        Hits.fetch_add(1, std::memory_order_relaxed);
        Out = Compact;
        return true;
    }

    void FNavTileCache::Store(int32 X, int32 Y, uint32 Generation, const rcCompactHeightfield& Compact)
    {
        if (BudgetBytes == 0)
        {
            return;
        }

        auto Entry = MakeUnique<FEntry>();
        Entry->Generation = Generation;
        Entry->Width = Compact.width;
        Entry->Height = Compact.height;
        Entry->WalkableHeight = Compact.walkableHeight;
        Entry->WalkableClimb = Compact.walkableClimb;
        Entry->BorderSize = Compact.borderSize;
        Entry->MaxDistance = Compact.maxDistance;
        Entry->MaxRegions = Compact.maxRegions;
        rcVcopy(Entry->BMin, Compact.bmin);
        rcVcopy(Entry->BMax, Compact.bmax);
        Entry->CellSize = Compact.cs;
        Entry->CellHeight = Compact.ch;
        Entry->Cells.assign(Compact.cells, Compact.cells + (size_t)Compact.width * (size_t)Compact.height);
        Entry->Spans.assign(Compact.spans, Compact.spans + Compact.spanCount);
        Entry->Areas.assign(Compact.areas, Compact.areas + Compact.spanCount);

        const uint64 Key = NavTile::PackKey(X, Y);
        FScopeLock Guard(Lock);
        auto Current = Generations.find(Key);
        if ((Current != Generations.end() ? Current->second : 0) != Generation)
        {
            return;
        }

        Entry->LastUse = ++Clock;
        Bytes += Entry->GetBytes();
        auto [It, bAdded] = Entries.try_emplace(Key);
        if (!bAdded)
        {
            // A pinned entry is what a bake in flight was promised, so it stays as it is until released.
            if (It->second->Pins > 0)
            {
                Bytes -= Entry->GetBytes();
                return;
            }
            Bytes -= It->second->GetBytes();
        }
        It->second = std::move(Entry);

        // Least recently used first, never one a bake has pinned.
        while (Bytes > BudgetBytes)
        {
            auto Oldest = Entries.end();
            for (auto Candidate = Entries.begin(); Candidate != Entries.end(); ++Candidate)
            {
                if (Candidate->second->Pins == 0 && Candidate->first != Key && (Oldest == Entries.end() || Candidate->second->LastUse < Oldest->second->LastUse))
                {
                    Oldest = Candidate;
                }
            }
            if (Oldest == Entries.end())
            {
                break;
            }
            Bytes -= Oldest->second->GetBytes();
            Entries.erase(Oldest);
        }
    }
#else
    bool FNavTileCache::Load(int32, int32, uint32, rcCompactHeightfield*&) { return false; }
    void FNavTileCache::Store(int32, int32, uint32, const rcCompactHeightfield&) {}
#endif
}
