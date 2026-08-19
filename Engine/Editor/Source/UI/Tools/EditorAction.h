#pragma once

#include "imgui.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    // Keyboard chord that triggers an FEditorAction. Modifiers are matched exactly:
    // bCtrl=false means CTRL must NOT be held. Use ImGuiKey_None for "no key".
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

    // A named editor command shown in Help > Keyboard Shortcuts. Tools register these in
    // OnInitialize(); FEditorTool::Update() dispatches them once per frame.
    struct FEditorAction
    {
        // Short imperative name shown in UI. e.g. "Translate Mode".
        FString             Name;

        // Group label used by the shortcuts window. e.g. "Gizmo", "Selection".
        FString             Category;

        // One-line tooltip shown next to the binding.
        FString             Description;

        // Default chord that fires the callback; single source of truth for tool
        // keybinds (remapping can wrap this later without touching tool code).
        FInputChord         DefaultChord;

        // Invoked when the chord triggers and CanExecute (if set) returns true.
        TFunction<void()>   Callback;

        // Optional gate evaluated immediately before Callback. Use for
        // viewport-hovered / tool-state checks. Null => always executable.
        TFunction<bool()>   CanExecute;

        // True for held-style actions (e.g. arrow-key nudges) that should fire
        // each repeat tick, not just on the initial press.
        bool                bRepeatOnHold = false;
    };
}
