#pragma once

#include "GameplayTags/GameplayTag.h"
#include "Core/Object/ObjectMacros.h"
#include "GameplayTagComponent.generated.h"

namespace Lumina
{
    // The gameplay tags attached to an entity. Author the starting set in the editor (each element uses the
    // tag picker); scripts add/remove/query at runtime via World.Tags. Hierarchical matching applies to
    // queries (an entity tagged "Status.Burning" satisfies a "Status" query).
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SGameplayTagComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Gameplay")
        FGameplayTagContainer Tags;
    };
}
