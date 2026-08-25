#pragma once

#include "Core/Math/Math.h"
#include "LocalFogVolumeComponent.generated.h"

namespace Lumina
{
    // Only affects the froxel range; the analytic far field is closed-form and cannot see these.
    REFLECT(Component, Category = "Environment")
    struct RUNTIME_API SLocalFogVolumeComponent
    {
        GENERATED_BODY()

        /** When false this volume contributes nothing. */
        PROPERTY(Editable, Category = "Fog Volume")
        bool bEnabled = true;

        /** Sphere instead of box. A sphere uses the largest axis of Extent as its radius. */
        PROPERTY(Editable, Category = "Fog Volume")
        bool bSphere = false;

        /** Half extents in local units, before the entity transform scale. */
        PROPERTY(Editable, Category = "Fog Volume", ClampMin = 0.01f, Units = "m")
        FVector3 Extent = FVector3(5.0f, 5.0f, 5.0f);

        /** Distance inside the volume at which it alone reaches ~98% opacity. Lower is thicker. */
        PROPERTY(Editable, Category = "Fog Volume", ClampMin = 1.0f, Delta = 1.0f, Units = "m")
        float VisibilityDistance = 40.0f;

        /** Fraction of the volume over which density ramps to zero at the boundary. */
        PROPERTY(Editable, Category = "Fog Volume", ClampMin = 0.001f, ClampMax = 1.0f, Delta = 0.01f)
        float EdgeSoftness = 0.2f;

        /** Albedo of this volume's medium, blended with the global fog by relative density. */
        PROPERTY(Editable, Color, Category = "Fog Volume")
        FVector3 Albedo = FVector3(0.5f, 0.6f, 0.7f);

        /** Light this volume emits on its own, independent of any light reaching it. */
        PROPERTY(Editable, Color, Category = "Fog Volume")
        FVector3 EmissiveColor = FVector3(0.0f, 0.0f, 0.0f);

        /** Scales EmissiveColor so an HDR glow can be authored without a >1 color picker. */
        PROPERTY(Editable, Category = "Fog Volume", ClampMin = 0.0f)
        float EmissiveIntensity = 0.0f;

        /** Multiplies the in-scattered light inside this volume, on top of the fog's own intensity. */
        PROPERTY(Editable, Category = "Fog Volume", ClampMin = 0.0f)
        float ScatteringIntensity = 1.0f;
    };
}
