#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Containers/Name.h"
#include "EntityGuidComponent.generated.h"

namespace Lumina
{
    // An authored entity's identity, kept through save and load and never shared with a copy.
    REFLECT(Component, HideInComponentList, HideInDetails)
    struct RUNTIME_API SEntityGuidComponent
    {
        GENERATED_BODY()

        PROPERTY(ReadOnly)
        FName Guid;
    };
}
