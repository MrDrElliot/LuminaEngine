#pragma once
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "PostProcessSettings.h"
#include "PostProcessComponent.generated.h"

namespace Lumina
{
    class CMaterialInterface;
    // Volumetric post-process override tested against the camera, stacking by Priority (highest wins) and softened by BlendDistance.
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API SPostProcessComponent
    {
        GENERATED_BODY()

        /** Master enable. Disabled volumes contribute nothing. */
        PROPERTY(Editable, Category = "Volume")
        bool bEnabled = true;

        /** When true the BoxExtent is ignored and this volume is always
         *  active. Use one of these as the level's "global look". */
        PROPERTY(Editable, Category = "Volume")
        bool bInfiniteExtent = false;

        // Local-space half-extent (box is 2*BoxExtent per axis, centered on the entity); rotates/scales with the transform.
        PROPERTY(Editable, Category = "Volume", ClampMin = 0.01f)
        FVector3 BoxExtent = FVector3(500.0f);

        // World-space fade distance at the boundary. 0 = hard edge (pop); 100..500 typical for outdoor transitions.
        PROPERTY(Editable, Category = "Volume", ClampMin = 0.0f, ClampMax = 5000.0f)
        float BlendDistance = 100.0f;

        // How strongly Settings replace the camera/lower-priority volumes when fully inside (1.0 = full, 0.0 = none).
        PROPERTY(Editable, Category = "Volume", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.01f)
        float BlendWeight = 1.0f;

        // Higher priority blends last (overrides lower). Ties resolve by iteration order -- use distinct priorities if it matters.
        PROPERTY(Editable, Category = "Volume")
        int32 Priority = 0;

        /** The grading + tone-mapping settings this volume contributes.
         *  Identical layout to the camera's own PostProcess. */
        PROPERTY(Editable, Category = "Settings")
        SPostProcessSettings Settings;

        // Post-process materials appended after the camera's own while the camera is inside, which must be MaterialType PostProcess.
        PROPERTY(Editable, Category = "Settings")
        TVector<TStrongObjectPtr<CMaterialInterface>> PostProcessMaterials;
    };
}
