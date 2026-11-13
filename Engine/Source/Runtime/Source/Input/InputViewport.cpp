#include "RuntimePCH.h"
#include "InputViewport.h"

#include "Config/GameUserSettings.h"
#include "Config/InputSettings.h"
#include "Config/UISettings.h"
#include "Core/Engine/Engine.h"
#include "Paths/Paths.h"
#include "Containers/StringFormat.h"
#include "Core/Diagnostics/BenchmarkRun.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Windows/Window.h"
#include "Events/Event.h"
#include "Input/InputActionMap.h"
#include "Input/InputContext.h"
#include "UI/RmlUiBridge.h"
#include "World/Entity/Systems/SystemSingletons.h"
#include "World/World.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Input.h>

namespace Lumina
{
    namespace
    {
        void ApplyCursorModeToWindow(void* NativeWindow, EMouseMode Mode)
        {
            ECursorMode CursorMode = ECursorMode::Normal;
            switch (Mode)
            {
            case EMouseMode::Hidden:   CursorMode = ECursorMode::Hidden;   break;
            case EMouseMode::Normal:   CursorMode = ECursorMode::Normal;   break;
            case EMouseMode::Captured: CursorMode = ECursorMode::Disabled; break;
            }
            Windowing::SetCursorModeForNativeWindow(NativeWindow, CursorMode, FInputActionMap::Get().GetPlayerSettings().bRawMouseInput);
        }

        ESystemCursor ToSystemCursor(EMouseCursor Cursor)
        {
            switch (Cursor)
            {
            case EMouseCursor::None:            return ESystemCursor::Hidden;
            case EMouseCursor::Default:         return ESystemCursor::Arrow;
            case EMouseCursor::TextEditBeam:    return ESystemCursor::TextBeam;
            case EMouseCursor::ResizeLeftRight: return ESystemCursor::ResizeEastWest;
            case EMouseCursor::ResizeUpDown:    return ESystemCursor::ResizeNorthSouth;
            case EMouseCursor::ResizeSouthEast: return ESystemCursor::ResizeNorthWestSouthEast;
            case EMouseCursor::ResizeSouthWest: return ESystemCursor::ResizeNorthEastSouthWest;
            case EMouseCursor::CardinalCross:   return ESystemCursor::ResizeAll;
            case EMouseCursor::Crosshairs:      return ESystemCursor::Crosshair;
            case EMouseCursor::Hand:            return ESystemCursor::PointingHand;
            case EMouseCursor::GrabHand:        return ESystemCursor::PointingHand;
            case EMouseCursor::GrabHandClosed:  return ESystemCursor::PointingHand;
            case EMouseCursor::SlashedCircle:   return ESystemCursor::NotAllowed;
            case EMouseCursor::EyeDropper:      return ESystemCursor::Crosshair;
            }
            return ESystemCursor::Arrow;
        }

