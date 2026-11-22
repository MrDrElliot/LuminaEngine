#pragma once

#include "imgui.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    // Keyboard chord for an FEditorAction, matching modifiers exactly. Use ImGuiKey_None for no key.
    struct FInputChord
    {
        ImGuiKey Key   = ImGuiKey_None;
        bool     bCtrl  = false;
        bool     bShift = false;
        bool     bAlt   = false;

        bool IsValid() const { return Key != ImGuiKey_None; }

        // "Ctrl+Shift+V", "F", "Home", etc. Empty string when invalid.
        FString ToDisplayString() const;
    };

    // A named editor command shown in Help > Keyboard Shortcuts, registered in OnInitialize.
    struct FEditorAction
    {
        // Short imperative name shown in UI. e.g. "Translate Mode".
        FString             Name;

        // Group label used by the shortcuts window. e.g. "Gizmo", "Selection".
        FString             Category;

        // One-line tooltip shown next to the binding.
        FString             Description;

        // Default chord that fires the callback, the single source of truth for tool keybinds.
        FInputChord         DefaultChord;

        // Invoked when the chord triggers and CanExecute (if set) returns true.
        TFunction<void()>   Callback;

        // Optional gate evaluated before Callback, and null means always executable.
        TFunction<bool()>   CanExecute;

        // True for held-style actions such as arrow-key nudges that fire on each repeat tick.
        bool                bRepeatOnHold = false;
    };
}
