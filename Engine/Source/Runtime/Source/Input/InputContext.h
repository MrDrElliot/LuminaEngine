#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"
#include "Input/Input.h"
#include "Input/InputEvent.h"
#include "Input/InputMode.h"

namespace Lumina
{
    class FEvent;

    class FInputContext
    {
    public:

        LE_NO_COPYMOVE(FInputContext);
        FInputContext() = default;
        ~FInputContext() = default;

        RUNTIME_API EInputMode GetInputMode() const { return InputMode; }
        RUNTIME_API void       SetInputMode(EInputMode Mode) { InputMode = Mode; }

        RUNTIME_API EMouseMode GetMouseMode() const { return MouseMode; }
        RUNTIME_API void       SetMouseMode(EMouseMode Mode);

        void SetWindowRect(int MinX, int MinY, int MaxX, int MaxY);
        bool ContainsWindowPoint(int WindowX, int WindowY) const;

        void SetRenderTargetSize(uint32 W, uint32 H);
        uint32 GetRenderTargetWidth() const { return RTWidth; }
        uint32 GetRenderTargetHeight() const { return RTHeight; }

        bool WindowToContext(double WindowX, double WindowY, double& OutX, double& OutY) const;

        bool OnEvent(FEvent& Event);

        RUNTIME_API double GetMouseX()      const { return IsGameInputGated() ? 0.0 : MouseX; }
        RUNTIME_API double GetMouseY()      const { return IsGameInputGated() ? 0.0 : MouseY; }
        RUNTIME_API double GetMouseZ()      const { return IsGameInputGated() ? 0.0 : MouseZ; }
        RUNTIME_API double GetMouseDeltaX() const { return IsGameInputGated() ? 0.0 : MouseDeltaX; }
        RUNTIME_API double GetMouseDeltaY() const { return IsGameInputGated() ? 0.0 : MouseDeltaY; }

        RUNTIME_API Input::EKeyState   GetKeyState(EKey K) const
        {
            return IsGameInputGated() ? Input::EKeyState::Up : KeyStates[(uint32)K];
        }
        RUNTIME_API Input::EMouseState GetMouseButtonState(EMouseKey M) const
        {
            return IsGameInputGated() ? Input::EMouseState::Up : MouseStates[(uint32)M];
        }

        RUNTIME_API bool IsKeyDown(EKey K)     const { auto S = GetKeyState(K); return S == Input::EKeyState::Pressed || S == Input::EKeyState::Held || S == Input::EKeyState::Repeated; }
        RUNTIME_API bool IsKeyUp(EKey K)       const { return GetKeyState(K) == Input::EKeyState::Up; }
        RUNTIME_API bool IsKeyPressed(EKey K)  const { return GetKeyState(K) == Input::EKeyState::Pressed; }
        RUNTIME_API bool IsKeyReleased(EKey K) const { return GetKeyState(K) == Input::EKeyState::Released; }
        RUNTIME_API bool IsKeyRepeated(EKey K) const { return GetKeyState(K) == Input::EKeyState::Repeated; }

        RUNTIME_API bool IsMouseButtonDown(EMouseKey M)     const { auto S = GetMouseButtonState(M); return S == Input::EMouseState::Pressed || S == Input::EMouseState::Held; }
        RUNTIME_API bool IsMouseButtonUp(EMouseKey M)       const { return GetMouseButtonState(M) == Input::EMouseState::Up; }
        RUNTIME_API bool IsMouseButtonPressed(EMouseKey M)  const { return GetMouseButtonState(M) == Input::EMouseState::Pressed; }
        RUNTIME_API bool IsMouseButtonReleased(EMouseKey M) const { return GetMouseButtonState(M) == Input::EMouseState::Released; }
        RUNTIME_API float GetMouseButtonHeldTime(EMouseKey M) const { return IsGameInputGated() ? -1.0f : MouseKeyDownTimes[(uint32)M]; }

        // Skips the UI-mode gate. Used by FInputActionMap, which gates per-action.
        RUNTIME_API bool IsKeyDownRaw(EKey K) const
        {
            const Input::EKeyState S = KeyStates[(uint32)K];
            return S == Input::EKeyState::Pressed || S == Input::EKeyState::Held || S == Input::EKeyState::Repeated;
        }
        RUNTIME_API bool IsMouseButtonDownRaw(EMouseKey M) const
        {
            const Input::EMouseState S = MouseStates[(uint32)M];
            return S == Input::EMouseState::Pressed || S == Input::EMouseState::Held;
        }

        // The gated GetMouseX/Y return 0 in UI mode to hide the cursor from
        // game-side queries; the RmlUi forwarder still needs the real position.
        double GetMouseXRaw() const { return MouseX; }
        double GetMouseYRaw() const { return MouseY; }

        // Discrete events that arrived this frame, in order. Populated by OnEvent, drained (read) by the
        // script OnInput dispatch during the world update, cleared in EndFrame.
        const TVector<SInputEvent>& GetFrameEvents() const { return FrameEvents; }

        RUNTIME_API void EndFrame(double DeltaSeconds);

        // Drops state when focus is taken so a release we never saw doesn't latch.
        RUNTIME_API void ResetState();

        int  GetCachedModifierState() const { return CachedModifierState; }
        void SetCachedModifierState(int Mods) { CachedModifierState = Mods; }

        bool WasActionDownLastFrame(FName ActionName) const;
        void SetActionDownLastFrame(FName ActionName, bool bDown);

        void UpdateActionEdgeState();

    private:

        bool IsGameInputGated() const { return InputMode == EInputMode::UI; }

        EInputMode  InputMode = EInputMode::Game;
        EMouseMode  MouseMode = EMouseMode::Normal;

        int32  RectMinX = 0;
        int32  RectMinY = 0;
        int32  RectMaxX = 0;
        int32  RectMaxY = 0;
        uint32 RTWidth = 0;
        uint32 RTHeight = 0;
        int    CachedModifierState = 0;

        double MouseX = 0.0;
        double MouseY = 0.0;
        double MouseZ = 0.0;
        double MouseDeltaX = 0.0;
        double MouseDeltaY = 0.0;

        TArray<float, (uint32)EMouseKey::Num>               MouseKeyDownTimes = {};
        TArray<Input::EKeyState, (uint32)EKey::Num>         KeyStates = {};
        TArray<Input::EMouseState, (uint32)EMouseKey::Num>  MouseStates = {};

        THashMap<FName, bool>          ActionDownLastFrame;
        TVector<SInputEvent>           FrameEvents;
    };
}
