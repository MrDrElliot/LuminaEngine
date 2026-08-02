#pragma once

#include "Core/Math/Math.h"
#include "VelocityComponent.generated.h"

namespace Lumina
{
    REFLECT()
    struct RUNTIME_API SVelocityComponent
    {
        GENERATED_BODY()

        /** Current velocity vector in world space (meters/second). */
        PROPERTY(ReadOnly)
        FVector3 Velocity;

        /** Magnitude of the velocity (meters/second). */
        PROPERTY(Editable)
        float Speed;

        /** Uniform scale applied to the velocity vector. */
        PROPERTY(Editable)
        float Scale; 
    };
    
}
