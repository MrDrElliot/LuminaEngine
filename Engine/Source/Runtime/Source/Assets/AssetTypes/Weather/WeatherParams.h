#pragma once

#include "Core/Math/Math.h"

namespace Lumina
{
    struct SEnvironmentComponent;

    struct FRainParams
    {
        float    Intensity       = 0.0f;
        float    DropDensity     = 2.5f;
        float    DropSize        = 3.0f;
        float    SizeVariation   = 0.5f;
        float    FallSpeed       = 9.0f;
        float    WindInfluence   = 1.0f;
        float    Turbulence      = 0.6f;
        float    StreakLength    = 1.5f;
        float    Brightness      = 1.0f;
        float    Opacity         = 1.0f;
        float    Radius          = 25.0f;
        float    SplashDensity   = 1.0f;
        float    SplashSize      = 1.0f;
        float    WettingRate     = 0.08f;
        float    DryingRate      = 0.01f;
        float    MaxWetness      = 1.0f;
        float    PuddleCoverage  = 0.4f;
        float    PuddleScale     = 4.0f;
        float    RippleIntensity = 1.0f;
        float    DripIntensity   = 1.0f;
        float    HazeDensity     = 0.5f;
        FVector3 Tint            = FVector3(1.0f);
        FVector3 SecondaryTint   = FVector3(1.0f);
        float    ColorVariation  = 0.0f;
        FVector3 EmissiveColor   = FVector3(1.0f);
        float    EmissiveIntensity = 0.0f;
        float    Refraction      = 1.0f;
        float    Reflection      = 1.0f;
        float    Glint           = 1.0f;
        float    CurtainDensity  = 1.0f;
        float    SplashMist      = 1.0f;
        float    EdgeDrips       = 1.0f;

        static FRainParams Lerp(const FRainParams& A, const FRainParams& B, float T);
    };

    struct FSnowParams
    {
        float Intensity        = 0.0f;
        float FlakeDensity     = 5.0f;
        float FlakeSize        = 16.0f;
        float SizeVariation    = 0.6f;
        float FallSpeed        = 1.2f;
        float Flutter          = 0.5f;
        float WindInfluence    = 1.2f;
        float Brightness       = 1.5f;
        float Opacity          = 0.9f;
        float Radius           = 20.0f;
        float AccumulationRate = 0.015f;
        float MeltRate         = 0.004f;
        float MaxCover         = 1.0f;
        float CoverScale       = 3.0f;
        float PrintDepth       = 1.0f;

        static FSnowParams Lerp(const FSnowParams& A, const FSnowParams& B, float T);
    };

    struct FCloudWeatherParams
    {
        // How much these replace the cloud component's own values, so a preset without clouds leaves them alone.
        float Weight         = 0.0f;
        float Coverage       = 0.42f;
        float Density        = 1.3f;
        float Billow         = 0.8f;
        float DetailStrength = 0.4f;
        float SunScale       = 1.0f;
        float AmbientScale   = 1.0f;

        static FCloudWeatherParams Lerp(const FCloudWeatherParams& A, const FCloudWeatherParams& B, float T);
    };

    struct FEnvironmentWeatherParams
    {
        // How much these pull the environment toward the storm look, so a preset without the layer leaves it alone.
        float    Weight         = 0.0f;
        float    SunlightScale  = 0.45f;
        float    SunDiscScale   = 0.2f;
        float    SkyBrightness  = 0.55f;
        float    AerosolDensity = 3.0f;
        FVector3 SunTint        = FVector3(0.85f, 0.9f, 1.0f);
        float    StarVisibility = 0.0f;

        // Pulls Env toward these values by Weight, the way the renderer sees it this frame.
        void ApplyTo(SEnvironmentComponent& Env) const;

        static FEnvironmentWeatherParams Lerp(const FEnvironmentWeatherParams& A, const FEnvironmentWeatherParams& B, float T);
    };

    struct FWindParams
    {
        FVector2 Direction       = FVector2(1.0f, 0.0f);
        float    Speed           = 0.0f;
        float    Gustiness       = 0.0f;
        float    GustFrequency   = 0.15f;
        float    CloudSpeedScale = 4.0f;

        static FWindParams Lerp(const FWindParams& A, const FWindParams& B, float T);
    };

    struct FWeatherParams
    {
        FRainParams         Rain;
        FSnowParams         Snow;
        FCloudWeatherParams Clouds;
        FEnvironmentWeatherParams Environment;
        FWindParams         Wind;

        static FWeatherParams Lerp(const FWeatherParams& A, const FWeatherParams& B, float T);
    };
}
