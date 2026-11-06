#pragma once

#include "Core/Object/ObjectMacros.h"
#include "NavTypes.generated.h"

namespace Lumina
{
    // Area ids for dtQueryFilter run from 0 to 63, and 0 to 15 are reserved by the bake.
    REFLECT()
    enum class ENavArea : uint8
    {
        Null    = 0,
        Ground  = 1,
        Water   = 2,
        Door    = 3,
        Danger  = 4,
        User0   = 16,
    };

    // Per-poly flag set, AND-ed at query time.
    REFLECT()
    enum class ENavPolyFlag : uint16
    {
        Walk    = 1 << 0,
        Swim    = 1 << 1,
        Door    = 1 << 2,
        Jump    = 1 << 3,
        Disabled = 1 << 4,
        All     = 0xFFFF,
    };

    // How a tile's walkable area is split into regions before polygonization.
    REFLECT()
    enum class ENavRegionPartition : uint8
    {
        // Whatever the project's navigation settings choose.
        ProjectDefault,
        // Best polygons and the slowest, about two fifths of a tile bake.
        Watershed,
        // Fastest, but leaves long thin polygons.
        Monotone,
        // Close to watershed quality at a fraction of its cost, and suited to small tiles.
        Layers,
    };

    REFLECT()
    struct RUNTIME_API SNavBuildSettings
    {
        GENERATED_BODY()

        // Voxel cell size in world units, where smaller gives sharper edges at a cubic bake cost.
        PROPERTY(Editable, Category = "Voxel", ClampMin = 0.05f)
        float CellSize = 0.30f;

        // Vertical voxel size, and how far a step can round past AgentMaxClimb and still read as climbable.
        PROPERTY(Editable, Category = "Voxel", ClampMin = 0.05f)
        float CellHeight = 0.10f;

        // How far the walkable surface is eroded from walls.
        PROPERTY(Editable, Category = "Agent", ClampMin = 0.0f)
        float AgentRadius = 0.40f;

        // Surfaces with less clearance than this are excluded.
        PROPERTY(Editable, Category = "Agent", ClampMin = 0.1f)
        float AgentHeight = 1.80f;

        // Tallest step, such as a stair or ledge, the agent can climb.
        PROPERTY(Editable, Category = "Agent", ClampMin = 0.0f)
        float AgentMaxClimb = 0.40f;

        PROPERTY(Editable, Category = "Agent", ClampMin = 0.0f, ClampMax = 89.0f)
        float AgentMaxSlopeDeg = 45.0f;

        // Contour edges longer than this many world units are split.
        PROPERTY(Editable, Category = "Polygonization", ClampMin = 0.0f)
        float EdgeMaxLength = 12.0f;

        // How far a simplified contour edge may deviate from the raw contour.
        PROPERTY(Editable, Category = "Polygonization", ClampMin = 0.1f)
        float EdgeMaxError = 1.3f;

        // Regions with less floor area than this many voxels are dropped.
        PROPERTY(Editable, Category = "Region", ClampMin = 0)
        int32 RegionMinSize = 8;

        // Adjacent regions smaller than this are merged.
        PROPERTY(Editable, Category = "Region", ClampMin = 0)
        int32 RegionMergeSize = 20;

        PROPERTY(Editable, Category = "Region")
        ENavRegionPartition Partition = ENavRegionPartition::ProjectDefault;

        PROPERTY(Editable, Category = "Polygonization", ClampMin = 3, ClampMax = 6)
        int32 VertsPerPoly = 6;

        PROPERTY(Editable, Category = "DetailMesh", ClampMin = 0.0f)
        float DetailSampleDist = 6.0f;

        PROPERTY(Editable, Category = "DetailMesh", ClampMin = 0.0f)
        float DetailSampleMaxError = 1.0f;

        // Tiles are the unit of parallel bake and runtime streaming.
        PROPERTY(Editable, Category = "Tiling", ClampMin = 16, ClampMax = 1024)
        int32 TileSizeVoxels = 64;
    };

    // Convex XZ footprint plus a vertical band that stamps an area id onto the voxelized surface.
    struct FNavAreaVolume
    {
        // World-space footprint in perimeter order, with Y ignored.
        TVector<FVector3> Hull;

        float MinY = 0.0f;
        float MaxY = 0.0f;

        // ENavArea id stamped onto covered spans, where Null carves the surface away.
        uint8 Area = 0;
    };

    // Point-to-point jump, ladder or teleport link stitched into the navmesh at bake time.
    struct FNavOffMeshLink
    {
        FVector3 Start = FVector3(0.0f);
        FVector3 End   = FVector3(0.0f);

        // Snap radius around each endpoint used to attach the link to a polygon.
        float Radius = 0.5f;

        bool bBidirectional = true;

        uint8  Area  = 1;                        // ENavArea::Ground
        uint16 Flags = (uint16)(1 << 3);         // ENavPolyFlag::Jump

        // Echoed back by Detour on traversal so gameplay can identify the link.
        uint32 UserId = 0;
    };

    struct FNavTileCoord
    {
        int32 X = 0;
        int32 Y = 0;

        // The FNavTileCache generation the tile's geometry was gathered at, which a cached heightfield has to match.
        uint32 CacheGeneration = 0;
    };

    namespace NavTile
    {
        // Tile coords are signed, so the two halves are packed rather than added.
        FORCEINLINE uint64 PackKey(int32 TX, int32 TY)
        {
            return ((uint64)(uint32)TY << 32) | (uint64)(uint32)TX;
        }

        FORCEINLINE void UnpackKey(uint64 Key, int32& OutTX, int32& OutTY)
        {
            OutTX = (int32)(uint32)(Key & 0xFFFFFFFFull);
            OutTY = (int32)(uint32)(Key >> 32);
        }

