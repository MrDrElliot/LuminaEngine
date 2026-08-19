#include "RuntimePCH.h"
#include "InputViewport.h"

#include "Core/Windows/Window.h"
#include "Events/Event.h"
#include "Input/InputActionMap.h"
#include "Input/InputContext.h"
#include "UI/RmlUiBridge.h"
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
            Windowing::SetCursorModeForNativeWindow(NativeWindow, CursorMode);
        }

        Rml::Input::KeyIdentifier ToRmlKey(EKey Key)
        {
            using K = Rml::Input::KeyIdentifier;
            switch (Key)
            {
            case EKey::A: return K::KI_A;  case EKey::B: return K::KI_B;
            case EKey::C: return K::KI_C;  case EKey::D: return K::KI_D;
            case EKey::E: return K::KI_E;  case EKey::F: return K::KI_F;
            case EKey::G: return K::KI_G;  case EKey::H: return K::KI_H;
            case EKey::I: return K::KI_I;  case EKey::J: return K::KI_J;
            case EKey::K: return K::KI_K;  case EKey::L: return K::KI_L;
            case EKey::M: return K::KI_M;  case EKey::N: return K::KI_N;
            case EKey::O: return K::KI_O;  case EKey::P: return K::KI_P;
            case EKey::Q: return K::KI_Q;  case EKey::R: return K::KI_R;
            case EKey::S: return K::KI_S;  case EKey::T: return K::KI_T;
            case EKey::U: return K::KI_U;  case EKey::V: return K::KI_V;
            case EKey::W: return K::KI_W;  case EKey::X: return K::KI_X;
            case EKey::Y: return K::KI_Y;  case EKey::Z: return K::KI_Z;
            case EKey::D0: return K::KI_0; case EKey::D1: return K::KI_1;
            case EKey::D2: return K::KI_2; case EKey::D3: return K::KI_3;
            case EKey::D4: return K::KI_4; case EKey::D5: return K::KI_5;
            case EKey::D6: return K::KI_6; case EKey::D7: return K::KI_7;
            case EKey::D8: return K::KI_8; case EKey::D9: return K::KI_9;
            case EKey::F1:  return K::KI_F1;  case EKey::F2:  return K::KI_F2;
            case EKey::F3:  return K::KI_F3;  case EKey::F4:  return K::KI_F4;
            case EKey::F5:  return K::KI_F5;  case EKey::F6:  return K::KI_F6;
            case EKey::F7:  return K::KI_F7;  case EKey::F8:  return K::KI_F8;
            case EKey::F9:  return K::KI_F9;  case EKey::F10: return K::KI_F10;
            case EKey::F11: return K::KI_F11; case EKey::F12: return K::KI_F12;
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

        int ModifiersFromKeyEvent(const FKeyEvent& KeyEvent)
        {
            using namespace Rml::Input;
            int Mods = 0;
            if (KeyEvent.IsCtrlDown())  Mods |= KM_CTRL;
            if (KeyEvent.IsShiftDown()) Mods |= KM_SHIFT;
            if (KeyEvent.IsAltDown())   Mods |= KM_ALT;
            if (KeyEvent.IsSuperDown()) Mods |= KM_META;
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
        // State updates run unconditionally so a release we routed as a
        // fallback still latches, otherwise keys can stay "down" forever.
        Context->OnEvent(Event);

        const EInputMode Mode = Context->GetInputMode();
        const bool bWantsUI   = (Mode == EInputMode::UI || Mode == EInputMode::GameAndUI);

        if (!bWantsUI)
        {
            return false;
        }

        if (Event.IsA<FMouseMovedEvent>())
        {
            FMouseMovedEvent& E = Event.As<FMouseMovedEvent>();
            if (!ContainsWindowPoint(int(E.GetX()), int(E.GetY())))
            {
                return false;
            }
            return ForwardMouseEventToRmlUi(Event);
        }
        if (Event.IsA<FMouseButtonPressedEvent>())
        {
            FMouseButtonEvent& E = Event.As<FMouseButtonPressedEvent>();
            if (!ContainsWindowPoint(int(E.GetX()), int(E.GetY())))
            {
                return false;
            }
            return ForwardMouseEventToRmlUi(Event);
        }
        if (Event.IsA<FMouseButtonReleasedEvent>())
        {
            // Forward unconditionally so RmlUi can close out an in-progress drag.
            return ForwardMouseEventToRmlUi(Event);
        }
        if (Event.IsA<FMouseScrolledEvent>())
        {
            return ForwardMouseEventToRmlUi(Event);
        }
        if (Event.IsA<FKeyPressedEvent>()
            || Event.IsA<FKeyReleasedEvent>()
            || Event.IsA<FCharInputEvent>())
        {
            return ForwardKeyEventToRmlUi(Event);
        }

        return false;
    }

    bool FInputViewport::ForwardMouseEventToRmlUi(FEvent& Event)
    {
        // Hold lock for full ProcessXxx: event listeners can mutate the DOM concurrently read by render.
        RmlUi::FLockedWorldContext Ctx(World);
        if (!Ctx)
        {
            return false;
        }

        const int Mods = Context->GetCachedModifierState();

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
            return !Ctx->ProcessMouseMove(int(Px), int(Py), Mods);
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
            const int Mods = ModifiersFromKeyEvent(E);
            Context->SetCachedModifierState(Mods);
            const Rml::Input::KeyIdentifier RmlKey = ToRmlKey(E.GetKeyCode());
            if (RmlKey == Rml::Input::KI_UNKNOWN) return false;
            return !Ctx->ProcessKeyDown(RmlKey, Mods);
        }
        if (Event.IsA<FKeyReleasedEvent>())
        {
            FKeyReleasedEvent& E = Event.As<FKeyReleasedEvent>();
            const int Mods = ModifiersFromKeyEvent(E);
            Context->SetCachedModifierState(Mods);
            const Rml::Input::KeyIdentifier RmlKey = ToRmlKey(E.GetKeyCode());
            if (RmlKey == Rml::Input::KI_UNKNOWN) return false;
            return !Ctx->ProcessKeyUp(RmlKey, Mods);
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
        if (std::find(Viewports.begin(), Viewports.end(), Viewport) != Viewports.end()) return;
        Viewports.push_back(Viewport);

        if (ActiveViewport == nullptr)
        {
            SetActiveViewport(Viewport);
        }
    }

    void FInputViewportRegistry::Unregister(FInputViewport* Viewport)
    {
        if (Viewport == nullptr) return;

        auto It = std::find(Viewports.begin(), Viewports.end(), Viewport);
        if (It != Viewports.end())
        {
            Viewports.erase(It);
        }

        if (ActiveViewport  == Viewport) ActiveViewport  = Viewports.empty() ? nullptr : Viewports.front();
        if (HoveredViewport == Viewport) HoveredViewport = nullptr;
        if (FocusedViewport == Viewport) FocusedViewport = nullptr;

        ApplyActiveCursorMode();
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
        ActiveViewport = Viewport;
        ApplyActiveCursorMode();
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

        // Cursor ownership follows input focus: releasing hands the cursor back across every window (the
        // editor needs it) and re-acquiring restores the active game world's capture. Each world's desired
        // mouse mode is preserved (not wiped), so it survives the round-trip -- ApplyActiveCursorMode gates
        // a game world's capture on this flag instead.
        ApplyActiveCursorMode();
    }

    void FInputViewportRegistry::SetFocusedViewport(FInputViewport* Viewport)
    {
        if (FocusedViewport == Viewport)
        {
            return;
        }

        // Otherwise a key held during the focus change reappears as "down" on return.
        if (FocusedViewport != nullptr)
        {
            FocusedViewport->GetContext().ResetState();
        }
        FocusedViewport = Viewport;
    }

    void FInputViewportRegistry::BeginFrame(double DeltaSeconds)
    {
        // Actions are evaluated once here, after the frame's events have been pumped and before anything
        // updates, so every query during the frame sees the same snapshot and edges fire exactly once.
        const FInputActionMap& Map = FInputActionMap::Get();
        for (FInputViewport* V : Viewports)
        {
            Map.UpdateContext(V->GetContext(), (float)DeltaSeconds);
        }

        Map.UpdateContext(*RawInput, (float)DeltaSeconds);
    }

    void FInputViewportRegistry::EndFrame(double DeltaSeconds)
    {
        for (FInputViewport* V : Viewports)
        {
            V->GetContext().EndFrame(DeltaSeconds);
        }

        RawInput->EndFrame(DeltaSeconds);
    }

    void FInputViewportRegistry::OnWindowFocusLost()
    {
        for (FInputViewport* V : Viewports)
        {
            V->GetContext().ResetState();
        }

        RawInput->ResetState();
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

        // Captured cursor owns input, ImGui hover/focus flags lie under
        // GLFW_CURSOR_DISABLED, so don't trust them for routing.
        for (FInputViewport* V : Viewports)
        {
            if (V->GetContext().GetMouseMode() == EMouseMode::Captured)
            {
                return V->RouteEvent(Event);
            }
        }

        if (bIsKeyEvent)
        {
            FInputViewport* Target = FocusedViewport ? FocusedViewport : ActiveViewport;
            if (Target == nullptr)
            {
                return false;
            }
            return Target->RouteEvent(Event);
        }

        // Fall through to focused/active so a release after the cursor leaves
        // the panel still reaches whichever viewport saw the press.
        FInputViewport* Target = HoveredViewport;
        if (Target == nullptr)
        {
            Target = FocusedViewport;
        }
        if (Target == nullptr)
        {
            Target = ActiveViewport;
        }
        if (Target == nullptr)
        {
            return false;
        }
        return Target->RouteEvent(Event);
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
            EMouseMode Mode = ActiveViewport->GetContext().GetMouseMode();

            // A game world only holds the cursor while it owns input focus; releasing focus (Shift+F1 / Stop)
            // shows the cursor without discarding the world's desired mode. Editor worlds keep their mode so
            // RMB-look capture is unaffected.
            const CWorld* ActiveWorld = ActiveViewport->GetWorld();
            if (ActiveWorld != nullptr && ActiveWorld->IsGameWorld() && !bGameInputFocused)
            {
                Mode = EMouseMode::Normal;
            }

            ApplyCursorModeToWindow(ActiveWindow, Mode);
        }
    }

    void FInputViewportRegistry::ReapplyActiveCursorMode()
    {
        ApplyActiveCursorMode();
    }
}
