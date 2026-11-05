#include "RuntimePCH.h"

#include "World/Subsystems/InputSubsystem.h"

#include "Input/InputActionMap.h"
#include "Input/InputQuery.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        // Higher keeps more of the game away from the player, so the most guarded request among the leases wins.
        constexpr int32 InputModeRank(EInputMode Mode)
        {
            switch (Mode)
            {
                case EInputMode::UI:        return 2;
                case EInputMode::GameAndUI: return 1;
                default:                    return 0;
            }
        }

        // Higher shows more of the cursor, so one panel needing a pointer is enough to keep it visible.
        constexpr int32 MouseModeRank(EMouseMode Mode)
        {
            switch (Mode)
            {
                case EMouseMode::Normal: return 2;
                case EMouseMode::Hidden: return 1;
                default:                 return 0;
            }
        }
    }

    bool CInputSubsystem::IsReceivingInput()
    {
        return Input::IsReceivingInput(GetWorld());
    }

    SInputActionState CInputSubsystem::GetActionState(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action });
    }

    bool CInputSubsystem::IsActionDown(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action }).IsDown();
    }

    bool CInputSubsystem::WasActionPressed(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action }).IsPressed();
    }

    bool CInputSubsystem::WasActionReleased(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action }).IsReleased();
    }

    bool CInputSubsystem::IsActionHeld(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action }).IsHeld();
    }

    bool CInputSubsystem::WasActionTapped(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action }).IsTapped();
    }

    bool CInputSubsystem::WasActionDoubleTapped(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action }).IsDoubleTapped();
    }

    bool CInputSubsystem::IsActionRepeated(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action }).IsRepeated();
    }

    bool CInputSubsystem::IsActionBuffered(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action }).IsBuffered();
    }

    bool CInputSubsystem::ConsumeBufferedPress(const FName& Action)
    {
        return Input::ConsumeBufferedPress(GetWorld(), SInputActionHandle{ Action });
    }

    float CInputSubsystem::GetActionAxis(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action }).X;
    }

    FVector2 CInputSubsystem::GetActionAxis2D(const FName& Action)
    {
        const SInputActionState& State = Input::GetActionState(GetWorld(), SInputActionHandle{ Action });
        return FVector2(State.X, State.Y);
    }

    float CInputSubsystem::GetActionHeldTime(const FName& Action)
    {
        return Input::GetActionState(GetWorld(), SInputActionHandle{ Action }).HeldTime;
    }

    float CInputSubsystem::GetAxisPair(const FName& Positive, const FName& Negative)
    {
        return Input::GetAxisPair(GetWorld(), SInputActionHandle{ Positive }, SInputActionHandle{ Negative });
    }

    void CInputSubsystem::PushLayer(const FName& Layer)
    {
        Input::PushLayer(GetWorld(), Layer);
    }

    bool CInputSubsystem::PopLayer(const FName& Layer)
    {
        return Input::PopLayer(GetWorld(), Layer);
    }

    bool CInputSubsystem::HasLayer(const FName& Layer)
    {
        return Input::HasLayer(GetWorld(), Layer);
    }

    void CInputSubsystem::ClearLayers()
    {
        Input::ClearLayers(GetWorld());
    }

    bool CInputSubsystem::IsKeyDown(EKey Key)
    {
        return Input::IsKeyDown(GetWorld(), Key);
    }

    bool CInputSubsystem::WasKeyPressed(EKey Key)
    {
        return Input::IsKeyPressed(GetWorld(), Key);
    }

    bool CInputSubsystem::WasKeyReleased(EKey Key)
    {
        return Input::IsKeyReleased(GetWorld(), Key);
    }

    bool CInputSubsystem::IsMouseButtonDown(EMouseKey Button)
    {
        return Input::IsMouseButtonDown(GetWorld(), Button);
    }

    bool CInputSubsystem::WasMouseButtonPressed(EMouseKey Button)
    {
        return Input::IsMouseButtonPressed(GetWorld(), Button);
    }

    bool CInputSubsystem::WasMouseButtonReleased(EMouseKey Button)
    {
        return Input::IsMouseButtonReleased(GetWorld(), Button);
    }

    FVector2 CInputSubsystem::GetMousePosition()
    {
        return Input::GetMousePosition(GetWorld());
    }

    FVector2 CInputSubsystem::GetMouseDelta()
    {
        return Input::GetMouseDelta(GetWorld());
    }

    float CInputSubsystem::GetMouseWheel()
    {
        return Input::GetMouseWheel(GetWorld());
    }

    bool CInputSubsystem::IsGamepadConnected()
    {
        return Input::IsGamepadConnected(GetWorld());
    }

    bool CInputSubsystem::IsGamepadButtonDown(EGamepadButton Button)
    {
        return Input::IsGamepadButtonDown(GetWorld(), Button);
    }

    bool CInputSubsystem::WasGamepadButtonPressed(EGamepadButton Button)
    {
        return Input::IsGamepadButtonPressed(GetWorld(), Button);
    }

    bool CInputSubsystem::WasGamepadButtonReleased(EGamepadButton Button)
    {
        return Input::IsGamepadButtonReleased(GetWorld(), Button);
    }

    float CInputSubsystem::GetGamepadAxis(EGamepadAxis Axis)
    {
        return Input::GetGamepadAxis(GetWorld(), Axis);
    }

    FVector2 CInputSubsystem::GetGamepadLeftStick()
    {
        return Input::GetGamepadLeftStick(GetWorld());
    }

    FVector2 CInputSubsystem::GetGamepadRightStick()
    {
        return Input::GetGamepadRightStick(GetWorld());
    }

    EInputDevice CInputSubsystem::GetLastUsedDevice()
    {
        return Input::GetLastUsedDevice(GetWorld());
    }

    FString CInputSubsystem::GetActionPrompt(const FName& Action)
    {
        return Input::GetActionPrompt(GetWorld(), SInputActionHandle{ Action });
    }

    void CInputSubsystem::SetInputMode(EInputMode Mode)
    {
        Input::SetInputMode(GetWorld(), Mode);
    }

    EInputMode CInputSubsystem::GetInputMode()
    {
        return Input::GetInputMode(GetWorld());
    }

    void CInputSubsystem::SetMouseMode(EMouseMode Mode)
    {
        Input::SetMouseMode(GetWorld(), Mode);
    }

    EMouseMode CInputSubsystem::GetMouseMode()
    {
        return Input::GetMouseMode(GetWorld());
    }

    void CInputSubsystem::OnTeardown()
    {
        ClearInputModes();
    }

    uint64 CInputSubsystem::AcquireInputMode(EInputMode InputMode, EMouseMode MouseMode)
    {
        if (Leases.IsEmpty())
        {
            BaseInputMode = Input::GetInputMode(GetWorld());
            BaseMouseMode = Input::GetMouseMode(GetWorld());
        }
        const uint64 Id = NextId++;
        Leases.Emplace(Id, InputMode, MouseMode);
        ApplyInputModes();
        return Id;
    }

    bool CInputSubsystem::ReleaseInputMode(uint64 Lease)
    {
        for (int32 Index = 0; Index < Leases.Num(); ++Index)
        {
            if (Leases[Index].Id == Lease)
            {
                Leases.RemoveAt(Index);
                ApplyInputModes();
                return true;
            }
        }
        return false;
    }

    void CInputSubsystem::ClearInputModes()
    {
        if (!Leases.IsEmpty())
        {
            Leases.Reset();
            ApplyInputModes();
        }
    }

    void CInputSubsystem::ApplyInputModes()
    {
        if (Leases.IsEmpty())
        {
            Input::SetInputMode(GetWorld(), BaseInputMode);
            Input::SetMouseMode(GetWorld(), BaseMouseMode);
            return;
        }

        EInputMode InputMode = Leases[0].InputMode;
        EMouseMode MouseMode = Leases[0].MouseMode;
        for (const FLease& Lease : Leases)
        {
            if (InputModeRank(Lease.InputMode) > InputModeRank(InputMode))
            {
                InputMode = Lease.InputMode;
            }
            if (MouseModeRank(Lease.MouseMode) > MouseModeRank(MouseMode))
            {
                MouseMode = Lease.MouseMode;
            }
        }
        Input::SetInputMode(GetWorld(), InputMode);
        Input::SetMouseMode(GetWorld(), MouseMode);
    }
}
