#include "RuntimePCH.h"
#include "StateTreeCompiler.h"

#include "StateTree.h"
#include "Containers/Algorithm.h"
#include "Containers/Format.h"
#include "Core/Math/Math.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Templates/Align.h"
#include "Scripting/DotNet/DotNetStateTree.h"

namespace Lumina
{
    namespace StateTreeCompilerInternal
    {
        // A struct reports owned storage whenever it has a constructor, so its fields decide instead.
        bool IsByteCopyable(const FProperty& Property)
        {
            if (Property.IsA(EPropertyTypeFlags::Struct))
            {
                const CStruct* Struct = static_cast<const FStructProperty&>(Property).GetStruct();
                return Algo::AllOf(Struct->GetProperties(), [](const FProperty* Field) { return IsByteCopyable(*Field); });
            }
            return !Property.OwnsStorage();
        }
    }

    static_assert(std::is_trivially_copyable_v<FName> && std::is_trivially_copyable_v<ECS::FEntity>, "Bindings copy names and entities as bytes.");

    bool FStateTreeCompiler::Compile(const CStateTree& Tree, FStateTreeRuntime& Out)
    {
        Errors.Reset();
        SourceStates.Reset();
        NodeStates.Reset();
        ScopeTrees.Reset();
        TreeStack.Reset();
        Out = FStateTreeRuntime();
        Runtime = &Out;

        Allocate(SStateTreeContextData::StaticStruct());
        AddStates(Tree, Tree.Parameters, kStateTreeNoIndex, 0);
        if (Errors.IsEmpty())
        {
            AddNodes();
            AddTransitions();
            ResolveBindings();
        }

        if (Out.States.Num() >= kStateTreeNoIndex || Out.Nodes.Num() >= kStateTreeNoIndex || Out.Copies.Num() >= kStateTreeNoIndex || Out.Calls.Num() >= kStateTreeNoIndex || Out.Transitions.Num() >= kStateTreeNoIndex)
        {
            Error("The tree has more states, nodes, bindings, calls or transitions than a 16 bit index holds.");
        }

        Runtime = nullptr;
        if (!Errors.IsEmpty())
        {
            Out = FStateTreeRuntime();
            return false;
        }
        return true;
    }

    bool FStateTreeCompiler::CanBind(const FProperty& Target, const FProperty& Source)
    {
        return Target.HasSameValueType(&Source) && StateTreeCompilerInternal::IsByteCopyable(Target);
    }

    void FStateTreeCompiler::Error(FString Message)
    {
        Errors.Add(std::move(Message));
    }

    uint32 FStateTreeCompiler::Allocate(uint32 Size, uint32 Alignment)
    {
        Alignment = Math::Max(Alignment, 1u);
        const uint32 Offset = Align(Runtime->InstanceDataSize, Alignment);
        Runtime->InstanceDataSize = Offset + Size;
        Runtime->InstanceDataAlignment = Math::Max(Runtime->InstanceDataAlignment, Alignment);
        return Offset;
    }

    uint32 FStateTreeCompiler::Allocate(const CStruct* Type)
    {
        return Allocate(Type->GetSize(), Type->GetAlignment());
    }

    FString FStateTreeCompiler::DescribeNode(uint16 NodeIndex) const
    {
        const SStateTreeNodeBase* Node = Runtime->Nodes[NodeIndex].Definition.GetPtr<SStateTreeNodeBase>();
        const FString NodeName = Node != nullptr ? Node->Name.ToString() : FString();
        const uint16 State = NodeStates[NodeIndex];
        return State == kStateTreeNoIndex ? Format("Evaluator '{}'", NodeName) : Format("Node '{}' in state '{}'", NodeName, Runtime->States[State].Name.ToString());
    }

