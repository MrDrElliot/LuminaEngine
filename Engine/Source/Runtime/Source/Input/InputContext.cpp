#include "RuntimePCH.h"
#include "InputContext.h"

#include "Events/Event.h"
#include "Input/InputActionMap.h"

namespace Lumina
{
    void FInputContext::SetMouseMode(EMouseMode Mode)
    {
        MouseMode = Mode;
    }

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

    bool FInputContext::OnEvent(FEvent& Event)
    {
        // Build an SKey for a mouse button, taking the modifier chord from the cached modifier state
        // (mouse events carry no modifier flags of their own).
        auto MouseKeyWithMods = [this](EMouseKey Button) -> SKey
        {
            SKey K;
            K.SetMouseButton(Button);
            K.bCtrl  = (CachedModifierState & 1) != 0;
            K.bShift = (CachedModifierState & 2) != 0;
            K.bAlt   = (CachedModifierState & 4) != 0;
            return K;
        };

        if (Event.IsA<FMouseMovedEvent>())
        {
            FMouseMovedEvent& MouseEvent = Event.As<FMouseMovedEvent>();
            // Deltas stay in window pixels; position is translated to RT space.
            MouseDeltaX += MouseEvent.GetDeltaX();
            MouseDeltaY += MouseEvent.GetDeltaY();

            double Cx = 0.0;
            double Cy = 0.0;
            WindowToContext(MouseEvent.GetX(), MouseEvent.GetY(), Cx, Cy);
            MouseX = Cx;
            MouseY = Cy;

            SInputEvent Ev;
            Ev.Type   = EInputEventType::MouseMove;
            Ev.MouseX = MouseX;
            Ev.MouseY = MouseY;
            Ev.DeltaX = MouseEvent.GetDeltaX();
            Ev.DeltaY = MouseEvent.GetDeltaY();
            FrameEvents.push_back(Ev);
        }
        else if (Event.IsA<FMouseButtonPressedEvent>())
        {
            FMouseButtonEvent& MouseButtonEvent = Event.As<FMouseButtonPressedEvent>();
            const uint32 MouseCode = (uint32)MouseButtonEvent.GetButton();
            MouseStates[MouseCode]        = Input::EMouseState::Pressed;
            MouseKeyDownTimes[MouseCode]  = 0.0f;

            SInputEvent Ev;
            Ev.Type   = EInputEventType::MouseDown;
            Ev.Key    = MouseKeyWithMods(MouseButtonEvent.GetButton());
            Ev.MouseX = MouseX;
            Ev.MouseY = MouseY;
            FrameEvents.push_back(Ev);
        }
        else if (Event.IsA<FMouseButtonReleasedEvent>())
        {
            FMouseButtonEvent& MouseButtonEvent = Event.As<FMouseButtonReleasedEvent>();
            const uint32 MouseCode = (uint32)MouseButtonEvent.GetButton();
            MouseStates[MouseCode]        = Input::EMouseState::Released;
            MouseKeyDownTimes[MouseCode]  = -1.0f;

            SInputEvent Ev;
            Ev.Type   = EInputEventType::MouseUp;
            Ev.Key    = MouseKeyWithMods(MouseButtonEvent.GetButton());
            Ev.MouseX = MouseX;
            Ev.MouseY = MouseY;
            FrameEvents.push_back(Ev);
        }
        else if (Event.IsA<FKeyPressedEvent>())
        {
            FKeyPressedEvent& KeyEvent = Event.As<FKeyPressedEvent>();
            const uint32 KeyCode = (uint32)KeyEvent.GetKeyCode();
            KeyStates[KeyCode] = KeyEvent.IsRepeat()
                ? Input::EKeyState::Repeated
                : Input::EKeyState::Pressed;

            SInputEvent Ev;
            Ev.Type    = EInputEventType::KeyDown;
            Ev.bRepeat = KeyEvent.IsRepeat();
            Ev.Key.SetKey(KeyEvent.GetKeyCode());
            Ev.Key.bCtrl  = KeyEvent.IsCtrlDown();
            Ev.Key.bShift = KeyEvent.IsShiftDown();
            Ev.Key.bAlt   = KeyEvent.IsAltDown();
            FrameEvents.push_back(Ev);
        }
        else if (Event.IsA<FKeyReleasedEvent>())
        {
            FKeyReleasedEvent& KeyEvent = Event.As<FKeyReleasedEvent>();
            const uint32 KeyCode = (uint32)KeyEvent.GetKeyCode();
            KeyStates[KeyCode] = Input::EKeyState::Released;

            SInputEvent Ev;
            Ev.Type = EInputEventType::KeyUp;
            Ev.Key.SetKey(KeyEvent.GetKeyCode());
            Ev.Key.bCtrl  = KeyEvent.IsCtrlDown();
            Ev.Key.bShift = KeyEvent.IsShiftDown();
            Ev.Key.bAlt   = KeyEvent.IsAltDown();
            FrameEvents.push_back(Ev);
        }
        else if (Event.IsA<FMouseScrolledEvent>())
        {
            FMouseScrolledEvent& MouseEvent = Event.As<FMouseScrolledEvent>();
            MouseZ = MouseEvent.GetOffset();
            const uint32 KeyCode = (uint32)MouseEvent.GetCode();
            MouseStates[KeyCode] = MouseZ > 0.0
                ? Input::EMouseState::Up
                : Input::EMouseState::Held;

            SInputEvent Ev;
            Ev.Type   = EInputEventType::MouseScroll;
            Ev.Scroll = MouseEvent.GetOffset();
            Ev.MouseX = MouseX;
            Ev.MouseY = MouseY;
            FrameEvents.push_back(Ev);
        }

        return false;
    }

