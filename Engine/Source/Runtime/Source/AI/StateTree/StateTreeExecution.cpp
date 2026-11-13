#include "RuntimePCH.h"
#include "StateTreeExecution.h"

#include <limits>

#include "Containers/Algorithm.h"
#include "Core/Math/Math.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "HierarchicalTags/HierarchicalTagRegistry.h"
#include "Log/Log.h"
#include "Memory/Memcpy.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/DotNet/DotNetStateTree.h"

namespace Lumina
{
    namespace StateTreeExecutionInternal
    {
        constexpr double kNever = std::numeric_limits<double>::infinity();

        // Failed outranks Succeeded outranks Running, which is their enum order.
        EStateTreeRunStatus Combine(EStateTreeRunStatus A, EStateTreeRunStatus B)
        {
            return (EStateTreeRunStatus)Math::Max((uint8)A, (uint8)B);
        }
    }

    void FStateTreeExecutionContext::RequestWakeAt(double WakeTime) const
    {
        Instance.TaskWakeTime = Math::Min(Instance.TaskWakeTime, WakeTime);
    }

    bool FStateTreeExecutionContext::SendEvent(uint32 EventTag) const
    {
        return Instance.SendEvent(EventTag);
    }

    FRandomStream& FStateTreeExecutionContext::GetRandom() const
    {
        return Instance.Random;
    }

    void* FStateTreeExecutionContext::GetParameters(const CStruct*& OutType) const
    {
        const FStateTreeParameters& Parameters = Instance.Runtime->Parameters[NodeScope];
        OutType = Parameters.Defaults.GetScriptStruct();
        return OutType != nullptr ? Instance.Block + Parameters.Offset : nullptr;
    }

    int32 FStateTreeExecutionContext::CallScript(EStateTreeScriptOp Op, int32 Default, uint32 EventTag) const
    {
        if (!bScriptNode)
        {
            return Default;
        }
        StateTreeScript::FCall Call = { this, World, NodeData, Time, (uint32)Owner, DeltaTime, EventTag, Default };
        return StateTreeScript::Invoke(Instance.Runtime->ScriptDispatch, NodeDataType, Op, Call);
    }

    EStateTreeRunStatus SStateTreeTaskBase::EnterState(FStateTreeExecutionContext& Context) const
    {
        return (EStateTreeRunStatus)Context.CallScript(EStateTreeScriptOp::EnterState, (int32)EStateTreeRunStatus::Running);
    }

    EStateTreeRunStatus SStateTreeTaskBase::Tick(FStateTreeExecutionContext& Context) const
    {
        return (EStateTreeRunStatus)Context.CallScript(EStateTreeScriptOp::Tick, (int32)EStateTreeRunStatus::Running);
    }

    void SStateTreeTaskBase::ExitState(FStateTreeExecutionContext& Context) const
    {
        Context.CallScript(EStateTreeScriptOp::ExitState, 0);
    }

    void SStateTreeTaskBase::OnEvent(FStateTreeExecutionContext& Context, uint32 EventTag) const
    {
        Context.CallScript(EStateTreeScriptOp::OnEvent, 0, EventTag);
    }

    bool SStateTreeConditionBase::TestCondition(FStateTreeExecutionContext& Context) const
    {
        return Context.CallScript(EStateTreeScriptOp::TestCondition, 1) != 0;
    }

    void SStateTreeEvaluatorBase::Tick(FStateTreeExecutionContext& Context) const
    {
        Context.CallScript(EStateTreeScriptOp::Tick, 0);
    }

    FStateTreeInstance::~FStateTreeInstance()
    {
        Stop(LastTickTime);
    }

    void FStateTreeInstance::ReleaseBlock()
    {
        if (Block == nullptr)
        {
            return;
        }
        for (const FStateTreeParameters& Parameters : Runtime->Parameters)
        {
            if (const CStruct* ParametersType = Parameters.Defaults.GetScriptStruct())
            {
                ParametersType->DestroyStruct(Block + Parameters.Offset);
            }
        }
        for (const FStateTreeNode& Node : Runtime->Nodes)
        {
            if (Node.InstanceDataType != nullptr)
            {
                Node.InstanceDataType->DestroyStruct(Block + Node.InstanceDataOffset);
            }
        }
        for (const FStateTreeCall& Call : Runtime->Calls)
        {
            Call.Function->DestructFrame(Block + Call.FrameOffset);
        }
        void* Memory = Block;
        Memory::Free(Memory);
        Block = nullptr;
    }

