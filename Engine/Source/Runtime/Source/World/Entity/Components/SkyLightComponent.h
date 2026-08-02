#pragma once

#include "Core/Math/Math.h"
#include "SkyLightComponent.generated.h"

namespace Lumina
{
    /**
     * Skylight: the scene's ambient/indirect fill. Drives the flat ambient term and
     * scales the IBL diffuse/specular contribution (GetAmbientLightIntensity() in the
     * shaders). When bAmbientFromSky is set, the color is captured from the active
     * SEnvironmentComponent's sky each frame.
     *
     * Singleton-style: only one enabled instance per frame is read.
     */
    REFLECT(Component, Category = "Environment")
    struct RUNTIME_API SSkyLightComponent
    {
        GENERATED_BODY()

        /** When false, the skylight contributes no ambient this frame. */
        PROPERTY(Editable, Category = "Sky Light")
        bool bAffectsWorld = true;

        /** Base ambient (skylight) color; multiplied by Intensity. Ignored when bAmbientFromSky is set. */
        PROPERTY(Editable, Color, Category = "Sky Light")
        FVector3 AmbientColor = FVector3(0.6f, 0.7f, 1.0f);

        /** Ambient brightness; also scales IBL ambient. >0.3 over-fills shadows. */
        PROPERTY(Editable, Category = "Sky Light", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.001f)
        float Intensity = 0.05f;

        /** When true, AmbientColor is auto-derived from the active sky; Intensity still scales. */
        PROPERTY(Editable, Category = "Sky Light")
        bool bAmbientFromSky = false;
    };
}