    void FStateTreeCompiler::AddStates(const CStateTree& Tree, const FInstancedStruct& Parameters, uint16 RuntimeParent, uint8 Depth)
    {
        if (Tree.Version > kStateTreeVersion)
        {
            Error(Format("Saved as state tree version {}, newer than this build's {}.", Tree.Version, kStateTreeVersion));
        }
        const int32 NumStates = Tree.States.Num();
        if (NumStates == 0)
        {
            Error("The tree has no states.");
            return;
        }

        const uint16 Scope = (uint16)Runtime->Parameters.Num();
        FStateTreeParameters& TreeParameters = Runtime->Parameters.AddDefaultedGetRef();
        if (const CStruct* ParametersType = Tree.Parameters.GetScriptStruct())
        {
            TreeParameters.Defaults = Parameters.GetScriptStruct() != nullptr ? Parameters : Tree.Parameters;
            TreeParameters.Offset = Allocate(ParametersType);
        }
        ScopeTrees.Add(&Tree);
        TreeStack.Add(&Tree);
        const int32 NumErrors = Errors.Num();

        TVector<int32> Parents(NumStates, Constants::kIndexNone);
        for (int32 Index = 0; Index < NumStates; ++Index)
        {
            const SStateTreeState& State = Tree.States[Index];
            const FString StateName = State.Name.ToString();
            if (State.Name.IsNone())
            {
                Error(Format("State {} has no name.", Index));
            }
            else if (Algo::IndexOf(Tree.States, State.Name, &SStateTreeState::Name) != Index)
            {
                Error(Format("State name '{}' is used twice.", StateName));
            }
            if (!State.Parent.IsNone())
            {
                Parents[Index] = Algo::IndexOf(Tree.States, State.Parent, &SStateTreeState::Name);
                if (Parents[Index] == Constants::kIndexNone)
                {
                    Error(Format("State '{}' names a missing parent '{}'.", StateName, State.Parent.ToString()));
                }
            }
            if (Parents[Index] != Constants::kIndexNone && Tree.States[Parents[Index]].Kind == EStateTreeStateKind::LinkedSubtree)
            {
                Error(Format("State '{}' is a child of linked subtree '{}', which takes its children from the linked tree.", StateName, State.Parent.ToString()));
            }
            if (State.Kind != EStateTreeStateKind::LinkedSubtree)
            {
                continue;
            }
            const CStateTree* Linked = State.LinkedSubtree.Get();
            const CStruct* LinkedType = State.LinkedParameters.GetScriptStruct();
            if (Linked == nullptr)
            {
                Error(Format("State '{}' is a linked subtree with no tree to link.", StateName));
            }
            else if (Algo::Contains(TreeStack, Linked))
            {
                Error(Format("State '{}' links '{}', which links back to itself in a cycle.", StateName, Linked->GetName().ToString()));
            }
            else if (LinkedType != nullptr && LinkedType != Linked->Parameters.GetScriptStruct())
            {
                Error(Format("State '{}' passes {}, which is not the parameter struct of '{}'.", StateName, LinkedType->GetName().ToString(), Linked->GetName().ToString()));
            }
        }
        if (Errors.Num() > NumErrors)
        {
            TreeStack.Pop();
            return;
        }

        int32 NumReached = 0;
        auto AddChildren = [&](this auto& Self, int32 Parent, uint16 RuntimeParent, uint8 Depth) -> void
        {
            uint16 Previous = kStateTreeNoIndex;
            for (int32 Index = 0; Index < NumStates; ++Index)
            {
                if (Parents[Index] != Parent)
                {
                    continue;
                }
                if (Depth >= kStateTreeMaxDepth)
                {
                    Error(Format("State '{}' is deeper than the {} levels an active path holds.", Tree.States[Index].Name.ToString(), kStateTreeMaxDepth));
                    return;
                }

                const SStateTreeState& Source = Tree.States[Index];
                const uint16 RuntimeIndex = (uint16)Runtime->States.Num();
                FStateTreeRuntimeState& State = Runtime->States.AddDefaultedGetRef();
                State.Name = Source.Name;
                State.Kind = Source.Kind;
                State.Parent = RuntimeParent;
                State.Scope = Scope;
                State.Depth = Depth;
                SourceStates.Add(&Source);
                ++NumReached;

                if (Previous != kStateTreeNoIndex)
                {
                    Runtime->States[Previous].NextSibling = RuntimeIndex;
                }
                else if (RuntimeParent != kStateTreeNoIndex)
                {
                    Runtime->States[RuntimeParent].FirstChild = RuntimeIndex;
                }
                Previous = RuntimeIndex;
                if (Source.Kind == EStateTreeStateKind::LinkedSubtree)
                {
                    AddStates(*Source.LinkedSubtree, Source.LinkedParameters, RuntimeIndex, Depth + 1);
                }
                else
                {
                    Self(Index, RuntimeIndex, Depth + 1);
                }
            }
        };
        AddChildren(Constants::kIndexNone, RuntimeParent, Depth);
        TreeStack.Pop();

        for (int32 Index = 0; Index < NumStates && NumReached < NumStates; ++Index)
        {
            if (!Algo::Contains(SourceStates, &Tree.States[Index]))
            {
                Error(Format("State '{}' is never reached because its parent chain loops.", Tree.States[Index].Name.ToString()));
            }
        }
    }