        // Shortest XZ distance from a point to the tile's footprint, zero when inside it.
        FORCEINLINE float DistanceToTile(const FVector3& Point, const FVector3& Origin, float TileWorldSize, int32 TX, int32 TY)
        {
            const float MinX = Origin.x + (float)TX * TileWorldSize;
            const float MinZ = Origin.z + (float)TY * TileWorldSize;
            const float DX = Math::Max(0.0f, Math::Max(MinX - Point.x, Point.x - (MinX + TileWorldSize)));
            const float DZ = Math::Max(0.0f, Math::Max(MinZ - Point.z, Point.z - (MinZ + TileWorldSize)));
            return Math::Sqrt(DX * DX + DZ * DZ);
        }
    }

    // Per-tile baked blob in the format dtNavMesh::addTile expects.
    REFLECT()
    struct RUNTIME_API SNavTileData
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        int32 X = 0;

        PROPERTY(Hidden)
        int32 Y = 0;

        PROPERTY(Hidden)
        TVector<uint8> Blob;
    };

    // Per-corner marker from findStraightPath, so a follower can tell ground from a link hop.
    enum class ENavCornerFlag : uint8
    {
        None            = 0,
        PathStart       = 1 << 0,
        PathEnd         = 1 << 1,
        OffMeshLink     = 1 << 2,
    };

    // Why a path query ended as it did, set on every FindPath whether or not it produced a route.
    REFLECT()
    enum class ENavPathResult : uint8
    {
        NotQueried,
        Success,
        NoNavMesh,
        NavigationCompiledOut,
        QueryUnavailable,
        StartOffNavMesh,
        EndOffNavMesh,
        NoRoute,
        CornersUnavailable,
        Partial,
        Truncated,
    };

    // Reason text for a path result, phrased to read as the tail of a log line.
    constexpr const char* ToString(ENavPathResult Result)
    {
        switch (Result)
        {
            case ENavPathResult::NotQueried:            return "no path query has run yet";
            case ENavPathResult::Success:               return "found a complete path";
            case ENavPathResult::NoNavMesh:             return "no baked navmesh is ready in this world";
            case ENavPathResult::NavigationCompiledOut: return "navigation was compiled out of this build";
            case ENavPathResult::QueryUnavailable:      return "every pooled navmesh query was busy, so nothing ran";
            case ENavPathResult::StartOffNavMesh:       return "the start location is not on the navmesh";
            case ENavPathResult::EndOffNavMesh:         return "the target location is not on the navmesh";
            case ENavPathResult::NoRoute:               return "no route connects the start to the target";
            case ENavPathResult::CornersUnavailable:    return "a corridor was found but its corners could not be extracted";
            case ENavPathResult::Partial:               return "only found a partial path, so the target is unreachable";
            case ENavPathResult::Truncated:             return "the path was cut short by a buffer or search limit";
        }
        return "unrecognized path result";
    }

    // Result of an async path request, owned by the requester and polled or awaited.
    struct FNavPath
    {
        TVector<FVector3> Corners;

        // One ENavCornerFlag per corner. OffMeshLink means gameplay drives the hop, not the follower.
        TVector<uint8> CornerFlags;

        // The route misses the goal, and the last corner is as close as the query got.
        bool bPartial = false;

        // A buffer or search limit cut the result, so re-query from the last corner instead of stopping there.
        bool bTruncated = false;

        bool bValid   = false;

        // Every pooled query was busy, so nothing ran. Distinct from no route found.
        bool bQueryUnavailable = false;

        // Topology epoch this was found against, so a follower across frames re-queries once it stops matching the mesh.
        uint64 Epoch = 0;

        // Why the query ended as it did, and ToString gives the loggable reason.
        ENavPathResult Result = ENavPathResult::NotQueried;
    };

    // Outcome of a surface walk along a straight line.
    REFLECT()
    struct SNavRaycastResult
    {
        GENERATED_BODY()

        // Where the walk stopped, at the wall it hit or at End when nothing blocked it.
        PROPERTY(Hidden)
        FVector3 Point = FVector3(0.0f);

        // Wall normal at the hit, zero when unobstructed.
        PROPERTY(Hidden)
        FVector3 Normal = FVector3(0.0f);

        // Fraction along Start to End at which the walk stopped, one when unobstructed.
        PROPERTY(Hidden)
        float T = 1.0f;

        // True when a navmesh edge blocked the walk.
        PROPERTY(Hidden)
        bool bHit = false;
    };

    // A point query's answer, carrying the found flag a by-value result cannot express as an optional.
    REFLECT()
    struct SNavPoint
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FVector3 Point = FVector3(0.0f);

        PROPERTY(Hidden)
        bool bFound = false;
    };

    // Per-call query parameters, cheap to copy with no heap.
    struct FNavQueryFilter
    {
        uint16 IncludeFlags = 0xFFFF;
        uint16 ExcludeFlags = 0;
        float  AreaCost[64] = {};   // 1.0 by default and copied into dtQueryFilter

        // Half-extents for findNearestPoly snapping, generous on Y for cell-quantized poly Y.
        FVector3 QueryExtents = FVector3(2.0f, 16.0f, 2.0f);

        // Corners this caller can store, where zero takes the project default.
        int32 MaxCorners = 0;

        FNavQueryFilter()
        {
            for (int32 i = 0; i < 64; ++i) AreaCost[i] = 1.0f;
            AreaCost[(uint8)ENavArea::Water] = 4.0f;
        }
    };

    enum class ENavBakeState : uint8
    {
        Idle,
        Gathering,
        Building,
        Combining,
        Initializing,
        Ready,
        Failed,
    };
}
