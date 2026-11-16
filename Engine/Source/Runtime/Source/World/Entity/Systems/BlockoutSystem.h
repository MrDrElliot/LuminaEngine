#pragma once

#include "EntitySystem.h"
#include "StampedEntityCache.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/BlockoutComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "BlockoutSystem.generated.h"

namespace Lumina
{
    // Stamped so a recycled entity slot never reads as already queued by a stale pass.
    struct FBlockoutDirtyMark
    {
        ECS::FEntity Owner = ECS::NullEntity;
        uint32       Stamp = 0;
    };

    /** Meshes every SBlockoutComponent whose parameters moved, and every one a load left unbuilt. */
    REFLECT()
    class RUNTIME_API CBlockoutSystem : public CEntitySystem
    {
        GENERATED_BODY()

        friend struct FBlockoutSystemTestAccess;

    public:

        void Configure() override;
        void OnUpdate(const SSystemContext& Context) override;

        /** Immediate rebuild for callers that cannot wait a tick, such as the editor placing a shape. A collider gets the same triangles. */
        static bool Rebuild(SBlockoutComponent& Shape, SDynamicMeshComponent& Mesh, SDynamicMeshColliderComponent* Collider);

    private:

        // A shape only needs a look when it was written, added or loaded, or when a collider arrived after its build.
        using FChangedShapes = ECS::TQuery<ECS::Read<SBlockoutComponent>, ECS::Read<SDynamicMeshComponent>, ECS::Changed<SBlockoutComponent>>;
        using FNewColliders  = ECS::TQuery<ECS::Read<SBlockoutComponent>, ECS::Read<SDynamicMeshComponent>, ECS::Added<SDynamicMeshColliderComponent>>;

        // Fills DirtyScratch, ColliderScratch and MaterialScratch from ChangedShapes and NewColliders, with no rebuild side effects.
        void GatherDirty();

        // A collider that arrived after its shape's build gets the shape's triangles; the mesh is left alone.
        static void CommitCollision(const SBlockoutComponent& Shape, SDynamicMeshColliderComponent& Collider);

        FChangedShapes ChangedShapes;
        FNewColliders  NewColliders;
        ECS::TQuery<ECS::Write<SBlockoutComponent>, ECS::Write<SDynamicMeshComponent>> Builds;
        ECS::TQuery<ECS::Write<SDynamicMeshColliderComponent>> Colliders;
        TVector<ECS::FEntity> DirtyScratch;
        TVector<ECS::FEntity> ColliderScratch;
        TVector<ECS::FEntity> MaterialScratch;

        // O(1) membership test for DirtyScratch, so NewColliders never rescans it per entity.
        TStampedEntityCache<FBlockoutDirtyMark> DirtyLookup;
    };
}