        Rml::Input::KeyIdentifier ToRmlKey(EKey Key)
        {
            using K = Rml::Input::KeyIdentifier;

            // Digits, letters, function keys and the numpad run contiguously in both enums.
            auto InRange = [Key](EKey First, EKey Last) { return Key >= First && Key <= Last; };
            auto Offset  = [Key](EKey First, K RmlFirst) { return K(int(RmlFirst) + (int(Key) - int(First))); };
            if (InRange(EKey::D0, EKey::D9))   { return Offset(EKey::D0, K::KI_0); }
            if (InRange(EKey::A, EKey::Z))     { return Offset(EKey::A, K::KI_A); }
            if (InRange(EKey::F1, EKey::F24))  { return Offset(EKey::F1, K::KI_F1); }
            if (InRange(EKey::KP0, EKey::KP9)) { return Offset(EKey::KP0, K::KI_NUMPAD0); }

            switch (Key)
            {
            case EKey::Space:        return K::KI_SPACE;
            case EKey::Enter:        return K::KI_RETURN;
            case EKey::Tab:          return K::KI_TAB;
            case EKey::Backspace:    return K::KI_BACK;
            case EKey::Escape:       return K::KI_ESCAPE;
            case EKey::Delete:       return K::KI_DELETE;
            case EKey::Insert:       return K::KI_INSERT;
            case EKey::Home:         return K::KI_HOME;
            case EKey::End:          return K::KI_END;
            case EKey::PageUp:       return K::KI_PRIOR;
            case EKey::PageDown:     return K::KI_NEXT;
            case EKey::Up:           return K::KI_UP;
            case EKey::Down:         return K::KI_DOWN;
            case EKey::Left:         return K::KI_LEFT;
            case EKey::Right:        return K::KI_RIGHT;
            case EKey::LeftShift:    return K::KI_LSHIFT;
            case EKey::RightShift:   return K::KI_RSHIFT;
            case EKey::LeftControl:  return K::KI_LCONTROL;
            case EKey::RightControl: return K::KI_RCONTROL;
            case EKey::LeftAlt:      return K::KI_LMENU;
            case EKey::RightAlt:     return K::KI_RMENU;
            case EKey::Comma:        return K::KI_OEM_COMMA;
            case EKey::Period:       return K::KI_OEM_PERIOD;
            case EKey::Minus:        return K::KI_OEM_MINUS;
            case EKey::Equal:        return K::KI_OEM_PLUS;
            case EKey::Slash:        return K::KI_OEM_2;
            case EKey::Backslash:    return K::KI_OEM_5;
            case EKey::Apostrophe:   return K::KI_OEM_7;
            case EKey::Semicolon:    return K::KI_OEM_1;
            case EKey::LeftBracket:  return K::KI_OEM_4;
            case EKey::RightBracket: return K::KI_OEM_6;
            case EKey::GraveAccent:  return K::KI_OEM_3;
            case EKey::World1:       return K::KI_OEM_102;
            case EKey::KPEnter:      return K::KI_NUMPADENTER;
            case EKey::KPDecimal:    return K::KI_DECIMAL;
            case EKey::KPAdd:        return K::KI_ADD;
            case EKey::KPSubtract:   return K::KI_SUBTRACT;
            case EKey::KPMultiply:   return K::KI_MULTIPLY;
            case EKey::KPDivide:     return K::KI_DIVIDE;
            case EKey::KPEqual:      return K::KI_OEM_NEC_EQUAL;
            case EKey::LeftSuper:    return K::KI_LWIN;
            case EKey::RightSuper:   return K::KI_RWIN;
            case EKey::Menu:         return K::KI_APPS;
            case EKey::CapsLock:     return K::KI_CAPITAL;
            case EKey::NumLock:      return K::KI_NUMLOCK;
            case EKey::ScrollLock:   return K::KI_SCROLL;
            case EKey::PrintScreen:  return K::KI_SNAPSHOT;
            case EKey::Pause:        return K::KI_PAUSE;
            default:                 return K::KI_UNKNOWN;
            }
        }

        int ToRmlMouseButton(EMouseKey Button)
        {
            switch (Button)
            {
            case EMouseKey::ButtonLeft:   return 0;
            case EMouseKey::ButtonRight:  return 1;
            case EMouseKey::ButtonMiddle: return 2;
            default:                      return int(Button);
            }
        }

        int ModifiersFromContext(const FInputContext& Context)
        {
            using namespace Rml::Input;
            int Mods = 0;
            if (Context.IsCtrlDownRaw())  Mods |= KM_CTRL;
            if (Context.IsShiftDownRaw()) Mods |= KM_SHIFT;
            if (Context.IsAltDownRaw())   Mods |= KM_ALT;
            if (Context.IsKeyDownRaw(EKey::LeftSuper) || Context.IsKeyDownRaw(EKey::RightSuper)) Mods |= KM_META;
            return Mods;
        }

        int ModifiersFromKeyEvent(const FKeyEvent& KeyEvent)
        {
            using namespace Rml::Input;
            int Mods = 0;
            if (KeyEvent.IsCtrlDown())  Mods |= KM_CTRL;
            if (KeyEvent.IsShiftDown()) Mods |= KM_SHIFT;
            if (KeyEvent.IsAltDown())   Mods |= KM_ALT;
            if (KeyEvent.IsSuperDown()) Mods |= KM_META;
            // GLFW lock mods never reach us; with num lock off no char event follows, so nothing is typed.
            if (KeyEvent.GetKeyCode() >= EKey::KP0 && KeyEvent.GetKeyCode() <= EKey::KPDecimal) Mods |= KM_NUMLOCK;
            return Mods;
        }
    }

