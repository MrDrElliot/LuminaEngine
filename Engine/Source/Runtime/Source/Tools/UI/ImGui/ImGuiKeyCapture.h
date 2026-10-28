#pragma once

#include "Containers/StaticArray.h"
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"
#include "Input/Key.h"

#include "imgui.h"

// Shared ImGui-side key capture helpers: poll the first key / mouse button pressed this frame and map
// ImGui's key ids onto the engine's EKey / EMouseKey. Used by the Input Action editor and the SKey
// property customization. We poll ImGui rather than FEventProcessor because detached tool windows are
// secondary ImGui viewports whose GLFW key callbacks never chain back to Lumina's window callback.

namespace Lumina::ImGuiX
{
    // Engine EKey for an ImGuiKey, or EKey::Num when there's no equivalent.
    constexpr EKey ImGuiKeyToEKey(ImGuiKey Key)
    {
        if (Key >= ImGuiKey_A && Key <= ImGuiKey_Z) return static_cast<EKey>(int(EKey::A) + (Key - ImGuiKey_A));
        if (Key >= ImGuiKey_0 && Key <= ImGuiKey_9) return static_cast<EKey>(int(EKey::D0) + (Key - ImGuiKey_0));
        if (Key >= ImGuiKey_F1 && Key <= ImGuiKey_F24) return static_cast<EKey>(int(EKey::F1) + (Key - ImGuiKey_F1));
        if (Key >= ImGuiKey_Keypad0 && Key <= ImGuiKey_Keypad9) return static_cast<EKey>(int(EKey::KP0) + (Key - ImGuiKey_Keypad0));

        switch (Key)
        {
        case ImGuiKey_Tab:           return EKey::Tab;
        case ImGuiKey_LeftArrow:     return EKey::Left;
        case ImGuiKey_RightArrow:    return EKey::Right;
        case ImGuiKey_UpArrow:       return EKey::Up;
        case ImGuiKey_DownArrow:     return EKey::Down;
        case ImGuiKey_PageUp:        return EKey::PageUp;
        case ImGuiKey_PageDown:      return EKey::PageDown;
        case ImGuiKey_Home:          return EKey::Home;
        case ImGuiKey_End:           return EKey::End;
        case ImGuiKey_Insert:        return EKey::Insert;
        case ImGuiKey_Delete:        return EKey::Delete;
        case ImGuiKey_Backspace:     return EKey::Backspace;
        case ImGuiKey_Space:         return EKey::Space;
        case ImGuiKey_Enter:         return EKey::Enter;
        case ImGuiKey_Escape:        return EKey::Escape;
        case ImGuiKey_LeftCtrl:      return EKey::LeftControl;
        case ImGuiKey_LeftShift:     return EKey::LeftShift;
        case ImGuiKey_LeftAlt:       return EKey::LeftAlt;
        case ImGuiKey_LeftSuper:     return EKey::LeftSuper;
        case ImGuiKey_RightCtrl:     return EKey::RightControl;
        case ImGuiKey_RightShift:    return EKey::RightShift;
        case ImGuiKey_RightAlt:      return EKey::RightAlt;
        case ImGuiKey_RightSuper:    return EKey::RightSuper;
        case ImGuiKey_Menu:          return EKey::Menu;
        case ImGuiKey_Apostrophe:    return EKey::Apostrophe;
        case ImGuiKey_Comma:         return EKey::Comma;
        case ImGuiKey_Minus:         return EKey::Minus;
        case ImGuiKey_Period:        return EKey::Period;
        case ImGuiKey_Slash:         return EKey::Slash;
        case ImGuiKey_Semicolon:     return EKey::Semicolon;
        case ImGuiKey_Equal:         return EKey::Equal;
        case ImGuiKey_LeftBracket:   return EKey::LeftBracket;
        case ImGuiKey_Backslash:     return EKey::Backslash;
        case ImGuiKey_RightBracket:  return EKey::RightBracket;
        case ImGuiKey_GraveAccent:   return EKey::GraveAccent;
        case ImGuiKey_CapsLock:      return EKey::CapsLock;
        case ImGuiKey_ScrollLock:    return EKey::ScrollLock;
        case ImGuiKey_NumLock:       return EKey::NumLock;
        case ImGuiKey_PrintScreen:   return EKey::PrintScreen;
        case ImGuiKey_Pause:         return EKey::Pause;
        case ImGuiKey_KeypadDecimal: return EKey::KPDecimal;
        case ImGuiKey_KeypadDivide:  return EKey::KPDivide;
        case ImGuiKey_KeypadMultiply:return EKey::KPMultiply;
        case ImGuiKey_KeypadSubtract:return EKey::KPSubtract;
        case ImGuiKey_KeypadAdd:     return EKey::KPAdd;
        case ImGuiKey_KeypadEnter:   return EKey::KPEnter;
        case ImGuiKey_KeypadEqual:   return EKey::KPEqual;
        default:                     return EKey::Num;
        }
    }

