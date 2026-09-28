#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "AI/Navigation/NavTypes.h"
#include "NavMeshSystem.generated.h"

namespace Lumina
{
    struct SNavMeshComponent;

    /** Owns SNavMeshComponent lifecycle: rehydrate, drain bakes, rebuild on Tiles change. */
    REFLECT()
    class RUNTIME_API SNavMeshSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        // Paused stage required so editor-mode ticks for bake button, debug draw, dirty detection.
        void Configure() override;

    public:

        // Update only reads colliders/transforms and writes SNavMeshComponent (no structural changes),
        // so it overlaps animation/camera in the editor (Paused) stage. Defined in the .cpp.

        void OnStartup() override;
        void OnUpdate() override;
        void OnTeardown() override;

        /** Kick async bake; no-op if one is already in flight. */
        static void RequestBake(const FSystemContext& Context, SNavMeshComponent& Component);
    };

    class CWorld;
    class FNavMesh;

    /** Navigation helpers; each function dispatches to the first ready SNavMeshComponent. */
    namespace Nav
    {
        /** First ready navmesh in the world, or null. Resolve once and reuse across a batch of queries. */
        // An unnamed agent takes a volume with no agent name, or failing that any ready volume. A named one takes only its own.
        RUNTIME_API FNavMesh* GetReadyNavMesh(const FSystemContext& Context, FName Agent = FName());

        RUNTIME_API bool FindPath(const FSystemContext& Context, const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FNavPath& Out, FName Agent = FName());
        RUNTIME_API bool ProjectPoint(const FSystemContext& Context, const FVector3& World, const FVector3& Extents, const FNavQueryFilter& Filter, FVector3& Out, FName Agent = FName());
        RUNTIME_API bool Raycast(const FSystemContext& Context, const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FNavRaycastResult& Out, FName Agent = FName());

        RUNTIME_API bool IsReady(CWorld* World, FName Agent = FName());

        /** Flag every navmesh volume in the world for an async rebuild (consumed next tick). Returns the
         *  number of volumes flagged. The script-facing "rebuild navigation" entry point. */
        RUNTIME_API int32 RequestRebuild(CWorld* World);

        RUNTIME_API bool FindPath(CWorld* World, const FVector3& Start, const FVector3& End, FNavPath& Out, FName Agent = FName());

        // MaxCorners bounds the result to what the caller stores; a longer route comes back truncated.
        RUNTIME_API bool FindPath(CWorld* World, const FVector3& Start, const FVector3& End, int32 MaxCorners, FNavPath& Out, FName Agent = FName());
        RUNTIME_API bool ProjectPoint(CWorld* World, const FVector3& Point, const FVector3& Extents, FVector3& Out, FName Agent = FName());
        /** Returns false when the query could not run at all; Out.bHit says whether a wall blocked the walk. */
        RUNTIME_API bool Raycast(CWorld* World, const FVector3& Start, const FVector3& End, FNavRaycastResult& Out, FName Agent = FName());

        /** True when the straight line from From to To stays on walkable surface the whole way. */
        RUNTIME_API bool IsWalkableLine(CWorld* World, const FVector3& From, const FVector3& To, FName Agent = FName());

        /** Random walkable point inside (Origin, Radius). */
        RUNTIME_API bool FindRandomReachablePoint(CWorld* World, const FVector3& Origin, float Radius, FVector3& Out, FName Agent = FName());

        RUNTIME_API bool IsReachable(CWorld* World, const FVector3& From, const FVector3& To, FName Agent = FName());

        /** Returns negative when no path exists. */
        RUNTIME_API float PathLength(CWorld* World, const FVector3& From, const FVector3& To, FName Agent = FName());

        /** Visualize an FNavPath via the World's debug drawer. Lift offsets the polyline above the mesh. */
        RUNTIME_API void DrawPath(CWorld* World, const FNavPath& Path, const FVector4& Color, float Thickness = 3.0f, float Lift = 0.15f, float Duration = 0.0f);

        /** Convenience: FindPath + DrawPath. Returns true if a (possibly partial) path was found and drawn. */
        RUNTIME_API bool DrawDebugPath(CWorld* World, const FVector3& From, const FVector3& To, const FVector4& Color, float Duration = 0.0f, FName Agent = FName());
    }
}
