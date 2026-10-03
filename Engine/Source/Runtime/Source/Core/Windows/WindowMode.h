#pragma once

#include "Core/Object/ObjectMacros.h"
#include "WindowMode.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EWindowMode : uint8
    {
        // A decorated, movable window at the chosen size.
        Windowed,

        // An undecorated window covering the whole monitor at its desktop resolution. Alt-tab is instant.
        BorderlessFullscreen,

        // Takes the monitor at the chosen resolution and refresh rate, which may switch the display mode.
        Fullscreen,
    };

    REFLECT()
    struct RUNTIME_API SDisplayMode
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Width = 0;

        PROPERTY()
        int32 Height = 0;

        PROPERTY()
        int32 RefreshRate = 0;
    };
}
