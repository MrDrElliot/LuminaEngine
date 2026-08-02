#pragma once

#include "Core/Object/ObjectMacros.h"
#include "World/Entity/EntityHandle.h"
#include "CollisionEvent.generated.h"

namespace Lumina
{
    // Payload for OnContact/OnOverlap. Fields are self-oriented: Entity/Velocity = self, Normal away from self.
    // Every field is blittable (entt::entity surfaces as the C# Entity handle), so the Reflector auto-generates
    // the LuminaSharp SCollisionEvent value mirror + a native size assert, no hand-written mirror.
    REFLECT(Event)
    struct SCollisionEvent
    {
        GENERATED_BODY()

        /** This script's entity. */
        PROPERTY(Script)
        FEntity Entity = entt::null;

        /** The other body's entity. */
        PROPERTY(Script)
        FEntity Other = entt::null;

        /** This body's Jolt body id. */
        PROPERTY(Script)
        uint32 BodyID = 0;

        /** The other body's Jolt body id. */
        PROPERTY(Script)
        uint32 OtherBodyID = 0;

        /** World-space contact point. */
        PROPERTY(Script)
        FVector3 Point = FVector3(0.0f);

        /** Contact normal pointing away from self toward the other body. */
        PROPERTY(Script)
        FVector3 Normal = FVector3(0.0f, 1.0f, 0.0f);

        /** This body's linear velocity at contact time (m/s). */
        PROPERTY(Script)
        FVector3 Velocity = FVector3(0.0f);

        /** The other body's linear velocity at contact time (m/s). */
        PROPERTY(Script)
        FVector3 OtherVelocity = FVector3(0.0f);

        /** Other - Self linear velocity (m/s). */
        PROPERTY(Script)
        FVector3 RelativeVelocity = FVector3(0.0f);

        /** |relative velocity along the normal| (m/s). */
        PROPERTY(Script)
        float ImpactSpeed = 0.0f;

        /** True if the OTHER side was a trigger/sensor. */
        PROPERTY(Script)
        bool bIsTrigger = false;
    };
}
