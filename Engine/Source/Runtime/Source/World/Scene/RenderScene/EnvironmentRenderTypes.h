#pragma once

#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"

#ifndef VERIFY_SSBO_ALIGNMENT
#define VERIFY_SSBO_ALIGNMENT(Type) \
    static_assert(sizeof(Type) % 16 == 0, #Type " must be 16-byte aligned")
#endif

namespace Lumina
{
    // Sky mode constants. Must match SKY_MODE_* in Environment.slang.
    constexpr uint32 GSkyMode_SolidColor = 0u;
    constexpr uint32 GSkyMode_Gradient   = 1u;
    constexpr uint32 GSkyMode_Dynamic    = 2u;
    constexpr uint32 GSkyMode_HDRI       = 3u;
    // Not a mode. The SPEC_SKY_MODE default, meaning the shader branches on Misc.x at runtime.
    constexpr uint32 GSkyMode_Runtime    = 4u;

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

    struct alignas(16) FEnvironmentParams
    {
        FVector4   SolidSkyColor    = FVector4(0.45f, 0.65f, 1.0f, 0.0f); // rgb=color, w unused
        FVector4   ZenithColor      = FVector4(0.05f, 0.1f, 0.4f, 0.7f);  // rgb=color, w=horizonExponent
        FVector4   HorizonColor     = FVector4(0.6f, 0.8f, 1.0f, 0.0f);   // rgb=color, w unused
        FVector4   GroundColor      = FVector4(0.2f, 0.18f, 0.15f, 0.0f); // rgb=color, w unused
        FVector4   SunTint          = FVector4(1.0f, 1.0f, 1.0f, 20.0f);  // rgb=tint, w=sunIntensity
        // x=skyMode (uint cast to float), y=sunDiscScale, z=skyExposure, w=mieAnisotropy
        FVector4   Misc             = FVector4(2.0f, 1.0f, 1.5f, 0.76f);

        FVector4   NightSkyColor    = FVector4(0.012f, 0.018f, 0.04f, 0.4f);
        // x=density, y=brightness, z=twinkleSpeed, w=size
        FVector4   StarParams       = FVector4(0.55f, 1.0f, 2.5f, 0.5f);
        // x=size (multiples of 0.5deg), y=glowSize, z=brightness, w=autoOpposeSun (>=0.5 = auto)
        FVector4   MoonParams       = FVector4(3.0f, 0.4f, 0.6f, 1.0f);
        // xyz = manual moon direction (used when MoonParams.w < 0.5), w unused
        FVector4   MoonDirection    = FVector4(0.0f, -1.0f, 0.0f, 0.0f);
        // x = milky-way band intensity, y = band tilt (radians), z/w reserved
        FVector4   GalaxyParams     = FVector4(0.06f, 0.45f, 0.0f, 0.0f);

        FVector4   HDRIParams       = FVector4(1.0f, 1.0f, 0.0f, 0.0f);
    };
    VERIFY_SSBO_ALIGNMENT(FEnvironmentParams);

    struct alignas(16) FExponentialHeightFogParams
    {
        // rgb = fog albedo, w = fog density at base height
        FVector4   InscatteringColor = FVector4(0.5f, 0.6f, 0.7f, 0.02f);
        // x = height falloff, y = base height (world Y), z = start distance, w = max opacity
        FVector4   HeightParams      = FVector4(0.2f, 0.0f, 0.0f, 1.0f);
        // rgb = directional (sun) inscatter tint, w = directional exponent
        FVector4   DirectionalColor  = FVector4(1.0f, 0.9f, 0.7f, 4.0f);
        FVector4   VolumetricParams  = FVector4(1.0f, 0.6f, 200.0f, 0.0f);
        // Multiple-scattering octaves as count, scattering, shadow and phase attenuation, where x = 1 is a bit-exact no-op default.
        FVector4   MultiScatterParams = FVector4(1.0f, 0.5f, 0.5f, 0.5f);
        // x = noise strength (0 disables), y = inverse world scale, z = octave count, w = per-octave gain
        FVector4   NoiseParams        = FVector4(0.0f, 1.0f / 40.0f, 3.0f, 0.5f);
        // xyz = wind velocity in world units per second, w = distance the noise has faded out by
        FVector4   NoiseWind          = FVector4(0.0f, 0.0f, 0.0f, 200.0f);
    };
    VERIFY_SSBO_ALIGNMENT(FExponentialHeightFogParams);

    // One artist-placed density volume. WorldToVolume maps world space into the unit cube or sphere.
    struct alignas(16) FGPUFogVolume
    {
        FMatrix4 WorldToVolume = FMatrix4(1.0f);
        // rgb = albedo, w = extinction added at full strength
        FVector4 Albedo        = FVector4(0.5f, 0.6f, 0.7f, 0.02f);
        // rgb = emissive in-scatter, w unused
        FVector4 Emissive      = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
        // x = shape (0 box, 1 sphere), y = edge softness, z = scatter intensity, w unused
        FVector4 Params        = FVector4(0.0f, 0.2f, 1.0f, 0.0f);
    };
    VERIFY_SSBO_ALIGNMENT(FGPUFogVolume);

    // Mirrors FOG_MAX_VOLUMES in Fog.slang.
    inline constexpr uint32 GFogMaxVolumes = 32;

    // Mirrors FGroundFogLayer in GroundFog.slang. The two rows map world space to the layer's area, inside while both stay within one.
    struct alignas(16) FGPUGroundFogLayer
    {
        FVector4 WorldToLayerX = FVector4(0.0f);
        FVector4 WorldToLayerZ = FVector4(0.0f);
        // x = ground height, y = thickness, z = top softness as a fraction of the thickness, w = extinction
        FVector4 Shape         = FVector4(0.0f);
        // rgb = albedo, w = scattering intensity
        FVector4 Albedo        = FVector4(0.0f);
        // xy = edge fade in layer units along x and z, z = fade-out distance from the camera, w = anisotropy
        FVector4 Edge          = FVector4(0.0f);
        // x = wisp strength, y = inverse wisp scale, zw = distance the drift has carried the wisps along x and z
        FVector4 Wisps         = FVector4(0.0f);
    };
    VERIFY_SSBO_ALIGNMENT(FGPUGroundFogLayer);

    // Mirrors FGroundFogSim in GroundFog.slang, one per frame for every simulation pass.
    struct alignas(16) FGPUGroundFogSim
    {
        // Window's minimum corner along x and z, its texel size and its resolution.
        FVector2    Origin       = FVector2(0.0f);
        float       TexelWorld   = 0.1f;
        uint32      Resolution   = 0;
        // Texels the window moved since the state was written, read through by the first pass.
        FIntVector2 Shift        = FIntVector2(0);
        float       Dt           = 0.0f;
        uint32      bIngest      = 0;
        // Obstacle map coordinates as u = x * U.x + z * U.y + U.z, the same for v.
        FVector4    ObstacleU    = FVector4(0.0f);
        FVector4    ObstacleV    = FVector4(0.0f);
        // Height of an obstacle depth as Y = Depth * x + y, and the vertical band the fog fills.
        FVector2    DepthToY     = FVector2(0.0f);
        float       BandBottom   = 0.0f;
        float       BandTop      = 0.0f;
        FVector2    Wind         = FVector2(0.0f);
        // Fractions of the flow and of the wisp offset that survive one step.
        float       VelocityKeep = 1.0f;
        float       SwirlKeep    = 1.0f;
        // Fraction of a parted path that fills back in per step.
        float       RefillRate   = 0.0f;
        float       Displacement = 0.0f;
        float       Vorticity    = 0.0f;
        float       MaxSpeed     = 0.0f;
    };
    VERIFY_SSBO_ALIGNMENT(FGPUGroundFogSim);

    // Mirrors GROUND_FOG_MAX_LAYERS in GroundFog.slang.
    inline constexpr uint32 GGroundFogMaxLayers = 8;
}
