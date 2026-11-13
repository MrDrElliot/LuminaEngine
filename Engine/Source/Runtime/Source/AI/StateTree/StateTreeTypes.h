#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "StateTreeTypes.generated.h"

namespace Lumina
{
    class CStruct;
    class FStateTreeExecutionContext;

    REFLECT()
    enum class EStateTreeRunStatus : uint8
    {
        Running,
        Succeeded,
        Failed,
    };

    REFLECT()
    enum class EStateTreeStateKind : uint8
    {
        State,
        // Holds children only and is never the leaf of the active path.
        Group,
        // Runs another tree's states as its children, so like a group it is never the leaf.
        LinkedSubtree,
    };

    REFLECT()
    enum class EStateTreeTransitionTrigger : uint8
    {
        OnCompleted,
        OnSucceeded,
        OnFailed,
        OnEvent,
        OnTick,
    };

    REFLECT()
    enum class EStateTreeTransitionTarget : uint8
    {
        State,
        NextSibling,
        Parent,
        Succeeded,
        Failed,
    };

    REFLECT()
    enum class EStateTreeConditionOperand : uint8
    {
        And,
        Or,
    };

    REFLECT()
    enum class EStateTreeBindingSource : uint8
    {
        Parameter,
        Evaluator,
        Task,
        Context,
        // A static reflected function of the type SourceNode names.
        Function,
    };

    // Where a binding, or one argument of the function it calls, reads its value.
    REFLECT()
    struct RUNTIME_API SStateTreeBindingPath
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        EStateTreeBindingSource Source = EStateTreeBindingSource::Parameter;

        // The evaluator, task or function owning type named here, ignored for Parameter and Context.
        PROPERTY(Editable)
        FName SourceNode;

        // A field, or a reflected function on the source whose return value is copied instead.
        PROPERTY(Editable)
        FName SourceField;
    };

    // Copies the source into TargetField of the owning node's instance data before the node runs.
    REFLECT()
    struct RUNTIME_API SStateTreeBinding : public SStateTreeBindingPath
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        FName TargetField;

        // One field per argument when the source is a function.
        PROPERTY(Editable)
        TVector<SStateTreeBindingPath> Arguments;
    };

    // The fields a Context binding reads, refreshed by the executor before every tick.
    REFLECT()
    struct RUNTIME_API SStateTreeContextData
    {
        GENERATED_BODY()

        PROPERTY(ReadOnly)
        ECS::FEntity Owner = ECS::NullEntity;

        PROPERTY(ReadOnly)
        float DeltaTime = 0.0f;
    };

    // Base of every tree parameter struct.
    REFLECT()
    struct RUNTIME_API SStateTreeParametersBase
    {
        GENERATED_BODY()
    };

    // One definition is shared by every running instance, so per-entity state lives in its instance data struct.
    REFLECT()
    struct RUNTIME_API SStateTreeNodeBase
    {
        GENERATED_BODY()

        virtual ~SStateTreeNodeBase() = default;

        // The reflected struct holding this node's per-instance state, or null when it keeps none.
        virtual CStruct* GetInstanceDataType() const { return nullptr; }

        // Authored values the instance data starts from instead of its type's defaults, so unbound fields keep what the designer set.
        virtual const void* GetInstanceDataDefaults() const { return nullptr; }

        // How bindings and the debugger refer to this node.
        PROPERTY(Editable)
        FName Name;

        PROPERTY(Editable)
        TVector<SStateTreeBinding> Bindings;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeTaskBase : public SStateTreeNodeBase
    {
        GENERATED_BODY()

        // A status other than Running completes the state at once. The base versions run a C# node's methods.
        virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context) const;

        virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context) const;

        virtual void ExitState(FStateTreeExecutionContext& Context) const;

        virtual void OnEvent(FStateTreeExecutionContext& Context, uint32 EventTag) const;

        // False lets the instance sleep until an event or a wake time the task asks for with RequestWakeAt.
        virtual bool NeedsTick() const { return true; }
    };

    REFLECT()
    struct RUNTIME_API SStateTreeConditionBase : public SStateTreeNodeBase
    {
        GENERATED_BODY()

        virtual bool TestCondition(FStateTreeExecutionContext& Context) const;

        PROPERTY(Editable)
        bool bInvert = false;

        // How this result joins the running result of the conditions before it, ignored on the first.
        PROPERTY(Editable)
        EStateTreeConditionOperand Operand = EStateTreeConditionOperand::And;
    };

    // Ticks once per tree tick before the states, publishing its instance data fields as outputs.
    REFLECT()
    struct RUNTIME_API SStateTreeEvaluatorBase : public SStateTreeNodeBase
    {
        GENERATED_BODY()

        virtual void Tick(FStateTreeExecutionContext& Context) const;
    };
}
