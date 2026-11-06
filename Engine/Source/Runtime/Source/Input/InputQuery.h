#pragma once

#include "Containers/Span.h"
#include "Containers/String.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"
#include "Input/InputAction.h"
#include "Input/InputEvent.h"
#include "Input/InputMode.h"

namespace Lumina
{
    class CWorld;
    class FInputContext;
}

// The C++ per-world input polling surface, which CInputSubsystem exposes to scripts as World.Input while CInputLibrary only holds rebinding. Every read returns neutral values while the world is not receiving.
namespace Lumina::Input
{
    // The one gate on gameplay input. Null unless the world's viewport is the active one and the editor has handed input to the game.
    RUNTIME_API const FInputContext* GetReceivingContext(const CWorld* World);

    RUNTIME_API bool IsReceivingInput(const CWorld* World);

    //~ Authored actions, the rebindable surface.

    // Zeroed when the world is not receiving or the handle names no authored action, so callers never null-check.
    RUNTIME_API const SInputActionState& GetActionState(const CWorld* World, const SInputActionHandle& Action);

    inline bool     IsActionDown     (const CWorld* World, const SInputActionHandle& Action) { return GetActionState(World, Action).IsDown(); }
    inline bool     IsActionPressed  (const CWorld* World, const SInputActionHandle& Action) { return GetActionState(World, Action).IsPressed(); }
    inline bool     IsActionReleased (const CWorld* World, const SInputActionHandle& Action) { return GetActionState(World, Action).IsReleased(); }
    inline bool     IsActionHeld     (const CWorld* World, const SInputActionHandle& Action) { return GetActionState(World, Action).IsHeld(); }
    inline bool     WasActionTapped  (const CWorld* World, const SInputActionHandle& Action) { return GetActionState(World, Action).IsTapped(); }
    inline bool     WasActionDoubleTapped(const CWorld* World, const SInputActionHandle& Action) { return GetActionState(World, Action).IsDoubleTapped(); }
    inline bool     IsActionRepeated (const CWorld* World, const SInputActionHandle& Action) { return GetActionState(World, Action).IsRepeated(); }
    inline bool     IsActionBuffered (const CWorld* World, const SInputActionHandle& Action) { return GetActionState(World, Action).IsBuffered(); }
    inline float    GetActionAxis    (const CWorld* World, const SInputActionHandle& Action) { return GetActionState(World, Action).X; }
    inline float    GetActionHeldTime(const CWorld* World, const SInputActionHandle& Action) { return GetActionState(World, Action).HeldTime; }
    inline FVector2 GetActionAxis2D  (const CWorld* World, const SInputActionHandle& Action)
    {
        const SInputActionState& State = GetActionState(World, Action);
        return FVector2(State.X, State.Y);
    }

    // Plus one while Positive is down and minus one while Negative is, so holding both cancels.
    RUNTIME_API float GetAxisPair(const CWorld* World, const SInputActionHandle& Positive, const SInputActionHandle& Negative);

    // Takes the action's buffered press so it acts once, returning whether there was one.
    RUNTIME_API bool ConsumeBufferedPress(const CWorld* World, const SInputActionHandle& Action);

    //~ Prompts, which follow whichever device the player touched last.

    RUNTIME_API EInputDevice   GetLastUsedDevice(const CWorld* World);
    RUNTIME_API EGamepadFamily GetGamepadFamily();

    // Such as "E" or "Cross", from the action's first binding on the last used device, and empty when it has none there.
    RUNTIME_API FString GetActionPrompt(const CWorld* World, const SInputActionHandle& Action);

    //~ Rebinding, for a controls menu. A rebind applies at once and is kept by saving the user settings.

    RUNTIME_API int32   GetBindingCount(FName Action);
    RUNTIME_API FString GetBindingDisplayName(FName Action, int32 BindingIndex);

    // Only key bindings rebind, so a continuous source such as Mouse X reports false.
    RUNTIME_API bool    IsBindingRebindable(FName Action, int32 BindingIndex);

    // Captures the next key, mouse button or pad button pressed, where Escape cancels.
    RUNTIME_API bool    BeginRebind(FName Action, int32 BindingIndex);
    RUNTIME_API void    CancelRebind();
    RUNTIME_API bool    IsRebinding();

    // Another action already bound to the key the last rebind captured, or none.
    RUNTIME_API FName   GetRebindConflict();

    // Back to the authored keys for Action, or for every action when it is none.
    RUNTIME_API void    ResetBindings(FName Action);

    //~ Mapping layers, authored on CInputSettings and stacked per world, which a pause menu pushes to block gameplay actions.

    RUNTIME_API void PushLayer(const CWorld* World, FName Layer);
    RUNTIME_API bool PopLayer(const CWorld* World, FName Layer);
    RUNTIME_API bool HasLayer(const CWorld* World, FName Layer);
    RUNTIME_API void ClearLayers(const CWorld* World);

    //~ Raw devices, shared by every entity in the world and not rebindable, so prefer an action.

    RUNTIME_API bool IsKeyDown    (const CWorld* World, EKey Key);
    RUNTIME_API bool IsKeyPressed (const CWorld* World, EKey Key);
    RUNTIME_API bool IsKeyReleased(const CWorld* World, EKey Key);

    RUNTIME_API bool IsMouseButtonDown    (const CWorld* World, EMouseKey Button);
    RUNTIME_API bool IsMouseButtonPressed (const CWorld* World, EMouseKey Button);
    RUNTIME_API bool IsMouseButtonReleased(const CWorld* World, EMouseKey Button);

    RUNTIME_API FVector2 GetMousePosition(const CWorld* World);
    RUNTIME_API FVector2 GetMouseDelta(const CWorld* World);
    RUNTIME_API float    GetMouseWheel(const CWorld* World);

    RUNTIME_API bool IsGamepadConnected     (const CWorld* World);
    RUNTIME_API bool IsGamepadButtonDown    (const CWorld* World, EGamepadButton Button);
    RUNTIME_API bool IsGamepadButtonPressed (const CWorld* World, EGamepadButton Button);
    RUNTIME_API bool IsGamepadButtonReleased(const CWorld* World, EGamepadButton Button);

    // Dead-zoned, with stick Y positive up and triggers from 0 to 1.
    RUNTIME_API float    GetGamepadAxis(const CWorld* World, EGamepadAxis Axis);
    RUNTIME_API FVector2 GetGamepadLeftStick(const CWorld* World);
    RUNTIME_API FVector2 GetGamepadRightStick(const CWorld* World);

    // This frame's discrete events in order, cleared by the context at the end of the frame.
    RUNTIME_API TSpan<const SInputEvent> GetFrameEvents(const CWorld* World);

    //~ Cursor and routing, which act on the world's own viewport even while it is not receiving, as layers do.

    RUNTIME_API void       SetMouseMode(const CWorld* World, EMouseMode Mode);
    RUNTIME_API EMouseMode GetMouseMode(const CWorld* World);

    RUNTIME_API void       SetInputMode(const CWorld* World, EInputMode Mode);
    RUNTIME_API EInputMode GetInputMode(const CWorld* World);
}
