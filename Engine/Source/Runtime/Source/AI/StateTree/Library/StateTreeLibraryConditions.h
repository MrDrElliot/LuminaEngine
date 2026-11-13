#pragma once

#include "AI/Perception/PerceptionTypes.h"
#include "AI/StateTree/StateTreeTypes.h"
#include "Core/Math/Math.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "StateTreeLibraryConditions.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EStateTreeCompareKind : uint8
    {
        Number,
        // Passes when bValue is true.
        Bool,
        // Passes when Tag is MatchTag or a descendant of it.
        Tag,
        // Passes when Entity is alive.
        EntityValid,
    };

    REFLECT()
    enum class EStateTreeCompareOp : uint8
    {
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
    };

    REFLECT()
    struct RUNTIME_API SStateTreeCompareConditionData
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        float Left = 0.0f;

        PROPERTY(Editable)
        float Right = 0.0f;

        PROPERTY(Editable)
        bool bValue = false;

        PROPERTY(Editable)
        SHierarchicalTag Tag;

        PROPERTY(Editable)
        SHierarchicalTag MatchTag;

        PROPERTY(Editable)
        ECS::FEntity Entity = ECS::NullEntity;
    };

    // Tests the fields Kind names, typically bound to tree parameters or evaluator outputs.
    REFLECT()
    struct RUNTIME_API SStateTreeCompareCondition : public SStateTreeConditionBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeCompareConditionData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        bool TestCondition(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        EStateTreeCompareKind Kind = EStateTreeCompareKind::Number;

        // How Left is compared with Right for a Number.
        PROPERTY(Editable)
        EStateTreeCompareOp Op = EStateTreeCompareOp::Equal;

        PROPERTY(Editable)
        SStateTreeCompareConditionData Data;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeDistanceConditionData
    {
        GENERATED_BODY()

        // Measured to when set, otherwise TargetLocation is.
        PROPERTY(Editable)
        ECS::FEntity TargetEntity = ECS::NullEntity;

        PROPERTY(Editable)
        FVector3 TargetLocation = FVector3(0.0f);

        PROPERTY(Editable, ClampMin = 0.0f, Units = "m")
        float Radius = 2.0f;
    };

    // Passes while the entity is within Radius of the target; inverted it passes beyond it.
    REFLECT()
    struct RUNTIME_API SStateTreeDistanceCondition : public SStateTreeConditionBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeDistanceConditionData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        bool TestCondition(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        SStateTreeDistanceConditionData Data;
    };

    REFLECT()
    struct RUNTIME_API SStateTreePerceptionSensedConditionData
    {
        GENERATED_BODY()

        // The null entity passes for anything sensed.
        PROPERTY(Editable)
        ECS::FEntity Target = ECS::NullEntity;
    };

    // Passes while the entity's perception component senses the target through any of Senses.
    REFLECT()
    struct RUNTIME_API SStateTreePerceptionSensedCondition : public SStateTreeConditionBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreePerceptionSensedConditionData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        bool TestCondition(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        EAISenseChannel Senses = EAISenseChannel::Sight | EAISenseChannel::Hearing | EAISenseChannel::Damage;

        PROPERTY(Editable)
        SStateTreePerceptionSensedConditionData Data;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeHasTagConditionData
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        SHierarchicalTag Tag;

        // The null entity tests this tree's entity.
        PROPERTY(Editable)
        ECS::FEntity Target = ECS::NullEntity;
    };

    // Passes when the target's gameplay tags hold Tag, or a descendant of it unless bExact.
    REFLECT()
    struct RUNTIME_API SStateTreeHasTagCondition : public SStateTreeConditionBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeHasTagConditionData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        bool TestCondition(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        bool bExact = false;

        PROPERTY(Editable)
        SStateTreeHasTagConditionData Data;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeCooldownConditionData
    {
        GENERATED_BODY()

        PROPERTY(Editable, ClampMin = 0.0f, Units = "s")
        float Duration = 1.0f;

        double ReadyTime = 0.0;
    };

    // Passes at most once per Duration, so list it last among conditions or a later failing one still spends it.
    REFLECT()
    struct RUNTIME_API SStateTreeCooldownCondition : public SStateTreeConditionBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeCooldownConditionData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        bool TestCondition(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        SStateTreeCooldownConditionData Data;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeRandomChanceConditionData
    {
        GENERATED_BODY()

        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 1.0f)
        float Chance = 0.5f;
    };

    // Passes with probability Chance, drawn from the instance's seeded stream.
    REFLECT()
    struct RUNTIME_API SStateTreeRandomChanceCondition : public SStateTreeConditionBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeRandomChanceConditionData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        bool TestCondition(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        SStateTreeRandomChanceConditionData Data;
    };
}
