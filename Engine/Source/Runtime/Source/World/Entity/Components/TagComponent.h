#pragma once

#include "TagComponent.generated.h"

namespace Lumina
{
    // Tags render as chips in their own details section rather than as a component row.
    REFLECT(Component, HideInComponentList, HideInDetails)
    struct RUNTIME_API STagComponent
    {
        GENERATED_BODY()

        /** String identifier used to categorize or query this entity. */
        PROPERTY(Script)
        FName Tag;
    };
}
