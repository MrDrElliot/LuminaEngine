#include "RuntimePCH.h"
#include "InputActionMap.h"

#include "Config/InputSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Input/InputContext.h"
#include "Log/Log.h"

namespace Lumina
{
    FString FInputActionMap::KeyToString(EKey Key)
    {
        if (Key == EKey::Num) return FString("");
        const int V = static_cast<int>(Key);
        if (V >= 'A' && V <= 'Z') return FString(1, char(V));
        if (V >= '0' && V <= '9') return FString(1, char(V));
        if (V >= int(EKey::F1) && V <= int(EKey::F25))
        {
            char Buf[16];
            std::snprintf(Buf, sizeof(Buf), "F%d", V - int(EKey::F1) + 1);
            return FString(Buf);
        }
        if (V >= int(EKey::KP0) && V <= int(EKey::KP9))
        {
            char Buf[16];
            std::snprintf(Buf, sizeof(Buf), "KP%d", V - int(EKey::KP0));
            return FString(Buf);
        }
        switch (Key)
        {
        case EKey::Space:        return "Space";
        case EKey::Apostrophe:   return "Apostrophe";
        case EKey::Comma:        return "Comma";
        case EKey::Minus:        return "Minus";
        case EKey::Period:       return "Period";
        case EKey::Slash:        return "Slash";
        case EKey::Semicolon:    return "Semicolon";
        case EKey::Equal:        return "Equal";
        case EKey::LeftBracket:  return "LeftBracket";
        case EKey::Backslash:    return "Backslash";
        case EKey::RightBracket: return "RightBracket";
        case EKey::GraveAccent:  return "GraveAccent";
        case EKey::Escape:       return "Escape";
        case EKey::Enter:        return "Enter";
        case EKey::Tab:          return "Tab";
        case EKey::Backspace:    return "Backspace";
        case EKey::Insert:       return "Insert";
        case EKey::Delete:       return "Delete";
        case EKey::Right:        return "Right";
        case EKey::Left:         return "Left";
        case EKey::Down:         return "Down";
        case EKey::Up:           return "Up";
        case EKey::PageUp:       return "PageUp";
        case EKey::PageDown:     return "PageDown";
        case EKey::Home:         return "Home";
        case EKey::End:          return "End";
        case EKey::CapsLock:     return "CapsLock";
        case EKey::ScrollLock:   return "ScrollLock";
        case EKey::NumLock:      return "NumLock";
        case EKey::PrintScreen:  return "PrintScreen";
        case EKey::Pause:        return "Pause";
        case EKey::Menu:         return "Menu";
        case EKey::LeftShift:    return "LeftShift";
        case EKey::LeftControl:  return "LeftControl";
        case EKey::LeftAlt:      return "LeftAlt";
        case EKey::LeftSuper:    return "LeftSuper";
        case EKey::RightShift:   return "RightShift";
        case EKey::RightControl: return "RightControl";
        case EKey::RightAlt:     return "RightAlt";
        case EKey::RightSuper:   return "RightSuper";
        case EKey::KPDecimal:    return "KPDecimal";
        case EKey::KPDivide:     return "KPDivide";
        case EKey::KPMultiply:   return "KPMultiply";
        case EKey::KPSubtract:   return "KPSubtract";
        case EKey::KPAdd:        return "KPAdd";
        case EKey::KPEnter:      return "KPEnter";
        case EKey::KPEqual:      return "KPEqual";
        default:                 return "";
        }
    }

    FString FInputActionMap::MouseButtonToString(EMouseKey Button)
    {
        switch (Button)
        {
        case EMouseKey::ButtonLeft:   return "Left";
        case EMouseKey::ButtonRight:  return "Right";
        case EMouseKey::ButtonMiddle: return "Middle";
        default:                      return "";
        }
    }

