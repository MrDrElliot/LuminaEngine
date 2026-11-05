#include "RuntimePCH.h"
#include "Input/InputQuery.h"

#include "Config/GameUserSettings.h"

#include "Input/InputActionMap.h"
#include "Input/InputContext.h"
#include "Input/InputViewport.h"

namespace Lumina::Input
{
    const FInputContext* GetReceivingContext(const CWorld* World)
    {
        if (World == nullptr)
        {
            return nullptr;
        }

        const FInputViewportRegistry& Registry = FInputViewportRegistry::Get();
        const FInputViewport* Viewport = Registry.FindViewportForWorld(World);

        // Both conditions are global, so exactly one world is driven and the focus toggle stops all of them.
        if (Viewport == nullptr || !Registry.IsGameInputFocused() || Viewport != Registry.GetActiveViewport())
        {
            return nullptr;
        }
        return &Viewport->GetContext();
    }

    bool IsReceivingInput(const CWorld* World)
    {
        return GetReceivingContext(World) != nullptr;
    }

    const SInputActionState& GetActionState(const CWorld* World, const SInputActionHandle& Action)
    {
        static const SInputActionState Empty;
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr ? FInputActionMap::Get().GetActionState(Action, *Context) : Empty;
    }

    // A world must arm its pause layer while the editor holds focus, or the layer silently fails.
    static FInputContext* FindOwnContext(const CWorld* World)
    {
        FInputViewport* Viewport = FInputViewportRegistry::Get().FindViewportForWorld(World);
        return Viewport != nullptr ? &Viewport->GetContext() : nullptr;
    }

    void PushLayer(const CWorld* World, FName Layer)
    {
        if (FInputContext* Context = FindOwnContext(World))
        {
            Context->PushInputLayer(Layer);
        }
    }

    bool PopLayer(const CWorld* World, FName Layer)
    {
        FInputContext* Context = FindOwnContext(World);
        return Context != nullptr && Context->PopInputLayer(Layer);
    }

    bool HasLayer(const CWorld* World, FName Layer)
    {
        const FInputContext* Context = FindOwnContext(World);
        return Context != nullptr && Context->HasInputLayer(Layer);
    }

    void ClearLayers(const CWorld* World)
    {
        if (FInputContext* Context = FindOwnContext(World))
        {
            Context->ClearInputLayers();
        }
    }

