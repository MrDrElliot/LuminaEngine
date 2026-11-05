#pragma once

#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/BlockoutComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "BlockoutSystem.generated.h"

namespace Lumina
{
    /** Meshes every SBlockoutComponent whose parameters moved, and every one a load left unbuilt. */
    REFLECT()
    class RUNTIME_API SBlockoutSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;
        void OnUpdate(const SSystemContext& Context) override;

        /** Immediate rebuild for callers that cannot wait a tick, such as the editor placing a shape. */
        static bool Rebuild(SBlockoutComponent& Shape, SDynamicMeshComponent& Mesh, bool bKeepCPUData);

    private:

        // A shape only needs a look when it was written, added or loaded, or when a collider arrived after its build.
        using FChangedShapes = ECS::TQuery<ECS::Read<SBlockoutComponent>, ECS::Read<SDynamicMeshComponent>, ECS::Changed<SBlockoutComponent>>;
        using FNewColliders  = ECS::TQuery<ECS::Read<SBlockoutComponent>, ECS::Read<SDynamicMeshComponent>, ECS::Added<SDynamicMeshColliderComponent>>;

        FChangedShapes ChangedShapes;
        FNewColliders  NewColliders;
        ECS::TQuery<ECS::Write<SBlockoutComponent>, ECS::Write<SDynamicMeshComponent>> Builds;
        TVector<ECS::FEntity> DirtyScratch;
        TVector<ECS::FEntity> MaterialScratch;
    };
}