    EKey FInputActionMap::KeyFromString(FStringView Token)
    {
        if (Token.empty())              return EKey::Num;
        if (Token.size() == 1)
        {
            const char C = Token.data()[0];
            if (C >= 'A' && C <= 'Z')   return static_cast<EKey>(C);
            if (C >= '0' && C <= '9')   return static_cast<EKey>(C);
        }
        if ((Token.data()[0] == 'F' || Token.data()[0] == 'f') && Token.size() <= 3 && Token.size() >= 2)
        {
            // FStringView isn't null-terminated; copy before atoi.
            char Buf[4] = {};
            const size_t Len = Token.size() < 3 ? Token.size() : 3;
            for (size_t i = 0; i < Len; ++i) Buf[i] = Token.data()[i];
            const int N = std::atoi(Buf + 1);
            if (N >= 1 && N <= 25)      return static_cast<EKey>(int(EKey::F1) + (N - 1));
        }
        if (Token.size() == 3 && Token.data()[0] == 'K' && Token.data()[1] == 'P'
            && Token.data()[2] >= '0' && Token.data()[2] <= '9')
        {
            return static_cast<EKey>(int(EKey::KP0) + (Token.data()[2] - '0'));
        }

        const FString T(Token.data(), Token.size());
        if (T == "Space")           return EKey::Space;
        if (T == "Apostrophe")      return EKey::Apostrophe;
        if (T == "Comma")           return EKey::Comma;
        if (T == "Minus")           return EKey::Minus;
        if (T == "Period")          return EKey::Period;
        if (T == "Slash")           return EKey::Slash;
        if (T == "Semicolon")       return EKey::Semicolon;
        if (T == "Equal")           return EKey::Equal;
        if (T == "LeftBracket")     return EKey::LeftBracket;
        if (T == "Backslash")       return EKey::Backslash;
        if (T == "RightBracket")    return EKey::RightBracket;
        if (T == "GraveAccent")     return EKey::GraveAccent;
        if (T == "Escape")          return EKey::Escape;
        if (T == "Enter")           return EKey::Enter;
        if (T == "Tab")             return EKey::Tab;
        if (T == "Backspace")       return EKey::Backspace;
        if (T == "Insert")          return EKey::Insert;
        if (T == "Delete")          return EKey::Delete;
        if (T == "Right")           return EKey::Right;
        if (T == "Left")            return EKey::Left;
        if (T == "Down")            return EKey::Down;
        if (T == "Up")              return EKey::Up;
        if (T == "PageUp")          return EKey::PageUp;
        if (T == "PageDown")        return EKey::PageDown;
        if (T == "Home")            return EKey::Home;
        if (T == "End")             return EKey::End;
        if (T == "CapsLock")        return EKey::CapsLock;
        if (T == "ScrollLock")      return EKey::ScrollLock;
        if (T == "NumLock")         return EKey::NumLock;
        if (T == "PrintScreen")     return EKey::PrintScreen;
        if (T == "Pause")           return EKey::Pause;
        if (T == "Menu")            return EKey::Menu;
        if (T == "LeftShift")       return EKey::LeftShift;
        if (T == "LeftControl")     return EKey::LeftControl;
        if (T == "LeftAlt")         return EKey::LeftAlt;
        if (T == "LeftSuper")       return EKey::LeftSuper;
        if (T == "RightShift")      return EKey::RightShift;
        if (T == "RightControl")    return EKey::RightControl;
        if (T == "RightAlt")        return EKey::RightAlt;
        if (T == "RightSuper")      return EKey::RightSuper;
        if (T == "KPDecimal")       return EKey::KPDecimal;
        if (T == "KPDivide")        return EKey::KPDivide;
        if (T == "KPMultiply")      return EKey::KPMultiply;
        if (T == "KPSubtract")      return EKey::KPSubtract;
        if (T == "KPAdd")           return EKey::KPAdd;
        if (T == "KPEnter")         return EKey::KPEnter;
        if (T == "KPEqual")         return EKey::KPEqual;
        return EKey::Num;
    }

    EMouseKey FInputActionMap::MouseButtonFromString(FStringView Token)
    {
        const FString T(Token.data(), Token.size());
        if (T == "Left"   || T == "ButtonLeft")   return EMouseKey::ButtonLeft;
        if (T == "Right"  || T == "ButtonRight")  return EMouseKey::ButtonRight;
        if (T == "Middle" || T == "ButtonMiddle") return EMouseKey::ButtonMiddle;
        return EMouseKey::Num;
    }

