#pragma once

#include "Core/Math/Math.h"

namespace Breakout
{
    using namespace Lumina;

    // Mirrors FQuadInstance in Shaders.h; Slang lays a device-address struct out with C rules.
    struct FQuadInstance
    {
        FVector2 Center       { 0.0f, 0.0f };
        FVector2 HalfSize     { 0.0f, 0.0f };
        FVector4 Color        { 1.0f, 1.0f, 1.0f, 1.0f };
        FVector4 Accent       { 1.0f, 1.0f, 1.0f, 1.0f };
        float    Rotation     = 0.0f;
        float    CornerRadius = 0.3f;
        float    Glow         = 0.0f;
        float    Param0       = 0.0f;
        uint32   Kind         = 0;
        float    Param1       = 0.0f;
        float    Param2       = 0.0f;
        float    Param3       = 0.0f;
    };

    static_assert(sizeof(FQuadInstance) == 80, "The Slang mirror assumes a packed 80 byte instance.");
}
