#pragma once

#include "AI/Navigation/NavTypes.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "NavigationLibrary.generated.h"

namespace Lumina
{
    class CWorld;

    // With no baked navmesh every query reports not found. Agent names the volume to query, where empty means the infantry default.
    REFLECT()
    class RUNTIME_API CNavigationLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        /** True once a navmesh has been baked and finished hydrating, so queries can succeed. */
        FUNCTION()
        static bool IsReady(CWorld* World, FName Agent = FName());

        /** Writes the route's corners into the caller's buffer and returns how many it wrote. */
        FUNCTION()
        static int32 FindPath(CWorld* World, FVector3 Start, FVector3 End, FVector3* Corners, int32 MaxCorners, FName Agent = FName());

        /** Whether the last route between these points stops short of the goal, for any reason. */
        FUNCTION()
        static bool IsPathPartial(CWorld* World, FVector3 Start, FVector3 End, int32 MaxCorners, FName Agent = FName());

        // Why a route between these points ended as it did, so a failure can say which part failed.
        FUNCTION()
        static ENavPathResult GetPathResult(CWorld* World, FVector3 Start, FVector3 End, int32 MaxCorners, FName Agent = FName());

        // Reason text for a path result, ready to drop into a log line.
        FUNCTION()
        static FString DescribePathResult(ENavPathResult Result);

        /** Snaps onto the nearest walkable surface within the half-extents search box. */
        FUNCTION()
        static FNavPoint ProjectPoint(CWorld* World, FVector3 Point, FVector3 Extents, FName Agent = FName());

        /** Walks the surface toward End and reports where a wall stopped it. */
        FUNCTION()
        static FNavRaycastResult Raycast(CWorld* World, FVector3 Start, FVector3 End, FName Agent = FName());

        /** Whether the straight line stays on walkable surface the whole way, far cheaper than a path. */
        FUNCTION()
        static bool IsWalkableLine(CWorld* World, FVector3 From, FVector3 To, FName Agent = FName());

        FUNCTION()
        static FNavPoint FindRandomReachablePoint(CWorld* World, FVector3 Origin, float Radius, FName Agent = FName());

        /** Whether a complete route exists, so a partial one reports false. */
        FUNCTION()
        static bool IsReachable(CWorld* World, FVector3 From, FVector3 To, FName Agent = FName());

        /** Total route length in world units, or a negative value when unreachable. */
        FUNCTION()
        static float PathLength(CWorld* World, FVector3 From, FVector3 To, FName Agent = FName());

        /** Flags every navmesh volume for an async rebuild and returns how many it flagged. */
        FUNCTION()
        static int32 RequestRebuild(CWorld* World);

        FUNCTION()
        static void DrawPath(CWorld* World, FVector3 From, FVector3 To, FVector4 Color, float Duration = 0.0f, FName Agent = FName());
    };
}
