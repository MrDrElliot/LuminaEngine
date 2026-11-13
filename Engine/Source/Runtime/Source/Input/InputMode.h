#pragma once

#include "Core/Object/ObjectMacros.h"
#include "InputMode.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EInputMode : uint8
    {
        Game,
        UI,
        GameAndUI,
    };

    // What the mouse cursor means right now, which the project's hardware cursors give an image or the system draws itself.
    REFLECT()
    enum class EMouseCursor : uint8
    {
        None,
        Default,
        TextEditBeam,
        ResizeLeftRight,
        ResizeUpDown,
        ResizeSouthEast,
        ResizeSouthWest,
        CardinalCross,
        Crosshairs,
        Hand,
        GrabHand,
        GrabHandClosed,
        SlashedCircle,
        EyeDropper,
    };

    inline constexpr int32 MouseCursorTypeCount = static_cast<int32>(EMouseCursor::EyeDropper) + 1;
}
