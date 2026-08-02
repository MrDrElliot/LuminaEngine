#pragma once

#include "Core/Math/Math.h"
#include "BuoyancyComponent.generated.h"

namespace Lumina
{
    // Makes a rigid body float on water. Requires an SRigidBodyComponent on the same entity and a water body
    // (SWaterComponent with bBuoyancy) the entity is over. The buoyancy system samples the water surface
    // height + normal (matching the rendered Gerstner waves) at the body and hands it to Jolt's shape-accurate
    // buoyancy, which computes the submerged volume from the body's actual collider and applies lift + drag.
    // The body therefore bobs and self-rights from its real shape -- no manual sample points needed. Needs
    // gravity enabled on the rigid body.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SBuoyancyComponent
    {
        GENERATED_BODY()

        /** Fluid-to-body density ratio. 1 = neutral (hovers); >1 floats (≈2 settles half-submerged, ≈4 a quarter). */
        PROPERTY(Editable, Category = "Buoyancy", ClampMin = 0.0f)
        float Buoyancy = 2.0f;

        /** Linear drag while submerged. Damps bobbing and horizontal drift. */
        PROPERTY(Editable, Category = "Buoyancy", ClampMin = 0.0f)
        float LinearDrag = 1.0f;

        /** Angular drag while submerged. Damps rocking/spinning so the body settles upright. */
        PROPERTY(Editable, Category = "Buoyancy", ClampMin = 0.0f)
        float AngularDrag = 1.0f;
    };
}
