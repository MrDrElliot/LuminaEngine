#pragma once

#include "AI/Navigation/NavTypes.h"
#include "AI/Perception/PerceptionTypes.h"
#include "Containers/Vector.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "PerceptionSubsystem.generated.h"

namespace Lumina
{
    // AI perception, what each perceiver senses and the stimuli it hears.
    REFLECT()
    class RUNTIME_API CPerceptionSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        /** The targets the perceiver currently senses, empty when it has no perception component. */
        FUNCTION()
        void GetPerceivedTargets(ECS::FEntity Perceiver, TVector<ECS::FEntity>& Out);

        /** Whether any of the named sense channels currently detects the target. */
        FUNCTION()
        bool CanSense(ECS::FEntity Perceiver, ECS::FEntity Target, EAISenseChannel Senses);

        /** Held through the forget window, and not found once the target drops out of memory. */
        FUNCTION()
        SNavPoint GetLastKnownLocation(ECS::FEntity Perceiver, ECS::FEntity Target);

        /** Nearest perceived target to the perceiver's eye, or the null entity. */
        FUNCTION()
        ECS::FEntity GetClosestPerceivedTarget(ECS::FEntity Perceiver);

        /** One shot line of sight test, applying the eye and aim offsets when those components exist. */
        FUNCTION()
        bool HasLineOfSight(ECS::FEntity From, ECS::FEntity To);

        /** Heard by every perceiver in range that detects the instigator's affiliation. */
        FUNCTION()
        void ReportNoise(FVector3 Location, float Loudness, ECS::FEntity Instigator);

        /** The victim immediately perceives its attacker, whatever their affiliation. */
        FUNCTION()
        void ReportDamage(ECS::FEntity Victim, ECS::FEntity Instigator,
            FVector3 HitLocation, float Amount);

        /** Makes the entity sensible. Richer affiliation sets are authored in the editor. */
        FUNCTION()
        void RegisterSource(ECS::FEntity Entity, const FName& AffiliationTag,
            EAISenseChannel Senses);

        /** Only sources carrying a matching tag are sensed, and an empty filter senses everyone. */
        FUNCTION()
        void AddDetectableTag(ECS::FEntity Perceiver, const FName& Tag);
    };
}
