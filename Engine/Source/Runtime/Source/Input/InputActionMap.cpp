#include "RuntimePCH.h"
#include "InputActionMap.h"

#include "Config/InputSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Input/InputContext.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        // A chord's modifiers must be held, while an extra modifier the chord does not name never blocks it.
        bool ModifiersSatisfied(const SKey& Key, const FInputContext& Context)
        {
            return (!Key.bCtrl || Context.IsCtrlDownRaw())
                && (!Key.bShift || Context.IsShiftDownRaw())
                && (!Key.bAlt || Context.IsAltDownRaw());
        }

        bool IsSKeyDown(const SKey& Key, const FInputContext& Context)
        {
            switch (Key.Device)
            {
            case EKeyDevice::Keyboard: return Key.IsValid() && ModifiersSatisfied(Key, Context) && Context.IsKeyDownRaw(Key.Key);
            case EKeyDevice::Mouse:    return Key.IsValid() && ModifiersSatisfied(Key, Context) && Context.IsMouseButtonDownRaw(Key.MouseButton);
            case EKeyDevice::Gamepad:  return Key.IsValid() && Context.GetGamepadRaw().IsButtonDown(Key.GamepadButton);
            case EKeyDevice::None:     break;
            }
            return false;
        }

        // The stick or trigger a gamepad axis source reads.
        EGamepadAxis GetGamepadAxisFor(EInputAxisSource Source)
        {
            switch (Source)
            {
            case EInputAxisSource::GamepadLeftX:        return EGamepadAxis::LeftX;
            case EInputAxisSource::GamepadLeftY:        return EGamepadAxis::LeftY;
            case EInputAxisSource::GamepadRightX:       return EGamepadAxis::RightX;
            case EInputAxisSource::GamepadRightY:       return EGamepadAxis::RightY;
            case EInputAxisSource::GamepadLeftTrigger:  return EGamepadAxis::LeftTrigger;
            case EInputAxisSource::GamepadRightTrigger: return EGamepadAxis::RightTrigger;
            default:                                    return EGamepadAxis::LeftX;
            }
        }

        // The binding's own dead zone and response curve, on top of the device's.
        float ShapeGamepadAxis(float Value, const SInputActionBinding& Binding)
        {
            const float Cut = Input::ApplyDeadZone(Value, Binding.DeadZone);
            return Binding.ResponseExponent == 1.0f ? Cut : Math::Sign(Cut) * Math::Pow(Math::Abs(Cut), Binding.ResponseExponent);
        }
    }

    FString SInputActionBinding::GetDisplayName(EGamepadFamily Family) const
    {
        switch (Source)
        {
        case EInputAxisSource::Key:        return Key.IsValid() ? Key.GetDisplayName(Family) : FString();
        case EInputAxisSource::MouseX:     return "Mouse X";
        case EInputAxisSource::MouseY:     return "Mouse Y";
        case EInputAxisSource::MouseWheel: return "Mouse Wheel";
        default:                           return "Gamepad " + GetGamepadAxisDisplayName(GetGamepadAxisFor(Source), Family);
        }
    }

    FInputActionMap& FInputActionMap::Get()
    {
        static FInputActionMap Instance;
        return Instance;
    }

    void FInputActionMap::RebuildFromSettings()
    {
        Rebuild();

        // An edited action can keep its name and still change shape, so a reload always restarts every context's state.
        ++Serial;
        LOG_INFO("[InputActions] Loaded {} actions, {} mapping contexts.", Actions.Num(), MappingContexts.Num());
    }

    void FInputActionMap::SetPlayerSettings(const SPlayerInputSettings& Settings)
    {
        PlayerSettings = Settings;
        Rebuild();
    }

    void FInputActionMap::Rebuild()
    {
        TVector<FName> PreviousNames;
        PreviousNames.Reserve(Actions.Num());
        for (const SInputAction& Action : Actions)
        {
            PreviousNames.Add(Action.Name);
        }

        Actions = GetDefault<CInputSettings>()->Actions;
        MappingContexts = GetDefault<CInputSettings>()->MappingContexts;
        Lookup.Reset();
        for (int32 i = 0; i < Actions.Num(); ++i)
        {
            // Folded here rather than at every read, so nothing downstream has to know about the old flag.
            Actions[i].FoldLegacyAxisFlag();
            Lookup[Actions[i].Name] = i;
        }

        // A rebind only ever replaces a key binding, so an axis source or a stale index from an older project is skipped.
        for (const SInputBindingOverride& Override : PlayerSettings.BindingOverrides)
        {
            const int32 Index = FindActionIndex(Override.Action);
            if (Index == Constants::kIndexNone || Override.BindingIndex < 0 || Override.BindingIndex >= Actions[Index].Bindings.Num())
            {
                continue;
            }
            SInputActionBinding& Binding = Actions[Index].Bindings[Override.BindingIndex];
            if (Binding.Source == EInputAxisSource::Key)
            {
                Binding.Key = Override.Key;
            }
        }

        ToggleActions.Init(false, Actions.Num());
        for (const FName& Name : PlayerSettings.ToggleActions)
        {
            const int32 Index = FindActionIndex(Name);
            if (Index != Constants::kIndexNone)
            {
                ToggleActions[Index] = true;
            }
        }

        // A player settings change keeps the layout, so a held toggle survives a sensitivity tweak in the pause menu.
        bool bLayoutChanged = PreviousNames.Num() != Actions.Num();
        for (int32 i = 0; i < Actions.Num() && !bLayoutChanged; ++i)
        {
            bLayoutChanged = PreviousNames[i] != Actions[i].Name;
        }
        if (bLayoutChanged)
        {
            ++Serial;
        }
    }

    const SInputAction* FInputActionMap::FindAction(FName Name) const
    {
        const auto It = Lookup.Find(Name);
        return It != nullptr ? &Actions[(*It)] : nullptr;
    }

    int32 FInputActionMap::FindActionIndex(FName Name) const
    {
        const auto It = Lookup.Find(Name);
        return It != nullptr ? (*It) : Constants::kIndexNone;
    }

    int32 FInputActionMap::ResolveIndex(const SInputActionHandle& Handle) const
    {
        // A details-panel edit does not bump the serial, so a serial-only check answers stale.
        if (Handle.CachedSerial != Serial || Handle.CachedName != Handle.Name)
        {
            Handle.CachedIndex  = FindActionIndex(Handle.Name);
            Handle.CachedSerial = Serial;
            Handle.CachedName   = Handle.Name;
        }
        return Handle.CachedIndex;
    }

    FName FInputActionMap::FindActionUsingKey(const SKey& Key, FName IgnoreAction) const
    {
        for (const SInputAction& Action : Actions)
        {
            if (Action.Name == IgnoreAction)
            {
                continue;
            }
            for (const SInputActionBinding& Binding : Action.Bindings)
            {
                if (Binding.Source == EInputAxisSource::Key && Binding.Key == Key)
                {
                    return Action.Name;
                }
            }
        }
        return NAME_None;
    }

    const SInputMappingContext* FInputActionMap::FindMappingContext(FName Name) const
    {
        for (const SInputMappingContext& Layer : MappingContexts)
        {
            if (Layer.Name == Name)
            {
                return &Layer;
            }
        }
        return nullptr;
    }

    bool FInputActionMap::PassesGate(const SInputAction& Action, const FInputContext& Context) const
    {
        // A blocking layer that does not list the action swallows it before any layer underneath.
        const TVector<FName>& Stack = Context.GetInputLayers();
        for (int32 i = Stack.Num(); i > 0; --i)
        {
            const SInputMappingContext* Layer = FindMappingContext(Stack[i - 1]);
            if (Layer == nullptr)
            {
                continue;
            }

            for (const FName& Allowed : Layer->Actions)
            {
                if (Allowed == Action.Name)
                {
                    return true;
                }
            }

            if (Layer->bBlockLower)
            {
                return false;
            }
        }

        // The escape hatch for a project with no authored layers, where UI mode blocks what does not opt in.
        return Action.bRunsInUI || Context.GetInputMode() != EInputMode::UI;
    }

    void FInputActionMap::EvaluateRaw(const SInputAction& Action, const FInputContext& Context,
        float& OutX, float& OutY, bool& bOutAnyKeyDown) const
    {
        float Raw[2] = { 0.0f, 0.0f };
        bOutAnyKeyDown = false;

        const float MouseYSign = PlayerSettings.bInvertMouseY ? -1.0f : 1.0f;
        const float LookYSign  = PlayerSettings.bInvertGamepadLookY ? -1.0f : 1.0f;

        for (const SInputActionBinding& Binding : Action.Bindings)
        {
            // Everything else collapses onto X, so a demoted 2D action still reads its X bindings.
            const int32 Channel = (Action.Type == EInputActionType::Axis2D && Binding.Channel == EInputAxisChannel::Y) ? 1 : 0;

            if (Binding.Source == EInputAxisSource::Key)
            {
                if (IsSKeyDown(Binding.Key, Context))
                {
                    Raw[Channel] += Binding.Scale;
                    bOutAnyKeyDown = true;
                }
                continue;
            }

            float Value = 0.0f;
            switch (Binding.Source)
            {
            case EInputAxisSource::MouseX:        Value = float(Context.GetMouseDeltaXRaw()) * PlayerSettings.MouseSensitivity; break;
            case EInputAxisSource::MouseY:        Value = float(Context.GetMouseDeltaYRaw()) * PlayerSettings.MouseSensitivity * MouseYSign; break;
            case EInputAxisSource::MouseWheel:    Value = float(Context.GetMouseZRaw()); break;
            case EInputAxisSource::GamepadRightX: Value = ShapeGamepadAxis(Context.GetGamepadRaw().GetAxis(EGamepadAxis::RightX), Binding) * PlayerSettings.GamepadLookSensitivity; break;
            case EInputAxisSource::GamepadRightY: Value = ShapeGamepadAxis(Context.GetGamepadRaw().GetAxis(EGamepadAxis::RightY), Binding) * PlayerSettings.GamepadLookSensitivity * LookYSign; break;
            default:
                if (IsGamepadSource(Binding.Source))
                {
                    Value = ShapeGamepadAxis(Context.GetGamepadRaw().GetAxis(GetGamepadAxisFor(Binding.Source)), Binding);
                }
                break;
            }
            Raw[Channel] += Value * Binding.Scale;
        }

        const float Sign = Action.bInvert ? -1.0f : 1.0f;
        OutX = Raw[0] * Action.Sensitivity * Sign;
        OutY = Raw[1] * Action.Sensitivity * Sign;

        const float Dead = Math::Clamp(Action.DeadZone, 0.0f, 0.99f);
        if (Action.Type == EInputActionType::Axis2D)
        {
            Input::ApplyRadialDeadZone(OutX, OutY, Dead);
            return;
        }
        OutX = Input::ApplyDeadZone(OutX, Dead);
        OutY = Input::ApplyDeadZone(OutY, Dead);
    }

    bool FInputActionMap::IsRawDown(const SInputAction& Action, const FInputContext& Context) const
    {
        if (!PassesGate(Action, Context))
        {
            return false;
        }
        float X = 0.0f;
        float Y = 0.0f;
        bool bAnyKeyDown = false;
        EvaluateRaw(Action, Context, X, Y, bAnyKeyDown);
        return Action.Type == EInputActionType::Digital ? bAnyKeyDown : (bAnyKeyDown || X != 0.0f || Y != 0.0f);
    }

    void FInputActionMap::UpdateContext(FInputContext& Context, float DeltaSeconds) const
    {
        TVector<SInputActionState>& States = Context.GetMutableActionStates();
        TVector<FInputContext::FActionTriggerState>& Triggers = Context.GetMutableTriggerStates();

        // Carrying a stale HeldTime onto a different action would fire a phantom release next frame.
        if (Context.GetActionsSerial() != Serial || States.Num() != Actions.Num())
        {
            States.Init(SInputActionState(), Actions.Num());
            Triggers.Init(FInputContext::FActionTriggerState(), Actions.Num());
            Context.SetActionsSerial(Serial);
        }

        for (int32 Index = 0; Index < Actions.Num(); ++Index)
        {
            const SInputAction& Action = Actions[Index];
            SInputActionState& State = States[Index];
            FInputContext::FActionTriggerState& Trigger = Triggers[Index];
            const bool bWasDown = State.IsDown();

            float X = 0.0f;
            float Y = 0.0f;
            bool bAnyKeyDown = false;
            if (PassesGate(Action, Context))
            {
                EvaluateRaw(Action, Context, X, Y, bAnyKeyDown);
            }

            // A continuous action is down while its shaped value is non-zero, past the dead zone.
            bool bRawDown = (Action.Type == EInputActionType::Digital) ? bAnyKeyDown : (bAnyKeyDown || X != 0.0f || Y != 0.0f);

            if (!Action.ChordAction.IsNone())
            {
                const SInputAction* Chord = FindAction(Action.ChordAction);
                if (Chord == nullptr || Chord == &Action || !IsRawDown(*Chord, Context))
                {
                    bRawDown = false;
                    X = 0.0f;
                    Y = 0.0f;
                }
            }

            const bool bRawPressed = bRawDown && !Trigger.bRawDown;
            Trigger.bRawDown = bRawDown;

            bool bDown = bRawDown;
            if (ToggleActions[Index] && Action.Type == EInputActionType::Digital)
            {
                Trigger.bToggledOn = bRawPressed ? !Trigger.bToggledOn : Trigger.bToggledOn;
                bDown = Trigger.bToggledOn;
                X = bDown ? 1.0f : 0.0f;
            }

            const bool bPressed = bDown && !bWasDown;
            const float HeldTime = bDown ? (bWasDown ? State.HeldTime + DeltaSeconds : 0.0f) : 0.0f;

            uint32 Flags = 0;
            if (bDown)               { Flags |= SInputActionState::Flag_Down; }
            if (bPressed)            { Flags |= SInputActionState::Flag_Pressed; }
            if (!bDown && bWasDown)  { Flags |= SInputActionState::Flag_Released; }
            if (bDown && HeldTime >= Action.HoldTime) { Flags |= SInputActionState::Flag_Held; }
            // Tap is decided on the release frame from the press duration we are about to discard.
            if (!bDown && bWasDown && State.HeldTime <= Action.TapTime) { Flags |= SInputActionState::Flag_Tapped; }

            // A negative SinceLastPress means no press is waiting for its second, so a triple press counts one double.
            if (Action.DoubleTapTime > 0.0f)
            {
                Trigger.SinceLastPress = Trigger.SinceLastPress >= 0.0f ? Trigger.SinceLastPress + DeltaSeconds : -1.0f;
                if (Trigger.SinceLastPress > Action.DoubleTapTime)
                {
                    Trigger.SinceLastPress = -1.0f;
                }
                if (bPressed)
                {
                    const bool bSecond = Trigger.SinceLastPress >= 0.0f;
                    Flags |= bSecond ? SInputActionState::Flag_DoubleTapped : 0u;
                    Trigger.SinceLastPress = bSecond ? -1.0f : 0.0f;
                }
            }

            if (Action.RepeatInterval > 0.0f)
            {
                if (bPressed)
                {
                    Flags |= SInputActionState::Flag_Repeated;
                    Trigger.RepeatTimer = Action.RepeatDelay > 0.0f ? Action.RepeatDelay : Action.RepeatInterval;
                }
                else if (bDown)
                {
                    Trigger.RepeatTimer -= DeltaSeconds;
                    if (Trigger.RepeatTimer <= 0.0f)
                    {
                        Flags |= SInputActionState::Flag_Repeated;
                        Trigger.RepeatTimer = Math::Max(Trigger.RepeatTimer + Action.RepeatInterval, 0.0f);
                    }
                }
            }

            if (Action.BufferTime > 0.0f)
            {
                Trigger.BufferRemaining = bPressed ? Action.BufferTime : Math::Max(Trigger.BufferRemaining - DeltaSeconds, 0.0f);
                if (Trigger.BufferRemaining > 0.0f)
                {
                    Flags |= SInputActionState::Flag_Buffered;
                }
            }

            State.X = X;
            State.Y = Y;
            State.HeldTime = HeldTime;
            State.Flags = Flags;
        }
    }

    const SInputActionState& FInputActionMap::GetActionState(const SInputActionHandle& Handle, const FInputContext& Context) const
    {
        static const SInputActionState Empty;

        const int32 Index = ResolveIndex(Handle);
        const TVector<SInputActionState>& States = Context.GetActionStates();
        if (Index == Constants::kIndexNone || Context.GetActionsSerial() != Serial || Index >= States.Num())
        {
            return Empty;
        }
        return States[Index];
    }
}
