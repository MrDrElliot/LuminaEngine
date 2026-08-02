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
            char Buf[4];
            std::snprintf(Buf, sizeof(Buf), "F%d", V - int(EKey::F1) + 1);
            return FString(Buf);
        }
        if (V >= int(EKey::KP0) && V <= int(EKey::KP9))
        {
            char Buf[4];
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
        Lookup.clear();
        for (int32 i = 0; i < int32(Actions.size()); ++i)
        {
            Lookup[Actions[i].Name] = i;
        }
        LOG_INFO("[InputActions] Loaded {} actions.", Actions.size());
    }

    const SInputAction* FInputActionMap::FindAction(FName Name) const
    {
        const auto It = Lookup.find(Name);
        return It != Lookup.end() ? &Actions[It->second] : nullptr;
    }

    bool FInputActionMap::PassesUIGate(const SInputAction& Action, const FInputContext& Context) const
    {
        return Action.bRunsInUI || Context.GetInputMode() != EInputMode::UI;
    }

    bool FInputActionMap::EvaluateDown(const SInputAction& Action, const FInputContext& Context) const
    {
        for (const SInputActionBinding& Binding : Action.Bindings)
        {
            if (IsSKeyDown(Binding.Key, Context))
            {
                return true;
            }
        }
        return false;
    }

    float FInputActionMap::EvaluateAxis(const SInputAction& Action, const FInputContext& Context) const
    {
        float Sum = 0.0f;
        for (const SInputActionBinding& Binding : Action.Bindings)
        {
            if (IsSKeyDown(Binding.Key, Context))
            {
                Sum += Binding.Scale;
            }
        }
        return Sum;
    }

    bool FInputActionMap::IsActionDown(FName Name, const FInputContext& Context) const
    {
        const SInputAction* Action = FindAction(Name);
        if (Action == nullptr || !PassesUIGate(*Action, Context))
        {
            return false;
        }
        return EvaluateDown(*Action, Context);
    }

    bool FInputActionMap::IsActionPressed(FName Name, const FInputContext& Context) const
    {
        const SInputAction* Action = FindAction(Name);
        if (Action == nullptr || !PassesUIGate(*Action, Context))
        {
            return false;
        }
        const bool DownNow = EvaluateDown(*Action, Context);
        const bool DownLast = Context.WasActionDownLastFrame(Name);
        return DownNow && !DownLast;
    }

    bool FInputActionMap::IsActionReleased(FName Name, const FInputContext& Context) const
    {
        const SInputAction* Action = FindAction(Name);
        if (Action == nullptr || !PassesUIGate(*Action, Context))
        {
            return false;
        }
        const bool DownNow = EvaluateDown(*Action, Context);
        const bool DownLast = Context.WasActionDownLastFrame(Name);
        return !DownNow && DownLast;
    }

    float FInputActionMap::GetActionAxis(FName Name, const FInputContext& Context) const
    {
        const SInputAction* Action = FindAction(Name);
        if (Action == nullptr || !PassesUIGate(*Action, Context))
        {
            return 0.0f;
        }
        return EvaluateAxis(*Action, Context);
    }
}