    const TVector<EKey>& FInputActionMap::AllSupportedKeys()
    {
        // Must stay in sync with KeyToString / KeyFromString.
        static const TVector<EKey> Keys = []()
        {
            TVector<EKey> K;
            for (int C = 'A'; C <= 'Z'; ++C) K.push_back(static_cast<EKey>(C));
            for (int C = '0'; C <= '9'; ++C) K.push_back(static_cast<EKey>(C));
            for (int i = int(EKey::F1); i <= int(EKey::F25); ++i) K.push_back(static_cast<EKey>(i));
            for (int i = int(EKey::KP0); i <= int(EKey::KP9); ++i) K.push_back(static_cast<EKey>(i));
            const EKey Named[] = {
                EKey::Space, EKey::Apostrophe, EKey::Comma, EKey::Minus, EKey::Period,
                EKey::Slash, EKey::Semicolon, EKey::Equal, EKey::LeftBracket, EKey::Backslash,
                EKey::RightBracket, EKey::GraveAccent, EKey::Escape, EKey::Enter, EKey::Tab,
                EKey::Backspace, EKey::Insert, EKey::Delete, EKey::Right, EKey::Left,
                EKey::Down, EKey::Up, EKey::PageUp, EKey::PageDown, EKey::Home, EKey::End,
                EKey::CapsLock, EKey::ScrollLock, EKey::NumLock, EKey::PrintScreen, EKey::Pause,
                EKey::Menu, EKey::LeftShift, EKey::LeftControl, EKey::LeftAlt, EKey::LeftSuper,
                EKey::RightShift, EKey::RightControl, EKey::RightAlt, EKey::RightSuper,
                EKey::KPDecimal, EKey::KPDivide, EKey::KPMultiply, EKey::KPSubtract,
                EKey::KPAdd, EKey::KPEnter, EKey::KPEqual,
            };
            for (EKey E : Named) K.push_back(E);
            return K;
        }();
        return Keys;
    }

    const TVector<EMouseKey>& FInputActionMap::AllSupportedMouseButtons()
    {
        static const TVector<EMouseKey> Buttons =
        {
            EMouseKey::ButtonLeft, EMouseKey::ButtonRight, EMouseKey::ButtonMiddle,
        };
        return Buttons;
    }

    namespace
    {
        // Required modifiers (from the SKey chord) must be down; un-required modifiers don't suppress.
        bool ModifiersSatisfied(const SKey& Key, const FInputContext& Context)
        {
            const int Mods = Context.GetCachedModifierState();
            // Rml::Input::KeyModifier: KM_CTRL=1, KM_SHIFT=2, KM_ALT=4, KM_META=8.
            const bool CtrlDown  = (Mods & 1) != 0;
            const bool ShiftDown = (Mods & 2) != 0;
            const bool AltDown   = (Mods & 4) != 0;

            if (Key.bCtrl  && !CtrlDown)  return false;
            if (Key.bShift && !ShiftDown) return false;
            if (Key.bAlt   && !AltDown)   return false;
            return true;
        }

        // Is this binding's key/button held this frame, with its modifier chord satisfied?
        bool IsSKeyDown(const SKey& Key, const FInputContext& Context)
        {
            if (!Key.IsValid() || !ModifiersSatisfied(Key, Context))
            {
                return false;
            }
            if (Key.IsKeyboard()) return Context.IsKeyDownRaw(Key.Key);
            if (Key.IsMouse())    return Context.IsMouseButtonDownRaw(Key.MouseButton);
            return false;
        }
    }

    FInputActionMap& FInputActionMap::Get()
    {
        static FInputActionMap Instance;
        return Instance;
    }

    void FInputActionMap::RebuildFromSettings()
    {
        Actions = GetDefault<CInputSettings>()->Actions;
        MappingContexts = GetDefault<CInputSettings>()->MappingContexts;
        Lookup.clear();
        for (int32 i = 0; i < int32(Actions.size()); ++i)
        {
            // Actions authored before EInputActionType existed only said "this is an axis". Fold that into
            // the type here rather than at every read, so nothing downstream has to know about bAxis.
            SInputAction& Action = Actions[i];
            if (Action.bAxis && Action.Type == EInputActionType::Digital)
            {
                Action.Type = EInputActionType::Axis1D;
            }
            Lookup[Action.Name] = i;
        }

        // Invalidates every cached action index and every context's state array (they re-size on their
        // next update, which also clears state carried over from a removed action).
        ++Serial;
        LOG_INFO("[InputActions] Loaded {} actions, {} mapping contexts.", Actions.size(), MappingContexts.size());
    }

