#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Math.h"
#include "AI/Perception/PerceptionTypes.h"
#include "GameplayTags/GameplayTag.h"
#include "AIStimuliSourceComponent.generated.h"

namespace Lumina
{
    // Marks an entity as perceivable by AI. Sight/hearing register the entity passively (the perception
    // system scans for it); damage is reported on demand via World.Perception.ReportDamage. Affiliation tags
    // let a perceiver filter who it cares about.
    REFLECT(Component, Category = "AI")
    struct RUNTIME_API SAIStimuliSourceComponent
    {
        GENERATED_BODY()

        // Affiliation/identity tags this source broadcasts (e.g. "Team.Red"). A perceiver senses it only when
        // its DetectableTags matches any of these; an empty perceiver filter senses everyone.
        PROPERTY(Script, Editable, Category = "AI|Perception")
        FGameplayTagContainer AffiliationTags;

        // Senses this source registers for passive detection. Damage is always event-driven regardless.
        PROPERTY(Script, Editable, Category = "AI|Perception")
        EAISenseChannel RegisteredSenses = EAISenseChannel::Sight | EAISenseChannel::Hearing;

        // World-space offset (along entity up) used as the sight aim point so line-of-sight rays target the
        // chest/head instead of the floor.
        PROPERTY(Script, Editable, Category = "AI|Perception")
        FVector3 SightTargetOffset = FVector3(0.0f, 1.0f, 0.0f);
    };
}
