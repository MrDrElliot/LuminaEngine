#pragma once
#include <Containers/String.h>
#include "Core/Math/Math.h"

namespace Lumina
{
    struct FWindowSpecs
    {
        FString Title = "Lumina";
        FUIntVector2 Extent{};
        bool bFullscreen = false;
        bool bShowTitlebar = false;
    };

    enum class ECursorMode
    {
        Normal,     // visible, free
        Hidden,     // hidden, free
        Disabled,   // hidden, locked/captured
    };
}