    EStateTreeRunStatus FStateTreeInstance::Start(TSharedPtr<const FStateTreeRuntime> InRuntime, CWorld* InWorld, ECS::FEntity InOwner, double Time, const FInstancedStruct* Parameters, uint64 Seed)
    {
        ReleaseBlock();
        Runtime = Move(InRuntime);
        World = InWorld;
        Owner = InOwner;
        Random = FRandomStream(Seed);
        PathDepth = 0;
        NumEvents = 0;
        Pending = {};
        TaskWakeTime = StateTreeExecutionInternal::kNever;
        LastTickTime = Time;
        Status = EStateTreeRunStatus::Failed;
#if !defined(LE_SHIPPING)
        // Earlier records index the states of the runtime this run replaces.
        if (Trace != nullptr)
        {
            Trace->Reset();
        }
#endif
        if (!Runtime->IsValid())
        {
            return Status;
        }

        Block = static_cast<uint8*>(Memory::Malloc(Runtime->InstanceDataSize, Runtime->InstanceDataAlignment));
        for (int32 Index = 0; Index < Runtime->Parameters.Num(); ++Index)
        {
            const FStateTreeParameters& Tree = Runtime->Parameters[Index];
            const CStruct* ParametersType = Tree.Defaults.GetScriptStruct();
            if (ParametersType == nullptr)
            {
                continue;
            }
            // Only the root tree takes the caller's parameters, linked trees take what their linking state passes.
            const bool bOverride = Index == 0 && Parameters != nullptr && Parameters->GetScriptStruct() == ParametersType;
            if (Index == 0 && Parameters != nullptr && !bOverride)
            {
                LOG_WARN("State tree parameters do not match the tree's {}, so the defaults are used.", ParametersType->GetName().ToString());
            }
            ParametersType->InitializeStruct(Block + Tree.Offset);
            ParametersType->CopyStruct(Block + Tree.Offset, (bOverride ? *Parameters : Tree.Defaults).GetMemory());
        }
        for (const FStateTreeNode& Node : Runtime->Nodes)
        {
            InitializeNodeData(Node);
        }
        for (const FStateTreeCall& Call : Runtime->Calls)
        {
            Call.Function->InitializeFrame(Block + Call.FrameOffset);
        }

        Status = EStateTreeRunStatus::Running;
        FStateTreeExecutionContext Context = MakeContext(Time, 0.0f);
        TickEvaluators(Context);
        FPath Path = {};
        int32 Depth = 0;
        if (!SelectChildren(Context, kStateTreeNoIndex, Path, Depth))
        {
            Status = EStateTreeRunStatus::Failed;
            return Status;
        }
        EnterStates(Context, Path, Depth, 0);
        return Status;
    }