    bool IsKeyDown(const CWorld* World, EKey Key)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsKeyDown(Key);
    }

    bool IsKeyPressed(const CWorld* World, EKey Key)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsKeyPressed(Key);
    }

    bool IsKeyReleased(const CWorld* World, EKey Key)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsKeyReleased(Key);
    }

    bool IsMouseButtonDown(const CWorld* World, EMouseKey Button)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsMouseButtonDown(Button);
    }

    bool IsMouseButtonPressed(const CWorld* World, EMouseKey Button)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsMouseButtonPressed(Button);
    }

    bool IsMouseButtonReleased(const CWorld* World, EMouseKey Button)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsMouseButtonReleased(Button);
    }

    FVector2 GetMousePosition(const CWorld* World)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr ? FVector2((float)Context->GetMouseX(), (float)Context->GetMouseY()) : FVector2(0.0f);
    }

    FVector2 GetMouseDelta(const CWorld* World)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr ? FVector2((float)Context->GetMouseDeltaX(), (float)Context->GetMouseDeltaY()) : FVector2(0.0f);
    }

    float GetMouseWheel(const CWorld* World)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr ? (float)Context->GetMouseZ() : 0.0f;
    }

    bool IsGamepadConnected(const CWorld* World)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsGamepadConnected();
    }

    bool IsGamepadButtonDown(const CWorld* World, EGamepadButton Button)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsGamepadButtonDown(Button);
    }

    bool IsGamepadButtonPressed(const CWorld* World, EGamepadButton Button)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsGamepadButtonPressed(Button);
    }

    bool IsGamepadButtonReleased(const CWorld* World, EGamepadButton Button)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsGamepadButtonReleased(Button);
    }

    float GetGamepadAxis(const CWorld* World, EGamepadAxis Axis)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr ? Context->GetGamepadAxis(Axis) : 0.0f;
    }

    FVector2 GetGamepadLeftStick(const CWorld* World)
    {
        return FVector2(GetGamepadAxis(World, EGamepadAxis::LeftX), GetGamepadAxis(World, EGamepadAxis::LeftY));
    }

    FVector2 GetGamepadRightStick(const CWorld* World)
    {
        return FVector2(GetGamepadAxis(World, EGamepadAxis::RightX), GetGamepadAxis(World, EGamepadAxis::RightY));
    }

    bool ConsumeBufferedPress(const CWorld* World, const SInputActionHandle& Action)
    {
        FInputContext* Context = GetReceivingContext(World) != nullptr ? FindOwnContext(World) : nullptr;
        return Context != nullptr && Context->ConsumeBufferedPress(FInputActionMap::Get().ResolveIndex(Action));
    }

    EInputDevice GetLastUsedDevice(const CWorld* World)
    {
        const FInputContext* Context = FindOwnContext(World);
        return Context != nullptr ? Context->GetLastUsedDevice() : EInputDevice::KeyboardMouse;
    }

    EGamepadFamily GetGamepadFamily()
    {
        // Pads are global, while a viewport that is not active is fed a disconnected one with no family.
        return FInputViewportRegistry::Get().GetGamepads().GetCombined().Family;
    }

    FString GetActionPrompt(const CWorld* World, const SInputActionHandle& Action)
    {
        const FInputActionMap& Map = FInputActionMap::Get();
        const int32 Index = Map.ResolveIndex(Action);
        if (Index == Constants::kIndexNone)
        {
            return FString();
        }

        const bool bGamepad = GetLastUsedDevice(World) == EInputDevice::Gamepad;
        const EGamepadFamily Family = GetGamepadFamily();
        for (const SInputActionBinding& Binding : Map.GetAllActions()[Index].Bindings)
        {
            if (Binding.Source == EInputAxisSource::Key)
            {
                if (Binding.Key.IsValid() && Binding.Key.IsGamepad() == bGamepad)
                {
                    return bGamepad ? GetGamepadButtonDisplayName(Binding.Key.GamepadButton, Family) : Binding.Key.GetDisplayName();
                }
                continue;
            }

            const bool bGamepadSource = Binding.Source >= EInputAxisSource::GamepadLeftX;
            if (bGamepadSource == bGamepad)
            {
                return Binding.GetDisplayName(Family);
            }
        }
        return FString();
    }

    static const SInputActionBinding* FindBinding(FName Action, int32 BindingIndex)
    {
        const SInputAction* Found = FInputActionMap::Get().FindAction(Action);
        return (Found != nullptr && BindingIndex >= 0 && BindingIndex < Found->Bindings.Num()) ? &Found->Bindings[BindingIndex] : nullptr;
    }

    int32 GetBindingCount(FName Action)
    {
        const SInputAction* Found = FInputActionMap::Get().FindAction(Action);
        return Found != nullptr ? Found->Bindings.Num() : 0;
    }

    FString GetBindingDisplayName(FName Action, int32 BindingIndex)
    {
        const SInputActionBinding* Binding = FindBinding(Action, BindingIndex);
        return Binding != nullptr ? Binding->GetDisplayName(GetGamepadFamily()) : FString();
    }

    bool IsBindingRebindable(FName Action, int32 BindingIndex)
    {
        const SInputActionBinding* Binding = FindBinding(Action, BindingIndex);
        return Binding != nullptr && Binding->Source == EInputAxisSource::Key;
    }

    bool BeginRebind(FName Action, int32 BindingIndex)
    {
        return FInputViewportRegistry::Get().BeginRebind(Action, BindingIndex);
    }

    void CancelRebind()
    {
        FInputViewportRegistry::Get().CancelRebind();
    }

    bool IsRebinding()
    {
        return FInputViewportRegistry::Get().IsRebinding();
    }

    FName GetRebindConflict()
    {
        return FInputViewportRegistry::Get().GetRebindConflict();
    }

    void ResetBindings(FName Action)
    {
        CGameUserSettings::Get().ResetBindingOverrides(Action);
    }

    TSpan<const SInputEvent> GetFrameEvents(const CWorld* World)
    {
        const FInputContext* Context = GetReceivingContext(World);
        if (Context == nullptr)
        {
            return TSpan<const SInputEvent>();
        }
        const TVector<SInputEvent>& Events = Context->GetFrameEvents();
        return TSpan<const SInputEvent>(Events.GetData(), Events.Num());
    }

    void SetMouseMode(const CWorld* World, EMouseMode Mode)
    {
        FInputViewportRegistry::Get().SetMouseMode(Mode, World);
    }

    EMouseMode GetMouseMode(const CWorld* World)
    {
        const FInputContext* Context = FindOwnContext(World);
        return Context != nullptr ? Context->GetMouseMode() : EMouseMode::Normal;
    }

    void SetInputMode(const CWorld* World, EInputMode Mode)
    {
        if (FInputContext* Context = FindOwnContext(World))
        {
            Context->SetInputMode(Mode);
        }
    }

    EInputMode GetInputMode(const CWorld* World)
    {
        const FInputContext* Context = FindOwnContext(World);
        return Context != nullptr ? Context->GetInputMode() : EInputMode::Game;
    }

    float GetAxisPair(const CWorld* World, const SInputActionHandle& Positive, const SInputActionHandle& Negative)
    {
        // Resolved once, since GetActionState would re-derive the context per side.
        const FInputContext* Context = GetReceivingContext(World);
        if (Context == nullptr)
        {
            return 0.0f;
        }

        const FInputActionMap& Map = FInputActionMap::Get();
        const float Up   = Map.GetActionState(Positive, *Context).IsDown() ? 1.0f : 0.0f;
        const float Down = Map.GetActionState(Negative, *Context).IsDown() ? 1.0f : 0.0f;
        return Up - Down;
    }
}
