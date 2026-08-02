#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina::Input
{
    enum class EKeyState : uint8
    {
        Up       = 0,
        Pressed  = 1, // Just pressed this frame
        Held     = 2, // Held across frames
        Repeated = 3, // OS key repeat
        Released = 4, // Just released this frame
    };

    enum class EMouseState : uint8
    {
        Up       = 0,
        Pressed  = 1, // Just pressed this frame
        Held     = 2, // Held across frames
        Released = 3, // Just released this frame
    };

    enum class EInputMode : uint8
    {
        Normal = 0,
        Hidden,
        Disabled
    };

    enum class EInputDevice : uint8
    {
        Keyboard = 0,
        Mouse,
        Gamepad,
        Touch
    };
}