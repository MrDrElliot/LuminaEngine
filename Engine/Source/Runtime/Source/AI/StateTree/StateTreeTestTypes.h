#pragma once

#include "StateTree.h"
#include "StateTreeExecution.h"
#include "Containers/Algorithm.h"
#include "Containers/Format.h"
#include "Core/Object/FunctionLibrary.h"
#include "StateTreeTestTypes.generated.h"

namespace Lumina
{
    // Throwaway nodes the state tree tests run, each writing what it sees into a log the test owns.

    struct FStateTreeTestLog
    {
        TVector<FString> Lines;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeTestParameters : public SStateTreeParametersBase
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        int32 Speed = 0;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeTestTaskData
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        int32 Input = 0;

        PROPERTY(Hidden)
        int32 Output = 0;

        PROPERTY(Hidden)
        int32 TickCount = 0;

        PROPERTY(Hidden)
        float DeltaTime = 0.0f;

        PROPERTY(Hidden)
        ECS::FEntity Owner = ECS::NullEntity;

        PROPERTY(Hidden)
        double EndTime = 0.0;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeTestTask : public SStateTreeTaskBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeTestTaskData::StaticStruct(); }

        EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context) const override
        {
            SStateTreeTestTaskData& Data = Context.GetInstanceData<SStateTreeTestTaskData>();
            Data.Output = Data.Input + OutputOffset;
            if (WakeAfter > 0.0f)
            {
                Data.EndTime = Context.GetTime() + WakeAfter;
                Context.RequestWakeAt(Data.EndTime);
            }
            Record("Enter");
            return EStateTreeRunStatus::Running;
        }

        EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context) const override
        {
            SStateTreeTestTaskData& Data = Context.GetInstanceData<SStateTreeTestTaskData>();
            Data.Output = Data.Input + OutputOffset;
            ++Data.TickCount;
            Record("Tick");
            const bool bDone = WakeAfter > 0.0f ? Context.GetTime() >= Data.EndTime : (TicksToFinish > 0 && Data.TickCount >= TicksToFinish);
            return bDone ? Result : EStateTreeRunStatus::Running;
        }

        void ExitState(FStateTreeExecutionContext& Context) const override { Record("Exit"); }

        void OnEvent(FStateTreeExecutionContext& Context, uint32 EventTag) const override { Record("Event"); }

        bool NeedsTick() const override { return bNeedsTick; }

        void Record(const char* What) const
        {
            if (Log != nullptr)
            {
                Log->Lines.Add(Format("{} {}", What, Name.ToString()));
            }
        }

        // Zero runs until a transition leaves the state.
        PROPERTY(Editable)
        int32 TicksToFinish = 0;

        PROPERTY(Editable)
        EStateTreeRunStatus Result = EStateTreeRunStatus::Succeeded;

        PROPERTY(Editable)
        int32 OutputOffset = 0;

        // Finishes this long after entering, asking to be woken then instead of ticking.
        PROPERTY(Editable)
        float WakeAfter = 0.0f;

        PROPERTY(Editable)
        bool bNeedsTick = true;

        FStateTreeTestLog* Log = nullptr;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeTestConditionData
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        int32 Value = 0;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeTestCondition : public SStateTreeConditionBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeTestConditionData::StaticStruct(); }

        bool TestCondition(FStateTreeExecutionContext& Context) const override
        {
            return bPass && Context.GetInstanceData<SStateTreeTestConditionData>().Value >= MinValue;
        }

        PROPERTY(Editable)
        bool bPass = true;

        PROPERTY(Editable)
        int32 MinValue = 0;
    };

    REFLECT()
    struct RUNTIME_API SStateTreeTestEvaluatorData
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        int32 Counter = 0;

        FUNCTION()
        int32 GetDoubledCounter() const { return Counter * 2; }
    };

    REFLECT()
    class RUNTIME_API CStateTreeTestLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        FUNCTION()
        static int32 AddInts(int32 A, int32 B) { return A + B; }

        FUNCTION()
        static void ReturnsNothing() {}
    };

    REFLECT()
    struct RUNTIME_API SStateTreeTestEvaluator : public SStateTreeEvaluatorBase
    {
        GENERATED_BODY()

        CStruct* GetInstanceDataType() const override { return SStateTreeTestEvaluatorData::StaticStruct(); }

        void Tick(FStateTreeExecutionContext& Context) const override { ++Context.GetInstanceData<SStateTreeTestEvaluatorData>().Counter; }
    };

    namespace StateTreeTest
    {
        inline int32 AddState(CStateTree& Tree, const char* Name, const char* Parent = nullptr, EStateTreeStateKind Kind = EStateTreeStateKind::State)
        {
            SStateTreeState& State = Tree.States.AddDefaultedGetRef();
            State.Name = FName(Name);
            State.Parent = Parent != nullptr ? FName(Parent) : FName();
            State.Kind = Kind;
            return Tree.States.Num() - 1;
        }

        // The returned node lives in Slots, so finish with it before adding to the same list again.
        template<typename T, typename TBase>
        T& AddNode(TVector<TInstancedStruct<TBase>>& Slots, const char* Name = "")
        {
            T& Node = Slots.AddDefaultedGetRef().template InitializeAs<T>();
            Node.Name = FName(Name);
            return Node;
        }

        inline SStateTreeTransition& AddTransition(SStateTreeState& State, EStateTreeTransitionTrigger Trigger, EStateTreeTransitionTarget Target, const char* TargetState = nullptr)
        {
            SStateTreeTransition& Transition = State.Transitions.AddDefaultedGetRef();
            Transition.Trigger = Trigger;
            Transition.Target = Target;
            Transition.TargetState = TargetState != nullptr ? FName(TargetState) : FName();
            return Transition;
        }

        inline void SetPath(SStateTreeBindingPath& Path, EStateTreeBindingSource Source, const char* SourceNode, const char* SourceField)
        {
            Path.Source = Source;
            Path.SourceNode = FName(SourceNode);
            Path.SourceField = FName(SourceField);
        }

        inline void AddArgument(SStateTreeBinding& Binding, EStateTreeBindingSource Source, const char* SourceNode, const char* SourceField)
        {
            SetPath(Binding.Arguments.AddDefaultedGetRef(), Source, SourceNode, SourceField);
        }

        inline SStateTreeBinding& Bind(TVector<SStateTreeBinding>& Bindings, const char* TargetField, EStateTreeBindingSource Source, const char* SourceNode, const char* SourceField)
        {
            SStateTreeBinding& Binding = Bindings.AddDefaultedGetRef();
            Binding.TargetField = FName(TargetField);
            SetPath(Binding, Source, SourceNode, SourceField);
            return Binding;
        }

        inline SStateTreeBinding& Bind(SStateTreeNodeBase& Node, const char* TargetField, EStateTreeBindingSource Source, const char* SourceNode, const char* SourceField)
        {
            return Bind(Node.Bindings, TargetField, Source, SourceNode, SourceField);
        }

        inline uint16 FindState(const FStateTreeRuntime& Runtime, const char* Name)
        {
            return (uint16)Algo::IndexOf(Runtime.States, FName(Name), &FStateTreeRuntimeState::Name);
        }

        // The active path as "Root/Child/Leaf".
        inline FString PathOf(const FStateTreeInstance& Instance, const FStateTreeRuntime& Runtime)
        {
            FString Path;
            for (const FStateTreeActiveState& Active : Instance.GetActivePath())
            {
                Path += Path.IsEmpty() ? FString() : FString("/");
                Path += Runtime.States[Active.State].Name.ToString();
            }
            return Path;
        }
    }
}
