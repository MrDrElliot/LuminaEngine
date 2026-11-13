#pragma once

#include "World/ECS/Registry.h"
#include "World/ECS/EventMeta.h"

#include "Core/Object/ObjectMacros.h"
#include "CollisionEvent.generated.h"

namespace Lumina
{
    REFLECT()
    enum class ECollisionEventKind : uint8
    {
        ContactBegin,
        ContactEnd,
        OverlapBegin,
        OverlapEnd,
    };

    // One per side of a contact, oriented from Entity, queued for systems and fanned out to its rigid body's delegates.
    REFLECT(Event)
    struct RUNTIME_API SCollisionEvent
    {
        GENERATED_BODY()

        /** This script's entity. */
        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        /** The other body's entity. */
        PROPERTY(Hidden)
        ECS::FEntity Other = ECS::NullEntity;

        /** World-space contact point, or the origin for an overlap end whose shape was destroyed. */
        PROPERTY(Hidden)
        FVector3 Point = FVector3(0.0f);

        /** Unit normal pointing away from self toward the other body, zero for an overlap whose shapes share a bounds center or were destroyed. */
        PROPERTY(Hidden)
        FVector3 Normal = FVector3(0.0f, 1.0f, 0.0f);

        /** This body's linear velocity at contact time (m/s). */
        PROPERTY(Hidden)
        FVector3 Velocity = FVector3(0.0f);

        /** The other body's linear velocity at contact time (m/s). */
        PROPERTY(Hidden)
        FVector3 OtherVelocity = FVector3(0.0f);

        /** Other - Self linear velocity (m/s). */
        PROPERTY(Hidden)
        FVector3 RelativeVelocity = FVector3(0.0f);

        /** |relative velocity along the normal| (m/s). */
        PROPERTY(Hidden)
        float ImpactSpeed = 0.0f;

        /** True if the OTHER side was a trigger/sensor. */
        PROPERTY(Hidden)
        bool bIsTrigger = false;

        // Which of the rigid body's four delegates the event fans out to.
        PROPERTY(Hidden)
        ECollisionEventKind Kind = ECollisionEventKind::ContactBegin;
    };

    // A body woke or fell asleep, queued for systems and fanned out to its rigid body's OnWake and OnSleep.
    REFLECT(Event)
    struct RUNTIME_API SBodyActivationEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        PROPERTY(Hidden)
        bool bAwake = true;
    };

    // Defined beside the rigid body these deliver to, which this header cannot see.
    template<>
    struct TEventFanOut<SCollisionEvent>
    {
        RUNTIME_API static void Deliver(ECS::FRegistry& Registry, const SCollisionEvent& Event);
    };

    template<>
    struct TEventFanOut<SBodyActivationEvent>
    {
        RUNTIME_API static void Deliver(ECS::FRegistry& Registry, const SBodyActivationEvent& Event);
    };
}
