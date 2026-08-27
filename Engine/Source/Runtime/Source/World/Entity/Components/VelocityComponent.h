#pragma once

#include "Core/Math/Math.h"
#include "VelocityComponent.generated.h"

namespace Lumina
{
    REFLECT()
    struct RUNTIME_API SVelocityComponent
    {
        GENERATED_BODY()

        /** Current velocity vector in world space (meters/second). Written by SKinematicsSystem. */
        PROPERTY(ReadOnly)
        FVector3 Velocity = FVector3(0.0f);

        /** Magnitude of the velocity (meters/second). Written by SKinematicsSystem. */
        PROPERTY(ReadOnly)
        float Speed = 0.0f;
    };
    
}
