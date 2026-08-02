#pragma once

#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"
#include "Key.generated.h"

namespace Lumina
{
    // Which input device an SKey refers to.
    REFLECT()
    enum class EKeyDevice : uint8
    {
        None,
        Keyboard,
        Mouse,
    };

    // A single bindable input "key".
    REFLECT()
    struct RUNTIME_API SKey
    {
        GENERATED_BODY()

        SKey() = default;
        explicit SKey(EKey InKey)        : Device(EKeyDevice::Keyboard), Key(InKey) {}
        explicit SKey(EMouseKey InMouse) : Device(EKeyDevice::Mouse), MouseButton(InMouse) {}

        // Chord ctor: a keyboard key plus modifier flags. Handy for hotkey defaults, e.g.
        // SKey(EKey::S, true) for Ctrl+S or SKey(EKey::I, true, true) for Ctrl+Shift+I.
        SKey(EKey InKey, bool bInCtrl, bool bInShift = false, bool bInAlt = false)
            : Device(EKeyDevice::Keyboard), Key(InKey), bShift(bInShift), bCtrl(bInCtrl), bAlt(bInAlt) {}

        // The device this binding targets. None == unbound.
        PROPERTY()
        EKeyDevice Device = EKeyDevice::None;

        // The keyboard key, valid when Device == Keyboard.
        PROPERTY()
        EKey Key = EKey::Num;

        // The mouse button, valid when Device == Mouse.
        PROPERTY()
        EMouseKey MouseButton = EMouseKey::Num;

        // Modifier chord. A standalone modifier binding (e.g. Key == LeftControl) leaves these false.
        PROPERTY()
        bool bShift = false;
        PROPERTY()
        bool bCtrl = false;
        PROPERTY()
        bool bAlt = false;

        bool IsKeyboard() const { return Device == EKeyDevice::Keyboard; }
        bool IsMouse() const    { return Device == EKeyDevice::Mouse; }

        bool IsValid() const
        {
            if (Device == EKeyDevice::Keyboard) return Key != EKey::Num;
            if (Device == EKeyDevice::Mouse)    return MouseButton != EMouseKey::Num;
            return false;
        }

        void Clear()
        {
            Device      = EKeyDevice::None;
            Key         = EKey::Num;
            MouseButton = EMouseKey::Num;
            bShift = bCtrl = bAlt = false;
        }

        // Set the bound key/button, clearing modifiers (the customization sets them afterward).
        void SetKey(EKey InKey)              { Device = EKeyDevice::Keyboard; Key = InKey; MouseButton = EMouseKey::Num; bShift = bCtrl = bAlt = false; }
        void SetMouseButton(EMouseKey InBtn) { Device = EKeyDevice::Mouse; MouseButton = InBtn; Key = EKey::Num; bShift = bCtrl = bAlt = false; }

        // Human-readable label ("Ctrl+S", "F5", "Left Mouse Button", or "None"). Defined in Key.cpp.
        FString GetDisplayName() const;

        bool operator==(const SKey& Other) const
        {
            return Device == Other.Device && Key == Other.Key && MouseButton == Other.MouseButton
                && bShift == Other.bShift && bCtrl == Other.bCtrl && bAlt == Other.bAlt;
        }
        bool operator!=(const SKey& Other) const { return !(*this == Other); }
    };
}
