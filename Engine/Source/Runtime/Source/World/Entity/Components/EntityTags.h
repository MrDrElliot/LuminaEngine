#pragma once

#include "Core/Object/ObjectMacros.h"
#include "EntityTags.generated.h"

namespace Lumina
{
    // Exported so the editor can reach StaticStruct when recording it as a prefab override.
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SDisabledTag
    {
        GENERATED_BODY()
    };

    // Present suppresses this entity's scripts while the entity itself stays active.
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SScriptDisabledTag
    {
        GENERATED_BODY()
    };

    // Another peer is authoritative over this entity and this one only mirrors it, so local systems make no gameplay decisions for it.
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SNetProxyTag
    {
        GENERATED_BODY()
    };

    // A proxy whose transform that peer also streams, so local movement leaves it alone.
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SNetDrivenTransformTag
    {
        GENERATED_BODY()
    };

}