    void FStateTreeCompiler::AddNode(const FInstancedStruct& Definition, uint16 State, uint16 Scope, const char* Slot)
    {
        FStateTreeNode& Node = Runtime->Nodes.AddDefaultedGetRef();
        Node.Definition = Definition;
        NodeStates.Add(State);
        Node.Scope = Scope;

        const SStateTreeNodeBase* Base = Node.Definition.GetPtr<SStateTreeNodeBase>();
        if (Base == nullptr)
        {
            Error(Format("{} has an empty {}.", State == kStateTreeNoIndex ? FString("The tree") : Format("State '{}'", Runtime->States[State].Name.ToString()), Slot));
            return;
        }
        CStruct* DefinitionType = Node.Definition.GetScriptStruct();
        Node.bScript = StateTreeScript::IsScriptNode(DefinitionType);
        if (Node.bScript && !StateTreeScript::ExtendsNodeBase(DefinitionType))
        {
            Error(Format("C# {} '{}' does not extend SStateTreeTaskBase, SStateTreeConditionBase or SStateTreeEvaluatorBase directly.", Slot, DefinitionType->GetName().ToString()));
            return;
        }
        if (CStruct* Type = Node.bScript ? DefinitionType : Base->GetInstanceDataType())
        {
            Node.InstanceDataType = Type;
            Node.InstanceDataOffset = Allocate(Type);
        }
        if (Node.bScript && Runtime->ScriptDispatch == nullptr)
        {
            Runtime->ScriptDispatch = StateTreeScript::ResolveDispatch();
        }
    }

    void FStateTreeCompiler::AddNodes()
    {
        for (uint16 Scope = 0; Scope < ScopeTrees.Num(); ++Scope)
        {
            for (const TInstancedStruct<SStateTreeEvaluatorBase>& Evaluator : ScopeTrees[Scope]->Evaluators)
            {
                AddNode(Evaluator, kStateTreeNoIndex, Scope, "evaluator");
            }
        }
        Runtime->NumEvaluators = (uint16)Runtime->Nodes.Num();

        for (uint16 Index = 0; Index < Runtime->States.Num(); ++Index)
        {
            const SStateTreeState& Source = *SourceStates[Index];
            FStateTreeRuntimeState& State = Runtime->States[Index];

            State.FirstEnterCondition = (uint16)Runtime->Nodes.Num();
            for (const TInstancedStruct<SStateTreeConditionBase>& Condition : Source.EnterConditions)
            {
                AddNode(Condition, Index, State.Scope, "enter condition");
            }
            State.NumEnterConditions = (uint16)(Runtime->Nodes.Num() - State.FirstEnterCondition);

            State.FirstTask = (uint16)Runtime->Nodes.Num();
            for (const TInstancedStruct<SStateTreeTaskBase>& Task : Source.Tasks)
            {
                AddNode(Task, Index, State.Scope, "task");
                const FStateTreeNode& Node = Runtime->Nodes.Last();
                State.bNeedsTick |= Node.bScript ? StateTreeScript::NeedsTick(Runtime->ScriptDispatch, Node.InstanceDataType.Get()) : Task.Get() != nullptr && Task.Get()->NeedsTick();
            }
            State.NumTasks = (uint16)(Runtime->Nodes.Num() - State.FirstTask);
        }
    }