    EStateTreeRunStatus FStateTreeInstance::Tick(double Time)
    {
        if (Status == EStateTreeRunStatus::Running && Runtime->bSuperseded)
        {
            Stop(Time);
            return Status;
        }
        if (Status != EStateTreeRunStatus::Running || (IsWaiting() && Time < GetNextWakeTime()))
        {
            return Status;
        }

        FStateTreeExecutionContext Context = MakeContext(Time, (float)(Time - LastTickTime));
        LastTickTime = Time;
        if (Time >= TaskWakeTime)
        {
            TaskWakeTime = StateTreeExecutionInternal::kNever;
        }

        // Events sent while this tick runs wait for the next one.
        const TArray<uint32, kStateTreeMaxEvents> TickEvents = Events;
        const TSpan<const uint32> EventSpan(TickEvents.GetData(), NumEvents);
        NumEvents = 0;

        TickEvaluators(Context);
        for (const uint32 Event : EventSpan)
        {
            TraceChange(Context, EStateTreeTraceKind::Event, kStateTreeNoIndex, Status, Event, PathDepth);
            for (int32 Level = 0; Level < PathDepth; ++Level)
            {
                const FStateTreeRuntimeState& State = Runtime->States[ActivePath[Level].State];
                for (uint16 Index = State.FirstTask; Index < State.FirstTask + State.NumTasks; ++Index)
                {
                    Bind(Context, Index);
                    NodeAs<SStateTreeTaskBase>(Index).OnEvent(Context, Event);
                }
            }
        }
        for (int32 Level = 0; Level < PathDepth; ++Level)
        {
            const FStateTreeRuntimeState& State = Runtime->States[ActivePath[Level].State];
            for (uint16 Index = State.FirstTask; Index < State.FirstTask + State.NumTasks && ActivePath[Level].Status == EStateTreeRunStatus::Running; ++Index)
            {
                Bind(Context, Index);
                ActivePath[Level].Status = StateTreeExecutionInternal::Combine(ActivePath[Level].Status, NodeAs<SStateTreeTaskBase>(Index).Tick(Context));
            }
            RunCopies(State.FirstCopy, State.NumCopies);
        }
        UpdateTransitions(Context, EventSpan);
        return Status;
    }

    void FStateTreeInstance::Stop(double Time)
    {
        if (Block != nullptr && Status == EStateTreeRunStatus::Running)
        {
            FStateTreeExecutionContext Context = MakeContext(Time, (float)(Time - LastTickTime));
            Finish(Context, EStateTreeRunStatus::Failed);
        }
        ReleaseBlock();
    }

    bool FStateTreeInstance::SendEvent(uint32 EventTag)
    {
        if (Status != EStateTreeRunStatus::Running || NumEvents >= kStateTreeMaxEvents)
        {
            return false;
        }
        Events[NumEvents++] = EventTag;
        return true;
    }

    void* FStateTreeInstance::GetParameters(const CStruct* Type) const
    {
        const FStateTreeParameters* Root = Block != nullptr ? &Runtime->Parameters[0] : nullptr;
        const CStruct* RootType = Root != nullptr ? Root->Defaults.GetScriptStruct() : nullptr;
        return RootType != nullptr && RootType->IsChildOf(Type) ? Block + Root->Offset : nullptr;
    }

    bool FStateTreeInstance::IsWaiting() const
    {
        return Status == EStateTreeRunStatus::Running && NumEvents == 0 && !bPathNeedsTick;
    }

    double FStateTreeInstance::GetNextWakeTime() const
    {
        return Math::Min(TaskWakeTime, Pending.Transition != kStateTreeNoIndex ? Pending.FireTime : StateTreeExecutionInternal::kNever);
    }

    FStateTreeExecutionContext FStateTreeInstance::MakeContext(double Time, float DeltaTime)
    {
        SStateTreeContextData& Data = *new (Block) SStateTreeContextData();
        Data.Owner = Owner;
        Data.DeltaTime = DeltaTime;
        return FStateTreeExecutionContext(*this, World, Owner, Time, DeltaTime);
    }

    void FStateTreeInstance::InitializeNodeData(const FStateTreeNode& Node) const
    {
        if (Node.InstanceDataType == nullptr)
        {
            return;
        }
        uint8* Data = Block + Node.InstanceDataOffset;
        Node.InstanceDataType->InitializeStruct(Data);
        if (!Node.bScript)
        {
            if (const void* Defaults = Node.Definition.GetPtr<SStateTreeNodeBase>()->GetInstanceDataDefaults())
            {
                Node.InstanceDataType->CopyStruct(Data, Defaults);
            }
            return;
        }
        // A C# node is its own instance data, so it starts from its authored fields, leaving the base's bindings behind.
        const uint8* Authored = static_cast<const uint8*>(Node.Definition.GetMemory());
        for (const FProperty* Field : static_cast<const CScriptStruct*>(Node.InstanceDataType.Get())->GetOwnProperties())
        {
            Field->CopyCompleteValue(Data + Field->Offset, Authored + Field->Offset);
        }
    }

