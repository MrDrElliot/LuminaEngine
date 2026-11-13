#pragma once

#include "AI/StateTree/StateTreeTypes.h"
#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "StateTreeLibraryTasks.generated.h"

namespace Lumina
{
    REFLECT()
    struct RUNTIME_API SStateTreeWaitTaskData
    {
        GENERATED_BODY()

        PROPERTY(Editable, ClampMin = 0.0f, Units = "s")
        float Duration = 1.0f;

        // The wait is drawn from Duration plus or minus this.
        PROPERTY(Editable, ClampMin = 0.0f, Units = "s")
        float RandomDeviation = 0.0f;

        double EndTime = 0.0;
    };

    // Succeeds once its time is up, sleeping the instance until then instead of ticking it.
    REFLECT()
    struct RUNTIME_API SStateTreeWaitTask : public SStateTreeTaskBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeWaitTaskData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context) const override;
        EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context) const override;
        bool NeedsTick() const override { return false; }

        PROPERTY(Editable)
        SStateTreeWaitTaskData Data;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeMoveToTaskData
    {
        GENERATED_BODY()

        // Followed as it moves when set, otherwise TargetLocation is the goal.
        PROPERTY(Editable)
        ECS::FEntity TargetEntity = ECS::NullEntity;

        PROPERTY(Editable)
        FVector3 TargetLocation = FVector3(0.0f);
    };

    // Walks the entity's path follow component to the target, succeeding on arrival and failing when no full path reaches it.
    REFLECT()
    struct RUNTIME_API SStateTreeMoveToTask : public SStateTreeTaskBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeMoveToTaskData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context) const override;
        EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context) const override;
        void ExitState(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        SStateTreeMoveToTaskData Data;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeLookAtTaskData
    {
        GENERATED_BODY()

        // Faced when set, otherwise TargetLocation is.
        PROPERTY(Editable)
        ECS::FEntity TargetEntity = ECS::NullEntity;

        PROPERTY(Editable)
        FVector3 TargetLocation = FVector3(0.0f);

        // Zero turns at once.
        PROPERTY(Editable, ClampMin = 0.0f, Units = "deg/s")
        float TurnRate = 360.0f;

        PROPERTY(Editable, ClampMin = 0.0f, Units = "deg")
        float AcceptanceAngle = 5.0f;
    };

    // Turns the entity's yaw toward the target and succeeds once it faces it.
    REFLECT()
    struct RUNTIME_API SStateTreeLookAtTask : public SStateTreeTaskBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeLookAtTaskData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context) const override { return Tick(Context); }
        EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        SStateTreeLookAtTaskData Data;
    };

    REFLECT()
    struct RUNTIME_API SStateTreePlayMontageTaskData
    {
        GENERATED_BODY()

        PROPERTY(Editable, ClampMin = 0.0f)
        float PlayRate = 1.0f;

        // None starts at the montage's first section.
        PROPERTY(Editable)
        FName Section;
    };

    // Plays a montage on the entity's animation graph and succeeds when it ends, stopping it when the state exits first.
    REFLECT()
    struct RUNTIME_API SStateTreePlayMontageTask : public SStateTreeTaskBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreePlayMontageTaskData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context) const override;
        EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context) const override;
        void ExitState(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        TStrongObjectPtr<CAnimationMontage> Montage;

        PROPERTY(Editable)
        SStateTreePlayMontageTaskData Data;
    };

    // Acts once on entering and keeps running, so it can sit beside other tasks without completing their state.
    REFLECT()
    struct RUNTIME_API SStateTreeInstantTaskBase : public SStateTreeTaskBase
    {
        GENERATED_BODY()

        EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context) const override;
        bool NeedsTick() const override { return false; }

        // False fails the state.
        virtual bool Execute(FStateTreeExecutionContext& Context) const { return true; }

        // Succeeds at once instead, for a state holding only this task.
        PROPERTY(Editable)
        bool bCompleteState = false;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeSendEventTaskData
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        SHierarchicalTag Event;

        // The null entity sends to this tree.
        PROPERTY(Editable)
        ECS::FEntity Target = ECS::NullEntity;
    };

    // Sends Event to this tree or to the state tree on Target, which hears it on its next tick.
    REFLECT()
    struct RUNTIME_API SStateTreeSendEventTask : public SStateTreeInstantTaskBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeSendEventTaskData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        bool Execute(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        SStateTreeSendEventTaskData Data;
    };

    // One field per type a parameter can have; the one matching the parameter's type is written.
    REFLECT()
    struct RUNTIME_API SStateTreeSetParameterTaskData
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        float FloatValue = 0.0f;

        PROPERTY(Editable)
        int32 IntValue = 0;

        PROPERTY(Editable)
        bool bBoolValue = false;

        PROPERTY(Editable)
        FVector3 VectorValue = FVector3(0.0f);

        PROPERTY(Editable)
        SHierarchicalTag TagValue;

        PROPERTY(Editable)
        ECS::FEntity EntityValue = ECS::NullEntity;
    };

    // Writes the tree parameter named Parameter, which later bindings to it then read.
    REFLECT()
    struct RUNTIME_API SStateTreeSetParameterTask : public SStateTreeInstantTaskBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeSetParameterTaskData::StaticStruct(); }
        const void* GetInstanceDataDefaults() const override { return &Data; }

        bool Execute(FStateTreeExecutionContext& Context) const override;

        PROPERTY(Editable)
        FName Parameter;

        PROPERTY(Editable)
        SStateTreeSetParameterTaskData Data;
    };
}
