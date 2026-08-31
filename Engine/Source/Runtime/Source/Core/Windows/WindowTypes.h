#pragma once
#include <Containers/String.h>
#include "Core/Math/Math.h"

namespace Lumina
{
    struct FWindowSpecs
    {
        FString Title = "Lumina";
        FUIntVector2 Extent{};              // zero sizes the window off the primary monitor
        bool bFullscreen = false;
        bool bShowTitlebar = true;
    };

    enum class ECursorMode
    {
        Normal,     // visible, free
        Hidden,     // hidden, free
        Disabled,   // hidden, locked/captured
    };
}
