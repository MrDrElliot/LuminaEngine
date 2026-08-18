#pragma once

#include "Core/Object/ObjectMacros.h"
#include "InputComponent.generated.h"

namespace Lumina
{
    // Script-only: enables OnInput and binding events. Systems read Input:: (Input/InputQuery.h) directly.
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SInputComponent
    {
        GENERATED_BODY()

        /** When false the entity receives no input: no OnInput, no binding events. */
        PROPERTY(Editable)
        bool bEnabled = true;
    };
}
