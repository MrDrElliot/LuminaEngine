#pragma once

#include "Core/Math/Math.h"
#include "BuoyancyComponent.generated.h"

namespace Lumina
{
    // Makes a rigid body float on an SWaterComponent with bBuoyancy, from its real collider shape, and needs an SRigidBodyComponent with gravity.
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
