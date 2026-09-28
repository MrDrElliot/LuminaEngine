#pragma once

#include "Core/Math/Math.h"
#include "ParticleFieldComponents.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EParticleShapeType : uint8
    {
        Sphere,
        Box,
    };

    // Pulls nearby particles of every emitter with an Attractor Force module, taking the entity's position and rotation.
    REFLECT(Component, Category = "Effects")
    struct RUNTIME_API SParticleAttractorComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Attractor")
        bool bEnabled = true;

        PROPERTY(Editable, Category = "Attractor")
        EParticleShapeType Shape = EParticleShapeType::Sphere;

        // Radius in X for a sphere, half extents for a box, scaled by the entity.
        PROPERTY(Editable, Category = "Attractor")
        FVector3 Extent = FVector3(5.0f);

        // Acceleration in meters per second squared at the center. Negative pushes particles away.
        PROPERTY(Editable, Category = "Attractor")
        float Strength = 10.0f;

        // Exponent on the falloff toward the edge, where zero pulls evenly throughout.
        PROPERTY(Editable, Category = "Attractor", ClampMin = 0.0f)
        float Attenuation = 1.0f;

        // Blends the pull from toward the center to along the entity's forward axis, for wind and conveyors.
        PROPERTY(Editable, Category = "Attractor", ClampMin = 0.0f, ClampMax = 1.0f)
        float Directionality = 0.0f;
    };

    // A solid volume particles bounce off through a Collide With Shapes module, even off-screen.
    REFLECT(Component, Category = "Effects")
    struct RUNTIME_API SParticleColliderComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Collider")
        bool bEnabled = true;

        PROPERTY(Editable, Category = "Collider")
        EParticleShapeType Shape = EParticleShapeType::Box;

        // Radius in X for a sphere, half extents for a box, scaled by the entity.
        PROPERTY(Editable, Category = "Collider")
        FVector3 Extent = FVector3(1.0f);
    };
}
