#pragma once

#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    struct SEnvironmentComponent;
}

namespace Lumina::SkyAtmosphere
{
    // Fraction of each channel that crosses the air from Altitude meters above the ground to space, zero once the planet is in the way.
    RUNTIME_API FVector3 Transmittance(float CosZenith, float Altitude, float Aerosol);

    // Sunlight as the ground sees it against a noon sun, so the authored light color is the overhead one.
    RUNTIME_API FVector3 SunlightTint(float CosZenith, float Altitude, float Aerosol);

    // Where the sky draws the moon, mirroring Sky_MoonDirection.
    RUNTIME_API FVector3 MoonDirection(const SEnvironmentComponent& Env, const FVector3& SunDirection);

    // The lunar phase law, the moon's brightness against a full moon, which falls far faster than its lit area.
    RUNTIME_API float MoonPhaseBrightness(const SEnvironmentComponent& Env);
}