    inline constexpr TArray<ImGuiKey, (size_t)EKey::Num> EKeyToImGuiKeyTable = []
    {
        TArray<ImGuiKey, (size_t)EKey::Num> Table{};
        for (int K = ImGuiKey_NamedKey_BEGIN; K < ImGuiKey_NamedKey_END; ++K)
        {
            const EKey Mapped = ImGuiKeyToEKey(static_cast<ImGuiKey>(K));
            if (Mapped != EKey::Num && Table[(size_t)Mapped] == ImGuiKey_None)
            {
                Table[(size_t)Mapped] = static_cast<ImGuiKey>(K);
            }
        }
        return Table;
    }();

    // ImGuiKey for an engine EKey, or ImGuiKey_None when ImGui has no equivalent.
    constexpr ImGuiKey EKeyToImGuiKey(EKey Key)
    {
        return (size_t)Key < EKeyToImGuiKeyTable.size() ? EKeyToImGuiKeyTable[(size_t)Key] : ImGuiKey_None;
    }

    // The ImGui chord for a keyboard SKey with its modifiers, or ImGuiKey_None for any other device.
    inline ImGuiKeyChord ToKeyChord(const SKey& Key)
    {
        const ImGuiKey ImKey = Key.IsKeyboard() ? EKeyToImGuiKey(Key.Key) : ImGuiKey_None;
        if (ImKey == ImGuiKey_None)
        {
            return ImGuiKey_None;
        }
        return ImKey | (Key.bCtrl ? ImGuiMod_Ctrl : 0) | (Key.bShift ? ImGuiMod_Shift : 0) | (Key.bAlt ? ImGuiMod_Alt : 0);
    }

    // A keyboard SKey pressed through ImGui's shortcut routing, with ExtraMods held on top of its own modifiers.
    inline bool IsShortcutPressed(const SKey& Key, ImGuiKeyChord ExtraMods = 0, ImGuiInputFlags Flags = ImGuiInputFlags_Repeat)
    {
        const ImGuiKeyChord Chord = ToKeyChord(Key);
        if (Chord == ImGuiKey_None)
        {
            return false;
        }
        // Keypad Enter does what Enter does, as it does in every text field.
        if ((Chord & ~ImGuiMod_Mask_) == ImGuiKey_Enter && ImGui::Shortcut(ImGuiKey_KeypadEnter | (Chord & ImGuiMod_Mask_) | ExtraMods, Flags))
        {
            return true;
        }
        return ImGui::Shortcut(Chord | ExtraMods, Flags);
    }

    // First named key pressed this frame, or EKey::Num if none.
    inline EKey PollPressedKey()
    {
        for (int K = ImGuiKey_NamedKey_BEGIN; K < ImGuiKey_NamedKey_END; ++K)
        {
            const ImGuiKey Key = static_cast<ImGuiKey>(K);
            // Skip mouse / gamepad / mod-alias entries, they live in the named-key range too.
            if (Key >= ImGuiKey_MouseLeft && Key <= ImGuiKey_MouseWheelY) continue;
            if (Key >= ImGuiKey_GamepadStart && Key <= ImGuiKey_GamepadRStickDown) continue;
            if (Key >= ImGuiKey_ReservedForModCtrl && Key <= ImGuiKey_ReservedForModSuper) continue;

            if (ImGui::IsKeyPressed(Key, /*repeat*/ false))
            {
                const EKey Mapped = ImGuiKeyToEKey(Key);
                if (Mapped != EKey::Num)
                {
                    return Mapped;
                }
            }
        }
        return EKey::Num;
    }

