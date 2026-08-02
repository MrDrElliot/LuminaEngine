#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "Input/Key.h"
#include "InputAction.generated.h"

namespace Lumina
{
    // One physical binding for an action: an SKey (keyboard/mouse, with its own Ctrl/Shift/Alt chord)
    // plus a scale used only by axis actions.
    REFLECT()
    struct RUNTIME_API SInputActionBinding
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        SKey Key;

        // Axis actions only: value contributed while Key is held (e.g. +1 / -1). Ignored for digital.
        PROPERTY(Editable)
        float Scale = 1.0f;
    };

    // A named gameplay input. Digital actions fire when any bound key is down; axis actions sum the
    // Scale of every held binding.
    REFLECT()
    struct RUNTIME_API SInputAction
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        FName Name;

        // Axis = sum of held bindings' Scale; Digital = any bound key down.
        PROPERTY(Editable)
        bool bAxis = false;

        // Keep firing while the active context is in EInputMode::UI (pause / save hotkeys).
        PROPERTY(Editable)
        bool bRunsInUI = false;

        PROPERTY(Editable)
        TVector<SInputActionBinding> Bindings;
    };
}
