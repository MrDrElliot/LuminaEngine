#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Containers/Name.h"
#include "NameComponent.generated.h"

namespace Lumina
{
    // Hidden from details too, since the details header already shows the name and offers Rename.
    REFLECT(Component, HideInComponentList, HideInDetails)
    struct RUNTIME_API SNameComponent
    {
        GENERATED_BODY()

        SNameComponent() = default;
        SNameComponent(FName InName)
            : Name(Move(InName))
        {}

        /** Display name of the entity shown in the editor hierarchy. */
        PROPERTY(ReadOnly, Replicated)
        FName Name;
    };
}
