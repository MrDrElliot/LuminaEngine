#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Containers/Name.h"
#include "NameComponent.generated.h"

namespace Lumina
{
    // HideInDetails as well as HideInComponentList: the details header already shows the name and
    // offers Rename, so a component row for it would be a second way to edit the same field.
    REFLECT(Component, HideInComponentList, HideInDetails)
    struct RUNTIME_API SNameComponent
    {
        GENERATED_BODY()

        SNameComponent() = default;
        SNameComponent(FName InName)
            : Name(std::move(InName))
        {}

        /** Display name of the entity shown in the editor hierarchy. */
        PROPERTY(ReadOnly, Replicated)
        FName Name;
    };
}