    const SInputAction* FInputActionMap::FindAction(FName Name) const
    {
        const auto It = Lookup.find(Name);
        return It != Lookup.end() ? &Actions[It->second] : nullptr;
    }

    int32 FInputActionMap::FindActionIndex(FName Name) const
    {
        const auto It = Lookup.find(Name);
        return It != Lookup.end() ? It->second : INDEX_NONE;
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
        // Top of the stack down: the first layer listing the action allows it, and a blocking layer that
        // does not list it swallows it before any layer underneath is consulted.
        const TVector<FName>& Stack = Context.GetInputLayers();
        for (size_t i = Stack.size(); i > 0; --i)
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

        // No layer decided. bRunsInUI predates mapping layers and stays the escape hatch for a project
        // that has not authored any: UI mode behaves as a blocking layer over the actions that opt in.
        return Action.bRunsInUI || Context.GetInputMode() != EInputMode::UI;
    }

    void FInputActionMap::EvaluateRaw(const SInputAction& Action, const FInputContext& Context,
        float& OutX, float& OutY, bool& bOutAnyKeyDown) const
    {
        float Raw[2] = { 0.0f, 0.0f };
        bOutAnyKeyDown = false;

        for (const SInputActionBinding& Binding : Action.Bindings)
        {
            // Only Axis2D splits channels; everything else collapses onto X so a 2D action demoted to
            // Axis1D still reads its X bindings instead of silently going quiet.
            const int32 Channel = (Action.Type == EInputActionType::Axis2D && Binding.Channel == EInputAxisChannel::Y) ? 1 : 0;

            switch (Binding.Source)
            {
            case EInputAxisSource::Key:
                if (IsSKeyDown(Binding.Key, Context))
                {
                    Raw[Channel] += Binding.Scale;
                    bOutAnyKeyDown = true;
                }
                break;

            case EInputAxisSource::MouseX:
                Raw[Channel] += float(Context.GetMouseDeltaXRaw()) * Binding.Scale;
                break;

            case EInputAxisSource::MouseY:
                Raw[Channel] += float(Context.GetMouseDeltaYRaw()) * Binding.Scale;
                break;

            case EInputAxisSource::MouseWheel:
                Raw[Channel] += float(Context.GetMouseZRaw()) * Binding.Scale;
                break;
            }
        }

        const float Sign = Action.bInvert ? -1.0f : 1.0f;
        OutX = Raw[0] * Action.Sensitivity * Sign;
        OutY = Raw[1] * Action.Sensitivity * Sign;

        // Dead zone: radial for Axis2D so a diagonal isn't cut twice, per-channel otherwise. The remainder
        // is rescaled, so full deflection still reaches the value it had without a dead zone.
        const float Dead = Math::Clamp(Action.DeadZone, 0.0f, 0.99f);
        if (Dead <= 0.0f)
        {
            return;
        }

        if (Action.Type == EInputActionType::Axis2D)
        {
            const float Magnitude = Math::Sqrt(OutX * OutX + OutY * OutY);
            if (Magnitude <= Dead || Magnitude <= 0.0f)
            {
                OutX = 0.0f;
                OutY = 0.0f;
                return;
            }
            const float Scaled = (Magnitude - Dead) / (1.0f - Dead);
            OutX = (OutX / Magnitude) * Scaled;
            OutY = (OutY / Magnitude) * Scaled;
            return;
        }

        auto ApplyDeadZone = [Dead](float V)
        {
            const float Magnitude = Math::Abs(V);
            if (Magnitude <= Dead)
            {
                return 0.0f;
            }
            return Math::Sign(V) * ((Magnitude - Dead) / (1.0f - Dead));
        };
        OutX = ApplyDeadZone(OutX);
        OutY = ApplyDeadZone(OutY);
    }

