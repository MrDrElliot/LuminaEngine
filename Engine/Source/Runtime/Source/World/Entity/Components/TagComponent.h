#pragma once

#include "TagComponent.generated.h"

namespace Lumina
{
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API STagComponent
    {
        GENERATED_BODY()

        /** String identifier used to categorize or query this entity. */
        PROPERTY(Script)
        FName Tag;
    };
}
