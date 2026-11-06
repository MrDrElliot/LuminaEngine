#pragma once

#include "World/ECS/Entity.h"

#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Math.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "PerceptionTypes.generated.h"

namespace Lumina
{
    // Senses a perceiver runs and a stimulus source registers for, combinable so one source can be seen and heard.
    REFLECT(BitMask)
    enum class EAISenseChannel : uint8
    {
        Sight   = BIT(0),
        Hearing = BIT(1),
        Damage  = BIT(2),
    };

    ENUM_CLASS_FLAGS(EAISenseChannel);

    // A target a perceiver is or recently was aware of, recomputed by CPerceptionSystem every tick and never serialized.
    struct FPerceivedTarget
    {
        ECS::FEntity    Target              = ECS::NullEntity;
        FVector3        LastKnownLocation   = FVector3(0.0f);
        uint8           ActiveSenses        = 0;       // EAISenseChannel bits sensing it (Sight sticky; Hearing/Damage momentary).
        float           TimeSinceLastSensed = 0.0f;    // 0 while sensed; counts up after; > ForgetTime => dropped.
        float           LastStrength        = 0.0f;    // damage amount, noise loudness, or for sight how near the target stood (1 at the eye).
        bool            bIsNew              = false;   // set the tick it crossed the perceive threshold.
    };

    // A queued hearing or damage report from ReportNoise, ReportDamage or native damage, drained by CPerceptionSystem each tick.
    struct FAIStimulusEvent
    {
        EAISenseChannel         Sense       = EAISenseChannel::Hearing;
        ECS::FEntity            Instigator  = ECS::NullEntity;   // who made the noise / dealt the damage.
        ECS::FEntity            Target      = ECS::NullEntity;   // damage victim; ECS::NullEntity for area noise.
        FVector3                Location    = FVector3(0.0f);
        float                   Strength    = 1.0f;         // noise loudness (radius scale) or damage amount.
        SHierarchicalTagContainer   Tags;                       // optional affiliation override (empty => look up Instigator).
    };
}
