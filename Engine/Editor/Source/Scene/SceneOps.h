#pragma once

#include "World/ECS/Registry.h"

#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "Core/Reflection/PropertyChangedEvent.h"

namespace Lumina
{
    class CStruct;
}

namespace Lumina::SceneOps
{
    // Null when no reflected component answers to that name.
    NODISCARD EDITOR_API CStruct* ResolveComponentType(FStringView TypeName);

    // Worked out before a transaction opens, because opening one snapshots the whole registry.
    struct FAddComponentPlan
    {
        // Targets that do not have the component yet, which are the ones an apply would change.
        TVector<ECS::FEntity> Missing;

        uint32 AlreadyPresent = 0;

        // Null when the name resolved to nothing, which leaves prefabs unaware of the add.
        CStruct* Component = nullptr;

        NODISCARD bool HasWork() const { return !Missing.IsEmpty(); }
    };

    NODISCARD EDITOR_API FAddComponentPlan PlanAddComponent(ECS::FRegistry& Registry,
        const TVector<ECS::FEntity>& Targets, CStruct* Type);

    // Records nothing on the undo stack, so the caller brackets this in whichever transaction fits.
    EDITOR_API void ApplyAddComponent(ECS::FRegistry& Registry, const FAddComponentPlan& Plan,
        CStruct* Type);

    // True when the entity carries any kind of collider, so it already takes part in collision.
    NODISCARD EDITOR_API bool HasCollider(const ECS::FRegistry& Registry, ECS::FEntity Entity);

    // The entity shows a static mesh and has no collider, so AddMeshCollision would give it one.
    NODISCARD EDITOR_API bool CanAddMeshCollision(const ECS::FRegistry& Registry, ECS::FEntity Entity);

    // A Mesh Collider over the entity's own static mesh, on a static body unless it already has one. False when it cannot.
    EDITOR_API bool AddMeshCollision(ECS::FRegistry& Registry, ECS::FEntity Entity);

    // AddMeshCollision for an entity just placed into a world, when the project's Placement settings ask for it.
    EDITOR_API bool AddPlacementCollision(ECS::FRegistry& Registry, ECS::FEntity Entity);

    // Runs the hooks the details panel runs, for a caller that stores into a component directly.
    class EDITOR_API FPropertyEditScope
    {
    public:

        FPropertyEditScope(ECS::FRegistry& InRegistry, ECS::FEntity InEntity, CStruct* OuterType,
            void* InOuterInstance, FProperty* Property, const void* ValuePtr);

        ~FPropertyEditScope();

        FPropertyEditScope(const FPropertyEditScope&) = delete;
        FPropertyEditScope& operator = (const FPropertyEditScope&) = delete;
        FPropertyEditScope(FPropertyEditScope&&) = delete;
        FPropertyEditScope& operator=(FPropertyEditScope&&) = delete;

    private:

        ECS::FRegistry&        Registry;
        ECS::FEntity                 Entity;
        void*                   OuterInstance = nullptr;
        FPropertyChangedEvent   Event;
    };
}
