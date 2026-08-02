#pragma once

#include "Input/Key.h"

namespace Lumina
{
    // The kind of discrete input that occurred. Mirrors the raw event stream, collapsed to the
    // categories a gameplay script cares about.
    enum class EInputEventType : uint8
    {
        KeyDown,      // a keyboard key went down (bRepeat set for OS auto-repeat)
        KeyUp,        // a keyboard key was released
        MouseDown,    // a mouse button went down
        MouseUp,      // a mouse button was released
        MouseMove,    // the cursor moved (DeltaX/DeltaY carry the motion)
        MouseScroll,  // the wheel turned (Scroll carries the signed delta)
    };

    inline const char* InputEventTypeToString(EInputEventType Type)
    {
        switch (Type)
        {
        case EInputEventType::KeyDown:     return "KeyDown";
        case EInputEventType::KeyUp:       return "KeyUp";
        case EInputEventType::MouseDown:   return "MouseDown";
        case EInputEventType::MouseUp:     return "MouseUp";
        case EInputEventType::MouseMove:   return "MouseMove";
        case EInputEventType::MouseScroll: return "MouseScroll";
        }
        return "Unknown";
    }

    // A single discrete input event delivered to scripts via OnInput(event), so gameplay can react
    // to input as it happens instead of polling SInputComponent each frame. One of these is queued
    // per raw key/mouse event for the frame; Key carries the key/button plus modifier chord for the
    // Key*/Mouse* button types and is unbound for move/scroll.
    struct SInputEvent
    {
        EInputEventType Type = EInputEventType::KeyDown;
        SKey   Key;
        bool   bRepeat = false;
        double MouseX  = 0.0;
        double MouseY  = 0.0;
        double DeltaX  = 0.0;
        double DeltaY  = 0.0;
        double Scroll  = 0.0;
    };
}