    void FStateTreeInstance::RunCopies(uint16 First, uint16 Num) const
    {
        for (uint16 Index = First; Index < First + Num; ++Index)
        {
            const FStateTreeCopy& Copy = Runtime->Copies[Index];
            if (Copy.Call != kStateTreeNoIndex)
            {
                const FStateTreeCall& Call = Runtime->Calls[Copy.Call];
                RunCopies(Call.FirstArgument, Call.NumArguments);
                Call.Function->Invoke(Call.Function->IsStatic() ? nullptr : Block + Call.SelfOffset, Block + Call.FrameOffset);
            }
            Memory::Memcpy(Block + Copy.DstOffset, Block + Copy.SrcOffset, Copy.Size);
        }
    }

    void FStateTreeInstance::Bind(FStateTreeExecutionContext& Context, uint16 NodeIndex) const
    {
        const FStateTreeNode& Node = Runtime->Nodes[NodeIndex];
        RunCopies(Node.FirstCopy, Node.NumCopies);
        Context.NodeData = Block + Node.InstanceDataOffset;
        Context.NodeDataType = Node.InstanceDataType.Get();
        Context.NodeScope = Node.Scope;
        Context.bScriptNode = Node.bScript;
    }

    bool FStateTreeInstance::TestConditions(FStateTreeExecutionContext& Context, uint16 First, uint16 Num) const
    {
        bool bResult = true;
        for (uint16 Index = First; Index < First + Num; ++Index)
        {
            const SStateTreeConditionBase& Condition = NodeAs<SStateTreeConditionBase>(Index);
            const bool bAnd = Index == First || Condition.Operand == EStateTreeConditionOperand::And;
            // A false And or a true Or already decides the result, so the condition is skipped.
            if (bAnd != bResult)
            {
                continue;
            }
            Bind(Context, Index);
            bResult = Condition.TestCondition(Context) != Condition.bInvert;
        }
        return bResult;
    }

    bool FStateTreeInstance::SelectState(FStateTreeExecutionContext& Context, uint16 StateIndex, FPath& Path, int32& Depth) const
    {
        const FStateTreeRuntimeState& State = Runtime->States[StateIndex];
        if (!TestConditions(Context, State.FirstEnterCondition, State.NumEnterConditions))
        {
            return false;
        }
        Path[Depth++] = { StateIndex, EStateTreeRunStatus::Running };
        if (SelectChildren(Context, StateIndex, Path, Depth) || State.Kind == EStateTreeStateKind::State)
        {
            return true;
        }
        --Depth;
        return false;
    }

    bool FStateTreeInstance::SelectChildren(FStateTreeExecutionContext& Context, uint16 Parent, FPath& Path, int32& Depth) const
    {
        const uint16 First = Parent == kStateTreeNoIndex ? 0 : Runtime->States[Parent].FirstChild;
        for (uint16 Child = First; Child != kStateTreeNoIndex; Child = Runtime->States[Child].NextSibling)
        {
            if (SelectState(Context, Child, Path, Depth))
            {
                return true;
            }
        }
        return false;
    }

    bool FStateTreeInstance::SelectPathTo(FStateTreeExecutionContext& Context, uint16 Target, FPath& Path, int32& Depth) const
    {
        for (uint16 Ancestor = Runtime->States[Target].Parent; Ancestor != kStateTreeNoIndex; Ancestor = Runtime->States[Ancestor].Parent)
        {
            const FStateTreeRuntimeState& State = Runtime->States[Ancestor];
            const bool bActive = State.Depth < PathDepth && ActivePath[State.Depth].State == Ancestor;
            if (!bActive && !TestConditions(Context, State.FirstEnterCondition, State.NumEnterConditions))
            {
                return false;
            }
            Path[State.Depth] = { Ancestor, EStateTreeRunStatus::Running };
        }
        Depth = Runtime->States[Target].Depth;
        return SelectState(Context, Target, Path, Depth);
    }

