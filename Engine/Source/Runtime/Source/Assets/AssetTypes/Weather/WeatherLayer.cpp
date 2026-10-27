#include "RuntimePCH.h"
#include "WeatherLayer.h"

namespace Lumina
{
    namespace
    {
        float LerpF(float A, float B, float T) { return A + (B - A) * T; }
    }

    FRainParams FRainParams::Lerp(const FRainParams& A, const FRainParams& B, float T)
    {
        FRainParams R;
        R.Intensity       = LerpF(A.Intensity, B.Intensity, T);
        R.DropDensity     = LerpF(A.DropDensity, B.DropDensity, T);
        R.DropSize        = LerpF(A.DropSize, B.DropSize, T);
        R.SizeVariation   = LerpF(A.SizeVariation, B.SizeVariation, T);
        R.FallSpeed       = LerpF(A.FallSpeed, B.FallSpeed, T);
        R.WindInfluence   = LerpF(A.WindInfluence, B.WindInfluence, T);
        R.Turbulence      = LerpF(A.Turbulence, B.Turbulence, T);
        R.StreakLength    = LerpF(A.StreakLength, B.StreakLength, T);
        R.Brightness      = LerpF(A.Brightness, B.Brightness, T);
        R.Opacity         = LerpF(A.Opacity, B.Opacity, T);
        R.Radius          = LerpF(A.Radius, B.Radius, T);
        R.SplashDensity   = LerpF(A.SplashDensity, B.SplashDensity, T);
        R.SplashSize      = LerpF(A.SplashSize, B.SplashSize, T);
        R.WettingRate     = LerpF(A.WettingRate, B.WettingRate, T);
        R.DryingRate      = LerpF(A.DryingRate, B.DryingRate, T);
        R.MaxWetness      = LerpF(A.MaxWetness, B.MaxWetness, T);
        R.PuddleCoverage  = LerpF(A.PuddleCoverage, B.PuddleCoverage, T);
        R.PuddleScale     = LerpF(A.PuddleScale, B.PuddleScale, T);
        R.RippleIntensity = LerpF(A.RippleIntensity, B.RippleIntensity, T);
        R.DripIntensity   = LerpF(A.DripIntensity, B.DripIntensity, T);
        R.HazeDensity     = LerpF(A.HazeDensity, B.HazeDensity, T);
        R.Tint            = A.Tint + (B.Tint - A.Tint) * T;
        return R;
    }

    FSnowParams FSnowParams::Lerp(const FSnowParams& A, const FSnowParams& B, float T)
    {
        FSnowParams R;
        R.Intensity        = LerpF(A.Intensity, B.Intensity, T);
        R.FlakeDensity     = LerpF(A.FlakeDensity, B.FlakeDensity, T);
        R.FlakeSize        = LerpF(A.FlakeSize, B.FlakeSize, T);
        R.SizeVariation    = LerpF(A.SizeVariation, B.SizeVariation, T);
        R.FallSpeed        = LerpF(A.FallSpeed, B.FallSpeed, T);
        R.Flutter          = LerpF(A.Flutter, B.Flutter, T);
        R.WindInfluence    = LerpF(A.WindInfluence, B.WindInfluence, T);
        R.Brightness       = LerpF(A.Brightness, B.Brightness, T);
        R.Opacity          = LerpF(A.Opacity, B.Opacity, T);
        R.Radius           = LerpF(A.Radius, B.Radius, T);
        R.AccumulationRate = LerpF(A.AccumulationRate, B.AccumulationRate, T);
        R.MeltRate         = LerpF(A.MeltRate, B.MeltRate, T);
        R.MaxCover         = LerpF(A.MaxCover, B.MaxCover, T);
        R.CoverScale       = LerpF(A.CoverScale, B.CoverScale, T);
        R.PrintDepth       = LerpF(A.PrintDepth, B.PrintDepth, T);
        return R;
    }

    FCloudWeatherParams FCloudWeatherParams::Lerp(const FCloudWeatherParams& A, const FCloudWeatherParams& B, float T)
    {
        FCloudWeatherParams R;
        R.Weight         = LerpF(A.Weight, B.Weight, T);
        R.Coverage       = LerpF(A.Coverage, B.Coverage, T);
        R.Density        = LerpF(A.Density, B.Density, T);
        R.Billow         = LerpF(A.Billow, B.Billow, T);
        R.DetailStrength = LerpF(A.DetailStrength, B.DetailStrength, T);
        R.SunScale       = LerpF(A.SunScale, B.SunScale, T);
        R.AmbientScale   = LerpF(A.AmbientScale, B.AmbientScale, T);
        return R;
    }

