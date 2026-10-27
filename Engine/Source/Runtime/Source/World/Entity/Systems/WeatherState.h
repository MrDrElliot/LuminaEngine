#pragma once

#include "Assets/AssetTypes/Weather/WeatherParams.h"

namespace Lumina
{
    // Resolved weather for this frame, kept in the world's registry context for the renderer to read.
    struct FWeatherState
    {
        bool           bActive = false;
        FWeatherParams Params;

        // Current wind including gusts, in meters per second on the XZ plane.
        FVector2       Wind = FVector2(0.0f);

        // Integrated rather than speed times time, so a change of wind speed never jumps the clouds.
        FVector2       CloudWindOffset = FVector2(0.0f);

        float          Wetness = 0.0f;
        float          PuddleLevel = 0.0f;
        float          SnowCover = 0.0f;

        // Kept in double and wrapped by the renderer, so the drop field neither loses precision nor jumps.
        double         Time = 0.0;
        double         FallDistance = 0.0;
        double         SnowFallDistance = 0.0;
        double         RainWindX = 0.0;
        double         RainWindZ = 0.0;
    };
}
