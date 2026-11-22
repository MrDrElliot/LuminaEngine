#pragma once

#include "HierarchicalTags/HierarchicalTag.h"
#include "Core/Object/ObjectMacros.h"
#include "HierarchicalTagComponent.generated.h"

namespace Lumina
{
    // The hierarchical tags attached to an entity, where a tag of Status.Burning satisfies a Status query.
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SHierarchicalTagComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Gameplay")
        SHierarchicalTagContainer Tags;
    };
}
