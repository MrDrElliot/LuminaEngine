#pragma once

#include "Core/Math/Math.h"
#include "ExponentialHeightFogComponent.generated.h"

namespace Lumina
{
    // Exponential height fog: density falls off with world height (thick valleys, clear peaks). One medium:
    // the shadowed froxel volume covers the near range (when bVolumetricFog), then the same profile continues
    // analytically to the scene depth and over the sky/horizon. Singleton: one enabled instance/frame.
    REFLECT(Component, Category = "Environment")
    struct RUNTIME_API SExponentialHeightFogComponent
    {
        GENERATED_BODY()

        /** When false, no fog passes run this frame. */
        PROPERTY(Editable, Category = "Fog")
        bool bEnabled = true;

        /** Horizontal distance at FogBaseHeight where fog reaches ~98% opacity. Lower = thicker. */
        PROPERTY(Editable, Category = "Fog", ClampMin = 1.0f, Delta = 10.0f, Units = "m")
        float FogVisibilityDistance = 3000.0f;

        /** How quickly density drops with altitude; larger = thinner aloft, sharper layer. */
        PROPERTY(Editable, Category = "Fog", ClampMin = 0.0f, ClampMax = 4.0f, Delta = 0.001f)
        float FogHeightFalloff = 0.2f;

        /** World-space Y at which FogDensity applies (the base of the fog layer). */
        PROPERTY(Editable, Category = "Fog", Units = "m")
        float FogBaseHeight = 0.0f;

        /** Distance from the camera before fog begins to accumulate. */
        PROPERTY(Editable, Category = "Fog", ClampMin = 0.0f, Units = "m")
        float FogStartDistance = 0.0f;

        /** Upper bound on fog opacity so distant geometry never fully disappears. */
        PROPERTY(Editable, Category = "Fog", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.001f)
        float FogMaxOpacity = 1.0f;

        /** Base fog color (HDR; values >1 feed bloom). */
        PROPERTY(Editable, Color, Category = "Fog")
        FVector3 FogInscatteringColor = FVector3(0.5f, 0.6f, 0.7f);

        /** Fog albedo blended in when looking toward the sun (warm haze glow around the sun). */
        PROPERTY(Editable, Color, Category = "Directional Inscatter")
        FVector3 DirectionalInscatteringColor = FVector3(1.0f, 0.9f, 0.7f);

        /** Tightness of the sun-facing inscatter lobe; larger = smaller, sharper glow. */
        PROPERTY(Editable, Category = "Directional Inscatter", ClampMin = 1.0f, ClampMax = 64.0f)
        float DirectionalInscatteringExponent = 4.0f;

        /** Distance before directional inscatter ramps in. */
        PROPERTY(Editable, Category = "Directional Inscatter", ClampMin = 0.0f, Units = "m")
        float DirectionalInscatteringStartDistance = 0.0f;

        /** When true, the near range gets shadowed volumetric scattering (god rays, light shafts)
        from the froxel volume; beyond VolumetricMaxDistance the fog continues analytically.
        When false, the whole range is analytic (cheap, no shafts). */
        PROPERTY(Editable, Category = "Volumetric")
        bool bVolumetricFog = true;

        /** Brightness multiplier on the fog's in-scattered light (volumetric and analytic alike).
        Decoupled from FogDensity, so raise this to make fog glow without thickening it. */
        PROPERTY(Editable, Category = "Volumetric", ClampMin = 0.0f)
        float VolumetricScatteringIntensity = 3.0f;

        /** Phase asymmetry for sun scattering (0 = isotropic, ~0.6 = forward god rays). Blended
        with an isotropic floor so shafts stay visible side-on, not only sun-facing. */
        PROPERTY(Editable, Category = "Volumetric", ClampMin = -0.95f, ClampMax = 0.95f)
        float VolumetricAnisotropy = 0.6f;

        /** Far plane of the shadowed froxel volume; the analytic fog takes over beyond it.
        Bigger = shafts reach farther but each froxel covers more space (softer detail). */
        PROPERTY(Editable, Category = "Volumetric", ClampMin = 1.0f, Units = "m")
        float VolumetricMaxDistance = 200.0f;

        /** Bounces of higher-order scattering to approximate. 1 = single scattering only (default, and
        free). Raising it stops thick fog reading as flat gray: each extra octave adds a dimmer, more
        isotropic pass that leaks further into shadow, so shadowed fog gains shape and the sun lobe
        softens as density rises. Energy is additive, so more octaves means brighter fog by design --
        trim with VolumetricScatteringIntensity. Costs one phase evaluation per octave per volumetric
        light (no extra shadow taps), so the expensive combination is many octaves and many lights. */
        PROPERTY(Editable, Category = "Multiple Scattering", ClampMin = 1, ClampMax = 4)
        int32 MultiScatterOctaves = 1;

        /** How fast each successive octave dims and flattens toward isotropic. Low = the extra bounces
        barely register; high = they nearly match the direct term, for a bright, diffuse, milky medium. */
        PROPERTY(Editable, Category = "Multiple Scattering", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.01f)
        float MultiScatterFalloff = 0.5f;

        /** How much each successive octave ignores the shadow map. 0 = every octave is shadowed exactly
        like the direct light (extra energy, but shafts stay hard-edged); 1 = later octaves ignore
        shadowing entirely and fill shafts with soft light, as multiply-scattered light really does. */
        PROPERTY(Editable, Category = "Multiple Scattering", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.01f)
        float MultiScatterShadowLeak = 0.5f;
    };
}
