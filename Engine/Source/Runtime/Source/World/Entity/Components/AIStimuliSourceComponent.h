#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Math.h"
#include "AI/Perception/PerceptionTypes.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "AIStimuliSourceComponent.generated.h"

namespace Lumina
{
    // Marks an entity as perceivable by AI, with sight and hearing passive and damage reported through World.Perception.ReportDamage.
    REFLECT(Component, Category = "AI")
    struct RUNTIME_API SAIStimuliSourceComponent
    {
        GENERATED_BODY()

        // Affiliation tags this source broadcasts, sensed only by a perceiver whose DetectableTags matches (empty senses everyone).
        PROPERTY(Editable, Category = "AI|Perception")
        SHierarchicalTagContainer AffiliationTags;

        // Senses this source registers for passive detection. Damage is always event-driven regardless.
        PROPERTY(Editable, Category = "AI|Perception")
        EAISenseChannel RegisteredSenses = EAISenseChannel::Sight | EAISenseChannel::Hearing;

        // Offset along entity up used as the sight aim point, so line-of-sight rays miss the floor.
        PROPERTY(Editable, Category = "AI|Perception")
        FVector3 SightTargetOffset = FVector3(0.0f, 1.0f, 0.0f);
    };
}