    void FStateTreeCompiler::AddTransitions()
    {
        for (uint16 Index = 0; Index < Runtime->States.Num(); ++Index)
        {
            const SStateTreeState& Source = *SourceStates[Index];
            const FString StateName = Source.Name.ToString();
            Runtime->States[Index].FirstTransition = (uint16)Runtime->Transitions.Num();
            Runtime->States[Index].NumTransitions = (uint16)Source.Transitions.Num();

            for (const SStateTreeTransition& Authored : Source.Transitions)
            {
                const FStateTreeRuntimeState& State = Runtime->States[Index];
                FStateTreeRuntimeTransition Transition;
                Transition.Trigger = Authored.Trigger;
                Transition.Target = Authored.Target;
                Transition.Priority = Authored.Priority;
                Transition.Delay = Authored.Delay;
                Transition.DelayRandomRange = Authored.DelayRandomRange;

                switch (Authored.Target)
                {
                    case EStateTreeTransitionTarget::State:
                    {
                        const int32 Found = Algo::IndexOfIf(Runtime->States, [&](const FStateTreeRuntimeState& Candidate)
                        {
                            return Candidate.Name == Authored.TargetState && Candidate.Scope == State.Scope;
                        });
                        Transition.TargetState = Found == Constants::kIndexNone ? kStateTreeNoIndex : (uint16)Found;
                        break;
                    }
                    case EStateTreeTransitionTarget::NextSibling:
                        Transition.TargetState = State.NextSibling;
                        break;
                    case EStateTreeTransitionTarget::Parent:
                        Transition.TargetState = State.Parent;
                        break;
                    default:
                        // The linking state, the first ancestor outside this state's tree, or none in the root tree.
                        Transition.TargetState = Index;
                        while (Transition.TargetState != kStateTreeNoIndex && Runtime->States[Transition.TargetState].Scope == State.Scope)
                        {
                            Transition.TargetState = Runtime->States[Transition.TargetState].Parent;
                        }
                        break;
                }
                const bool bTargetsState = Authored.Target != EStateTreeTransitionTarget::Succeeded && Authored.Target != EStateTreeTransitionTarget::Failed;
                if (bTargetsState)
                {
                    Transition.Target = EStateTreeTransitionTarget::State;
                    if (Transition.TargetState == kStateTreeNoIndex)
                    {
                        const FString Missing = Authored.Target == EStateTreeTransitionTarget::State ? Format("state '{}'", Authored.TargetState.ToString())
                            : FString(Authored.Target == EStateTreeTransitionTarget::Parent ? "parent" : "next sibling");
                        Error(Format("State '{}' has a transition to a missing {}.", StateName, Missing));
                    }
                }

                if (Authored.Trigger == EStateTreeTransitionTrigger::OnEvent)
                {
                    Transition.EventTag = Authored.EventTag.GetId();
                    if (Transition.EventTag == 0)
                    {
                        Error(Format("State '{}' has an event transition with no tag.", StateName));
                    }
                }
                Runtime->States[Index].bNeedsTick |= Authored.Trigger == EStateTreeTransitionTrigger::OnTick;

                Transition.FirstCondition = (uint16)Runtime->Nodes.Num();
                for (const TInstancedStruct<SStateTreeConditionBase>& Condition : Authored.Conditions)
                {
                    AddNode(Condition, Index, State.Scope, "transition condition");
                }
                Transition.NumConditions = (uint16)(Runtime->Nodes.Num() - Transition.FirstCondition);
                Runtime->Transitions.Add(Transition);
            }
        }
    }

    void FStateTreeCompiler::ResolveBindings()
    {
        for (uint16 NodeIndex = 0; NodeIndex < Runtime->Nodes.Num(); ++NodeIndex)
        {
            FStateTreeNode& Node = Runtime->Nodes[NodeIndex];
            if (const SStateTreeNodeBase* Definition = Node.Definition.GetPtr<SStateTreeNodeBase>())
            {
                ResolveBindings(Definition->Bindings, { DescribeNode(NodeIndex), NodeIndex, NodeStates[NodeIndex], Node.Scope, Node.InstanceDataType.Get(), Node.InstanceDataOffset }, Node.FirstCopy, Node.NumCopies);
            }
        }

        for (uint16 Index = 0; Index < Runtime->States.Num(); ++Index)
        {
            FStateTreeRuntimeState& State = Runtime->States[Index];
            if (State.Kind == EStateTreeStateKind::LinkedSubtree)
            {
                // Read after the state's own tasks entered or ticked, before the linked tree's states run.
                const FStateTreeParameters& Linked = Runtime->Parameters[Runtime->States[State.FirstChild].Scope];
                const FBindingReader Reader = { Format("State '{}' parameters", State.Name.ToString()), (uint16)(State.FirstTask + State.NumTasks), Index, State.Scope, Linked.Defaults.GetScriptStruct(), Linked.Offset };
                ResolveBindings(SourceStates[Index]->ParameterBindings, Reader, State.FirstCopy, State.NumCopies);
            }
        }
    }