    FInputViewport::FInputViewport()
        : Context(MakeUnique<FInputContext>())
    {
    }

    FInputViewport::~FInputViewport()
    {
        FInputViewportRegistry::Get().Unregister(this);
    }

    void FInputViewport::SetWindowRect(int MinX, int MinY, int MaxX, int MaxY)
    {
        Context->SetWindowRect(MinX, MinY, MaxX, MaxY);
    }

    void FInputViewport::SetRenderTargetSize(uint32 W, uint32 H)
    {
        Context->SetRenderTargetSize(W, H);
    }

    bool FInputViewport::ContainsWindowPoint(int WindowX, int WindowY) const
    {
        return Context->ContainsWindowPoint(WindowX, WindowY);
    }

    bool FInputViewport::RouteEvent(FEvent& Event)
    {
        // State updates run unconditionally, or a fallback-routed release leaves keys down forever.
        Context->OnEvent(Event);

        const EInputMode Mode = Context->GetInputMode();
        if (Mode != EInputMode::UI && Mode != EInputMode::GameAndUI)
        {
            return false;
        }

        if (Event.IsA<FKeyPressedEvent>() || Event.IsA<FKeyReleasedEvent>() || Event.IsA<FCharInputEvent>())
        {
            return ForwardKeyEventToRmlUi(Event);
        }

        // A move or press outside the panel is not the UI's, while a release always goes through so a drag can finish.
        if (Event.IsA<FMouseMovedEvent>())
        {
            const FMouseMovedEvent& Move = Event.As<FMouseMovedEvent>();
            return ContainsWindowPoint(int(Move.GetX()), int(Move.GetY())) && ForwardMouseEventToRmlUi(Event);
        }
        if (Event.IsA<FMouseButtonPressedEvent>())
        {
            const FMouseButtonEvent& Press = Event.As<FMouseButtonPressedEvent>();
            return ContainsWindowPoint(int(Press.GetX()), int(Press.GetY())) && ForwardMouseEventToRmlUi(Event);
        }
        return ForwardMouseEventToRmlUi(Event);
    }

    bool FInputViewport::ForwardMouseEventToRmlUi(FEvent& Event)
    {
        if (Event.IsA<FMouseMovedEvent>() && Context->GetMouseXRaw() == LastRmlMouseX && Context->GetMouseYRaw() == LastRmlMouseY)
        {
            return bLastRmlMoveConsumed;
        }

        // Event listeners can mutate the DOM that render reads concurrently, so hold the lock throughout.
        RmlUi::FLockedWorldContext Ctx(World);
        if (!Ctx)
        {
            return false;
        }

        const int Mods = ModifiersFromContext(*Context);

        if (Event.IsA<FMouseMovedEvent>())
        {
            // Raw accessors bypass UI-mode 0-pin; rescale RT space -> context space (panel stretches RT).
            const double Cx = Context->GetMouseXRaw();
            const double Cy = Context->GetMouseYRaw();
            const auto   Dims = Ctx->GetDimensions();
            const uint32 RTW  = Context->GetRenderTargetWidth();
            const uint32 RTH  = Context->GetRenderTargetHeight();
            const double Px = (RTW > 0) ? (Cx / double(RTW)) * double(Dims.x) : Cx;
            const double Py = (RTH > 0) ? (Cy / double(RTH)) * double(Dims.y) : Cy;
            bLastRmlMoveConsumed = !Ctx->ProcessMouseMove(int(Px), int(Py), Mods);
            LastRmlMouseX = Cx;
            LastRmlMouseY = Cy;
            return bLastRmlMoveConsumed;
        }
        if (Event.IsA<FMouseButtonPressedEvent>())
        {
            FMouseButtonEvent& E = Event.As<FMouseButtonPressedEvent>();
            return !Ctx->ProcessMouseButtonDown(ToRmlMouseButton(E.GetButton()), Mods);
        }
        if (Event.IsA<FMouseButtonReleasedEvent>())
        {
            FMouseButtonEvent& E = Event.As<FMouseButtonReleasedEvent>();
            return !Ctx->ProcessMouseButtonUp(ToRmlMouseButton(E.GetButton()), Mods);
        }
        if (Event.IsA<FMouseScrolledEvent>())
        {
            FMouseScrolledEvent& E = Event.As<FMouseScrolledEvent>();
            // Rml expects positive=down.
            return !Ctx->ProcessMouseWheel(-E.GetOffset(), Mods);
        }

        return false;
    }

