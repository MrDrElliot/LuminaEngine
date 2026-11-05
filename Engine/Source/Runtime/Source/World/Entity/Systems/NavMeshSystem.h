#pragma once

#include "EntitySystem.h"
#include "MovedTransforms.h"
#include "SignificanceSystem.h"
#include "AI/Navigation/NavTypes.h"
#include "Core/Object/ObjectMacros.h"
#include "Memory/SmartPtr.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/NavLinkComponent.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/NavModifierComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "NavMeshSystem.generated.h"

namespace Lumina
{
    struct FNavSourceTracker;

    // Owns SNavMeshComponent lifecycle: rehydrate, drain bakes, rebuild on Tiles change.
    REFLECT()
    class RUNTIME_API SNavMeshSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        SNavMeshSystem();
        ~SNavMeshSystem() override;

        // Paused stage required so editor-mode ticks for bake button, debug draw, dirty detection.
        void Configure() override;

        // Source tracking runs every frame, so a volume added later finds it current.
        bool HasWork(const SSystemContext&) override { return true; }

        void OnStartup(const SExclusiveContext& Context) override;
        void OnUpdate(const SSystemContext& Context) override;
        void OnTeardown(const SExclusiveContext& Context) override;

        // The colliders and authored volumes that feed the navmesh, walked by the helpers in the .cpp.
        struct FSourceQueries
        {
            template<typename T>
            using TSource = ECS::TQuery<ECS::Read<T>, ECS::Read<STransformComponent>>;

            TSource<SBoxColliderComponent>          Boxes;
            TSource<SSphereColliderComponent>       Spheres;
            TSource<SMeshColliderComponent>         MeshColliders;
            TSource<SDynamicMeshColliderComponent>  DynamicMeshColliders;
            TSource<SCapsuleColliderComponent>      Capsules;
            TSource<SCylinderColliderComponent>     Cylinders;
            TSource<SCharacterPhysicsComponent>     Characters;
            TSource<SCompoundColliderComponent>     Compounds;
            TSource<SNavModifierComponent>          Modifiers;
            TSource<SNavLinkComponent>              Links;
            TSource<STerrainColliderComponent>      TerrainColliders;

            ECS::TQuery<ECS::Read<SFoliageComponent>>               Foliage;
            ECS::TQuery<ECS::Read<STerrainColliderComponent>>       TerrainPool;
            ECS::TQuery<ECS::Read<SDynamicMeshColliderComponent>>   DynamicMeshPool;

            // Lookups only, each strict to its one component, for what a source reads beside its collider.
            ECS::TQuery<ECS::Read<SRigidBodyComponent>>             RigidBodies;
            ECS::TQuery<ECS::Read<STerrainComponent>>               Terrains;
            ECS::TQuery<ECS::Read<SStaticMeshComponent>>            StaticMeshes;
            ECS::TQuery<ECS::Read<SDynamicMeshComponent>>           DynamicMeshes;

            // Watched only, so a world of moving props never reaches the drain.
            FMovedTransforms Moved{ EMovedTransforms::Watched };
        };

    private:

        ECS::TQuery<ECS::Write<SNavMeshComponent>, ECS::Optional<ECS::Read<STransformComponent>>> Volumes;
        FSourceQueries                             SourceQueries;
        TRes<FSignificanceState>                   SignificanceState;
        TUniquePtr<FNavSourceTracker>              Tracker;
    };

    class CWorld;
    class FNavMesh;

    // Navigation helpers; each function dispatches to the first ready SNavMeshComponent.
    namespace Nav
    {
        using FNavMeshQuery = ECS::TQuery<ECS::Read<SNavMeshComponent>>;

        // An unnamed agent takes a volume with no agent name, or failing that any ready volume. A named one takes only its own.
        RUNTIME_API FNavMesh* GetReadyNavMesh(const FNavMeshQuery& NavMeshes, FName Agent = FName());

        RUNTIME_API bool FindPath(const FNavMeshQuery& NavMeshes, const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FNavPath& Out, FName Agent = FName());
        RUNTIME_API bool ProjectPoint(const FNavMeshQuery& NavMeshes, const FVector3& World, const FVector3& Extents, const FNavQueryFilter& Filter, FVector3& Out, FName Agent = FName());
        RUNTIME_API bool Raycast(const FNavMeshQuery& NavMeshes, const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, SNavRaycastResult& Out, FName Agent = FName());

        RUNTIME_API bool IsReady(CWorld* World, FName Agent = FName());

        // Flags every navmesh volume for an async rebuild next tick and returns how many were flagged.
        RUNTIME_API int32 RequestRebuild(CWorld* World);

        RUNTIME_API bool FindPath(CWorld* World, const FVector3& Start, const FVector3& End, FNavPath& Out, FName Agent = FName());

        // MaxCorners bounds the result to what the caller stores; a longer route comes back truncated.
        RUNTIME_API bool FindPath(CWorld* World, const FVector3& Start, const FVector3& End, int32 MaxCorners, FNavPath& Out, FName Agent = FName());
        RUNTIME_API bool ProjectPoint(CWorld* World, const FVector3& Point, const FVector3& Extents, FVector3& Out, FName Agent = FName());
        /** Returns false when the query could not run at all; Out.bHit says whether a wall blocked the walk. */
        RUNTIME_API bool Raycast(CWorld* World, const FVector3& Start, const FVector3& End, SNavRaycastResult& Out, FName Agent = FName());

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
