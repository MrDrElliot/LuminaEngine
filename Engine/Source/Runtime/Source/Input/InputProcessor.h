#pragma once
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"
#include "Input/Input.h"
#include "Input/InputMode.h"

namespace Lumina
{
    class FInputContext;
    class CWorld;

    // Facade over the active FInputContext; falls through to the viewport registry, safe defaults if none.
    class FInputProcessor
    {
    public:

        LE_NO_COPYMOVE(FInputProcessor);
        FInputProcessor() = default;
        ~FInputProcessor() = default;

        RUNTIME_API static FInputProcessor& Get();

        RUNTIME_API double GetMouseX() const;
        RUNTIME_API double GetMouseY() const;
        RUNTIME_API double GetMouseZ() const;
        RUNTIME_API double GetMouseDeltaX() const;
        RUNTIME_API double GetMouseDeltaY() const;

        RUNTIME_API Input::EKeyState   GetKeyState(EKey KeyCode) const;
        RUNTIME_API Input::EMouseState GetMouseButtonState(EMouseKey MouseCode) const;

        RUNTIME_API bool IsKeyDown(EKey KeyCode) const;
        RUNTIME_API bool IsKeyUp(EKey KeyCode) const;
        RUNTIME_API bool IsKeyPressed(EKey KeyCode) const;
        RUNTIME_API bool IsKeyReleased(EKey KeyCode) const;
        RUNTIME_API bool IsKeyRepeated(EKey KeyCode) const;

        RUNTIME_API bool IsMouseButtonDown(EMouseKey MouseCode) const;
        RUNTIME_API bool IsMouseButtonUp(EMouseKey MouseCode) const;
        RUNTIME_API bool IsMouseButtonPressed(EMouseKey MouseCode) const;
        RUNTIME_API bool IsMouseButtonReleased(EMouseKey MouseCode) const;
        RUNTIME_API float GetMouseButtonHeldTime(EMouseKey MouseCode) const;

        // CallerWorld targets that world's viewport (per-window capture); null = the global active viewport.
        // Routing per-caller is essential with multiple game-preview windows: a non-active player releasing
        // its own capture must not clobber the active viewport's mode.
        RUNTIME_API void       SetMouseMode(EMouseMode Mode, CWorld* CallerWorld = nullptr);
        RUNTIME_API void       SetInputMode(EInputMode Mode);
        RUNTIME_API EInputMode GetInputMode() const;

    private:

        FInputContext* GetActiveContext() const;
    };
}