    void FStateTreeCompiler::ResolveBindings(const TVector<SStateTreeBinding>& Bindings, const FBindingReader& Reader, uint16& FirstCopy, uint16& NumCopies)
    {
        TVector<FStateTreeCopy> Batch;
        for (const SStateTreeBinding& Binding : Bindings)
        {
            if (const TOptional<FStateTreeCopy> Copy = ResolveBinding(Reader, Binding))
            {
                Batch.Add(*Copy);
            }
        }
        FirstCopy = (uint16)Runtime->Copies.Num();
        NumCopies = (uint16)Batch.Num();
        Runtime->Copies.Append(Batch);
    }

    bool FStateTreeCompiler::IsVisibleSource(uint16 Source, const FBindingReader& Reader, EStateTreeBindingSource Kind) const
    {
        const uint16 ReaderState = Reader.State;
        const uint16 SourceState = NodeStates[Source];
        if (Runtime->Nodes[Source].Scope != Reader.Scope)
        {
            return false;
        }
        if (Kind == EStateTreeBindingSource::Evaluator)
        {
            return Source < Runtime->NumEvaluators && (ReaderState != kStateTreeNoIndex || Source < Reader.Node);
        }

        const FStateTreeRuntimeState* Owner = SourceState != kStateTreeNoIndex ? &Runtime->States[SourceState] : nullptr;
        if (Owner == nullptr || ReaderState == kStateTreeNoIndex || Source < Owner->FirstTask || Source >= Owner->FirstTask + Owner->NumTasks)
        {
            return false;
        }
        if (SourceState == ReaderState)
        {
            // Enter conditions precede the state's tasks and transition conditions follow them.
            return Source < Reader.Node;
        }
        for (uint16 Ancestor = Runtime->States[ReaderState].Parent; Ancestor != kStateTreeNoIndex; Ancestor = Runtime->States[Ancestor].Parent)
        {
            if (Ancestor == SourceState)
            {
                return true;
            }
        }
        return false;
    }

    const CStruct* FStateTreeCompiler::FindSource(const FBindingReader& Reader, const SStateTreeBindingPath& Path, uint32& OutOffset) const
    {
        OutOffset = 0;
        switch (Path.Source)
        {
            case EStateTreeBindingSource::Parameter:
                OutOffset = Runtime->Parameters[Reader.Scope].Offset;
                return Runtime->Parameters[Reader.Scope].Defaults.GetScriptStruct();
            case EStateTreeBindingSource::Context:
                return SStateTreeContextData::StaticStruct();
            case EStateTreeBindingSource::Function:
                return FindObject<CStruct>(Path.SourceNode);
            default:
                // The nearest match wins, so a deeper task shadows an ancestor's task of the same name.
                for (uint16 Source = Reader.Node; Source-- > 0;)
                {
                    const SStateTreeNodeBase* Candidate = Runtime->Nodes[Source].Definition.GetPtr<SStateTreeNodeBase>();
                    if (Candidate != nullptr && Candidate->Name == Path.SourceNode && IsVisibleSource(Source, Reader, Path.Source))
                    {
                        OutOffset = Runtime->Nodes[Source].InstanceDataOffset;
                        return Runtime->Nodes[Source].InstanceDataType.Get();
                    }
                }
                return nullptr;
        }
    }

