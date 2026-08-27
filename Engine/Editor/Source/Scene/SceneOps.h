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

        NODISCARD bool HasWork() const { return !Missing.empty(); }
    };

    NODISCARD EDITOR_API FAddComponentPlan PlanAddComponent(ECS::FRegistry& Registry,
        const TVector<ECS::FEntity>& Targets, CStruct* Type);

    // Records nothing on the undo stack, so the caller brackets this in whichever transaction fits.
    EDITOR_API void ApplyAddComponent(ECS::FRegistry& Registry, const FAddComponentPlan& Plan,
        CStruct* Type);

    // Runs the hooks the details panel runs, for a caller that stores into a component directly.
    class EDITOR_API FPropertyEditScope
    {
    public:

        FPropertyEditScope(ECS::FRegistry& InRegistry, ECS::FEntity InEntity, CStruct* OuterType,
            void* InOuterInstance, FProperty* Property, const void* ValuePtr);

        ~FPropertyEditScope();

        FPropertyEditScope(const FPropertyEditScope&) = delete;
        FPropertyEditScope& operator = (const FPropertyEditScope&) = delete;

    private:

        ECS::FRegistry&        Registry;
        ECS::FEntity                 Entity;
        void*                   OuterInstance = nullptr;
        FPropertyChangedEvent   Event;
    };
}