    bool FInputViewport::ForwardKeyEventToRmlUi(FEvent& Event)
    {
        RmlUi::FLockedWorldContext Ctx(World);
        if (!Ctx)
        {
            return false;
        }

        if (Event.IsA<FKeyPressedEvent>())
        {
            FKeyPressedEvent& E = Event.As<FKeyPressedEvent>();
            const Rml::Input::KeyIdentifier RmlKey = ToRmlKey(E.GetKeyCode());
            if (RmlKey == Rml::Input::KI_UNKNOWN) return false;
            return !Ctx->ProcessKeyDown(RmlKey, ModifiersFromKeyEvent(E));
        }
        if (Event.IsA<FKeyReleasedEvent>())
        {
            FKeyReleasedEvent& E = Event.As<FKeyReleasedEvent>();
            const Rml::Input::KeyIdentifier RmlKey = ToRmlKey(E.GetKeyCode());
            if (RmlKey == Rml::Input::KI_UNKNOWN) return false;
            return !Ctx->ProcessKeyUp(RmlKey, ModifiersFromKeyEvent(E));
        }
        if (Event.IsA<FCharInputEvent>())
        {
            FCharInputEvent& E = Event.As<FCharInputEvent>();
            return !Ctx->ProcessTextInput(Rml::Character(E.GetCodepoint()));
        }

        return false;
    }

    FInputViewportRegistry::FInputViewportRegistry()
        : RawInput(MakeUnique<FInputContext>())
    {
    }

    FInputViewportRegistry& FInputViewportRegistry::Get()
    {
        static FInputViewportRegistry Instance;
        return Instance;
    }

    void FInputViewportRegistry::Register(FInputViewport* Viewport)
    {
        if (Viewport == nullptr) return;
        if (Algo::Contains(Viewports, Viewport)) return;
        Viewports.Add(Viewport);

        if (ActiveViewport == nullptr)
        {
            SetActiveViewport(Viewport);
        }
    }

    void FInputViewportRegistry::Unregister(FInputViewport* Viewport)
    {
        if (Viewport == nullptr) return;

        Viewports.RemoveSingle(Viewport);

        if (ActiveViewport     == Viewport) ActiveViewport     = Viewports.IsEmpty() ? nullptr : Viewports[0];
        if (HoveredViewport    == Viewport) HoveredViewport    = nullptr;
        if (MousePressViewport == Viewport) MousePressViewport = nullptr;

        ApplyActiveCursorMode();
    }

    void FInputViewportRegistry::ReplaceWorld(const CWorld* OldWorld, CWorld* NewWorld)
    {
        if (OldWorld == nullptr)
        {
            return;
        }
        for (FInputViewport* Viewport : Viewports)
        {
            if (Viewport->GetWorld() == OldWorld)
            {
                Viewport->SetWorld(NewWorld);
            }
        }
    }

    FInputViewport* FInputViewportRegistry::FindViewportForWorld(const CWorld* World) const
    {
        if (World == nullptr)
        {
            return nullptr;
        }

        for (FInputViewport* Viewport : Viewports)
        {
            if (Viewport->GetWorld() == World)
            {
                return Viewport;
            }
        }
        return nullptr;
    }

    void FInputViewportRegistry::SetActiveViewport(FInputViewport* Viewport)
    {
        if (ActiveViewport == Viewport)
        {
            return;
        }
        if (ActiveViewport != nullptr)
        {
            ActiveViewport->GetContext().ResetState();
        }
        ActiveViewport = Viewport;
        ApplyActiveCursorMode();
    }

