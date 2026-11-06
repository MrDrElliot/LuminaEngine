#include "RuntimePCH.h"
#include "InputContext.h"

#include "Events/Event.h"

namespace Lumina
{
    // Movement under this many pixels in one event is hand tremor or sensor noise, not the player switching devices.
    static constexpr double kDeviceSwitchMousePixels = 3.0;

    // A stick or trigger past this is the player picking up the pad, well clear of a resting stick.
    static constexpr float kDeviceSwitchAxis = 0.3f;

    void FInputContext::SetWindowRect(int MinX, int MinY, int MaxX, int MaxY)
    {
        RectMinX = MinX;
        RectMinY = MinY;
        RectMaxX = MaxX;
        RectMaxY = MaxY;
    }

    bool FInputContext::ContainsWindowPoint(int WindowX, int WindowY) const
    {
        return WindowX >= RectMinX && WindowX < RectMaxX
            && WindowY >= RectMinY && WindowY < RectMaxY;
    }

    void FInputContext::SetRenderTargetSize(uint32 W, uint32 H)
    {
        RTWidth = W;
        RTHeight = H;
    }

    void FInputContext::ContextToWindow(double ContextX, double ContextY, double& OutWindowX, double& OutWindowY) const
    {
        const double PanelW = double(RectMaxX - RectMinX);
        const double PanelH = double(RectMaxY - RectMinY);
        if (PanelW <= 0.0 || PanelH <= 0.0 || RTWidth == 0 || RTHeight == 0)
        {
            OutWindowX = ContextX + double(RectMinX);
            OutWindowY = ContextY + double(RectMinY);
            return;
        }

        OutWindowX = double(RectMinX) + ContextX / double(RTWidth) * PanelW;
        OutWindowY = double(RectMinY) + ContextY / double(RTHeight) * PanelH;
    }

    bool FInputContext::WindowToContext(double WindowX, double WindowY, double& OutX, double& OutY) const
    {
        const double PanelW = double(RectMaxX - RectMinX);
        const double PanelH = double(RectMaxY - RectMinY);
        if (PanelW <= 0.0 || PanelH <= 0.0)
        {
            OutX = WindowX;
            OutY = WindowY;
            return false;
        }

        const double U = (WindowX - double(RectMinX)) / PanelW;
        const double V = (WindowY - double(RectMinY)) / PanelH;

        if (RTWidth > 0 && RTHeight > 0)
        {
            OutX = U * double(RTWidth);
            OutY = V * double(RTHeight);
        }
        else
        {
            OutX = WindowX - double(RectMinX);
            OutY = WindowY - double(RectMinY);
        }

        return U >= 0.0 && U <= 1.0 && V >= 0.0 && V <= 1.0;
    }

    void FInputContext::AddHeldModifiers(SInputEvent& Event) const
    {
        if (IsCtrlDownRaw())  { Event.AddFlag(EInputEventFlags::Ctrl); }
        if (IsShiftDownRaw()) { Event.AddFlag(EInputEventFlags::Shift); }
        if (IsAltDownRaw())   { Event.AddFlag(EInputEventFlags::Alt); }
    }

    bool FInputContext::OnEvent(FEvent& Event)
    {
        if (Event.IsA<FMouseMovedEvent>())
        {
            FMouseMovedEvent& MouseEvent = Event.As<FMouseMovedEvent>();
            // Deltas stay in window pixels; position is translated to RT space.
            MouseDeltaX += MouseEvent.GetDeltaX();
            MouseDeltaY += MouseEvent.GetDeltaY();
            if (Math::Abs(MouseEvent.GetDeltaX()) + Math::Abs(MouseEvent.GetDeltaY()) >= kDeviceSwitchMousePixels)
            {
                LastUsedDevice = EInputDevice::KeyboardMouse;
            }
            WindowToContext(MouseEvent.GetX(), MouseEvent.GetY(), MouseX, MouseY);

            SInputEvent Ev;
            Ev.Type   = EInputEventType::MouseMove;
            Ev.MouseX = MouseX;
            Ev.MouseY = MouseY;
            Ev.DeltaX = MouseEvent.GetDeltaX();
            Ev.DeltaY = MouseEvent.GetDeltaY();
            FrameEvents.Add(Ev);
        }
        else if (Event.IsA<FMouseButtonPressedEvent>() || Event.IsA<FMouseButtonReleasedEvent>())
        {
            const bool bPressed = Event.IsA<FMouseButtonPressedEvent>();
            const EMouseKey Button = static_cast<FMouseButtonEvent&>(Event).GetButton();
            MouseStates[(uint32)Button] = bPressed ? Input::EButtonState::Pressed : Input::EButtonState::Released;
            LastUsedDevice = EInputDevice::KeyboardMouse;

            SInputEvent Ev;
            Ev.Type = bPressed ? EInputEventType::MouseDown : EInputEventType::MouseUp;
            Ev.SetKey(SKey(Button));
            AddHeldModifiers(Ev);
            Ev.MouseX = MouseX;
            Ev.MouseY = MouseY;
            FrameEvents.Add(Ev);
        }
        else if (Event.IsA<FKeyPressedEvent>() || Event.IsA<FKeyReleasedEvent>())
        {
            const bool bPressed = Event.IsA<FKeyPressedEvent>();
            FKeyEvent& KeyEvent = static_cast<FKeyEvent&>(Event);
            const bool bRepeat = bPressed && Event.As<FKeyPressedEvent>().IsRepeat();
            Input::EButtonState& State = KeyStates[(uint32)KeyEvent.GetKeyCode()];
            LastUsedDevice = EInputDevice::KeyboardMouse;

            // A repeat after a focus reset still counts as held, but never as a fresh press.
            if (!bPressed)
            {
                State = Input::EButtonState::Released;
            }
            else if (!bRepeat)
            {
                State = Input::EButtonState::Pressed;
            }
            else if (!Input::IsDown(State))
            {
                State = Input::EButtonState::Held;
            }

            SInputEvent Ev;
            Ev.Type = bPressed ? EInputEventType::KeyDown : EInputEventType::KeyUp;
            Ev.SetKey(SKey(KeyEvent.GetKeyCode()));
            if (bRepeat)                { Ev.AddFlag(EInputEventFlags::Repeat); }
            if (KeyEvent.IsCtrlDown())  { Ev.AddFlag(EInputEventFlags::Ctrl); }
            if (KeyEvent.IsShiftDown()) { Ev.AddFlag(EInputEventFlags::Shift); }
            if (KeyEvent.IsAltDown())   { Ev.AddFlag(EInputEventFlags::Alt); }
            FrameEvents.Add(Ev);
        }
        else if (Event.IsA<FMouseScrolledEvent>())
        {
            MouseZ = Event.As<FMouseScrolledEvent>().GetOffset();

            SInputEvent Ev;
            Ev.Type   = EInputEventType::MouseScroll;
            Ev.Scroll = MouseZ;
            Ev.MouseX = MouseX;
            Ev.MouseY = MouseY;
            FrameEvents.Add(Ev);
        }

        return false;
    }

