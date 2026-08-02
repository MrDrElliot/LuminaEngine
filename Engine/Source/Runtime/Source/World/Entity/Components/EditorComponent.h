#pragma once

#include "Core/Object/ObjectMacros.h"
#include "EditorComponent.generated.h"

namespace Lumina
{
    struct RUNTIME_API FSelectedInEditorComponent { };
    struct RUNTIME_API FHideInSceneOutliner { };
    struct RUNTIME_API FEditorComponent { };

    // Viewport picks on any descendant resolve up to the nearest ancestor carrying this tag.
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API FSelectionRoot
    {
        GENERATED_BODY()
    };
}