    void FInputViewportRegistry::UpdateViewportFocus(FInputViewport* Viewport, bool bHovered, bool bEditorFocused)
    {
        if (Viewport == nullptr)
        {
            return;
        }

        if (bHovered)
        {
            HoveredViewport = Viewport;
        }
        else if (HoveredViewport == Viewport)
        {
            HoveredViewport = nullptr;
        }

        if (!bGameInputFocused)
        {
            if (bEditorFocused)
            {
                SetActiveViewport(Viewport);
            }
            return;
        }

        // Editor focus lies under a captured cursor, while exactly one OS window is focused, so separate preview windows hand over cleanly.
        const bool bActiveWindowFocused = ActiveViewport != nullptr && Windowing::IsNativeWindowFocused(ActiveViewport->GetNativeWindowHandle());
        if (!bActiveWindowFocused && Windowing::IsNativeWindowFocused(Viewport->GetNativeWindowHandle()))
        {
            SetActiveViewport(Viewport);
        }
    }

    void FInputViewportRegistry::TakeGameFocus(FInputViewport* Viewport)
    {
        if (Viewport != nullptr)
        {
            SetActiveViewport(Viewport);
        }
        SetGameInputFocused(true);
    }

    bool FInputViewportRegistry::IsGameOwningInput() const
    {
        const CWorld* ActiveWorld = ActiveViewport != nullptr ? ActiveViewport->GetWorld() : nullptr;
        return bGameInputFocused && ActiveWorld != nullptr && ActiveWorld->IsGameWorld();
    }

    void FInputViewportRegistry::SetHoveredViewport(FInputViewport* Viewport)
    {
        HoveredViewport = Viewport;
    }

    void FInputViewportRegistry::SetGameInputFocused(bool bFocused)
    {
        if (bGameInputFocused == bFocused)
        {
            return;
        }
        bGameInputFocused = bFocused;

        // Each world's desired mouse mode is preserved rather than wiped, so it survives the round-trip.
        ApplyActiveCursorMode();
    }

    void FInputViewportRegistry::SetDevOverlayCapturing(bool bCapturing)
    {
        if (bDevOverlayCapturing == bCapturing)
        {
            return;
        }
        bDevOverlayCapturing = bCapturing;

        // A key held as the overlay opens would otherwise stay down for the game until it is pressed again.
        OnWindowFocusLost();
        ApplyActiveCursorMode();
    }

    bool FInputViewportRegistry::IsActiveViewportReceivingGamepad() const
    {
        if (ActiveViewport == nullptr || !bGameInputFocused || bDevOverlayCapturing)
        {
            return false;
        }

        void* Window = ActiveViewport->GetNativeWindowHandle();
        if (Window == nullptr)
        {
            FWindow* Primary = Windowing::TryGetPrimaryWindowHandle();
            Window = Primary != nullptr ? Primary->GetWindow() : nullptr;
        }
        return Window != nullptr && Windowing::IsNativeWindowFocused(Window);
    }

    void FInputViewportRegistry::FeedGamepads()
    {
        const CInputSettings* Settings = GetDefault<CInputSettings>();
        FGamepadTuning Tuning;
        Tuning.StickDeadZone = Settings->GamepadStickDeadZone;
        Tuning.TriggerDeadZone = Settings->GamepadTriggerDeadZone;
        Tuning.TriggerPressThreshold = Settings->GamepadTriggerPressThreshold;
        Tuning.StickPressThreshold = Settings->GamepadStickPressThreshold;
        Gamepads.Poll(Tuning);

        const FGamepadState Disconnected;
        const bool bActiveReceives = IsActiveViewportReceivingGamepad();
        for (FInputViewport* V : Viewports)
        {
            V->GetContext().SetGamepadState((bActiveReceives && V == ActiveViewport) ? Gamepads.GetCombined() : Disconnected);
        }
        RawInput->SetGamepadState(Gamepads.GetCombined());
    }

    bool FInputViewportRegistry::BeginRebind(FName Action, int32 BindingIndex)
    {
        const SInputAction* Found = FInputActionMap::Get().FindAction(Action);
        if (Found == nullptr || BindingIndex < 0 || BindingIndex >= Found->Bindings.Num()
            || Found->Bindings[BindingIndex].Source != EInputAxisSource::Key)
        {
            return false;
        }
        RebindAction = Action;
        RebindIndex = BindingIndex;
        bRebindArmed = false;
        RebindConflict = FName();
        return true;
    }

