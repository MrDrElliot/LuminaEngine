#pragma once

#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"

#ifndef VERIFY_SSBO_ALIGNMENT
#define VERIFY_SSBO_ALIGNMENT(Type) \
    static_assert(sizeof(Type) % 16 == 0, #Type " must be 16-byte aligned");
#endif

namespace Lumina
{
    // Sky mode constants. Must match SKY_MODE_* in Environment.slang.
    constexpr uint32 GSkyMode_SolidColor = 0u;
    constexpr uint32 GSkyMode_Gradient   = 1u;
    constexpr uint32 GSkyMode_Dynamic    = 2u;
    // Visible sky reads the SkyCube (filled by the HDRI in the Equirect->Cube pass). HDRI mode with
    // no EnvironmentMap bound falls back to a black sky (documented in SEnvironmentComponent).
    constexpr uint32 GSkyMode_HDRI       = 3u;

    // Resolved IBL bake resolution, mapped from SEnvironmentComponent::IBLQuality during extraction.
    // The render thread recreates the sky/irradiance/prefilter cubes whenever this changes; comparison
    // gates that (rare, WaitIdle-guarded) rebuild. The default is the cheap (Low) baseline so scenes
    // without an environment (e.g. thumbnail scenes) stay small; the active env's tier bumps it on the
    // first rendered frame.
    struct FIBLBakeResolution
    {
        uint32 SkyCube    = 256u;   // sky-cube face size (IBL source + procedural sky)
        uint32 Prefilter  = 128u;   // specular prefilter base face size
        uint32 Mips       = 5u;     // specular prefilter mip count (roughness levels)
        uint32 Irradiance = 32u;    // diffuse irradiance face size

        bool operator==(const FIBLBakeResolution& O) const
        {
            return SkyCube == O.SkyCube && Prefilter == O.Prefilter && Mips == O.Mips && Irradiance == O.Irradiance;
        }
        bool operator!=(const FIBLBakeResolution& O) const { return !(*this == O); }
    };

    // CPU mirror of the per-frame environment CB; layout must match FEnvironmentParams in Environment.slang.
    // EnvironmentPass uploads one per frame; the shader switches on Misc.x (sky mode).
    struct alignas(16) FEnvironmentParams
    {
        FVector4   SolidSkyColor    = FVector4(0.45f, 0.65f, 1.0f, 0.0f); // rgb=color, w unused
        FVector4   ZenithColor      = FVector4(0.05f, 0.1f, 0.4f, 0.7f);  // rgb=color, w=horizonExponent
        FVector4   HorizonColor     = FVector4(0.6f, 0.8f, 1.0f, 0.0f);   // rgb=color, w unused
        FVector4   GroundColor      = FVector4(0.2f, 0.18f, 0.15f, 0.0f); // rgb=color, w unused
        FVector4   SunTint          = FVector4(1.0f, 1.0f, 1.0f, 20.0f);  // rgb=tint, w=sunIntensity
        // x=skyMode (uint cast to float), y=sunDiscScale, z=skyExposure, w=mieAnisotropy
        FVector4   Misc             = FVector4(2.0f, 1.0f, 1.5f, 0.76f);

        // Procedural night additions (Dynamic mode only)
        // rgb = night zenith tint (deep blue/purple), w = brightness scalar
        FVector4   NightSkyColor    = FVector4(0.012f, 0.018f, 0.04f, 0.4f);
        // x=density, y=brightness, z=twinkleSpeed, w=size
        FVector4   StarParams       = FVector4(0.55f, 1.0f, 2.5f, 0.5f);
        // x=size (multiples of 0.5deg), y=glowSize, z=brightness, w=autoOpposeSun (>=0.5 = auto)
        FVector4   MoonParams       = FVector4(3.0f, 0.4f, 0.6f, 1.0f);
        // xyz = manual moon direction (used when MoonParams.w < 0.5), w unused
        FVector4   MoonDirection    = FVector4(0.0f, -1.0f, 0.0f, 0.0f);
        // x = milky-way band intensity, y = band tilt (radians), z/w reserved
        FVector4   GalaxyParams     = FVector4(0.06f, 0.45f, 0.0f, 0.0f);
    };
    VERIFY_SSBO_ALIGNMENT(FEnvironmentParams);

    // CPU mirror of the per-frame exponential-height-fog CB; layout must match Includes/Fog.slang.
    // Read by the froxel inject (near-range shadowed scattering) and the fog apply pass (froxel
    // composite + analytic far fog/sky).
    struct alignas(16) FExponentialHeightFogParams
    {
        // rgb = fog albedo, w = fog density at base height
        FVector4   InscatteringColor = FVector4(0.5f, 0.6f, 0.7f, 0.02f);
        // x = height falloff, y = base height (world Y), z = start distance, w = max opacity
        FVector4   HeightParams      = FVector4(0.2f, 0.0f, 0.0f, 1.0f);
        // rgb = directional (sun) inscatter tint, w = directional exponent
        FVector4   DirectionalColor  = FVector4(1.0f, 0.9f, 0.7f, 4.0f);
        // x = volumetric scattering intensity, y = volumetric anisotropy,
        // z = froxel volume far plane, w = directional inscatter start distance
        FVector4   VolumetricParams  = FVector4(1.0f, 0.6f, 200.0f, 0.0f);
    };
    VERIFY_SSBO_ALIGNMENT(FExponentialHeightFogParams);
}