    void FStateTreeInstance::ExitStates(FStateTreeExecutionContext& Context, int32 KeepDepth)
    {
        for (int32 Level = PathDepth - 1; Level >= KeepDepth; --Level)
        {
            const FStateTreeRuntimeState& State = Runtime->States[ActivePath[Level].State];
            for (uint16 Index = State.FirstTask + State.NumTasks; Index-- > State.FirstTask;)
            {
                // A reload that removed a C# task empties its definition, and a superseded tree still exits through it.
                if (const SStateTreeTaskBase* Task = Runtime->Nodes[Index].Definition.GetPtr<SStateTreeTaskBase>())
                {
                    Bind(Context, Index);
                    Task->ExitState(Context);
                }
            }
            TraceChange(Context, EStateTreeTraceKind::Exit, ActivePath[Level].State, ActivePath[Level].Status, 0, Level);
        }
        PathDepth = Math::Min(PathDepth, KeepDepth);
        Pending = {};
    }

    void FStateTreeInstance::EnterStates(FStateTreeExecutionContext& Context, const FPath& Path, int32 Depth, int32 KeepDepth)
    {
        for (int32 Level = KeepDepth; Level < Depth; ++Level)
        {
            ActivePath[Level] = Path[Level];
            PathDepth = Level + 1;
            const FStateTreeRuntimeState& State = Runtime->States[Path[Level].State];
            for (uint16 Index = State.FirstTask; Index < State.FirstTask + State.NumTasks; ++Index)
            {
                const FStateTreeNode& Node = Runtime->Nodes[Index];
                if (Node.InstanceDataType != nullptr)
                {
                    Node.InstanceDataType->DestroyStruct(Block + Node.InstanceDataOffset);
                    InitializeNodeData(Node);
                }
                Bind(Context, Index);
                ActivePath[Level].Status = StateTreeExecutionInternal::Combine(ActivePath[Level].Status, NodeAs<SStateTreeTaskBase>(Index).EnterState(Context));
            }
            RunCopies(State.FirstCopy, State.NumCopies);
            TraceChange(Context, EStateTreeTraceKind::Enter, Path[Level].State, ActivePath[Level].Status, 0, Level + 1);
        }

        bPathNeedsTick = false;
        for (int32 Level = 0; Level < PathDepth; ++Level)
        {
            bPathNeedsTick |= Runtime->States[ActivePath[Level].State].bNeedsTick || ActivePath[Level].Status != EStateTreeRunStatus::Running;
        }
    }

    void FStateTreeInstance::ChangeState(FStateTreeExecutionContext& Context, uint16 Target)
    {
        FPath Path = {};
        int32 Depth = 0;
        if (!SelectPathTo(Context, Target, Path, Depth))
        {
            return;
        }

        // The target itself is always re-entered, so a transition to an active state restarts it.
        const int32 TargetDepth = Runtime->States[Target].Depth;
        int32 KeepDepth = 0;
        while (KeepDepth < TargetDepth && KeepDepth < PathDepth && ActivePath[KeepDepth].State == Path[KeepDepth].State)
        {
            ++KeepDepth;
        }
        ExitStates(Context, KeepDepth);
        EnterStates(Context, Path, Depth, KeepDepth);
    }

    void FStateTreeInstance::Finish(FStateTreeExecutionContext& Context, EStateTreeRunStatus Result)
    {
        ExitStates(Context, 0);
        Status = Result;
        TraceChange(Context, EStateTreeTraceKind::Finished, kStateTreeNoIndex, Result, 0, 0);
    }

    void FStateTreeInstance::TickEvaluators(FStateTreeExecutionContext& Context)
    {
        for (uint16 Index = 0; Index < Runtime->NumEvaluators; ++Index)
        {
            Bind(Context, Index);
            NodeAs<SStateTreeEvaluatorBase>(Index).Tick(Context);
        }
    }