    void FInputViewportRegistry::PollRebind()
    {
        if (RebindAction.IsNone())
        {
            return;
        }

        // The press that opened the capture, such as a menu click, can still read as pressed on the first frame.
        if (!bRebindArmed)
        {
            bRebindArmed = true;
            return;
        }

        FInputActionMap& Map = FInputActionMap::Get();
        const SInputAction* Action = Map.FindAction(RebindAction);
        const EKey PressedKey = RawInput->GetFirstPressedKey();
        if (Action == nullptr || RebindIndex >= Action->Bindings.Num() || PressedKey == EKey::Escape)
        {
            CancelRebind();
            return;
        }

        // A slot keeps its device class, so the keyboard column of a menu never ends up holding a pad button.
        const SKey& Current = Action->Bindings[RebindIndex].Key;
        SKey Captured;
        if (!Current.IsGamepad())
        {
            const EMouseKey PressedButton = RawInput->GetFirstPressedMouseButton();
            Captured = PressedKey != EKey::Num ? SKey(PressedKey) : PressedButton != EMouseKey::Num ? SKey(PressedButton) : SKey();
        }
        if (!Captured.IsValid() && !Current.IsKeyboard() && !Current.IsMouse())
        {
            const EGamepadButton PadButton = Gamepads.GetFirstPressedButton();
            Captured = PadButton != EGamepadButton::Num ? SKey(PadButton) : SKey();
        }
        if (!Captured.IsValid())
        {
            return;
        }

        const FName Rebound = RebindAction;
        CancelRebind();
        RebindConflict = Map.FindActionUsingKey(Captured, Rebound);
        CGameUserSettings::Get().SetBindingOverride(Rebound, RebindIndex, Captured);

        // Otherwise the held capture key reads as a fresh press of its new action on the next frame.
        for (FInputViewport* V : Viewports)
        {
            V->GetContext().ResetState();
        }
        RawInput->ResetState();
    }

    void FInputViewportRegistry::BeginFrame(double DeltaSeconds)
    {
        FeedGamepads();
        PollRebind();

        // Evaluated once after the events pump, so every query sees one snapshot and edges fire once.
        const FInputActionMap& Map = FInputActionMap::Get();
        for (FInputViewport* V : Viewports)
        {
            Map.UpdateContext(V->GetContext(), (float)DeltaSeconds);
        }

        Map.UpdateContext(*RawInput, (float)DeltaSeconds);

        // Hover moves without any event the registry sees, so the cursor image follows it once a frame.
        ApplyActiveCursor();
    }

    void FInputViewportRegistry::EndFrame()
    {
        for (FInputViewport* V : Viewports)
        {
            V->GetContext().EndFrame();
        }

        RawInput->EndFrame();
        LateLookWorld = nullptr;
    }

    bool FInputViewportRegistry::PumpLateMouseLook()
    {
        LateLookWorld = nullptr;

        FWindow* Window = Windowing::TryGetPrimaryWindowHandle();
        const CWorld* World = ActiveViewport != nullptr ? ActiveViewport->GetWorld() : nullptr;
        const FResolvedSceneView* View = World != nullptr ? World->GetResolvedView() : nullptr;
        if (Window == nullptr || View == nullptr || View->MouseLookDegreesPerCount <= 0.0f || !IsActiveCursorCaptured())
        {
            return false;
        }

        // Only the primary window holds its events, so a viewport in a window of its own keeps the update's view.
        const void* NativeWindow = ActiveViewport->GetNativeWindowHandle();
        if (NativeWindow != nullptr && NativeWindow != Window->GetWindow())
        {
            return false;
        }

        Window->PumpLateMessages();
        LateLookWorld = World;
        return true;
    }

    FVector2 FInputViewportRegistry::GetLateMouseLook(const CWorld* World) const
    {
        if (World == nullptr || World != LateLookWorld)
        {
            return FVector2(0.0f);
        }
        return Windowing::GetPrimaryWindowHandle()->GetLateMouseDelta() * World->GetResolvedView()->MouseLookDegreesPerCount;
    }

