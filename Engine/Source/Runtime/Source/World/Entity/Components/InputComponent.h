#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectMacros.h"
#include "Input/InputActionMap.h"
#include "Input/InputContext.h"
#include "Input/InputProcessor.h"
#include "Input/InputViewport.h"
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"
#include "InputComponent.generated.h"

namespace Lumina
{
    // Map a friendly Lua key name ("W", "Space", "Shift", "Ctrl") to an EKey. Letters are ASCII == EKey.
    inline bool KeyNameToEKey(const FName& Name, EKey& Out)
    {
        const char* S = Name.c_str();
        if (S == nullptr || S[0] == '\0') { return false; }
        if (S[1] == '\0')
        {
            char C = S[0];
            if (C >= 'a' && C <= 'z') { C = static_cast<char>(C - 'a' + 'A'); }
            if (C >= 'A' && C <= 'Z') { Out = static_cast<EKey>(static_cast<uint16>(C)); return true; }
            if (C >= '0' && C <= '9') { Out = static_cast<EKey>(static_cast<uint16>(C)); return true; }
            if (C == ' ')             { Out = EKey::Space; return true; }
        }
        if (strcmp(S, "Space") == 0)                                    { Out = EKey::Space;       return true; }
        if (strcmp(S, "Shift") == 0 || strcmp(S, "LeftShift") == 0)     { Out = EKey::LeftShift;   return true; }
        if (strcmp(S, "Ctrl") == 0  || strcmp(S, "LeftControl") == 0)   { Out = EKey::LeftControl; return true; }
        return false;
    }

    REFLECT(Component, Category = "Gameplay")
    struct RUNTIME_API SInputComponent
    {
        GENERATED_BODY()

        /** When false, every query returns its safe default (false / 0). */
        PROPERTY(Editable)
        bool bEnabled = true;

        /** Reserved for future split-screen routing; currently unused. */
        PROPERTY(Editable)
        int32 PlayerIndex = 0;
        
        CWorld* World = nullptr;
        
        bool   bReceivingInput = false;
        double SnapMouseX = 0.0;
        double SnapMouseY = 0.0;
        double SnapMouseZ = 0.0;
        double SnapMouseDeltaX = 0.0;
        double SnapMouseDeltaY = 0.0;
        TArray<Input::EKeyState,   (uint32)EKey::Num>      KeyStates   = {};
        TArray<Input::EMouseState, (uint32)EMouseKey::Num> MouseStates = {};

        void SnapshotFrom(const FInputContext& Ctx, bool bReceiving)
        {
            bReceivingInput = bReceiving;
            if (!bReceiving)
            {
                ResetSnapshot();
                return;
            }
            for (uint32 i = 0; i < (uint32)EKey::Num; ++i)      { KeyStates[i]   = Ctx.GetKeyState((EKey)i); }
            for (uint32 i = 0; i < (uint32)EMouseKey::Num; ++i) { MouseStates[i] = Ctx.GetMouseButtonState((EMouseKey)i); }
            SnapMouseX      = Ctx.GetMouseX();
            SnapMouseY      = Ctx.GetMouseY();
            SnapMouseZ      = Ctx.GetMouseZ();
            SnapMouseDeltaX = Ctx.GetMouseDeltaX();
            SnapMouseDeltaY = Ctx.GetMouseDeltaY();
        }

        void ResetSnapshot()
        {
            bReceivingInput = false;
            for (auto& S : KeyStates)   { S = Input::EKeyState::Up; }
            for (auto& S : MouseStates) { S = Input::EMouseState::Up; }
            SnapMouseX = SnapMouseY = SnapMouseZ = SnapMouseDeltaX = SnapMouseDeltaY = 0.0;
        }
        
        /** This frame's evaluated state for an action, or a zeroed state when input is off, the world has
         *  no viewport, or the name is not an authored action. Every action query reads through here. */
        const FInputActionState& GetActionState(const FName& Name) const
        {
            static const FInputActionState Empty;
            if (!bEnabled)
            {
                return Empty;
            }
            const FInputViewport* V = FInputViewportRegistry::Get().FindViewportForWorld(World);
            if (V == nullptr)
            {
                return Empty;
            }
            return FInputActionMap::Get().GetActionState(Name, V->GetContext());
        }

        FUNCTION(Script)
        bool IsActionDown(const FName& Name) const
        {
            return GetActionState(Name).IsDown();
        }

