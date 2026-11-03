#pragma once

#include "Core/LuminaMacros.h"

#if USING(WITH_EDITOR)

#include "Core/Math/Math.h"

namespace Lumina
{
    // Set each frame by an editor viewport in its world's registry context, and drawn by the renderer on the ground plane.
    struct FEditorGridRequest
    {
        FVector4 MinorColor   = {};
        FVector4 MajorColor   = {};
        float    Spacing      = 0.0f;
        float    FadeDistance = 0.0f;
        float    LineWidth    = 0.0f;
        float    AxisWidth    = 0.0f;
        bool     bEnabled     = false;
    };
}

#endif
