#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"
#include "Input/InputAction.h"
#include "Input/InputMode.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "InputSubsystem.generated.h"

namespace Lumina
{
    // Input read from the world's own viewport, neutral while the world is not receiving it.
    REFLECT()
    class RUNTIME_API CInputSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        /** False when the world has no viewport, is not the active one, or the editor holds input focus. */
        FUNCTION()
        bool IsReceivingInput();

        //~ Authored actions, the rebindable surface.

        /** This frame's evaluated state, zeroed when the name is not an authored action. */
        FUNCTION()
        SInputActionState GetActionState(const FName& Action);

        FUNCTION()
        bool IsActionDown(const FName& Action);

        FUNCTION()
        bool WasActionPressed(const FName& Action);

        FUNCTION()
        bool WasActionReleased(const FName& Action);

        FUNCTION()
        bool IsActionHeld(const FName& Action);

        FUNCTION()
        bool WasActionTapped(const FName& Action);

        FUNCTION()
        bool WasActionDoubleTapped(const FName& Action);

        /** True on the press and again each time a repeat comes due while held. */
        FUNCTION()
        bool IsActionRepeated(const FName& Action);

        FUNCTION()
        bool IsActionBuffered(const FName& Action);

        /** Takes the buffered press so it acts once, returning whether there was one. */
        FUNCTION()
        bool ConsumeBufferedPress(const FName& Action);

        FUNCTION()
        float GetActionAxis(const FName& Action);

        FUNCTION()
        FVector2 GetActionAxis2D(const FName& Action);

        FUNCTION()
        float GetActionHeldTime(const FName& Action);

        /** Plus one while Positive is down and minus one while Negative is, so holding both cancels. */
        FUNCTION()
        float GetAxisPair(const FName& Positive, const FName& Negative);

        //~ Mapping layers, per world. A blocking layer stops any action it does not list.

        FUNCTION()
        void PushLayer(const FName& Layer);

        /** False when the layer was not on the stack. */
        FUNCTION()
        bool PopLayer(const FName& Layer);

        FUNCTION()
        bool HasLayer(const FName& Layer);

        FUNCTION()
        void ClearLayers();

        //~ Raw device state, shared by every entity in the world and not rebindable.

        FUNCTION()
        bool IsKeyDown(EKey Key);

        FUNCTION()
        bool WasKeyPressed(EKey Key);

        FUNCTION()
        bool WasKeyReleased(EKey Key);

        FUNCTION()
        bool IsMouseButtonDown(EMouseKey Button);

        FUNCTION()
        bool WasMouseButtonPressed(EMouseKey Button);

        FUNCTION()
        bool WasMouseButtonReleased(EMouseKey Button);

        FUNCTION()
        FVector2 GetMousePosition();

        FUNCTION()
        FVector2 GetMouseDelta();

        FUNCTION()
        float GetMouseWheel();

        /** False when no pad is connected, or the world is not receiving input. */
        FUNCTION()
        bool IsGamepadConnected();

        FUNCTION()
        bool IsGamepadButtonDown(EGamepadButton Button);

        FUNCTION()
        bool WasGamepadButtonPressed(EGamepadButton Button);

        FUNCTION()
        bool WasGamepadButtonReleased(EGamepadButton Button);

        /** Dead-zoned, with stick Y positive up and triggers from 0 to 1. */
        FUNCTION()
        float GetGamepadAxis(EGamepadAxis Axis);

        FUNCTION()
        FVector2 GetGamepadLeftStick();

        FUNCTION()
        FVector2 GetGamepadRightStick();

        //~ Prompts, which follow whichever device the player touched last.

        FUNCTION()
        EInputDevice GetLastUsedDevice();

        /** Such as "E" or "Cross", and empty when the action has no binding on the last used device. */
        FUNCTION()
        FString GetActionPrompt(const FName& Action);

        //~ Cursor and routing, which act on the world's own viewport rather than the receiving one.

        FUNCTION()
        void SetInputMode(EInputMode Mode);

        FUNCTION()
        EInputMode GetInputMode();

        FUNCTION()
        void SetMouseMode(EMouseMode Mode);

        FUNCTION()
        EMouseMode GetMouseMode();

        // Combined with every other holder's request, and the last release restores the modes from before the first.
        FUNCTION()
        uint64 AcquireInputMode(EInputMode InputMode, EMouseMode MouseMode);

        FUNCTION()
        bool ReleaseInputMode(uint64 Lease);

        // Drops every lease and restores the earlier modes.
        void ClearInputModes();

        int32 NumInputModeLeases() const { return Leases.Num(); }

        void OnTeardown() override;

    private:

        struct FLease
        {
            uint64     Id;
            EInputMode InputMode;
            EMouseMode MouseMode;
        };

        void ApplyInputModes();

        TVector<FLease> Leases;
        uint64          NextId = 1;
        EInputMode      BaseInputMode = EInputMode::Game;
        EMouseMode      BaseMouseMode = EMouseMode::Captured;
    };
}
