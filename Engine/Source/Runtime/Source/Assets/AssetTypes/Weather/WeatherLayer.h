#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "WeatherParams.h"
#include "WeatherLayer.generated.h"

namespace Lumina
{
    REFLECT(ToolTip = "One aspect of a weather preset, such as rain, clouds or wind.")
    class RUNTIME_API CWeatherLayer : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        virtual void ApplyTo(FWeatherParams& Params) const {}
    };

    REFLECT(ToolTip = "Falling rain, its splashes, and the wetness, puddles and ripples it leaves on every surface.")
    class RUNTIME_API CRainWeatherLayer : public CWeatherLayer
    {
        GENERATED_BODY()

    public:

        void ApplyTo(FWeatherParams& Params) const override;

        PROPERTY(Editable, Category = "Rain", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "How hard it rains. Scales drop count, splashes, haze and how fast surfaces soak.")
        float Intensity = 0.7f;

        PROPERTY(Editable, Category = "Rain|Drops", ClampMin = 0.0f, ClampMax = 40.0f, Delta = 0.05f, ToolTip = "Visible drops per cubic meter at full intensity.")
        float DropDensity = 2.5f;

        PROPERTY(Editable, Category = "Rain|Drops", ClampMin = 0.2f, ClampMax = 8.0f, Units = "mm", ToolTip = "Average drop diameter. Larger drops fall faster and draw wider streaks.")
        float DropSize = 3.0f;

        PROPERTY(Editable, Category = "Rain|Drops", ClampMin = 0.0f, ClampMax = 1.0f)
        float SizeVariation = 0.5f;

        PROPERTY(Editable, Category = "Rain|Drops", ClampMin = 0.5f, ClampMax = 30.0f, ToolTip = "Fall speed in meters per second of an average drop.")
        float FallSpeed = 9.0f;

        PROPERTY(Editable, Category = "Rain|Drops", ClampMin = 0.0f, ClampMax = 2.0f, ToolTip = "How strongly the weather's wind blows the rain sideways.")
        float WindInfluence = 1.0f;

        PROPERTY(Editable, Category = "Rain|Drops", ClampMin = 0.0f, ClampMax = 5.0f, ToolTip = "Random sideways sway in meters per second, so drops do not fall in lockstep.")
        float Turbulence = 0.6f;

        PROPERTY(Editable, Category = "Rain|Drops", ClampMin = 5.0f, ClampMax = 60.0f, Units = "m", ToolTip = "Radius around the camera where individual drops are drawn.")
        float Radius = 25.0f;

        PROPERTY(Editable, Category = "Rain|Look", ClampMin = 0.0f, ClampMax = 4.0f, ToolTip = "Scales the motion streak each drop draws.")
        float StreakLength = 1.5f;

        PROPERTY(Editable, Category = "Rain|Look", ClampMin = 0.0f, ClampMax = 8.0f)
        float Brightness = 1.0f;

        PROPERTY(Editable, Category = "Rain|Look", ClampMin = 0.0f, ClampMax = 1.0f)
        float Opacity = 1.0f;

        PROPERTY(Editable, Category = "Rain|Look", Color)
        FVector3 Tint = FVector3(1.0f);

        PROPERTY(Editable, Category = "Rain|Look", ClampMin = 0.0f, ClampMax = 4.0f, ToolTip = "Veil of distant rain beyond the drawn drops.")
        float HazeDensity = 0.5f;

        PROPERTY(Editable, Category = "Rain|Splashes", ClampMin = 0.0f, ClampMax = 4.0f)
        float SplashDensity = 1.0f;

        PROPERTY(Editable, Category = "Rain|Splashes", ClampMin = 0.1f, ClampMax = 4.0f)
        float SplashSize = 1.0f;

        PROPERTY(Editable, Category = "Rain|Surfaces", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "Wetness surfaces reach after soaking in this rain.")
        float MaxWetness = 1.0f;

        PROPERTY(Editable, Category = "Rain|Surfaces", ClampMin = 0.0f, ClampMax = 2.0f, ToolTip = "Wetness gained per second at full intensity.")
        float WettingRate = 0.08f;

        PROPERTY(Editable, Category = "Rain|Surfaces", ClampMin = 0.0f, ClampMax = 2.0f, ToolTip = "Wetness lost per second once the rain stops.")
        float DryingRate = 0.01f;

        PROPERTY(Editable, Category = "Rain|Surfaces", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "Share of flat ground that collects standing water when fully soaked.")
        float PuddleCoverage = 0.4f;

        PROPERTY(Editable, Category = "Rain|Surfaces", ClampMin = 0.25f, ClampMax = 50.0f, Units = "m", ToolTip = "World size of the puddle pattern.")
        float PuddleScale = 4.0f;

        PROPERTY(Editable, Category = "Rain|Surfaces", ClampMin = 0.0f, ClampMax = 2.0f, ToolTip = "Strength of the impact rings on puddles and wet flat ground.")
        float RippleIntensity = 1.0f;

        PROPERTY(Editable, Category = "Rain|Surfaces", ClampMin = 0.0f, ClampMax = 2.0f, ToolTip = "Strength of the water running down walls and other steep surfaces.")
        float DripIntensity = 1.0f;
    };

    REFLECT(ToolTip = "Falling snow, the cover it lays on every open surface, and the prints feet leave in it.")
    class RUNTIME_API CSnowWeatherLayer : public CWeatherLayer
    {
        GENERATED_BODY()

    public:

        void ApplyTo(FWeatherParams& Params) const override;

        PROPERTY(Editable, Category = "Snow", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "How hard it snows. Scales the flakes and how fast the cover builds.")
        float Intensity = 0.7f;

        PROPERTY(Editable, Category = "Snow|Flakes", ClampMin = 0.0f, ClampMax = 20.0f, Delta = 0.05f, ToolTip = "Visible flakes per cubic meter at full intensity.")
        float FlakeDensity = 5.0f;

        PROPERTY(Editable, Category = "Snow|Flakes", ClampMin = 1.0f, ClampMax = 40.0f, Units = "mm", ToolTip = "Average flake width.")
        float FlakeSize = 16.0f;

        PROPERTY(Editable, Category = "Snow|Flakes", ClampMin = 0.0f, ClampMax = 1.0f)
        float SizeVariation = 0.6f;

        PROPERTY(Editable, Category = "Snow|Flakes", ClampMin = 0.2f, ClampMax = 5.0f, ToolTip = "Fall speed in meters per second of an average flake.")
        float FallSpeed = 1.2f;

        PROPERTY(Editable, Category = "Snow|Flakes", ClampMin = 0.0f, ClampMax = 3.0f, Units = "m", ToolTip = "How far flakes swirl from their path as they tumble.")
        float Flutter = 0.5f;

        PROPERTY(Editable, Category = "Snow|Flakes", ClampMin = 0.0f, ClampMax = 3.0f, ToolTip = "How strongly the weather's wind carries the flakes, more than rain since they fall slowly.")
        float WindInfluence = 1.2f;

        PROPERTY(Editable, Category = "Snow|Flakes", ClampMin = 5.0f, ClampMax = 60.0f, Units = "m", ToolTip = "Radius around the camera where individual flakes are drawn.")
        float Radius = 20.0f;

        PROPERTY(Editable, Category = "Snow|Look", ClampMin = 0.0f, ClampMax = 8.0f)
        float Brightness = 1.5f;

        PROPERTY(Editable, Category = "Snow|Look", ClampMin = 0.0f, ClampMax = 1.0f)
        float Opacity = 0.9f;

        PROPERTY(Editable, Category = "Snow|Cover", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "Cover the ground reaches after snowing long enough.")
        float MaxCover = 1.0f;

        PROPERTY(Editable, Category = "Snow|Cover", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "Cover gained per second at full intensity.")
        float AccumulationRate = 0.015f;

        PROPERTY(Editable, Category = "Snow|Cover", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "Cover lost per second once the snow stops.")
        float MeltRate = 0.004f;

        PROPERTY(Editable, Category = "Snow|Cover", ClampMin = 0.25f, ClampMax = 50.0f, Units = "m", ToolTip = "World size of the drifts that break thin cover into patches.")
        float CoverScale = 3.0f;

        PROPERTY(Editable, Category = "Snow|Cover", ClampMin = 0.0f, ClampMax = 2.0f, ToolTip = "How deep footprints and trails read in the cover.")
        float PrintDepth = 1.0f;
    };

    REFLECT(ToolTip = "Drives the scene's cloud layer: coverage, thickness and how much a storm dims the light.")
    class RUNTIME_API CCloudWeatherLayer : public CWeatherLayer
    {
        GENERATED_BODY()

    public:

        void ApplyTo(FWeatherParams& Params) const override;

        PROPERTY(Editable, Category = "Clouds", ClampMin = 0.0f, ClampMax = 1.0f)
        float Coverage = 0.8f;

        PROPERTY(Editable, Category = "Clouds", ClampMin = 0.0f, ClampMax = 8.0f, Delta = 0.01f)
        float Density = 1.6f;

        PROPERTY(Editable, Category = "Clouds", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "0 keeps flat stratus, 1 gives tall billowing cumulus.")
        float Billow = 0.4f;

        PROPERTY(Editable, Category = "Clouds", ClampMin = 0.0f, ClampMax = 1.0f)
        float DetailStrength = 0.35f;

        PROPERTY(Editable, Category = "Clouds|Lighting", ClampMin = 0.0f, ClampMax = 2.0f, ToolTip = "Scales the sunlit brightness of the clouds, below 1 for a gloomy storm.")
        float SunScale = 0.6f;

        PROPERTY(Editable, Category = "Clouds|Lighting", ClampMin = 0.0f, ClampMax = 2.0f)
        float AmbientScale = 0.8f;
    };

    REFLECT(ToolTip = "Wind that tilts the rain, drifts the clouds and gusts over time.")
    class RUNTIME_API CWindWeatherLayer : public CWeatherLayer
    {
        GENERATED_BODY()

    public:

        void ApplyTo(FWeatherParams& Params) const override;

        PROPERTY(Editable, Category = "Wind", ClampMin = -360.0f, ClampMax = 360.0f, ToolTip = "Compass heading the wind blows toward, in degrees around the up axis.")
        float Heading = 30.0f;

        PROPERTY(Editable, Category = "Wind", ClampMin = 0.0f, ClampMax = 60.0f, ToolTip = "Average ground wind speed in meters per second.")
        float Speed = 4.0f;

        PROPERTY(Editable, Category = "Wind", ClampMin = 0.0f, ClampMax = 1.0f, ToolTip = "How far gusts swing above and below the average speed.")
        float Gustiness = 0.4f;

        PROPERTY(Editable, Category = "Wind", ClampMin = 0.01f, ClampMax = 2.0f, ToolTip = "Gusts per second.")
        float GustFrequency = 0.15f;

        PROPERTY(Editable, Category = "Wind", ClampMin = 0.0f, ClampMax = 20.0f, ToolTip = "Cloud drift speed as a multiple of the ground wind, since wind aloft is faster.")
        float CloudSpeedScale = 4.0f;
    };
}
