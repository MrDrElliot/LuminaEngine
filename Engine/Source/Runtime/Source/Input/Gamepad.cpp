#include "RuntimePCH.h"
#include "Gamepad.h"

#include "Core/Windows/Window.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        // The platform reports the 15 standard buttons in EGamepadButton order, ahead of the virtual analog ones.
        constexpr uint32 kPhysicalButtonCount = (uint32)EGamepadButton::LeftTrigger;

        // Past this an axis counts as the player using the pad, well clear of a resting stick's noise.
        constexpr float kActivityThreshold = 0.25f;

        // Matched against the mapping database's controller names, so unknown pads fall back to Generic.
        EGamepadFamily DetectFamily(const FString& Name)
        {
            FString Lower = Name;
            Lower.ToLower();
            auto Has = [&Lower](const char* Token) { return Lower.Find(Token) != Constants::kIndexNone; };

            if (Has("xbox") || Has("xinput"))
            {
                return EGamepadFamily::Xbox;
            }
            if (Has("ps3") || Has("ps4") || Has("ps5") || Has("dualshock") || Has("dualsense") || Has("playstation") || Has("sony"))
            {
                return EGamepadFamily::PlayStation;
            }
            if (Has("nintendo") || Has("switch") || Has("joy-con") || Has("pro controller"))
            {
                return EGamepadFamily::Switch;
            }
            return EGamepadFamily::Generic;
        }

        void ReadPad(const Windowing::FRawGamepad& Raw, const FGamepadTuning& Tuning,
                     TArray<float, (uint32)EGamepadAxis::Num>& OutAxes, TArray<bool, (uint32)EGamepadButton::Num>& OutDown)
        {
            float LeftX = Raw.Axes[0];
            float LeftY = -Raw.Axes[1];
            float RightX = Raw.Axes[2];
            float RightY = -Raw.Axes[3];
            Input::ApplyRadialDeadZone(LeftX, LeftY, Tuning.StickDeadZone);
            Input::ApplyRadialDeadZone(RightX, RightY, Tuning.StickDeadZone);

            // Triggers rest at -1 on the raw device.
            const float LeftTrigger = Input::ApplyDeadZone((Raw.Axes[4] + 1.0f) * 0.5f, Tuning.TriggerDeadZone);
            const float RightTrigger = Input::ApplyDeadZone((Raw.Axes[5] + 1.0f) * 0.5f, Tuning.TriggerDeadZone);

            OutAxes[(uint32)EGamepadAxis::LeftX] = LeftX;
            OutAxes[(uint32)EGamepadAxis::LeftY] = LeftY;
            OutAxes[(uint32)EGamepadAxis::RightX] = RightX;
            OutAxes[(uint32)EGamepadAxis::RightY] = RightY;
            OutAxes[(uint32)EGamepadAxis::LeftTrigger] = LeftTrigger;
            OutAxes[(uint32)EGamepadAxis::RightTrigger] = RightTrigger;

            for (uint32 i = 0; i < kPhysicalButtonCount; ++i)
            {
                OutDown[i] = Raw.Buttons[i] != 0;
            }

            const float Stick = Tuning.StickPressThreshold;
            OutDown[(uint32)EGamepadButton::LeftTrigger] = LeftTrigger >= Tuning.TriggerPressThreshold;
            OutDown[(uint32)EGamepadButton::RightTrigger] = RightTrigger >= Tuning.TriggerPressThreshold;
            OutDown[(uint32)EGamepadButton::LeftStickUp] = LeftY >= Stick;
            OutDown[(uint32)EGamepadButton::LeftStickDown] = LeftY <= -Stick;
            OutDown[(uint32)EGamepadButton::LeftStickLeft] = LeftX <= -Stick;
            OutDown[(uint32)EGamepadButton::LeftStickRight] = LeftX >= Stick;
            OutDown[(uint32)EGamepadButton::RightStickUp] = RightY >= Stick;
            OutDown[(uint32)EGamepadButton::RightStickDown] = RightY <= -Stick;
            OutDown[(uint32)EGamepadButton::RightStickLeft] = RightX <= -Stick;
            OutDown[(uint32)EGamepadButton::RightStickRight] = RightX >= Stick;
        }
    }

    void FGamepadDevices::Poll(const FGamepadTuning& Tuning)
    {
        TArray<bool, (uint32)EGamepadButton::Num> AnyDown = {};
        TArray<float, (uint32)EGamepadAxis::Num>  CombinedAxes = {};
        Combined.bConnected = false;

        for (int32 Index = 0; Index < kMaxGamepads; ++Index)
        {
            FGamepadState& Pad = Pads[Index];
            Windowing::FRawGamepad Raw;
            const bool bConnected = Windowing::ReadGamepad(Index, Raw);

            if (bConnected != Pad.bConnected)
            {
                Names[Index] = (bConnected && Raw.Name != nullptr) ? FString(Raw.Name) : FString();
                Pad.Family = DetectFamily(Names[Index]);
                if (bConnected)
                {
                    LOG_INFO("[Input] Gamepad {} connected: {}", Index, Names[Index]);
                }
                else
                {
                    LOG_INFO("[Input] Gamepad {} disconnected.", Index);
                }
            }
            Pad.bConnected = bConnected;

            TArray<float, (uint32)EGamepadAxis::Num> Axes = {};
            TArray<bool, (uint32)EGamepadButton::Num> Down = {};
            if (bConnected)
            {
                ReadPad(Raw, Tuning, Axes, Down);
                Combined.bConnected = true;
            }

            // A pad that vanishes mid-press still reports its release, so nothing stays latched.
            for (uint32 Button = 0; Button < (uint32)EGamepadButton::Num; ++Button)
            {
                bool& bSuppressed = SuppressedUntilRelease[Index][Button];
                bSuppressed = bSuppressed && Down[Button];
                Down[Button] = Down[Button] && !bSuppressed;

                Pad.Buttons[Button] = Input::Advance(Pad.Buttons[Button], Down[Button]);
                AnyDown[Button] = AnyDown[Button] || Down[Button];
            }
            bool bActive = false;
            for (uint32 Axis = 0; Axis < (uint32)EGamepadAxis::Num; ++Axis)
            {
                Pad.Axes[Axis] = Axes[Axis];
                bActive = bActive || Math::Abs(Axes[Axis]) > kActivityThreshold;
                if (Math::Abs(Axes[Axis]) > Math::Abs(CombinedAxes[Axis]))
                {
                    CombinedAxes[Axis] = Axes[Axis];
                }
            }
            for (uint32 Button = 0; Button < (uint32)EGamepadButton::Num && !bActive; ++Button)
            {
                bActive = Down[Button];
            }
            if (bActive || (bConnected && (LastActivePad == Constants::kIndexNone || !Pads[LastActivePad].bConnected)))
            {
                LastActivePad = Index;
            }
        }

        for (uint32 Button = 0; Button < (uint32)EGamepadButton::Num; ++Button)
        {
            Combined.Buttons[Button] = Input::Advance(Combined.Buttons[Button], AnyDown[Button]);
        }
        Combined.Axes = CombinedAxes;
        const bool bLastActiveConnected = LastActivePad != Constants::kIndexNone && Pads[LastActivePad].bConnected;
        Combined.Family = bLastActiveConnected ? Pads[LastActivePad].Family : EGamepadFamily::Generic;
    }

    void FGamepadDevices::Reset()
    {
        for (int32 Index = 0; Index < kMaxGamepads; ++Index)
        {
            FGamepadState& Pad = Pads[Index];
            for (uint32 Button = 0; Button < (uint32)EGamepadButton::Num; ++Button)
            {
                SuppressedUntilRelease[Index][Button] = Pad.IsButtonDown((EGamepadButton)Button);
            }

            const FGamepadState Previous = Pad;
            Pad = FGamepadState();
            Pad.bConnected = Previous.bConnected;
            Pad.Family = Previous.Family;
        }
        const FGamepadState PreviousCombined = Combined;
        Combined = FGamepadState();
        Combined.bConnected = PreviousCombined.bConnected;
        Combined.Family = PreviousCombined.Family;
    }

    EGamepadButton FGamepadDevices::GetFirstPressedButton() const
    {
        for (uint32 Button = 0; Button < (uint32)EGamepadButton::Num; ++Button)
        {
            if (Combined.Buttons[Button] == Input::EButtonState::Pressed)
            {
                return (EGamepadButton)Button;
            }
        }
        return EGamepadButton::Num;
    }

    FString GetGamepadButtonDisplayName(EGamepadButton Button, EGamepadFamily Family)
    {
        if (Family == EGamepadFamily::PlayStation)
        {
            switch (Button)
            {
            case EGamepadButton::FaceBottom:    return "Cross";
            case EGamepadButton::FaceRight:     return "Circle";
            case EGamepadButton::FaceLeft:      return "Square";
            case EGamepadButton::FaceTop:       return "Triangle";
            case EGamepadButton::LeftShoulder:  return "L1";
            case EGamepadButton::RightShoulder: return "R1";
            case EGamepadButton::LeftTrigger:   return "L2";
            case EGamepadButton::RightTrigger:  return "R2";
            case EGamepadButton::LeftThumb:     return "L3";
            case EGamepadButton::RightThumb:    return "R3";
            case EGamepadButton::Back:          return "Create";
            case EGamepadButton::Start:         return "Options";
            case EGamepadButton::Guide:         return "PS";
            default:                            break;
            }
        }
        else if (Family == EGamepadFamily::Switch)
        {
            // Nintendo prints the face letters mirrored from Xbox, so the bottom button is B.
            switch (Button)
            {
            case EGamepadButton::FaceBottom:    return "B";
            case EGamepadButton::FaceRight:     return "A";
            case EGamepadButton::FaceLeft:      return "Y";
            case EGamepadButton::FaceTop:       return "X";
            case EGamepadButton::LeftShoulder:  return "L";
            case EGamepadButton::RightShoulder: return "R";
            case EGamepadButton::LeftTrigger:   return "ZL";
            case EGamepadButton::RightTrigger:  return "ZR";
            case EGamepadButton::Back:          return "Minus";
            case EGamepadButton::Start:         return "Plus";
            case EGamepadButton::Guide:         return "Home";
            default:                            break;
            }
        }

        switch (Button)
        {
        case EGamepadButton::FaceBottom:      return "A";
        case EGamepadButton::FaceRight:       return "B";
        case EGamepadButton::FaceLeft:        return "X";
        case EGamepadButton::FaceTop:         return "Y";
        case EGamepadButton::LeftShoulder:    return "Left Bumper";
        case EGamepadButton::RightShoulder:   return "Right Bumper";
        case EGamepadButton::Back:            return "View";
        case EGamepadButton::Start:           return "Menu";
        case EGamepadButton::Guide:           return "Guide";
        case EGamepadButton::LeftThumb:       return "Left Stick Press";
        case EGamepadButton::RightThumb:      return "Right Stick Press";
        case EGamepadButton::DPadUp:          return "D-Pad Up";
        case EGamepadButton::DPadRight:       return "D-Pad Right";
        case EGamepadButton::DPadDown:        return "D-Pad Down";
        case EGamepadButton::DPadLeft:        return "D-Pad Left";
        case EGamepadButton::LeftTrigger:     return "Left Trigger";
        case EGamepadButton::RightTrigger:    return "Right Trigger";
        case EGamepadButton::LeftStickUp:     return "Left Stick Up";
        case EGamepadButton::LeftStickDown:   return "Left Stick Down";
        case EGamepadButton::LeftStickLeft:   return "Left Stick Left";
        case EGamepadButton::LeftStickRight:  return "Left Stick Right";
        case EGamepadButton::RightStickUp:    return "Right Stick Up";
        case EGamepadButton::RightStickDown:  return "Right Stick Down";
        case EGamepadButton::RightStickLeft:  return "Right Stick Left";
        case EGamepadButton::RightStickRight: return "Right Stick Right";
        case EGamepadButton::Num:             break;
        }
        return FString();
    }

    FString GetGamepadAxisDisplayName(EGamepadAxis Axis, EGamepadFamily Family)
    {
        switch (Axis)
        {
        case EGamepadAxis::LeftX:        return "Left Stick X";
        case EGamepadAxis::LeftY:        return "Left Stick Y";
        case EGamepadAxis::RightX:       return "Right Stick X";
        case EGamepadAxis::RightY:       return "Right Stick Y";
        case EGamepadAxis::LeftTrigger:  return GetGamepadButtonDisplayName(EGamepadButton::LeftTrigger, Family);
        case EGamepadAxis::RightTrigger: return GetGamepadButtonDisplayName(EGamepadButton::RightTrigger, Family);
        case EGamepadAxis::Num:          break;
        }
        return FString();
    }
}