    TOptional<FStateTreeCopy> FStateTreeCompiler::ResolveBinding(const FBindingReader& Reader, const SStateTreeBinding& Binding)
    {
        const FProperty* Target = Reader.Type != nullptr ? Reader.Type->GetProperty(Binding.TargetField) : nullptr;
        if (Target == nullptr)
        {
            Error(Format("{} binds a missing field '{}'.", Reader.Description, Binding.TargetField.ToString()));
            return {};
        }

        uint32 SourceOffset = 0;
        const CStruct* SourceType = FindSource(Reader, Binding, SourceOffset);
        const bool bFunctionSource = Binding.Source == EStateTreeBindingSource::Function;
        const FProperty* SourceField = SourceType != nullptr && !bFunctionSource ? SourceType->GetProperty(Binding.SourceField) : nullptr;
        const FFunction* Function = SourceType != nullptr && SourceField == nullptr ? SourceType->FindFunction(Binding.SourceField) : nullptr;
        if (Function != nullptr)
        {
            const char* Unusable = !Function->HasReturn() ? "returns nothing" : bFunctionSource && !Function->IsStatic() ? "is not static" : nullptr;
            if (Unusable != nullptr)
            {
                Error(Format("{} binds '{}' to function '{}', which {}.", Reader.Description, Binding.TargetField.ToString(), Binding.SourceField.ToString(), Unusable));
                return {};
            }
            SourceField = Function->GetReturnParam();
        }
        if (SourceField == nullptr)
        {
            Error(Format("{} binds '{}' to {} '{}.{}', which is missing or not visible from there.", Reader.Description, Binding.TargetField.ToString(),
                bFunctionSource ? "unknown function" : "field", Binding.SourceNode.ToString(), Binding.SourceField.ToString()));
            return {};
        }
        if (!Target->HasSameValueType(SourceField))
        {
            Error(Format("{} binds '{}' of type {} to '{}' of type {}.", Reader.Description,
                Binding.TargetField.ToString(), Target->GetTypeName().ToString(), Binding.SourceField.ToString(), SourceField->GetTypeName().ToString()));
            return {};
        }
        if (!StateTreeCompilerInternal::IsByteCopyable(*Target))
        {
            Error(Format("{} binds '{}', which is not a plain value a binding can copy.", Reader.Description, Binding.TargetField.ToString()));
            return {};
        }

        FStateTreeCopy Copy = { SourceOffset + SourceField->Offset, Reader.Offset + Target->Offset, (uint32)Target->GetElementSize() };
        if (Function != nullptr)
        {
            const TOptional<uint16> Call = AddCall(Reader, Binding, *Function, SourceOffset);
            if (!Call)
            {
                return {};
            }
            Copy.Call = *Call;
            Copy.SrcOffset = Runtime->Calls[*Call].FrameOffset + SourceField->Offset;
        }
        return Copy;
    }

    TOptional<uint16> FStateTreeCompiler::AddCall(const FBindingReader& Reader, const SStateTreeBinding& Binding, const FFunction& Function, uint32 SelfOffset)
    {
        const TSpan<FProperty* const> Parameters = Function.GetArguments();
        const FString FunctionName = Function.GetFunctionName().ToString();
        if (Binding.Arguments.Num() != Parameters.Num())
        {
            Error(Format("{} calls '{}' with {} arguments, and it takes {}.", Reader.Description, FunctionName, Binding.Arguments.Num(), Parameters.Num()));
            return {};
        }

        const FStateTreeCall Call = { &Function, SelfOffset, Allocate(Function.GetParmsSize(), FFunction::kFrameAlignment), (uint16)Runtime->Copies.Num(), (uint16)Parameters.Num() };
        const int32 NumErrors = Errors.Num();
        for (int32 Index = 0; Index < Parameters.Num(); ++Index)
        {
            const SStateTreeBindingPath& Argument = Binding.Arguments[Index];
            const FProperty* Parameter = Parameters[Index];
            uint32 Offset = 0;
            const CStruct* Type = Argument.Source != EStateTreeBindingSource::Function ? FindSource(Reader, Argument, Offset) : nullptr;
            const FProperty* Field = Type != nullptr ? Type->GetProperty(Argument.SourceField) : nullptr;
            if (Field == nullptr)
            {
                Error(Format("{} passes '{}.{}' to '{}', which is not a field visible from there.", Reader.Description, Argument.SourceNode.ToString(), Argument.SourceField.ToString(), FunctionName));
            }
            else if (!CanBind(*Parameter, *Field))
            {
                Error(Format("{} passes '{}' of type {} as argument '{}' of '{}', which takes a plain {}.", Reader.Description, Argument.SourceField.ToString(),
                    Field->GetTypeName().ToString(), Parameter->GetPropertyName().ToString(), FunctionName, Parameter->GetTypeName().ToString()));
            }
            else
            {
                Runtime->Copies.Add({ Offset + Field->Offset, Call.FrameOffset + Parameter->Offset, (uint32)Parameter->GetElementSize() });
            }
        }
        if (Errors.Num() > NumErrors)
        {
            return {};
        }
        Runtime->Calls.Add(Call);
        return (uint16)(Runtime->Calls.Num() - 1);
    }
}
