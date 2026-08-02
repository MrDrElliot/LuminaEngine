#include "Key.h"

#include "Input/InputActionMap.h"

namespace Lumina
{
    FString SKey::GetDisplayName() const
    {
        // Modifier prefix shared by keyboard and mouse bindings (e.g. "Ctrl+Shift+").
        FString Prefix;
        if (bCtrl)  Prefix += "Ctrl+";
        if (bShift) Prefix += "Shift+";
        if (bAlt)   Prefix += "Alt+";

        switch (Device)
        {
        case EKeyDevice::Keyboard:
            {
                if (Key == EKey::Num) return FString("None");
                FString Name = FInputActionMap::KeyToString(Key);
                if (Name.empty()) return FString("None");
                return Prefix + Name;
            }
        case EKeyDevice::Mouse:
            switch (MouseButton)
            {
            case EMouseKey::ButtonLeft:   return Prefix + "Left Mouse Button";
            case EMouseKey::ButtonRight:  return Prefix + "Right Mouse Button";
            case EMouseKey::ButtonMiddle: return Prefix + "Middle Mouse Button";
            case EMouseKey::Num:          return FString("None");
            default:                      return Prefix + "Mouse " + FInputActionMap::MouseButtonToString(MouseButton);
            }
        case EKeyDevice::None:
        default:
            return FString("None");
        }
    }
}