    FWindParams FWindParams::Lerp(const FWindParams& A, const FWindParams& B, float T)
    {
        FWindParams R;
        const FVector2 Direction = A.Direction + (B.Direction - A.Direction) * T;
        const float    Length    = Math::Length(Direction);
        R.Direction       = Length > 1e-4f ? Direction / Length : B.Direction;
        R.Speed           = LerpF(A.Speed, B.Speed, T);
        R.Gustiness       = LerpF(A.Gustiness, B.Gustiness, T);
        R.GustFrequency   = LerpF(A.GustFrequency, B.GustFrequency, T);
        R.CloudSpeedScale = LerpF(A.CloudSpeedScale, B.CloudSpeedScale, T);
        return R;
    }

    FWeatherParams FWeatherParams::Lerp(const FWeatherParams& A, const FWeatherParams& B, float T)
    {
        FWeatherParams R;
        R.Rain   = FRainParams::Lerp(A.Rain, B.Rain, T);
        R.Snow   = FSnowParams::Lerp(A.Snow, B.Snow, T);
        R.Clouds = FCloudWeatherParams::Lerp(A.Clouds, B.Clouds, T);
        R.Wind   = FWindParams::Lerp(A.Wind, B.Wind, T);
        return R;
    }

    void CRainWeatherLayer::ApplyTo(FWeatherParams& Params) const
    {
        FRainParams& R = Params.Rain;
        R.Intensity       = Intensity;
        R.DropDensity     = DropDensity;
        R.DropSize        = DropSize;
        R.SizeVariation   = SizeVariation;
        R.FallSpeed       = FallSpeed;
        R.WindInfluence   = WindInfluence;
        R.Turbulence      = Turbulence;
        R.StreakLength    = StreakLength;
        R.Brightness      = Brightness;
        R.Opacity         = Opacity;
        R.Radius          = Radius;
        R.SplashDensity   = SplashDensity;
        R.SplashSize      = SplashSize;
        R.WettingRate     = WettingRate;
        R.DryingRate      = DryingRate;
        R.MaxWetness      = MaxWetness;
        R.PuddleCoverage  = PuddleCoverage;
        R.PuddleScale     = PuddleScale;
        R.RippleIntensity = RippleIntensity;
        R.DripIntensity   = DripIntensity;
        R.HazeDensity     = HazeDensity;
        R.Tint            = Tint;
    }

    void CCloudWeatherLayer::ApplyTo(FWeatherParams& Params) const
    {
        FCloudWeatherParams& C = Params.Clouds;
        C.Weight         = 1.0f;
        C.Coverage       = Coverage;
        C.Density        = Density;
        C.Billow         = Billow;
        C.DetailStrength = DetailStrength;
        C.SunScale       = SunScale;
        C.AmbientScale   = AmbientScale;
    }

    void CWindWeatherLayer::ApplyTo(FWeatherParams& Params) const
    {
        FWindParams& W = Params.Wind;
        const float Radians = Math::Radians(Heading);
        W.Direction       = FVector2(Math::Cos(Radians), Math::Sin(Radians));
        W.Speed           = Speed;
        W.Gustiness       = Gustiness;
        W.GustFrequency   = GustFrequency;
        W.CloudSpeedScale = CloudSpeedScale;
    }

    void CSnowWeatherLayer::ApplyTo(FWeatherParams& Params) const
    {
        FSnowParams& S = Params.Snow;
        S.Intensity        = Intensity;
        S.FlakeDensity     = FlakeDensity;
        S.FlakeSize        = FlakeSize;
        S.SizeVariation    = SizeVariation;
        S.FallSpeed        = FallSpeed;
        S.Flutter          = Flutter;
        S.WindInfluence    = WindInfluence;
        S.Brightness       = Brightness;
        S.Opacity          = Opacity;
        S.Radius           = Radius;
        S.AccumulationRate = AccumulationRate;
        S.MeltRate         = MeltRate;
        S.MaxCover         = MaxCover;
        S.CoverScale       = CoverScale;
        S.PrintDepth       = PrintDepth;
    }
}
