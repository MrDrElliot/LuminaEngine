#pragma once

#include "Core/Math/Math.h"
#include "CloudComponent.generated.h"

namespace Lumina
{
    // Raymarched through a shell around the same planet the atmosphere uses. Singleton per frame.
    REFLECT(Component, Category = "Environment")
    struct RUNTIME_API SCloudComponent
    {
        GENERATED_BODY()

        /** When false, the cloud pass does not run. */
        PROPERTY(Editable, Category = "Clouds")
        bool bEnabled = true;

        /** Fraction of sky covered. Low values give scattered puffs, high gives overcast. */
        PROPERTY(Editable, Category = "Clouds", ClampMin = 0.0f, ClampMax = 1.0f)
        float Coverage = 0.45f;

        /** Optical density of the cloud medium; higher is thicker and darker inside. */
        PROPERTY(Editable, Category = "Clouds", ClampMin = 0.0f, Delta = 0.01f)
        float Density = 1.0f;

        /** Altitude of the cloud layer's underside. */
        PROPERTY(Editable, Category = "Clouds|Layer", ClampMin = 100.0f, Units = "m")
        float LayerBottom = 1500.0f;

        /** Altitude of the cloud layer's top. Thicker layers give taller, more billowed shapes. */
        PROPERTY(Editable, Category = "Clouds|Layer", ClampMin = 200.0f, Units = "m")
        float LayerTop = 4000.0f;

        /** World size of one tile of the base shape noise; larger means bigger cloud masses. */
        PROPERTY(Editable, Category = "Clouds|Shape", ClampMin = 100.0f, Units = "m")
        float ShapeScale = 6000.0f;

        /** World size of one tile of the erosion noise that carves the wispy edges. */
        PROPERTY(Editable, Category = "Clouds|Shape", ClampMin = 10.0f, Units = "m")
        float DetailScale = 700.0f;

        /** How aggressively the detail noise erodes the base shape's edges. */
        PROPERTY(Editable, Category = "Clouds|Shape", ClampMin = 0.0f, ClampMax = 1.0f)
        float DetailStrength = 0.35f;

        /** Vertical bias of the shape: 0 keeps flat stratus, 1 gives tall billowing cumulus. */
        PROPERTY(Editable, Category = "Clouds|Shape", ClampMin = 0.0f, ClampMax = 1.0f)
        float Billow = 0.6f;

        /** Direction the layer drifts, in the XZ plane. */
        PROPERTY(Editable, Category = "Clouds|Wind")
        FVector2 WindDirection = FVector2(1.0f, 0.2f);

        /** Drift speed of the whole layer. */
        PROPERTY(Editable, Category = "Clouds|Wind", ClampMin = 0.0f, Units = "m")
        float WindSpeed = 25.0f;

        /** How much faster the eroding detail scrolls than the base shape, which reads as churn. */
        PROPERTY(Editable, Category = "Clouds|Wind", ClampMin = 0.0f, ClampMax = 8.0f)
        float DetailWindFactor = 2.0f;

        /** Brightness of directly sunlit cloud. */
        PROPERTY(Editable, Category = "Clouds|Lighting", ClampMin = 0.0f)
        float SunIntensity = 8.0f;

        /** Skylight fill inside shadowed cloud, so undersides read blue-grey rather than black. */
        PROPERTY(Editable, Category = "Clouds|Lighting", ClampMin = 0.0f)
        float AmbientIntensity = 1.0f;

        /** Forward-scatter asymmetry; higher concentrates the silver lining toward the sun. */
        PROPERTY(Editable, Category = "Clouds|Lighting", ClampMin = 0.0f, ClampMax = 0.95f)
        float ForwardScattering = 0.8f;

        /** Back-scatter asymmetry, blended against the forward lobe to keep away-from-sun cloud lit. */
        PROPERTY(Editable, Category = "Clouds|Lighting", ClampMin = -0.95f, ClampMax = 0.0f)
        float BackScattering = -0.15f;

        /** Darkening of thin sunward edges that makes cloud read as solid rather than as fog. */
        PROPERTY(Editable, Category = "Clouds|Lighting", ClampMin = 0.0f, ClampMax = 1.0f)
        float PowderStrength = 0.5f;

        /** Samples along the view ray. The dominant cost; lower it before anything else. */
        PROPERTY(Editable, Category = "Clouds|Quality", ClampMin = 16, ClampMax = 256)
        int32 MarchSteps = 96;

        /** Samples toward the sun per lit view sample, for self-shadowing. */
        PROPERTY(Editable, Category = "Clouds|Quality", ClampMin = 1, ClampMax = 16)
        int32 LightSteps = 6;

        /** View distance past which the layer is not marched at all. */
        PROPERTY(Editable, Category = "Clouds|Quality", ClampMin = 1000.0f, Units = "m")
        float MaxDistance = 60000.0f;
    };
}