    void FInputContext::SetGamepadState(const FGamepadState& State)
    {
        Gamepad = State;
        for (float Axis : Gamepad.Axes)
        {
            if (Math::Abs(Axis) >= kDeviceSwitchAxis)
            {
                LastUsedDevice = EInputDevice::Gamepad;
            }
        }

        for (uint32 Button = 0; Button < (uint32)EGamepadButton::Num; ++Button)
        {
            const Input::EButtonState ButtonState = Gamepad.Buttons[Button];
            if (ButtonState != Input::EButtonState::Pressed && ButtonState != Input::EButtonState::Released)
            {
                continue;
            }
            LastUsedDevice = EInputDevice::Gamepad;

            SInputEvent Ev;
            Ev.Type = (ButtonState == Input::EButtonState::Pressed) ? EInputEventType::GamepadDown : EInputEventType::GamepadUp;
            Ev.SetKey(SKey((EGamepadButton)Button));
            FrameEvents.Add(Ev);
        }
    }

    void FInputContext::PushInputLayer(FName Name)
    {
        if (Name.IsNone())
        {
            return;
        }
        PopInputLayer(Name);
        InputLayers.Add(Name);
    }

    bool FInputContext::PopInputLayer(FName Name)
    {
        for (int32 i = InputLayers.Num(); i > 0; --i)
        {
            if (InputLayers[i - 1] == Name)
            {
                InputLayers.RemoveAt((i - 1));
                return true;
            }
        }
        return false;
    }

    bool FInputContext::HasInputLayer(FName Name) const
    {
        return Algo::Contains(InputLayers, Name);
    }

    void FInputContext::ClearInputLayers()
    {
        InputLayers.Reset();
    }

    EKey FInputContext::GetFirstPressedKey() const
    {
        for (uint32 Key = 0; Key < (uint32)EKey::Num; ++Key)
        {
            if (KeyStates[Key] == Input::EButtonState::Pressed)
            {
                return (EKey)Key;
            }
        }
        return EKey::Num;
    }

    EMouseKey FInputContext::GetFirstPressedMouseButton() const
    {
        for (uint32 Button = (uint32)EMouseKey::Button0; Button <= (uint32)EMouseKey::ButtonLast; ++Button)
        {
            if (MouseStates[Button] == Input::EButtonState::Pressed)
            {
                return (EMouseKey)Button;
            }
        }
        return EMouseKey::Num;
    }

    bool FInputContext::ConsumeBufferedPress(int32 ActionIndex)
    {
        if (ActionIndex < 0 || ActionIndex >= ActionStates.Num() || ActionIndex >= TriggerStates.Num())
        {
            return false;
        }
        SInputActionState& State = ActionStates[ActionIndex];
        const bool bHadPress = State.IsBuffered();
        State.Flags &= ~SInputActionState::Flag_Buffered;
        TriggerStates[ActionIndex].BufferRemaining = 0.0f;
        return bHadPress;
    }

    void FInputContext::EndFrame()
    {
        for (Input::EButtonState& State : KeyStates)
        {
            State = Input::Settle(State);
        }
        for (Input::EButtonState& State : MouseStates)
        {
            State = Input::Settle(State);
        }

        MouseDeltaX = 0.0;
        MouseDeltaY = 0.0;
        MouseZ      = 0.0;

        FrameEvents.Reset();
    }

    void FInputContext::ResetState()
    {
        KeyStates = {};
        MouseStates = {};
        Gamepad = FGamepadState();
        MouseDeltaX = 0.0;
        MouseDeltaY = 0.0;
        MouseZ      = 0.0;
        // Without this a focus regain fires a spurious Released callback.
        for (SInputActionState& State : ActionStates)
        {
            State = SInputActionState();
        }
        for (FActionTriggerState& Trigger : TriggerStates)
        {
            Trigger = FActionTriggerState();
        }
        FrameEvents.Reset();
    }
}
