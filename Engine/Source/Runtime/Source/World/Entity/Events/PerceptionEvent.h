#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Math.h"
#include "AI/Perception/PerceptionTypes.h"
#include "World/Entity/EntityHandle.h"
#include "PerceptionEvent.generated.h"

namespace Lumina
{
    // Payload for the OnTargetPerceived / OnTargetLost EntityScript callbacks. Self-oriented: Perceiver is
    // this entity, Target is the sensed/lost entity. Every field is blittable (entt::entity surfaces as the
    // C# Entity handle), so the Reflector auto-generates the LuminaSharp SPerceptionEvent value mirror + a
    // native size assert, no hand-written mirror.
    REFLECT(Event)
    struct SPerceptionEvent
    {
        GENERATED_BODY()

        /** This perceiving entity. */
        PROPERTY(Script)
        FEntity Perceiver = entt::null;

        /** The perceived (or lost) entity. */
        PROPERTY(Script)
        FEntity Target = entt::null;

        /** Last known / stimulus world location of the target. */
        PROPERTY(Script)
        FVector3 Location = FVector3(0.0f);

        /** The EAISenseChannel bit(s) that triggered this event. */
        PROPERTY(Script)
        EAISenseChannel Sense = EAISenseChannel::Sight;

        /** Damage amount / noise loudness for hearing+damage; 0 for sight. */
        PROPERTY(Script)
        float Strength = 0.0f;
    };
}
