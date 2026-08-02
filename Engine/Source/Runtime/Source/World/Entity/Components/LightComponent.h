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

        /** When true, this light contributes to the shadow pass. */
        PROPERTY(Editable, Category = "Shadows")
        bool bCastShadows = false;

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

        /** When true, this light contributes to the shadow pass. */
        PROPERTY(Editable, Category = "Shadows")
        bool bCastShadows = false;

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
        FVector3 Color = FVector4(1.0f);

        /** Normalized world-space direction pointing FROM the surface TOWARD the sun (the to-light vector). The default (0, 0.3, 0.8) places the sun above the horizon. */
        PROPERTY(Editable, Category = "Light")
        FVector3 Direction = FVector3(0.0f, 0.3f, 0.8f);

        /** Brightness multiplier of the directional light. */
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f)
        float Intensity = 1.5f;

        /** When true, Color is tinted by a physical black-body color from Temperature. */
        PROPERTY(Editable, Category = "Light|Temperature")
        bool bUseTemperature = false;

        /** Correlated color temperature in Kelvin (≈6500 = neutral daylight, lower = warmer, higher = cooler/blue). */
        PROPERTY(Editable, Category = "Light|Temperature", ClampMin = 1000.0f, ClampMax = 15000.0f, Units = "K")
        float Temperature = 6500.0f;

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

        /** Penumbra softness (PCSS light size); 0 = hard edges, larger = softer distant shadows. Above
        ~0.01 an elevated caster's penumbra saturates the far-cascade filter cap and stipples/goes blocky. */
        PROPERTY(Editable, Category = "Cascaded Shadows|Tuning", ClampMin = 0.0f, ClampMax = 0.5f, Delta = 0.001f)
        float ShadowSoftness = 0.005f;

        /** PCF taps per cascade sample; higher = smoother penumbra (less dither grain) at higher GPU cost. */
        PROPERTY(Editable, Category = "Cascaded Shadows|Tuning", ClampMin = 1, ClampMax = 64)
        int32 ShadowSampleCount = 8;

        /** Fraction of each cascade over which it cross-fades into the next; reduces visible split seams. */
        PROPERTY(Editable, Category = "Cascaded Shadows|Tuning", ClampMin = 0.0f, ClampMax = 0.5f, Delta = 0.01f)
        float CascadeBlend = 0.20f;

        /** Fraction of the last cascade over which shadows fade to fully lit, so the edge doesn't pop at max distance. */
        PROPERTY(Editable, Category = "Cascaded Shadows|Tuning", ClampMin = 0.0f, ClampMax = 0.5f, Delta = 0.01f)
        float ShadowDistanceFade = 0.15f;

        /** When true, the sun scatters through participating media (god rays / light shafts). */
        PROPERTY(Editable, Category = "Advanced")
        bool bVolumetric = false;

        /** Strength of the volumetric scattering contribution. */
        PROPERTY(Editable, Category = "Advanced")
        float VolumetricIntensity = 1.0f;
    };
}
