#pragma once

#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Object/ObjectMacros.h"
#include "LagCompensationComponent.generated.h"

namespace Lumina
{
    // A capsule the host remembers for the last moments, so a shot is judged against where its target was on the shooter's screen.
    REFLECT(Component, Category = "Networking")
    struct NETWORKINGRUNTIME_API SLagCompensationComponent
    {
        GENERATED_BODY()

        // The capsule's middle, in the entity's local space.
        PROPERTY(Editable, Category = "Hitbox")
        FVector3 Center = FVector3(0.0f, 0.9f, 0.0f);

        PROPERTY(Editable, Category = "Hitbox", ClampMin = 0.0f, Units = "m")
        float Radius = 0.4f;

        // Half the straight part along the entity's up axis, the end caps not included.
        PROPERTY(Editable, Category = "Hitbox", ClampMin = 0.0f, Units = "m")
        float HalfHeight = 0.5f;
    };
}