        FUNCTION(Script)
        bool IsActionPressed(const FName& Name) const
        {
            return GetActionState(Name).IsPressed();
        }

        FUNCTION(Script)
        bool IsActionReleased(const FName& Name) const
        {
            return GetActionState(Name).IsReleased();
        }

        FUNCTION(Script)
        float GetActionAxis(const FName& Name) const
        {
            return GetActionState(Name).X;
        }

        /** The Y channel of an Axis2D action (GetActionAxis reads X). 0 for any other action type. */
        FUNCTION(Script)
        float GetActionAxisY(const FName& Name) const
        {
            return GetActionState(Name).Y;
        }

        /** True while the action has been down for at least its authored HoldTime. */
        FUNCTION(Script)
        bool IsActionHeld(const FName& Name) const
        {
            return GetActionState(Name).IsHeld();
        }

        /** True on the frame a press shorter than the action's TapTime was released. */
        FUNCTION(Script)
        bool WasActionTapped(const FName& Name) const
        {
            return GetActionState(Name).IsTapped();
        }

        /** Seconds the current press has lasted, 0 while the action is up. */
        FUNCTION(Script)
        float GetActionHeldTime(const FName& Name) const
        {
            return GetActionState(Name).HeldTime;
        }

        /** +1 while Positive is down, -1 while Negative is down, 0 when neither is. Holding both
         *  cancels out, which is what a key pair should do rather than latching whichever side is
         *  tested first. For a single action already declared as an axis, use GetActionAxis. */
        FUNCTION(Script)
        float GetAxis(const FName& Positive, const FName& Negative) const
        {
            if (!bEnabled)
            {
                return 0.0f;
            }
            const FInputViewport* V = FInputViewportRegistry::Get().FindViewportForWorld(World);
            if (V == nullptr)
            {
                return 0.0f;
            }

            // Resolved once and reused: the pair is two lookups against the same context, and
            // going back through IsActionDown would find the viewport again for each.
            const FInputActionMap& Map = FInputActionMap::Get();
            const FInputContext& Ctx = V->GetContext();

            return (Map.IsActionDown(Positive, Ctx) ? 1.0f : 0.0f)
                 - (Map.IsActionDown(Negative, Ctx) ? 1.0f : 0.0f);
        }

        /** GetAxis over raw key names ("W"/"S"), for input that is not worth an action mapping. */
        FUNCTION(Script)
        float GetKeyAxis(const FName& PositiveKey, const FName& NegativeKey) const
        {
            return (IsKeyDown(PositiveKey) ? 1.0f : 0.0f) - (IsKeyDown(NegativeKey) ? 1.0f : 0.0f);
        }

        FUNCTION(Script)
        bool IsInputActive() const { return bEnabled && bReceivingInput; }
        
        FUNCTION(Script)
        bool IsKeyDown(const FName& KeyName) const
        {
            if (!bEnabled)
            {
                return false;
            }
            EKey Key;
            if (!KeyNameToEKey(KeyName, Key)) { return false; }
            const Input::EKeyState S = KeyStates[(uint32)Key];
            return S == Input::EKeyState::Pressed || S == Input::EKeyState::Held || S == Input::EKeyState::Repeated;
        }

        FUNCTION(Script)
        bool IsKeyPressed(const FName& KeyName) const
        {
            if (!bEnabled)
            {
                return false;
            }
            EKey Key;
            return KeyNameToEKey(KeyName, Key) && KeyStates[(uint32)Key] == Input::EKeyState::Pressed;
        }

        FUNCTION(Script)
        bool IsKeyReleased(const FName& KeyName) const
        {
            if (!bEnabled)
            {
                return false;
            }
            EKey Key;
            return KeyNameToEKey(KeyName, Key) && KeyStates[(uint32)Key] == Input::EKeyState::Released;
        }

        FUNCTION(Script)
        double GetMouseX()      const { return bEnabled ? SnapMouseX      : 0.0; }

        FUNCTION(Script)
        double GetMouseY()      const { return bEnabled ? SnapMouseY      : 0.0; }

        FUNCTION(Script)
        double GetMouseZ()      const { return bEnabled ? SnapMouseZ      : 0.0; }

        FUNCTION(Script)
        double GetMouseDeltaX() const { return bEnabled ? SnapMouseDeltaX : 0.0; }

        FUNCTION(Script)
        double GetMouseDeltaY() const { return bEnabled ? SnapMouseDeltaY : 0.0; }
    };
}
