#pragma once

#include "Containers/StaticArray.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"
#include "Input/Gamepad.h"
#include "Input/Input.h"
#include "Input/InputAction.h"
#include "Input/InputEvent.h"
#include "Input/InputMode.h"

namespace Lumina
{
    class FEvent;

    // One viewport's devices, mode and action states. Gated getters read zero in UI mode, and Raw ones are for the action map.
    class FInputContext
    {
    public:

        LE_NO_COPYMOVE(FInputContext);
        FInputContext() = default;
        ~FInputContext() = default;

        RUNTIME_API EInputMode GetInputMode() const { return InputMode; }
        RUNTIME_API void       SetInputMode(EInputMode Mode) { InputMode = Mode; }

        RUNTIME_API EMouseMode GetMouseMode() const { return MouseMode; }
        RUNTIME_API void       SetMouseMode(EMouseMode Mode) { MouseMode = Mode; }

        // The cursor this world shows while it holds the mouse, drawn from the project's hardware cursors or by the system.
        RUNTIME_API EMouseCursor GetMouseCursor() const { return MouseCursor; }
        RUNTIME_API void         SetMouseCursor(EMouseCursor Cursor) { MouseCursor = Cursor; }

        void SetWindowRect(int MinX, int MinY, int MaxX, int MaxY);
        bool ContainsWindowPoint(int WindowX, int WindowY) const;

        void SetRenderTargetSize(uint32 W, uint32 H);
        uint32 GetRenderTargetWidth() const { return RTWidth; }
        uint32 GetRenderTargetHeight() const { return RTHeight; }

        bool WindowToContext(double WindowX, double WindowY, double& OutX, double& OutY) const;

        // The inverse, for input injected at a pixel of the rendered view.
        RUNTIME_API void ContextToWindow(double ContextX, double ContextY, double& OutWindowX, double& OutWindowY) const;

        RUNTIME_API bool OnEvent(FEvent& Event);

        //~ Keyboard and mouse.

        RUNTIME_API bool IsKeyDown(EKey K)     const { return !IsGameInputGated() && IsKeyDownRaw(K); }
        RUNTIME_API bool IsKeyPressed(EKey K)  const { return !IsGameInputGated() && KeyStates[(uint32)K] == Input::EButtonState::Pressed; }
        RUNTIME_API bool IsKeyReleased(EKey K) const { return !IsGameInputGated() && KeyStates[(uint32)K] == Input::EButtonState::Released; }

        RUNTIME_API bool IsMouseButtonDown(EMouseKey M)     const { return !IsGameInputGated() && IsMouseButtonDownRaw(M); }
        RUNTIME_API bool IsMouseButtonPressed(EMouseKey M)  const { return !IsGameInputGated() && MouseStates[(uint32)M] == Input::EButtonState::Pressed; }
        RUNTIME_API bool IsMouseButtonReleased(EMouseKey M) const { return !IsGameInputGated() && MouseStates[(uint32)M] == Input::EButtonState::Released; }

        RUNTIME_API double GetMouseX()      const { return IsGameInputGated() ? 0.0 : MouseX; }
        RUNTIME_API double GetMouseY()      const { return IsGameInputGated() ? 0.0 : MouseY; }
        RUNTIME_API double GetMouseZ()      const { return IsGameInputGated() ? 0.0 : MouseZ; }
        RUNTIME_API double GetMouseDeltaX() const { return IsGameInputGated() ? 0.0 : MouseDeltaX; }
        RUNTIME_API double GetMouseDeltaY() const { return IsGameInputGated() ? 0.0 : MouseDeltaY; }

        bool IsKeyDownRaw(EKey K) const                 { return Input::IsDown(KeyStates[(uint32)K]); }
        bool IsMouseButtonDownRaw(EMouseKey M) const    { return Input::IsDown(MouseStates[(uint32)M]); }

        // Either side's key counts, read from key state so a chord works in every input mode.
        bool IsCtrlDownRaw() const  { return IsKeyDownRaw(EKey::LeftControl) || IsKeyDownRaw(EKey::RightControl); }
        bool IsShiftDownRaw() const { return IsKeyDownRaw(EKey::LeftShift) || IsKeyDownRaw(EKey::RightShift); }
        bool IsAltDownRaw() const   { return IsKeyDownRaw(EKey::LeftAlt) || IsKeyDownRaw(EKey::RightAlt); }

        // The game UI still needs the real cursor while the gated getters hide it from gameplay.
        double GetMouseXRaw() const      { return MouseX; }
        double GetMouseYRaw() const      { return MouseY; }
        double GetMouseDeltaXRaw() const { return MouseDeltaX; }
        double GetMouseDeltaYRaw() const { return MouseDeltaY; }
        double GetMouseZRaw() const      { return MouseZ; }