    void FInputActionMap::UpdateContext(FInputContext& Context, float DeltaSeconds) const
    {
        TVector<FInputActionState>& States = Context.GetMutableActionStates();

        // A rebuild reshuffles indices, so the whole array is discarded rather than migrated: carrying a
        // stale HeldTime onto a different action would fire a phantom release on the next frame.
        if (Context.GetActionsSerial() != Serial || States.size() != Actions.size())
        {
            States.assign(Actions.size(), FInputActionState());
            Context.SetActionsSerial(Serial);
        }

        for (size_t Index = 0; Index < Actions.size(); ++Index)
        {
            const SInputAction& Action = Actions[Index];
            FInputActionState& State = States[Index];
            const bool bWasDown = State.IsDown();

            float X = 0.0f;
            float Y = 0.0f;
            bool bAnyKeyDown = false;
            if (PassesGate(Action, Context))
            {
                EvaluateRaw(Action, Context, X, Y, bAnyKeyDown);
            }

            // A digital action is down while a key binding is held; a continuous one is down while its
            // shaped value is non-zero (the dead zone has already decided what counts as movement).
            const bool bDown = (Action.Type == EInputActionType::Digital)
                ? bAnyKeyDown
                : (bAnyKeyDown || X != 0.0f || Y != 0.0f);

            const float HeldTime = bDown ? (bWasDown ? State.HeldTime + DeltaSeconds : 0.0f) : 0.0f;

            uint32 Flags = 0;
            if (bDown)               { Flags |= FInputActionState::Flag_Down; }
            if (bDown && !bWasDown)  { Flags |= FInputActionState::Flag_Pressed; }
            if (!bDown && bWasDown)  { Flags |= FInputActionState::Flag_Released; }
            if (bDown && HeldTime >= Action.HoldTime) { Flags |= FInputActionState::Flag_Held; }
            // Tap is decided on the release frame from the press duration we are about to discard.
            if (!bDown && bWasDown && State.HeldTime <= Action.TapTime) { Flags |= FInputActionState::Flag_Tapped; }

            State.X = X;
            State.Y = Y;
            State.HeldTime = HeldTime;
            State.Flags = Flags;
        }
    }

    const FInputActionState& FInputActionMap::GetActionState(FName Name, const FInputContext& Context) const
    {
        static const FInputActionState Empty;

        const int32 Index = FindActionIndex(Name);
        if (Index == INDEX_NONE)
        {
            return Empty;
        }

        // The context has not been updated against this action table yet (a viewport queried before its
        // first frame, or a context outside the registry). Reading Empty is the safe default.
        const TVector<FInputActionState>& States = Context.GetActionStates();
        if (Context.GetActionsSerial() != Serial || Index >= int32(States.size()))
        {
            return Empty;
        }
        return States[Index];
    }

    const FInputActionState& FInputActionMap::GetActionState(const FInputActionHandle& Handle, const FInputContext& Context) const
    {
        static const FInputActionState Empty;

        // Name too: a details-panel edit does not bump the serial, so a serial-only check answers stale.
        if (Handle.CachedSerial != Serial || Handle.CachedName != Handle.Name)
        {
            Handle.CachedIndex  = FindActionIndex(Handle.Name);
            Handle.CachedSerial = Serial;
            Handle.CachedName   = Handle.Name;
        }

        if (Handle.CachedIndex == INDEX_NONE)
        {
            return Empty;
        }

        const TVector<FInputActionState>& States = Context.GetActionStates();
        if (Context.GetActionsSerial() != Serial || Handle.CachedIndex >= int32(States.size()))
        {
            return Empty;
        }
        return States[Handle.CachedIndex];
    }

    bool FInputActionMap::IsActionDown(FName Name, const FInputContext& Context) const
    {
        return GetActionState(Name, Context).IsDown();
    }

    bool FInputActionMap::IsActionPressed(FName Name, const FInputContext& Context) const
    {
        return GetActionState(Name, Context).IsPressed();
    }

    bool FInputActionMap::IsActionReleased(FName Name, const FInputContext& Context) const
    {
        return GetActionState(Name, Context).IsReleased();
    }

    bool FInputActionMap::IsActionHeld(FName Name, const FInputContext& Context) const
    {
        return GetActionState(Name, Context).IsHeld();
    }

    bool FInputActionMap::WasActionTapped(FName Name, const FInputContext& Context) const
    {
        return GetActionState(Name, Context).IsTapped();
    }

    float FInputActionMap::GetActionAxis(FName Name, const FInputContext& Context) const
    {
        return GetActionState(Name, Context).X;
    }

    float FInputActionMap::GetActionAxisY(FName Name, const FInputContext& Context) const
    {
        return GetActionState(Name, Context).Y;
    }

    float FInputActionMap::GetActionHeldTime(FName Name, const FInputContext& Context) const
    {
        return GetActionState(Name, Context).HeldTime;
    }
}