    // First mouse button clicked this frame, or EMouseKey::Num if none.
    inline EMouseKey PollPressedMouseButton()
    {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left,   false)) return EMouseKey::ButtonLeft;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right,  false)) return EMouseKey::ButtonRight;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle, false)) return EMouseKey::ButtonMiddle;
        return EMouseKey::Num;
    }

    // True for the standalone modifier keys, so a chord capture doesn't double-count the held modifier
    // when the bound key IS that modifier (binding "Ctrl" shouldn't also set bCtrl).
    inline bool IsModifierEKey(EKey K)
    {
        return K == EKey::LeftControl || K == EKey::RightControl
            || K == EKey::LeftShift   || K == EKey::RightShift
            || K == EKey::LeftAlt     || K == EKey::RightAlt
            || K == EKey::LeftSuper   || K == EKey::RightSuper;
    }

    // Whether the modifier K stands for is still down, by either side, which is all a release check needs.
    inline bool IsModifierHeld(EKey K)
    {
        const ImGuiIO& Io = ImGui::GetIO();
        switch (K)
        {
        case EKey::LeftControl: case EKey::RightControl: return Io.KeyCtrl;
        case EKey::LeftShift:   case EKey::RightShift:   return Io.KeyShift;
        case EKey::LeftAlt:     case EKey::RightAlt:     return Io.KeyAlt;
        case EKey::LeftSuper:   case EKey::RightSuper:   return Io.KeySuper;
        default:                                         return false;
        }
    }

    // True when the chord described by Key was pressed this frame, with its modifiers held exactly.
    // Use to drive rebindable editor hotkeys from an SKey setting.
    inline bool IsChordPressed(const SKey& Key)
    {
        if (!Key.IsValid())
        {
            return false;
        }

        // A bound modifier holds its own flag down while pressed, so that flag is expected rather than compared.
        const ImGuiIO& Io = ImGui::GetIO();
        const bool bSelfCtrl  = Key.IsKeyboard() && (Key.Key == EKey::LeftControl || Key.Key == EKey::RightControl);
        const bool bSelfShift = Key.IsKeyboard() && (Key.Key == EKey::LeftShift || Key.Key == EKey::RightShift);
        const bool bSelfAlt   = Key.IsKeyboard() && (Key.Key == EKey::LeftAlt || Key.Key == EKey::RightAlt);
        if ((Key.bCtrl || bSelfCtrl) != Io.KeyCtrl || (Key.bShift || bSelfShift) != Io.KeyShift || (Key.bAlt || bSelfAlt) != Io.KeyAlt)
        {
            return false;
        }

        if (Key.IsMouse())
        {
            switch (Key.MouseButton)
            {
            case EMouseKey::ButtonLeft:   return ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
            case EMouseKey::ButtonRight:  return ImGui::IsMouseClicked(ImGuiMouseButton_Right, false);
            case EMouseKey::ButtonMiddle: return ImGui::IsMouseClicked(ImGuiMouseButton_Middle, false);
            default:                      return false;
            }
        }

        // Find an ImGuiKey pressed this frame that maps to the bound EKey.
        for (int K = ImGuiKey_NamedKey_BEGIN; K < ImGuiKey_NamedKey_END; ++K)
        {
            const ImGuiKey IK = static_cast<ImGuiKey>(K);
            if (IK >= ImGuiKey_MouseLeft && IK <= ImGuiKey_MouseWheelY) continue;
            if (IK >= ImGuiKey_GamepadStart && IK <= ImGuiKey_GamepadRStickDown) continue;
            if (IK >= ImGuiKey_ReservedForModCtrl && IK <= ImGuiKey_ReservedForModSuper) continue;

            if (ImGui::IsKeyPressed(IK, /*repeat*/ false) && ImGuiKeyToEKey(IK) == Key.Key)
            {
                return true;
            }
        }
        return false;
    }
}
