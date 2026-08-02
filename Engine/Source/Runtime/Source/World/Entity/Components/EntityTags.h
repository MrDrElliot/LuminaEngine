#pragma once

#include "Core/Object/ObjectMacros.h"
#include "EntityTags.generated.h"

namespace Lumina
{
    REFLECT(Component, HideInComponentList)
    struct SDisabledTag
    {
        GENERATED_BODY()
    };

    // Present = this entity's Lua script is suppressed (does not tick) while the entity itself
    // stays active. Toggled from the outliner's per-row script button; the script system excludes it.
    REFLECT(Component, HideInComponentList)
    struct SScriptDisabledTag
    {
        GENERATED_BODY()
    };

}