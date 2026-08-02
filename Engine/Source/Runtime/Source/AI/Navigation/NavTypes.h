#pragma once

#include "Core/Object/ObjectMacros.h"
#include "NavTypes.generated.h"

namespace Lumina
{
    /** Area ids for dtQueryFilter (0..63). 0..15 are reserved by the bake. */
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

    /** Per-poly flag set; AND-ed at query time. */
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

    /** Offline voxelization + region build settings. */
    REFLECT()
    struct RUNTIME_API FNavBuildSettings
    {
        GENERATED_BODY()

        /** Voxel cell size in world units. Smaller = sharper edges, much slower bake (cubic cost). */
        PROPERTY(Editable, Category = "Voxel", ClampMin = 0.05f)
        float CellSize = 0.30f;

        /** Vertical voxel size. Drives stair / climb resolution. */
        PROPERTY(Editable, Category = "Voxel", ClampMin = 0.05f)
        float CellHeight = 0.20f;

        /** Agent radius used to erode the walkable surface. */
        PROPERTY(Editable, Category = "Agent", ClampMin = 0.0f)
        float AgentRadius = 0.40f;

        /** Agent capsule height; surfaces with less clearance are excluded. */
        PROPERTY(Editable, Category = "Agent", ClampMin = 0.1f)
        float AgentHeight = 1.80f;

        /** Maximum vertical step (stairs, ledges) the agent can climb. */
        PROPERTY(Editable, Category = "Agent", ClampMin = 0.0f)
        float AgentMaxClimb = 0.40f;

        /** Steepest walkable slope, in degrees. */
        PROPERTY(Editable, Category = "Agent", ClampMin = 0.0f, ClampMax = 89.0f)
        float AgentMaxSlopeDeg = 45.0f;

        /** Max contour edge length in world units. Longer edges are split. */
        PROPERTY(Editable, Category = "Polygonization", ClampMin = 0.0f)
        float EdgeMaxLength = 12.0f;

        /** Max distance a simplified contour edge may deviate from the raw contour. */
        PROPERTY(Editable, Category = "Polygonization", ClampMin = 0.1f)
        float EdgeMaxError = 1.3f;

        /** Floor area below which a region is dropped (in voxels). */
        PROPERTY(Editable, Category = "Region", ClampMin = 0)
        int32 RegionMinSize = 8;

        /** Adjacent regions smaller than this are merged. */
        PROPERTY(Editable, Category = "Region", ClampMin = 0)
        int32 RegionMergeSize = 20;

        /** Max vertices per nav polygon (3..6 typical). */
        PROPERTY(Editable, Category = "Polygonization", ClampMin = 3, ClampMax = 6)
        int32 VertsPerPoly = 6;

        /** Detail mesh sample distance (world units). */
        PROPERTY(Editable, Category = "DetailMesh", ClampMin = 0.0f)
        float DetailSampleDist = 6.0f;

        /** Detail mesh max sample error (world units). */
        PROPERTY(Editable, Category = "DetailMesh", ClampMin = 0.0f)
        float DetailSampleMaxError = 1.0f;

        /** Voxels per tile side. Tiles are the unit of parallel bake + runtime streaming. */
        PROPERTY(Editable, Category = "Tiling", ClampMin = 16, ClampMax = 1024)
        int32 TileSizeVoxels = 64;
    };

    /** Per-tile baked blob in the format dtNavMesh::addTile expects. */
    REFLECT()
    struct RUNTIME_API FNavTileData
    {
        GENERATED_BODY()

        PROPERTY()
        int32 X = 0;

        PROPERTY()
        int32 Y = 0;

        PROPERTY()
        TVector<uint8> Blob;
    };

    /** Result of an async path request. Owned by the requester; can be polled or awaited. */
    struct FNavPath
    {
        TVector<FVector3> Corners;
        bool bPartial = false;
        bool bValid   = false;
    };

    /** Per-call query parameters. Cheap to copy, no heap. */
    struct FNavQueryFilter
    {
        uint16 IncludeFlags = 0xFFFF;
        uint16 ExcludeFlags = 0;
        float  AreaCost[64] = {};   // 1.0 default; copied into dtQueryFilter

        /** Half-extents for findNearestPoly snapping; generous on Y for cell-quantized poly Y. */
        FVector3 QueryExtents = FVector3(2.0f, 16.0f, 2.0f);

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
