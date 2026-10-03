#pragma once

#include "Core/Math/Math.h"
#include "LightComponent.generated.h"

namespace Lumina
{
    REFLECT(Component, Category = "Lights")
    struct RUNTIME_API SPointLightComponent
    {
        GENERATED_BODY()

        /** RGB color of the light emission. */
        PROPERTY(Editable, Color, Category = "Light")
        FVector3 LightColor = FVector3(1.0f);

        /** Brightness of the light in lux. */
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, Units = "lux")
        float Intensity = 10.0f;

        /** Radius in meters within which the light affects objects. */
        PROPERTY(Editable, Category = "Light", Units = "m")
        float Attenuation = 10.0f;

        /** Controls the steepness of the intensity falloff curve toward the attenuation radius. */
        PROPERTY(Editable, Category = "Light")
        float Falloff = 0.8f;

        /** Roughness floor this light shades with, widening its highlight so it stops sparkling on near-mirror surfaces. */
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, ClampMax = 1.0f)
        float MinRoughness = 0.0f;

        /** When true, this light contributes to the shadow pass. */
        PROPERTY(Editable, Category = "Shadows")
        bool bCastShadows = false;

        /** Adds a short screen-space trace so small objects stop floating above what they rest on. */
        PROPERTY(Editable, Category = "Shadows")
        bool bContactShadows = false;

        /** When true, the light scatters through participating media (fog/atmosphere). */
        PROPERTY(Editable, Category = "Advanced")
        bool bVolumetric = false;

        /** Strength of the volumetric scattering contribution. */
        PROPERTY(Editable, Category = "Advanced")
        float VolumetricIntensity = 0.5f;

        /** Soft source radius (fraction of the attenuation radius) for volumetric scattering; spreads the
            near-light in-scatter so it doesn't concentrate into a pixelated hot spot. 0 = hard 1/d^2. */
        PROPERTY(Editable, Category = "Advanced", ClampMin = 0.0f, ClampMax = 1.0f)
        float VolumetricScatteringRadius = 0.1f;
    };

    REFLECT(Component, Category = "Lights")
    struct RUNTIME_API SSpotLightComponent
    {
        GENERATED_BODY()

        /** RGB color of the light emission. */
        PROPERTY(Editable, Color, Category = "Light")
        FVector3 LightColor = FVector3(1.0f);

        /** Brightness of the light in lux. */
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, ClampMax = 1000.0f, Units = "lux")
        float Intensity = 10.0f;

        /** Angle (degrees) of the fully-lit inner cone, no falloff inside this region. */
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, Units = "deg")
        float InnerConeAngle = 20.0f;

        /** Angle (degrees) of the outer cone edge, light fades from inner to outer. */
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, Units = "deg")
        float OuterConeAngle = 30.0f;

        /** Radius in meters within which the spotlight affects objects. */
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, Units = "m")
        float Attenuation = 10.0f;

        /** Controls the steepness of the intensity falloff curve toward the attenuation radius. */
        PROPERTY(Editable, Category = "Light")
        float Falloff = 0.8f;

        /** Roughness floor this light shades with, widening its highlight so it stops sparkling on near-mirror surfaces. */
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, ClampMax = 1.0f)
        float MinRoughness = 0.0f;

        /** When true, this light contributes to the shadow pass. */
        PROPERTY(Editable, Category = "Shadows")
        bool bCastShadows = false;

        /** Adds a short screen-space trace so small objects stop floating above what they rest on. */
        PROPERTY(Editable, Category = "Shadows")
        bool bContactShadows = false;

        /** Depth bias to prevent shadow acne on receiving surfaces. */
        PROPERTY(Editable, Category = "Shadows")
        float ShadowBias = 0.005f;

        /** World-space radius used to build the shadow map frustum. */
        PROPERTY(Editable, Category = "Shadows")
        float ShadowRadius = 1.0f;

        /** When true, the light scatters through participating media (fog/atmosphere). */
        PROPERTY(Editable, Category = "Advanced")
        bool bVolumetric = false;

        /** Strength of the volumetric scattering contribution. */
        PROPERTY(Editable, Category = "Advanced")
        float VolumetricIntensity = 0.5f;

        /** Soft source radius (fraction of the attenuation radius) for volumetric scattering; spreads the
            near-light in-scatter so it doesn't concentrate into a pixelated hot spot. 0 = hard 1/d^2. */
        PROPERTY(Editable, Category = "Advanced", ClampMin = 0.0f, ClampMax = 1.0f)
        float VolumetricScatteringRadius = 0.1f;

        /** Index into the shadow map atlas, managed by the renderer. */
        int32 ShadowMapIndex = -1;
    };


    REFLECT(Component, Category = "Lights")
    struct RUNTIME_API SDirectionalLightComponent
    {
        GENERATED_BODY()

        /** RGB color of the directional light. Multiplied by the temperature tint when bUseTemperature is set. */
        PROPERTY(Editable, Color, Category = "Light")
        FVector3 Color = FVector3(1.0f, 0.92f, 0.82f);

        /** Normalized world-space direction pointing FROM the surface TOWARD the sun (the to-light vector). The default (0, 0.3, 0.8) places the sun above the horizon. */
        PROPERTY(Editable, Category = "Light")
        FVector3 Direction = FVector3(0.0f, 0.3f, 0.8f);

        /** Brightness multiplier of the directional light. */
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f)
        float Intensity = 2.4f;

        /** When true, Color is tinted by a physical black-body color from Temperature. */
        PROPERTY(Editable, Category = "Light|Temperature")
        bool bUseTemperature = false;

        /** Correlated color temperature in Kelvin (≈6500 = neutral daylight, lower = warmer, higher = cooler/blue). */
        PROPERTY(Editable, Category = "Light|Temperature", ClampMin = 1000.0f, ClampMax = 15000.0f, Units = "K")
        float Temperature = 6500.0f;

        // Below the horizon the light turns to the moon opposite the sun, while the sky keeps the true sun for its night.
        PROPERTY(Editable, Category = "Light|Moon")
        bool bMoonlight = false;

        PROPERTY(Editable, Category = "Light|Moon", ClampMin = 0.0f)
        float MoonIntensity = 0.3f;

        PROPERTY(Editable, Color, Category = "Light|Moon")
        FVector3 MoonColor = FVector3(0.55f, 0.65f, 1.0f);

        bool IsMoonLit() const
        {
            return bMoonlight && Math::Normalize(Direction).y < 0.0f;
        }

        // The direction surfaces are lit and shadowed from, which is the moon once the sun has set.
        FVector3 GetLightingDirection() const
        {
            const FVector3 Sun = Math::Normalize(Direction);
            return IsMoonLit() ? -Sun : Sun;
        }

        // Fades to nothing at the horizon from either side, so the swap between sun and moon never pops.
        float GetLightingIntensity() const
        {
            if (!bMoonlight)
            {
                return Intensity;
            }

            // A low moon crosses far more atmosphere than a low sun needs to fade out over, so it brightens more slowly.
            constexpr float SunFadeElevation  = 0.1f;
            constexpr float MoonFadeElevation = 0.35f;
            const float Elevation = Math::Normalize(Direction).y;
            return Elevation >= 0.0f
                ? Intensity * Math::SmoothStep(0.0f, SunFadeElevation, Elevation)
                : MoonIntensity * Math::SmoothStep(0.0f, MoonFadeElevation, -Elevation);
        }

        /** When true, this light contributes to the shadow pass. */
        PROPERTY(Editable, Category = "Cascaded Shadows")
        bool bCastShadows = true;

        // Blend uniform (0) to logarithmic (1) cascade split distribution. Higher packs near-camera detail
        // but shrinks cascade 0; ~0.85 pushes the first split outward for a longer high-quality range.
        PROPERTY(Editable, Category = "Cascaded Shadows", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.01f)
        float CascadeSplitLambda = 0.85f;

        /** Maximum view distance that receives cascaded shadows; shadows fade out before this. Lower values
        concentrate the 4 cascades over less ground for sharper shadows; raise it for long-vista scenes. */
        PROPERTY(Editable, Category = "Cascaded Shadows", ClampMin = 1.0f, Units = "m")
        float ShadowMaxDistance = 500.0f;

        /** Distance the light eye is pushed behind each cascade so off-screen occluders still cast.
        Low sun angles need larger values or tall casters clip at the ortho near plane and shadows go hollow. */
        PROPERTY(Editable, Category = "Cascaded Shadows", ClampMin = 1.0f, Units = "m")
        float CascadeBackDistance = 100.0f;

        /** Normal-offset bias scale; raise to kill shadow acne, lower if contact shadows detach (peter-panning). */
        PROPERTY(Editable, Category = "Cascaded Shadows|Tuning", ClampMin = 0.0f, ClampMax = 8.0f, Delta = 0.05f)
        float ShadowNormalBias = 1.0f;

        /** Constant depth bias added at the shadow comparison; small values only. */
        PROPERTY(Editable, Category = "Cascaded Shadows|Tuning", ClampMin = 0.0f, ClampMax = 0.01f, Delta = 0.0001f)
        float ShadowDepthBias = 0.0f;

        // Scales the filter width the renderer's Shadow Quality sets, so one sun can be softer or harder. Zero is a hard edge.
        PROPERTY(Editable, Category = "Cascaded Shadows|Tuning", ClampMin = 0.0f, ClampMax = 8.0f, Delta = 0.05f)
        float ShadowBlur = 1.0f;

        /** Fraction of each cascade that cross-fades into the next. 0 is off; pixels in the band sample two cascades. */
        PROPERTY(Editable, Category = "Cascaded Shadows|Tuning", ClampMin = 0.0f, ClampMax = 0.5f, Delta = 0.01f)
        float CascadeBlend = 0.0f;

        /** Fraction of the last cascade over which shadows fade to fully lit, so the edge doesn't pop at max distance. */
        PROPERTY(Editable, Category = "Cascaded Shadows|Tuning", ClampMin = 0.0f, ClampMax = 0.5f, Delta = 0.01f)
        float ShadowDistanceFade = 0.15f;

        /** Drop casters whose bounds cover fewer than this many texels of the cascade doing the rejecting
        (0 = off). Trades shadow detail for geometry throughput. Thin geometry (railings, grates, wires)
        loses its shadow entirely above 0, since its bounds are sub-texel in the coarser cascades. */
        PROPERTY(Editable, Category = "Cascaded Shadows|Culling", ClampMin = 0.0f, ClampMax = 16.0f, Delta = 0.1f)
        float CascadeMinTexels = 8.0f;

        /** Reject casters hidden behind other casters, tested against last frame's cascade Hi-Z. The test
        is one frame stale and has no late re-test, so a caster exposed this frame casts a frame late. */
        PROPERTY(Editable, Category = "Cascaded Shadows|Culling")
        bool bCascadeOcclusionCull = true;

        /** When true, the sun scatters through participating media (god rays / light shafts). */
        PROPERTY(Editable, Category = "Advanced")
        bool bVolumetric = true;

        /** Strength of the volumetric scattering contribution. */
        PROPERTY(Editable, Category = "Advanced")
        float VolumetricIntensity = 1.0f;
    };
}
