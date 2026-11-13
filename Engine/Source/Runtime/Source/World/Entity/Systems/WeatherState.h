#pragma once

#include "Assets/AssetTypes/Weather/WeatherParams.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Systems/SystemResources.h"
#include "WeatherState.generated.h"

namespace Lumina
{
    // Resolved weather for this frame, kept in the world's registry context for the renderer to read.
    REFLECT()
    struct RUNTIME_API SWeatherState : ECS::FResource
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        bool           bActive = false;

        FWeatherParams Params;

        // Current wind including gusts, in meters per second on the XZ plane.
        PROPERTY(Hidden)
        FVector2       Wind = FVector2(0.0f);

        // Integrated rather than speed times time, so a change of wind speed never jumps the clouds.
        PROPERTY(Hidden)
        FVector2       CloudWindOffset = FVector2(0.0f);

        PROPERTY(Hidden)
        float          Wetness = 0.0f;

        PROPERTY(Hidden)
        float          PuddleLevel = 0.0f;

        PROPERTY(Hidden)
        float          SnowCover = 0.0f;

        // Kept in double and wrapped by the renderer, so the drop field neither loses precision nor jumps.
        PROPERTY(Hidden)
        double         Time = 0.0;

        PROPERTY(Hidden)
        double         FallDistance = 0.0;

        PROPERTY(Hidden)
        double         SnowFallDistance = 0.0;

        PROPERTY(Hidden)
        double         RainWindX = 0.0;

        PROPERTY(Hidden)
        double         RainWindZ = 0.0;
    };
}
