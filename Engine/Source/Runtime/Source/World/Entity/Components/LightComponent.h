#pragma once

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/RenderLayers.h"
#include "LightComponent.generated.h"

namespace Lumina
{
    REFLECT(Component, Category = "Lights")
    struct RUNTIME_API SPointLightComponent
    {
        GENERATED_BODY()

        // RGB color of the light emission.
        PROPERTY(Editable, Color, Category = "Light")
        FVector3 LightColor = FVector3(1.0f);

        // Brightness of the light in lux.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, Units = "lux")
        float Intensity = 10.0f;

        // Radius in meters within which the light affects objects.
        PROPERTY(Editable, Category = "Light", Units = "m")
        float Attenuation = 10.0f;

        // Controls the steepness of the intensity falloff curve toward the attenuation radius.
        PROPERTY(Editable, Category = "Light")
        float Falloff = 0.8f;

        // Roughness floor this light shades with, widening its highlight so it stops sparkling on near-mirror surfaces.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, ClampMax = 1.0f)
        float MinRoughness = 0.0f;

        // Lights the view of any camera whose CullMask shares one of these layers.
        PROPERTY(Editable, Category = "Light")
        SRenderLayerMask RenderLayers;

        // When true, this light contributes to the shadow pass.
        PROPERTY(Editable, Category = "Shadows")
        bool bCastShadows = false;

        // Adds a short screen-space trace so small objects stop floating above what they rest on.
        PROPERTY(Editable, Category = "Shadows")
        bool bContactShadows = false;

        // How far each pixel's trace reaches toward the light. Longer finds taller occluders but takes coarser steps.
        PROPERTY(Editable, Category = "Shadows", ClampMin = 0.01f, ClampMax = 2.55f, Delta = 0.01f, Units = "m")
        float ContactShadowLength = 0.15f;

        // When true, the light scatters through participating media such as fog and atmosphere.
        PROPERTY(Editable, Category = "Advanced")
        bool bVolumetric = false;

        // Strength of the volumetric scattering contribution.
        PROPERTY(Editable, Category = "Advanced")
        float VolumetricIntensity = 0.5f;

        // Fraction of the attenuation radius that softens volumetric in-scatter near the light so it never forms a pixelated hot spot, 0 for a hard inverse square.
        PROPERTY(Editable, Category = "Advanced", ClampMin = 0.0f, ClampMax = 1.0f)
        float VolumetricScatteringRadius = 0.1f;

        // LightFunction-domain material whose Emissive color masks this light, as a gobo or flicker.
        PROPERTY(Editable, Category = "Light Function")
        TStrongObjectPtr<CMaterialInterface> LightFunctionMaterial;
    };

    REFLECT(Component, Category = "Lights")
    struct RUNTIME_API SSpotLightComponent
    {
        GENERATED_BODY()

        // RGB color of the light emission.
        PROPERTY(Editable, Color, Category = "Light")
        FVector3 LightColor = FVector3(1.0f);

        // Brightness of the light in lux.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, ClampMax = 1000.0f, Units = "lux")
        float Intensity = 10.0f;

        // Angle in degrees of the fully lit inner cone, with no falloff inside it.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, Units = "deg")
        float InnerConeAngle = 20.0f;

        // Angle in degrees of the outer cone edge, where light has faded out from the inner cone.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, Units = "deg")
        float OuterConeAngle = 30.0f;

        // Radius in meters within which the spotlight affects objects.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, Units = "m")
        float Attenuation = 10.0f;

        // Controls the steepness of the intensity falloff curve toward the attenuation radius.
        PROPERTY(Editable, Category = "Light")
        float Falloff = 0.8f;

        // Roughness floor this light shades with, widening its highlight so it stops sparkling on near-mirror surfaces.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, ClampMax = 1.0f)
        float MinRoughness = 0.0f;

        // Lights the view of any camera whose CullMask shares one of these layers.
        PROPERTY(Editable, Category = "Light")
        SRenderLayerMask RenderLayers;

        // When true, this light contributes to the shadow pass.
        PROPERTY(Editable, Category = "Shadows")
        bool bCastShadows = false;

        // Adds a short screen-space trace so small objects stop floating above what they rest on.
        PROPERTY(Editable, Category = "Shadows")
        bool bContactShadows = false;

        // How far each pixel's trace reaches toward the light. Longer finds taller occluders but takes coarser steps.
        PROPERTY(Editable, Category = "Shadows", ClampMin = 0.01f, ClampMax = 2.55f, Delta = 0.01f, Units = "m")
        float ContactShadowLength = 0.15f;