    void FInputViewportRegistry::OnWindowFocusLost()
    {
        for (FInputViewport* V : Viewports)
        {
            V->GetContext().ResetState();
        }

        RawInput->ResetState();
        Gamepads.Reset();
    }

    bool FInputViewportRegistry::OnEvent(FEvent& Event)
    {
        const bool bIsMouseEvent =
               Event.IsA<FMouseMovedEvent>()
            || Event.IsA<FMouseButtonPressedEvent>()
            || Event.IsA<FMouseButtonReleasedEvent>()
            || Event.IsA<FMouseScrolledEvent>();

        const bool bIsKeyEvent =
               Event.IsA<FKeyPressedEvent>()
            || Event.IsA<FKeyReleasedEvent>()
            || Event.IsA<FCharInputEvent>();

        if (!bIsMouseEvent && !bIsKeyEvent)
        {
            return false;
        }

        RawInput->OnEvent(Event);
        if (Event.IsA<FMouseMovedEvent>())
        {
            LastMouseWindowX = static_cast<int32>(Event.As<FMouseMovedEvent>().GetX());
            LastMouseWindowY = static_cast<int32>(Event.As<FMouseMovedEvent>().GetY());
        }

        FInputViewport* Target = bIsKeyEvent ? ActiveViewport : ResolveMouseTarget(Event);
        return Target != nullptr && Target->RouteEvent(Event);
    }

    FInputViewport* FInputViewportRegistry::ResolveMouseTarget(FEvent& Event)
    {
        // Hover is meaningless under a disabled cursor, and only the active viewport's cursor is ever captured.
        const bool bCaptured = IsActiveCursorCaptured();
        FInputViewport* Target = bCaptured ? ActiveViewport
                               : MousePressViewport != nullptr ? MousePressViewport
                               : HoveredViewport != nullptr ? HoveredViewport
                               : ActiveViewport;

        if (Event.IsA<FMouseButtonPressedEvent>() && !bCaptured)
        {
            MousePressViewport = Target;
        }
        else if (Event.IsA<FMouseButtonReleasedEvent>())
        {
            bool bAnyButtonDown = false;
            for (uint32 Button = (uint32)EMouseKey::Button0; Button <= (uint32)EMouseKey::ButtonLast; ++Button)
            {
                bAnyButtonDown = bAnyButtonDown || RawInput->IsMouseButtonDownRaw((EMouseKey)Button);
            }
            MousePressViewport = bAnyButtonDown ? MousePressViewport : nullptr;
        }
        return Target;
    }

    void FInputViewportRegistry::ApplyActiveCursorMode()
    {
        void* const ActiveWindow = (ActiveViewport != nullptr) ? ActiveViewport->GetNativeWindowHandle() : nullptr;

        for (FInputViewport* V : Viewports)
        {
            void* const Window = V->GetNativeWindowHandle();
            if (Window == nullptr || Window == ActiveWindow)
            {
                continue;
            }
            ApplyCursorModeToWindow(Window, EMouseMode::Normal);
        }

        if (ActiveViewport != nullptr)
        {
            ApplyCursorModeToWindow(ActiveWindow, GetActiveEffectiveMouseMode());
        }
        ApplyActiveCursor();
    }

    EMouseMode FInputViewportRegistry::GetActiveEffectiveMouseMode() const
    {
        // A benchmark run is unattended, and a captured cursor would vanish from whoever is using the machine.
        if (ActiveViewport == nullptr || Benchmark::IsActive() || bDevOverlayCapturing)
        {
            return EMouseMode::Normal;
        }

        // Releasing focus shows the cursor without discarding the world's desired mode.
        const CWorld* ActiveWorld = ActiveViewport->GetWorld();
        if (ActiveWorld != nullptr && ActiveWorld->IsGameWorld() && !bGameInputFocused)
        {
            return EMouseMode::Normal;
        }
        return ActiveViewport->GetContext().GetMouseMode();
    }

    bool FInputViewportRegistry::IsActiveCursorCaptured() const
    {
        return GetActiveEffectiveMouseMode() == EMouseMode::Captured;
    }

