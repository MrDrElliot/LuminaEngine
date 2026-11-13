#pragma once

#include "AI/StateTree/StateTreeTypes.h"
#include "Core/Math/Math.h"
#include "StateTreeLibraryEvaluators.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EStateTreeTargetPick : uint8
    {
        Nearest,
        MostRecent,
    };

    REFLECT()
    struct RUNTIME_API SStateTreePerceivedTargetEvaluatorData
    {
        GENERATED_BODY()

        PROPERTY(ReadOnly)
        bool bHasTarget = false;

        // The null entity while nothing is perceived.
        PROPERTY(ReadOnly)
        ECS::FEntity Target = ECS::NullEntity;

        // Where the target was last sensed.
        PROPERTY(ReadOnly)
        FVector3 Location = FVector3(0.0f);
    };

    // Publishes one target from the entity's perception component, held through the component's forget window.
    REFLECT()
    struct RUNTIME_API SStateTreePerceivedTargetEvaluator : public SStateTreeEvaluatorBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreePerceivedTargetEvaluatorData::StaticStruct(); }

        void Tick(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        EStateTreeTargetPick Pick = EStateTreeTargetPick::Nearest;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeSelfInfoEvaluatorData
    {
        GENERATED_BODY()

        PROPERTY(ReadOnly)
        FVector3 Location = FVector3(0.0f);

        // Averaged since the previous tick, so across a sleep it is the average over the sleep.
        PROPERTY(ReadOnly)
        FVector3 Velocity = FVector3(0.0f);
    };

    // Publishes the entity's world location and velocity.
    REFLECT()
    struct RUNTIME_API SStateTreeSelfInfoEvaluator : public SStateTreeEvaluatorBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeSelfInfoEvaluatorData::StaticStruct(); }

        void Tick(FStateTreeExecutionContext& Context) const override;
    };
}
