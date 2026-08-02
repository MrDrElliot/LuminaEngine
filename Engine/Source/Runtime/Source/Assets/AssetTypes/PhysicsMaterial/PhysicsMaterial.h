#pragma once

#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "PhysicsMaterial.generated.h"

namespace Lumina
{
    /** How two materials combine at a contact; the pair's effective mode is the max of both bodies'
     *  modes (Max > Min > Multiply > Average), so a "sticky" material wins. */
    REFLECT()
    enum class RUNTIME_API EPhysicsMaterialCombineMode : uint8
    {
        Average,
        Min,
        Multiply,
        Max,
    };

    /** Per-collider surface properties (friction, restitution, density) + combine modes, consumed by
     *  FJoltContactListener::OverrideFrictionAndRestitution. No material = rigid body *Override fallback / Jolt defaults. */
    REFLECT()
    class RUNTIME_API CPhysicsMaterial : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        /** Surface friction coefficient. 0 = ice, 1 = rubber on rubber. */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 4.0f, Category = "Physics Material")
        float Friction = 0.4f;

        /** Bounciness: 0 = inelastic, 1 = perfectly elastic. */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 1.0f, Category = "Physics Material")
        float Restitution = 0.3f;

        /** Volumetric density in kg/m^3. Used when the rigid body computes mass from shape volume. */
        PROPERTY(Editable, ClampMin = 0.001f, Category = "Physics Material")
        float Density = 1000.0f;

        /** Rule for combining this surface's friction with another at contact. */
        PROPERTY(Editable, Category = "Physics Material|Combine")
        EPhysicsMaterialCombineMode FrictionCombine = EPhysicsMaterialCombineMode::Average;

        /** Rule for combining this surface's restitution with another at contact. */
        PROPERTY(Editable, Category = "Physics Material|Combine")
        EPhysicsMaterialCombineMode RestitutionCombine = EPhysicsMaterialCombineMode::Max;
    };
}