    void FInputViewportRegistry::SetMouseMode(EMouseMode Mode, const CWorld* CallerWorld)
    {
        // Each player's script sets only its own window, so an inactive one cannot wipe the focused capture.
        FInputViewport* Target = FindViewportForWorld(CallerWorld);
        if (Target == nullptr)
        {
            Target = ActiveViewport;
        }
        if (Target == nullptr)
        {
            return;
        }
        Target->GetContext().SetMouseMode(Mode);
        ApplyActiveCursorMode();
    }

    void FInputViewportRegistry::SetMouseCursor(EMouseCursor Cursor, const CWorld* CallerWorld)
    {
        FInputViewport* Target = FindViewportForWorld(CallerWorld);
        if (Target == nullptr)
        {
            Target = ActiveViewport;
        }
        if (Target == nullptr)
        {
            return;
        }
        Target->GetContext().SetMouseCursor(Cursor);
        ApplyActiveCursor();
    }

    const FInputViewport* FInputViewportRegistry::GetGameCursorViewport() const
    {
        // Over the editor or a dev overlay, or with the cursor hidden or captured, the window keeps the editor's or the system's own.
        if (ActiveViewport == nullptr || bDevOverlayCapturing || !IsGameOwningInput())
        {
            return nullptr;
        }

        // The editor reports no hover while the game holds input, so the last mouse position decides it then.
        const bool bPointerOver = HoveredViewport == ActiveViewport
            || (HoveredViewport == nullptr && ActiveViewport->ContainsWindowPoint(LastMouseWindowX, LastMouseWindowY));
        if (!bPointerOver)
        {
            return nullptr;
        }
        return GetActiveEffectiveMouseMode() == EMouseMode::Normal ? ActiveViewport : nullptr;
    }

    void* FInputViewportRegistry::ResolveCursor(EMouseCursor Cursor)
    {
        const CUISettings* Settings = GetDefault<CUISettings>();
        if (const SHardwareCursor* Hardware = Settings != nullptr ? Settings->FindHardwareCursor(Cursor) : nullptr)
        {
            const FString Key = Format("{}#{},{}", Hardware->Image, Hardware->Hotspot.x, Hardware->Hotspot.y);
            void** Cached = HardwareCursorImages.Find(Key);
            if (Cached == nullptr)
            {
                const bool bProjectRelative = Hardware->Image[0] != '/' && Global::GEngine != nullptr;
                const FFixedString Path = bProjectRelative ? Paths::Combine(Global::GEngine->GetProjectPath(), Hardware->Image) : FFixedString(*Hardware->Image, Hardware->Image.Len());
                HardwareCursorImages.Emplace(Key, Windowing::CreateCursorFromImage(Path, Hardware->Hotspot));
                Cached = HardwareCursorImages.Find(Key);
            }
            if (Cached != nullptr && *Cached != nullptr)
            {
                return *Cached;
            }
        }

        const int32 Index = static_cast<int32>(Cursor);
        if (SystemCursors[Index] == nullptr)
        {
            SystemCursors[Index] = Windowing::CreateSystemCursor(ToSystemCursor(Cursor));
        }
        return SystemCursors[Index];
    }

    void FInputViewportRegistry::ApplyActiveCursor()
    {
        // Without a window there is no platform cursor to make, as in a headless run or a test.
        if (Windowing::TryGetPrimaryWindowHandle() == nullptr)
        {
            return;
        }

        const FInputViewport* Owner = GetGameCursorViewport();
        void* const Window = Owner != nullptr ? Owner->GetNativeWindowHandle() : nullptr;
        if (bGameCursorApplied && (Owner == nullptr || Window != AppliedCursorWindow))
        {
            Windowing::SetCursorForNativeWindow(AppliedCursorWindow, nullptr);
            bGameCursorApplied = false;
        }
        if (Owner == nullptr)
        {
            return;
        }

        // Set every frame rather than on change, since the editor's own UI may swap the cursor in the frame before it stands back.
        Windowing::SetCursorForNativeWindow(Window, ResolveCursor(Owner->GetContext().GetMouseCursor()));
        AppliedCursorWindow = Window;
        bGameCursorApplied = true;
    }
}
