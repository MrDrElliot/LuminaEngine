#pragma once

#include "Core/Object/ObjectMacros.h"
#include "ImpulseEvent.generated.h"

namespace Lumina
{
    REFLECT(Event)
    struct SImpulseEvent
    {
        GENERATED_BODY()

        /** Target physics body to apply the impulse to. */
        PROPERTY(Script)
        uint32 BodyID;

        /** Impulse vector in world space (kg·m/s). */
        PROPERTY(Script)
        FVector3 Impulse;
    };

    REFLECT(Event)
    struct SForceEvent
    {
        GENERATED_BODY()

        /** Target physics body to apply the force to. */
        PROPERTY(Script)
        uint32 BodyID;

        /** Continuous force vector in world space (N). Applied for one physics step. */
        PROPERTY(Script)
        FVector3 Force;
    };

    REFLECT(Event)
    struct STorqueEvent
    {
        GENERATED_BODY()

        /** Target physics body to apply torque to. */
        PROPERTY(Script)
        uint32 BodyID;

        /** Torque vector in world space (N·m). Applied for one physics step. */
        PROPERTY(Script)
        FVector3 Torque;
    };

    REFLECT(Event)
    struct SAngularImpulseEvent
    {
        GENERATED_BODY()

        /** Target physics body to apply the angular impulse to. */
        PROPERTY(Script)
        uint32 BodyID;

        /** Angular impulse in world space (kg·m²/s). */
        PROPERTY(Script)
        FVector3 AngularImpulse;
    };

    REFLECT(Event)
    struct SSetVelocityEvent
    {
        GENERATED_BODY()

        /** Target physics body whose velocity will be replaced. */
        PROPERTY(Script)
        uint32 BodyID;

        /** New linear velocity in world space (m/s). */
        PROPERTY(Script)
        FVector3 Velocity;
    };

    REFLECT(Event)
    struct SSetAngularVelocityEvent
    {
        GENERATED_BODY()

        /** Target physics body whose angular velocity will be replaced. */
        PROPERTY(Script)
        uint32 BodyID;

        /** New angular velocity in world space (rad/s). */
        PROPERTY(Script)
        FVector3 AngularVelocity;
    };

    REFLECT(Event)
    struct SAddImpulseAtPositionEvent
    {
        GENERATED_BODY()

        /** Target physics body to apply the impulse to. */
        PROPERTY(Script)
        uint32 BodyID;

        /** Impulse vector in world space (kg·m/s). */
        PROPERTY(Script)
        FVector3 Impulse;

        /** World-space position where the impulse is applied. */
        PROPERTY(Script)
        FVector3 Position;
    };

    REFLECT(Event)
    struct SAddForceAtPositionEvent
    {
        GENERATED_BODY()

        /** Target physics body to apply the force to. */
        PROPERTY(Script)
        uint32 BodyID;

        /** Force vector in world space (N). */
        PROPERTY(Script)
        FVector3 Force;

        /** World-space position where the force is applied. */
        PROPERTY(Script)
        FVector3 Position;
    };

    REFLECT(Event)
    struct SSetGravityFactorEvent
    {
        GENERATED_BODY()

        /** Target physics body whose gravity factor will be set. */
        PROPERTY(Script)
        uint32 BodyID;

        /** Gravity scale multiplier for this body (0 = no gravity, 1 = full gravity). */
        PROPERTY(Script)
        float GravityFactor;
    };
}
