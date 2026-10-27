#pragma once

#include "Assets/AssetTypes/Weather/WeatherPreset.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "WeatherComponent.generated.h"

namespace Lumina
{
    // Plays a weather preset over the whole world. Singleton per world; with none present no weather work runs.
    REFLECT(Component, Category = "Environment")
    struct RUNTIME_API SWeatherComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Weather")
        bool bEnabled = true;

        PROPERTY(Editable, Category = "Weather", ToolTip = "The weather to play. Swapping it blends to the new one over the transition time.")
        TStrongObjectPtr<CWeatherPreset> Preset;

        PROPERTY(Editable, Category = "Weather", ClampMin = 0.0f, ClampMax = 2.0f, ToolTip = "Scales the preset's rain, for driving a storm up and down from gameplay.")
        float Intensity = 1.0f;

        PROPERTY(Editable, Category = "Weather", ClampMin = 0.0f, ClampMax = 600.0f, ToolTip = "Seconds to blend from one preset to the next.")
        float TransitionTime = 8.0f;

        PROPERTY(Editable, Category = "Weather|Surfaces", ToolTip = "Holds surface wetness at a fixed value instead of soaking and drying over time.")
        bool bOverrideWetness = false;

        PROPERTY(Editable, Category = "Weather|Surfaces", ClampMin = 0.0f, ClampMax = 1.0f, EditCondition = "bOverrideWetness")
        float Wetness = 1.0f;

        PROPERTY(Editable, Category = "Weather|Surfaces", ToolTip = "Holds the snow cover at a fixed value instead of building and melting over time.")
        bool bOverrideSnowCover = false;

        PROPERTY(Editable, Category = "Weather|Surfaces", ClampMin = 0.0f, ClampMax = 1.0f, EditCondition = "bOverrideSnowCover")
        float SnowCover = 1.0f;
    };
}