        // When true, the light scatters through participating media such as fog and atmosphere.
        PROPERTY(Editable, Category = "Advanced")
        bool bVolumetric = false;

        // Strength of the volumetric scattering contribution.
        PROPERTY(Editable, Category = "Advanced")
        float VolumetricIntensity = 0.5f;

        // Fraction of the attenuation radius that softens volumetric in-scatter near the light so it never forms a pixelated hot spot, 0 for a hard inverse square.
        PROPERTY(Editable, Category = "Advanced", ClampMin = 0.0f, ClampMax = 1.0f)
        float VolumetricScatteringRadius = 0.1f;

        // LightFunction-domain material whose Emissive color masks this light, as a gobo or flicker.
        PROPERTY(Editable, Category = "Light Function")
        TStrongObjectPtr<CMaterialInterface> LightFunctionMaterial;
    };


    REFLECT(Component, Category = "Lights")
    struct RUNTIME_API SAreaLightComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Color, Category = "Light")
        FVector3 LightColor = FVector3(1.0f);

        // Matches a point light of the same value seen from afar, so swapping one for the other keeps the exposure.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, Units = "lux")
        float Intensity = 10.0f;

        // Size of the emitting rectangle along the transform's right axis.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.01f, Units = "m")
        float Width = 1.0f;

        // Size of the emitting rectangle along the transform's up axis.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.01f, Units = "m")
        float Height = 1.0f;

        // How far past the rectangle the light reaches.
        PROPERTY(Editable, Category = "Light", Units = "m")
        float Attenuation = 10.0f;

        PROPERTY(Editable, Category = "Light")
        float Falloff = 0.8f;

        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, ClampMax = 1.0f)
        float MinRoughness = 0.0f;

        // Lights the view of any camera whose CullMask shares one of these layers.
        PROPERTY(Editable, Category = "Light")
        SRenderLayerMask RenderLayers;

        PROPERTY(Editable, Category = "Advanced")
        bool bVolumetric = false;

        PROPERTY(Editable, Category = "Advanced")
        float VolumetricIntensity = 0.5f;
    };

    REFLECT(Component, Category = "Lights")
    struct RUNTIME_API SDirectionalLightComponent
    {
        GENERATED_BODY()

        // RGB color of the directional light. Multiplied by the temperature tint when bUseTemperature is set.
        PROPERTY(Editable, Color, Category = "Light")
        FVector3 Color = FVector3(1.0f, 0.92f, 0.82f);

        // World-space direction from the surface toward the sun, so the default (0, 0.3, 0.8) sits above the horizon.
        PROPERTY(Editable, Category = "Light")
        FVector3 Direction = FVector3(0.0f, 0.3f, 0.8f);

        // Brightness multiplier of the directional light.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f)
        float Intensity = 2.4f;

        // When true, Color is tinted by a physical black-body color from Temperature.
        PROPERTY(Editable, Category = "Light|Temperature")
        bool bUseTemperature = false;

        // Correlated color temperature in Kelvin where about 6500 is neutral daylight, lower is warmer and higher is bluer.
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

        // When true, this light contributes to the shadow pass.
        PROPERTY(Editable, Category = "Shadows")
        bool bCastShadows = true;

        // Farthest view distance that receives the sun's shadow pages. Past it only terrain and distance field shadows remain.
        PROPERTY(Editable, Category = "Shadows", ClampMin = 1.0f, Units = "m")
        float ShadowMaxDistance = 200.0f;

        // How far toward the sun a caster can stand and still shadow what the camera sees; a low sun needs more or tall casters clip and shadows go hollow.
        PROPERTY(Editable, Category = "Shadows", ClampMin = 1.0f, Units = "m")
        float ShadowCasterReach = 100.0f;

        // Each step coarsens the shadow texel by two against the pixel it shades, so negative is sharper and costs more pages.
        PROPERTY(Editable, Category = "Shadows|Resolution", ClampMin = -2.0f, ClampMax = 3.0f, Delta = 0.1f)
        float ShadowResolutionBias = 0.0f;

        // Shadow texels a caster's simplified silhouette may stray, so higher draws coarser meshes into the pages.
        PROPERTY(Editable, Category = "Shadows|Resolution", ClampMin = 0.25f, ClampMax = 32.0f, Delta = 0.25f)
        float ShadowLODTexels = 6.0f;

        // Physical pages held for the sun, each 128 by 128 texels in a static and a dynamic layer, so 2048 costs 256 MB.
        PROPERTY(Editable, Category = "Shadows|Resolution", ClampMin = 256, ClampMax = 4096)
        int32 ShadowPoolPages = 2048;

        // Normal-offset bias scale; raise to kill shadow acne, lower if contact shadows detach (peter-panning).
        PROPERTY(Editable, Category = "Shadows|Tuning", ClampMin = 0.0f, ClampMax = 8.0f, Delta = 0.05f)
        float ShadowNormalBias = 1.0f;

        // Constant depth bias in meters added at the shadow comparison; small values only.
        PROPERTY(Editable, Category = "Shadows|Tuning", ClampMin = 0.0f, ClampMax = 0.5f, Delta = 0.001f, Units = "m")
        float ShadowDepthBias = 0.0f;

        // Scales the filter width the renderer's Shadow Quality sets, so one sun can be softer or harder. Zero is a hard edge.
        PROPERTY(Editable, Category = "Shadows|Tuning", ClampMin = 0.0f, ClampMax = 8.0f, Delta = 0.05f)
        float ShadowBlur = 1.0f;

        // Fraction of the shadowed range over which shadows fade to fully lit, so the edge does not pop at max distance.
        PROPERTY(Editable, Category = "Shadows|Tuning", ClampMin = 0.0f, ClampMax = 0.5f, Delta = 0.01f)
        float ShadowDistanceFade = 0.1f;

        // Drops casters whose bounds cover fewer than this many shadow texels (0 is off). Thin geometry loses its shadow first.
        PROPERTY(Editable, Category = "Shadows|Culling", ClampMin = 0.0f, ClampMax = 16.0f, Delta = 0.1f)
        float ShadowMinTexels = 0.0f;

        // Adds a short screen-space march toward the sun so small props stop floating where a shadow texel is coarser than the gap. Off at Low shadow quality.
        PROPERTY(Editable, Category = "Shadows|Contact")
        bool bContactShadows = true;

        // How far each pixel's march reaches toward the sun. Longer finds taller occluders but takes coarser steps.
        PROPERTY(Editable, Category = "Shadows|Contact", ClampMin = 0.01f, ClampMax = 2.0f, Delta = 0.01f, Units = "m")
        float ContactShadowLength = 0.3f;

        // Keeps still casters in their shadow pages until something under them changes, redrawing only what moves, sways or is skinned each frame.
        PROPERTY(Editable, Category = "Shadows|Caching")
        bool bCacheStaticShadows = true;

        // Lets terrain shade everything behind it, at any distance, by marching each pixel's ray over the heightmap.
        PROPERTY(Editable, Category = "Far Shadows")
        bool bTerrainShadows = true;

        // How far each pixel's ray looks for terrain in the way.
        PROPERTY(Editable, Category = "Far Shadows", ClampMin = 1.0f, Units = "m")
        float TerrainShadowDistance = 6000.0f;

        // Meshes with a distance field keep casting past the shadowed range, traced per pixel against their fields.
        PROPERTY(Editable, Category = "Far Shadows")
        bool bDistanceFieldShadows = true;

        // View distance the distance-field shadows reach before fading out.
        PROPERTY(Editable, Category = "Far Shadows", ClampMin = 1.0f, Units = "m")
        float DistanceFieldShadowDistance = 600.0f;

        // Angular diameter of the sun in degrees (the real sun is 0.53), which widens its shadows' penumbra with the distance to their caster.
        PROPERTY(Editable, Category = "Light", ClampMin = 0.0f, ClampMax = 10.0f, Delta = 0.01f)
        float SourceAngle = 0.5357f;

        // When true, the sun scatters through participating media as god rays.
        PROPERTY(Editable, Category = "Advanced")
        bool bVolumetric = true;

        // Strength of the volumetric scattering contribution.
        PROPERTY(Editable, Category = "Advanced")
        float VolumetricIntensity = 1.0f;

        // LightFunction-domain material whose Emissive color masks the sun, tiled across the world as cloud shadows.
        PROPERTY(Editable, Category = "Light Function")
        TStrongObjectPtr<CMaterialInterface> LightFunctionMaterial;

        // World size in meters of one repeat of the light function, measured across the plane facing the sun.
        PROPERTY(Editable, Category = "Light Function", ClampMin = 0.01f, Units = "m")
        float LightFunctionScale = 50.0f;
    };
}