    void FInputContext::EndFrame(double DeltaSeconds)
    {
        for (auto& State : MouseStates)
        {
            if (State == Input::EMouseState::Pressed)  State = Input::EMouseState::Held;
            if (State == Input::EMouseState::Released) State = Input::EMouseState::Up;
        }

        for (auto& State : KeyStates)
        {
            if (State == Input::EKeyState::Pressed)  State = Input::EKeyState::Held;
            if (State == Input::EKeyState::Released) State = Input::EKeyState::Up;
        }

        for (uint32 i = 0; i < (uint32)EMouseKey::Num; ++i)
        {
            if (MouseKeyDownTimes[i] >= 0.0f)
            {
                MouseKeyDownTimes[i] += (float)DeltaSeconds;
            }
        }

        MouseDeltaX = 0.0;
        MouseDeltaY = 0.0;
        MouseZ      = 0.0;

        FrameEvents.clear();
    }

    void FInputContext::ResetState()
    {
        for (auto& S : KeyStates)   S = Input::EKeyState::Up;
        for (auto& S : MouseStates) S = Input::EMouseState::Up;
        for (auto& T : MouseKeyDownTimes) T = -1.0f;
        MouseDeltaX = 0.0;
        MouseDeltaY = 0.0;
        MouseZ      = 0.0;
        CachedModifierState = 0;
        // Without this a focus regain fires a spurious Released callback.
        ActionDownLastFrame.clear();
        FrameEvents.clear();
    }

    bool FInputContext::WasActionDownLastFrame(FName ActionName) const
    {
        const auto It = ActionDownLastFrame.find(ActionName);
        return It != ActionDownLastFrame.end() ? It->second : false;
    }

    void FInputContext::SetActionDownLastFrame(FName ActionName, bool bDown)
    {
        ActionDownLastFrame[ActionName] = bDown;
    }

    void FInputContext::UpdateActionEdgeState()
    {
        const FInputActionMap& Map = FInputActionMap::Get();
        for (const SInputAction& Action : Map.GetAllActions())
        {
            const bool bDownNow = Map.IsActionDown(Action.Name, *this);
            ActionDownLastFrame[Action.Name] = bDownNow;
        }
    }
}
