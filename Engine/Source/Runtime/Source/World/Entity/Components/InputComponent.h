#pragma once

#include "Containers/Name.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/ObjectMacros.h"
#include "Input/InputAction.h"
#include "Input/InputEvent.h"
#include "InputComponent.generated.h"

namespace Lumina
{
    // Where an entity's scripts hear input, while its viewport has game input focus. Systems read Input:: (Input/InputQuery.h) directly.
    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SInputComponent
    {
        GENERATED_BODY()

        // Off keeps the listeners and silences both events, where removing the component drops the listeners with it.
        PROPERTY(Editable)
        bool bEnabled = true;

        // Each raw input event of the frame, in arrival order.
        PROPERTY(Hidden)
        TScriptDelegate<SInputEvent> OnInput;

        // Each authored action that changed this frame, after every OnInput of the frame.
        PROPERTY(Hidden)
        TScriptDelegate<FName, SInputActionState> OnAction;
    };
}
