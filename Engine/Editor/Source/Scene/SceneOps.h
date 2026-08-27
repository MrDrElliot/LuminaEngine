#pragma once

#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "Core/Reflection/PropertyChangedEvent.h"
#include "World/Entity/EntityHandle.h"

namespace Lumina
{
    class CStruct;
}

namespace Lumina::SceneOps
{
    // Invalid when no reflected component answers to that name, which is what an unknown name means.
    NODISCARD EDITOR_API entt::meta_type ResolveComponentType(FStringView TypeName);

    // Worked out before a transaction opens, because opening one snapshots the whole registry.
    struct FAddComponentPlan
    {
        // Targets that do not have the component yet, which are the ones an apply would change.
        TVector<FEntity> Missing;

        uint32 AlreadyPresent = 0;

        // Null when the meta type carries no reflected struct, which leaves prefabs unaware of the add.
        CStruct* Component = nullptr;

        NODISCARD bool HasWork() const { return !Missing.empty(); }
    };

    NODISCARD EDITOR_API FAddComponentPlan PlanAddComponent(FEntityRegistry& Registry,
        const TVector<FEntity>& Targets, entt::meta_type Type);

    // Records nothing on the undo stack, so the caller brackets this in whichever transaction fits.
    EDITOR_API void ApplyAddComponent(FEntityRegistry& Registry, const FAddComponentPlan& Plan,
        entt::meta_type Type);

    // Runs the hooks the details panel runs, for a caller that stores into a component directly.
    class EDITOR_API FPropertyEditScope
    {
    public:

        FPropertyEditScope(FEntityRegistry& InRegistry, FEntity InEntity, CStruct* OuterType,
            void* InOuterInstance, FProperty* Property, const void* ValuePtr);

        ~FPropertyEditScope();

        FPropertyEditScope(const FPropertyEditScope&) = delete;
        FPropertyEditScope& operator = (const FPropertyEditScope&) = delete;

    private:

        FEntityRegistry&        Registry;
        FEntity                 Entity;
        void*                   OuterInstance = nullptr;
        FPropertyChangedEvent   Event;
    };
}