    bool FStateTreeInstance::IsTriggered(const FStateTreeRuntimeTransition& Transition, EStateTreeRunStatus Completion, TSpan<const uint32> TickEvents) const
    {
        switch (Transition.Trigger)
        {
            case EStateTreeTransitionTrigger::OnCompleted:
                return Completion != EStateTreeRunStatus::Running;
            case EStateTreeTransitionTrigger::OnSucceeded:
                return Completion == EStateTreeRunStatus::Succeeded;
            case EStateTreeTransitionTrigger::OnFailed:
                return Completion == EStateTreeRunStatus::Failed;
            case EStateTreeTransitionTrigger::OnEvent:
            {
                const FHierarchicalTagRegistry& Registry = FHierarchicalTagRegistry::Get();
                return Algo::AnyOf(TickEvents, [&](uint32 Event) { return Registry.Matches(Event, Transition.EventTag); });
            }
            case EStateTreeTransitionTrigger::OnTick:
                return true;
        }
        return false;
    }

    void FStateTreeInstance::UpdateTransitions(FStateTreeExecutionContext& Context, TSpan<const uint32> TickEvents)
    {
        // A state completes when it or any state below it does, so completion bubbles up to the root.
        EStateTreeRunStatus Completion = EStateTreeRunStatus::Running;
        uint16 Best = kStateTreeNoIndex;
        for (int32 Level = PathDepth - 1; Level >= 0; --Level)
        {
            Completion = StateTreeExecutionInternal::Combine(Completion, ActivePath[Level].Status);
            const FStateTreeRuntimeState& State = Runtime->States[ActivePath[Level].State];
            for (uint16 Index = State.FirstTransition; Index < State.FirstTransition + State.NumTransitions; ++Index)
            {
                const FStateTreeRuntimeTransition& Transition = Runtime->Transitions[Index];
                if ((Best == kStateTreeNoIndex || Transition.Priority > Runtime->Transitions[Best].Priority)
                    && IsTriggered(Transition, Completion, TickEvents)
                    && TestConditions(Context, Transition.FirstCondition, Transition.NumConditions))
                {
                    Best = Index;
                }
            }
        }

        if (Best != kStateTreeNoIndex)
        {
            const FStateTreeRuntimeTransition& Transition = Runtime->Transitions[Best];
            if (Transition.Delay <= 0.0f && Transition.DelayRandomRange <= 0.0f)
            {
                ExecuteTransition(Context, Best);
                return;
            }
            if (Pending.Transition != Best && (Pending.Transition == kStateTreeNoIndex || Transition.Priority > Runtime->Transitions[Pending.Transition].Priority))
            {
                const float Delay = Random.RandRange(Transition.Delay - Transition.DelayRandomRange, Transition.Delay + Transition.DelayRandomRange);
                Pending = { Best, Context.GetTime() + Math::Max(Delay, 0.0f) };
            }
        }

        if (Pending.Transition != kStateTreeNoIndex)
        {
            if (Context.GetTime() >= Pending.FireTime)
            {
                ExecuteTransition(Context, Pending.Transition);
            }
            return;
        }
        if (Completion != EStateTreeRunStatus::Running)
        {
            Finish(Context, Completion);
        }
    }

    void FStateTreeInstance::ExecuteTransition(FStateTreeExecutionContext& Context, uint16 TransitionIndex)
    {
        const FStateTreeRuntimeTransition& Transition = Runtime->Transitions[TransitionIndex];
        Pending = {};
        TraceChange(Context, EStateTreeTraceKind::Transition, Transition.TargetState, Status, TransitionIndex, PathDepth);
        if (Transition.Target == EStateTreeTransitionTarget::State)
        {
            ChangeState(Context, Transition.TargetState);
        }
        else
        {
            const EStateTreeRunStatus Result = Transition.Target == EStateTreeTransitionTarget::Succeeded ? EStateTreeRunStatus::Succeeded : EStateTreeRunStatus::Failed;
            if (Transition.TargetState == kStateTreeNoIndex)
            {
                Finish(Context, Result);
                return;
            }
            // A linked tree finishing completes its linking state, whose transitions see that on the next tick.
            const int32 Level = Runtime->States[Transition.TargetState].Depth;
            ExitStates(Context, Level + 1);
            ActivePath[Level].Status = Result;
            bPathNeedsTick = true;
        }
    }
}
