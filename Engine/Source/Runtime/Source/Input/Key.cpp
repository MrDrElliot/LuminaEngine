#include "RuntimePCH.h"
#include "Key.h"

#include "Containers/StringFormat.h"

namespace Lumina
{
    FString GetKeyDisplayName(EKey Key)
    {
        const int32 Code = (int32)Key;
        if ((Code >= 'A' && Code <= 'Z') || (Code >= '0' && Code <= '9'))
        {
            return FString(1, char(Code));
        }
        if (Key >= EKey::F1 && Key <= EKey::F25)
        {
            return Format("F{}", Code - (int32)EKey::F1 + 1);
        }
        if (Key >= EKey::KP0 && Key <= EKey::KP9)
        {
            return Format("KP{}", Code - (int32)EKey::KP0);
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
        default:                 return FString();
        }
    }

    FString GetMouseButtonDisplayName(EMouseKey Button)
    {
        switch (Button)
        {
        case EMouseKey::ButtonLeft:   return "Left Mouse Button";
        case EMouseKey::ButtonRight:  return "Right Mouse Button";
        case EMouseKey::ButtonMiddle: return "Middle Mouse Button";
        case EMouseKey::Scroll:
        case EMouseKey::Num:          return FString();
        default:                      return Format("Mouse Button {}", (int32)Button + 1);
        }
    }

    FString SKey::GetDisplayName(EGamepadFamily Family) const
    {
        FString Name;
        switch (Device)
        {
        case EKeyDevice::Keyboard: Name = GetKeyDisplayName(Key); break;
        case EKeyDevice::Mouse:    Name = GetMouseButtonDisplayName(MouseButton); break;
        case EKeyDevice::Gamepad:  Name = GetGamepadButtonDisplayName(GamepadButton, Family); break;
        case EKeyDevice::None:     break;
        }

        if (Name.IsEmpty())
        {
            return "None";
        }
        if (IsGamepad())
        {
            return "Gamepad " + Name;
        }

        FString Prefix;
        if (bCtrl)  { Prefix += "Ctrl+"; }
        if (bShift) { Prefix += "Shift+"; }
        if (bAlt)   { Prefix += "Alt+"; }
        return Prefix + Name;
    }
}
