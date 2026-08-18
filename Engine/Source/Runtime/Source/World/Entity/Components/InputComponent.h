#pragma once

#include "Core/Object/ObjectMacros.h"
#include "InputComponent.generated.h"

namespace Lumina
{
    // A tag, not a data store: it says this entity's scripts receive input. The input itself lives once per
    // world in FInputContext and is read through Input:: (Input/InputQuery.h). What reaches the entity is
    // shaped by the world's pushed mapping layers, not by anything stored here.
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SInputComponent
    {
        GENERATED_BODY()

        /** When false the entity receives no input: no OnInput, no binding events. */
        PROPERTY(Editable)
        bool bEnabled = true;
    };
}