        // The first key or mouse button that went down this frame, for a rebind capture, or Num.
        EKey      GetFirstPressedKey() const;
        EMouseKey GetFirstPressedMouseButton() const;

        //~ Gamepad. Only the receiving context is fed a pad, so every other context reads a disconnected one.

        // Takes this frame's pad and queues an event for every button edge, so OnInput sees pad presses like key presses.
        RUNTIME_API void SetGamepadState(const FGamepadState& State);

        RUNTIME_API bool  IsGamepadConnected() const                        { return Gamepad.bConnected; }
        RUNTIME_API bool  IsGamepadButtonDown(EGamepadButton B) const       { return !IsGameInputGated() && Gamepad.IsButtonDown(B); }
        RUNTIME_API bool  IsGamepadButtonPressed(EGamepadButton B) const    { return !IsGameInputGated() && Gamepad.IsButtonPressed(B); }
        RUNTIME_API bool  IsGamepadButtonReleased(EGamepadButton B) const   { return !IsGameInputGated() && Gamepad.IsButtonReleased(B); }
        RUNTIME_API float GetGamepadAxis(EGamepadAxis Axis) const           { return IsGameInputGated() ? 0.0f : Gamepad.GetAxis(Axis); }

        const FGamepadState& GetGamepadRaw() const { return Gamepad; }

        // The device the player touched last, which prompts follow, and the label family of the pad in use.
        RUNTIME_API EInputDevice   GetLastUsedDevice() const { return LastUsedDevice; }
        RUNTIME_API EGamepadFamily GetGamepadFamily() const  { return Gamepad.Family; }

        //~ Actions and layers.

        // This frame's action states, indexed like FInputActionMap::GetAllActions() and filled once per frame by UpdateContext.
        const TVector<SInputActionState>& GetActionStates() const { return ActionStates; }
        TVector<SInputActionState>&       GetMutableActionStates() { return ActionStates; }

        // What the triggers carry between frames, parallel to the action states and never seen by scripts.
        struct FActionTriggerState
        {
            bool  bRawDown = false;
            bool  bToggledOn = false;
            float SinceLastPress = -1.0f;
            float RepeatTimer = 0.0f;
            float BufferRemaining = 0.0f;
        };
        TVector<FActionTriggerState>& GetMutableTriggerStates() { return TriggerStates; }

        // Clears a buffered press so it acts once, returning whether there was one.
        RUNTIME_API bool ConsumeBufferedPress(int32 ActionIndex);

        // Pushed mapping layers, innermost last, held by name so re-authoring the settings cannot leave a dangling entry.
        const TVector<FName>& GetInputLayers() const { return InputLayers; }

        // Re-pushing a layer moves it to the top instead of stacking it, so a double push and one pop never leave it on.
        RUNTIME_API void PushInputLayer(FName Name);
        RUNTIME_API bool PopInputLayer(FName Name);
        RUNTIME_API bool HasInputLayer(FName Name) const;
        RUNTIME_API void ClearInputLayers();

        // The action-table serial these states were sized against; a mismatch means the settings changed.
        uint32 GetActionsSerial() const { return ActionsSerial; }
        void   SetActionsSerial(uint32 InSerial) { ActionsSerial = InSerial; }

        // This frame's discrete events in order, read by the script OnInput dispatch and cleared in EndFrame.
        const TVector<SInputEvent>& GetFrameEvents() const { return FrameEvents; }

        RUNTIME_API void EndFrame();

        // Drops state when focus is taken so a release we never saw doesn't latch.
        RUNTIME_API void ResetState();

    private:

        bool IsGameInputGated() const { return InputMode == EInputMode::UI; }

        // Mouse events carry no modifiers, so their chord comes from the keys held when they arrive.
        void AddHeldModifiers(SInputEvent& Event) const;

        EInputMode  InputMode = EInputMode::Game;
        EMouseMode  MouseMode = EMouseMode::Normal;
        EMouseCursor MouseCursor = EMouseCursor::Default;

        int32  RectMinX = 0;
        int32  RectMinY = 0;
        int32  RectMaxX = 0;
        int32  RectMaxY = 0;
        uint32 RTWidth = 0;
        uint32 RTHeight = 0;

        double MouseX = 0.0;
        double MouseY = 0.0;
        double MouseZ = 0.0;
        double MouseDeltaX = 0.0;
        double MouseDeltaY = 0.0;

        TArray<Input::EButtonState, (uint32)EKey::Num>      KeyStates = {};
        TArray<Input::EButtonState, (uint32)EMouseKey::Num> MouseStates = {};

        FGamepadState Gamepad;
        EInputDevice  LastUsedDevice = EInputDevice::KeyboardMouse;

        TVector<FName>                 InputLayers;
        TVector<SInputActionState>     ActionStates;
        TVector<FActionTriggerState>   TriggerStates;
        uint32                         ActionsSerial = 0;
        TVector<SInputEvent>           FrameEvents;
    };
}
