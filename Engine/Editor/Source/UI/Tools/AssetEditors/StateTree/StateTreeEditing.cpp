#include "StateTreeEditing.h"

#include "Containers/Algorithm.h"
#include "Containers/Format.h"
#include "Core/Math/Math.h"
#include "Core/Object/Class.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"

namespace Lumina::StateTreeEditing
{
    namespace
    {
        int32 FindState(const CStateTree& Tree, FName Name)
        {
            return Name.IsNone() ? Constants::kIndexNone : Algo::IndexOf(Tree.States, Name, &SStateTreeState::Name);
        }

        FName MakeUniqueName(FName Base, const TFunction<bool(FName)>& IsTaken)
        {
            if (!Base.IsNone() && !IsTaken(Base))
            {
                return Base;
            }
            const FString Root = Base.IsNone() ? FString("State") : Base.GetBaseName().ToString();
            for (uint32 Number = 1;; ++Number)
            {
                const FName Candidate(*Root, Number);
                if (!IsTaken(Candidate))
                {
                    return Candidate;
                }
            }
        }

        void RenameReferences(TVector<SStateTreeState>& States, FName From, FName To)
        {
            for (SStateTreeState& State : States)
            {
                State.Parent = State.Parent == From ? To : State.Parent;
                for (SStateTreeTransition& Transition : State.Transitions)
                {
                    Transition.TargetState = Transition.TargetState == From ? To : Transition.TargetState;
                }
            }
        }

        // Calls Fn with the typed list Node names, or returns Fallback when it names none.
        template<typename TResult, typename TFn>
        TResult VisitNodeList(CStateTree& Tree, const FStateTreeSelection& Node, TResult Fallback, TFn&& Fn)
        {
            if (Node.Slot == EStateTreeSlot::Evaluator)
            {
                return Fn(Tree.Evaluators);
            }
            if (!Tree.States.IsValidIndex(Node.State))
            {
                return Fallback;
            }
            SStateTreeState& State = Tree.States[Node.State];
            switch (Node.Slot)
            {
                case EStateTreeSlot::EnterCondition:
                    return Fn(State.EnterConditions);
                case EStateTreeSlot::Task:
                    return Fn(State.Tasks);
                case EStateTreeSlot::TransitionCondition:
                    return State.Transitions.IsValidIndex(Node.Transition) ? Fn(State.Transitions[Node.Transition].Conditions) : Fallback;
                default:
                    return Fallback;
            }
        }

        // The first node so named whose place InScope accepts, evaluators first and then each state's lists in order.
        const SStateTreeNodeBase* FindNodeNamed(const CStateTree& Tree, FName Name, const TFunction<bool(const FStateTreeSelection&)>& InScope, FStateTreeSelection* OutAt = nullptr)
        {
            const SStateTreeNodeBase* Found = nullptr;
            auto Search = [&](const auto& List, FStateTreeSelection At)
            {
                for (At.Index = 0; At.Index < List.Num() && Found == nullptr; ++At.Index)
                {
                    const SStateTreeNodeBase* Node = List[At.Index].template GetPtr<SStateTreeNodeBase>();
                    if (Node != nullptr && Node->Name == Name && InScope(At))
                    {
                        Found = Node;
                        if (OutAt != nullptr)
                        {
                            *OutAt = At;
                        }
                    }
                }
            };
            Search(Tree.Evaluators, { Constants::kIndexNone, EStateTreeSlot::Evaluator });
            for (int32 State = 0; State < Tree.States.Num(); ++State)
            {
                const SStateTreeState& Owner = Tree.States[State];
                Search(Owner.EnterConditions, { State, EStateTreeSlot::EnterCondition });
                Search(Owner.Tasks, { State, EStateTreeSlot::Task });
                for (int32 Transition = 0; Transition < Owner.Transitions.Num(); ++Transition)
                {
                    Search(Owner.Transitions[Transition].Conditions, { State, EStateTreeSlot::TransitionCondition, Transition });
                }
            }
            return Found;
        }

        const CStruct* FindNodeDataType(const CStateTree& Tree, EStateTreeBindingSource Source, FName Name)
        {
            const EStateTreeSlot Slot = Source == EStateTreeBindingSource::Evaluator ? EStateTreeSlot::Evaluator : EStateTreeSlot::Task;
            const SStateTreeNodeBase* Node = FindNodeNamed(Tree, Name, [Slot](const FStateTreeSelection& At) { return At.Slot == Slot; });
            return Node != nullptr ? Node->GetInstanceDataType() : nullptr;
        }

        // Compile errors name what they are about in quotes, a node first and then its state.
        FStateTreeSelection FindIssueTarget(const CStateTree& Tree, FStringView Message)
        {
            TVector<FName> Quoted;
            for (int32 Open = Message.Find('\''); Open != Constants::kIndexNone;)
            {
                const int32 Close = Message.Find('\'', Open + 1);
                if (Close == Constants::kIndexNone)
                {
                    break;
                }
                Quoted.Add(FName(Message.Mid(Open + 1, Close - Open - 1)));
                Open = Message.Find('\'', Close + 1);
            }
            if (Quoted.IsEmpty())
            {
                return {};
            }
            if (Message.StartsWith("Evaluator '"))
            {
                FStateTreeSelection Evaluator;
                FindNodeNamed(Tree, Quoted[0], [](const FStateTreeSelection& At) { return At.Slot == EStateTreeSlot::Evaluator; }, &Evaluator);
                return Evaluator;
            }
            if (Message.StartsWith("Node '") && Quoted.Num() > 1)
            {
                const int32 State = FindState(Tree, Quoted[1]);
                FStateTreeSelection Node = { State };
                FindNodeNamed(Tree, Quoted[0], [State](const FStateTreeSelection& At) { return At.State == State; }, &Node);
                return Node;
            }
            return Message.StartsWith("State '") ? FStateTreeSelection{ FindState(Tree, Quoted[0]) } : FStateTreeSelection();
        }
    }

    FName MakeUniqueStateName(const CStateTree& Tree, FName Base)
    {
        return MakeUniqueName(Base, [&](FName Name) { return FindState(Tree, Name) != Constants::kIndexNone; });
    }

    int32 AddState(CStateTree& Tree, FName Parent, int32 InsertAt)
    {
        SStateTreeState State;
        State.Name = MakeUniqueStateName(Tree, FName("State"));
        State.Parent = Parent;
        const int32 Index = Tree.States.IsValidIndex(InsertAt) ? InsertAt : Tree.States.Num();
        Tree.States.Insert(Move(State), Index);
        return Index;
    }

    bool RenameState(CStateTree& Tree, int32 Index, FName NewName)
    {
        const FName OldName = Tree.States[Index].Name;
        if (NewName.IsNone() || NewName == OldName || FindState(Tree, NewName) != Constants::kIndexNone)
        {
            return false;
        }
        Tree.States[Index].Name = NewName;
        RenameReferences(Tree.States, OldName, NewName);
        return true;
    }

    TVector<int32> GatherAncestors(const CStateTree& Tree, int32 Index)
    {
        TVector<int32> Ancestors;
        for (int32 Parent = FindState(Tree, Tree.States[Index].Parent); Parent != Constants::kIndexNone && Ancestors.Num() < Tree.States.Num(); Parent = FindState(Tree, Tree.States[Parent].Parent))
        {
            Ancestors.Add(Parent);
        }
        return Ancestors;
    }

    bool IsInSubtree(const CStateTree& Tree, int32 Index, int32 Root)
    {
        return Index == Root || Algo::Contains(GatherAncestors(Tree, Index), Root);
    }

    TVector<int32> GatherSubtree(const CStateTree& Tree, int32 Root)
    {
        TVector<int32> Subtree = { Root };
        for (int32 Index = 0; Index < Tree.States.Num(); ++Index)
        {
            if (Index != Root && IsInSubtree(Tree, Index, Root))
            {
                Subtree.Add(Index);
            }
        }
        return Subtree;
    }

    int32 MoveState(CStateTree& Tree, int32 Index, FName NewParent, int32 InsertBefore)
    {
        const int32 ParentIndex = FindState(Tree, NewParent);
        if (ParentIndex != Constants::kIndexNone && IsInSubtree(Tree, ParentIndex, Index))
        {
            return Constants::kIndexNone;
        }
        SStateTreeState Moved = Move(Tree.States[Index]);
        Moved.Parent = NewParent;
        Tree.States.RemoveAt(Index);
        InsertBefore = InsertBefore > Index ? InsertBefore - 1 : InsertBefore;
        InsertBefore = Math::Clamp(InsertBefore, 0, Tree.States.Num());
        Tree.States.Insert(Move(Moved), InsertBefore);
        return InsertBefore;
    }

    void DeleteState(CStateTree& Tree, int32 Index)
    {
        TVector<int32> Subtree = GatherSubtree(Tree, Index);
        Algo::Sort(Subtree, [](int32 A, int32 B) { return A > B; });
        for (int32 Removed : Subtree)
        {
            Tree.States.RemoveAt(Removed);
        }
    }

    TVector<SStateTreeState> CopySubtree(const CStateTree& Tree, int32 Root)
    {
        TVector<SStateTreeState> Copies;
        for (int32 Index : GatherSubtree(Tree, Root))
        {
            Copies.Add(Tree.States[Index]);
        }
        return Copies;
    }

    int32 PasteSubtree(CStateTree& Tree, TVector<SStateTreeState> States, FName Parent, int32 InsertAt)
    {
        if (States.IsEmpty())
        {
            return Constants::kIndexNone;
        }
        TVector<FName> Taken;
        for (SStateTreeState& State : States)
        {
            const FName NewName = MakeUniqueName(State.Name, [&](FName Name) { return FindState(Tree, Name) != Constants::kIndexNone || Algo::Contains(Taken, Name); });
            Taken.Add(NewName);
            if (NewName != State.Name)
            {
                // Pasted states may share names with the tree, so only references inside the pasted set follow the rename.
                const FName OldName = State.Name;
                State.Name = NewName;
                RenameReferences(States, OldName, NewName);
            }
        }
        States[0].Parent = Parent;
        const int32 Index = Tree.States.IsValidIndex(InsertAt) ? InsertAt : Tree.States.Num();
        Tree.States.Insert(States, Index);
        return Index;
    }

    int32 DuplicateState(CStateTree& Tree, int32 Index)
    {
        return PasteSubtree(Tree, CopySubtree(Tree, Index), Tree.States[Index].Parent, Index + 1);
    }

    void ExtractToLinkedSubtree(CStateTree& Tree, int32 Index, CStateTree& Into)
    {
        const FName RootName = Tree.States[Index].Name;
        TVector<SStateTreeState> Descendants = CopySubtree(Tree, Index);
        Descendants.RemoveAt(0);
        for (SStateTreeState& State : Descendants)
        {
            State.Parent = State.Parent == RootName ? FName() : State.Parent;
        }
        Into.States = Move(Descendants);
        Into.Parameters = Tree.Parameters;

        TVector<int32> Removed = GatherSubtree(Tree, Index);
        Removed.RemoveAt(0);
        Algo::Sort(Removed, [](int32 A, int32 B) { return A > B; });
        for (int32 Descendant : Removed)
        {
            Tree.States.RemoveAt(Descendant);
        }
        SStateTreeState& Root = Tree.States[FindState(Tree, RootName)];
        Root.Kind = EStateTreeStateKind::LinkedSubtree;
        Root.LinkedSubtree = &Into;
    }

    FInstancedStruct* FindNode(CStateTree& Tree, const FStateTreeSelection& Node)
    {
        return VisitNodeList(Tree, Node, (FInstancedStruct*)nullptr, [&](auto& List) -> FInstancedStruct*
        {
            return List.IsValidIndex(Node.Index) ? &List[Node.Index] : nullptr;
        });
    }

    CStruct* GetNodeBaseStruct(EStateTreeSlot Slot)
    {
        switch (Slot)
        {
            case EStateTreeSlot::Task:      return SStateTreeTaskBase::StaticStruct();
            case EStateTreeSlot::Evaluator: return SStateTreeEvaluatorBase::StaticStruct();
            case EStateTreeSlot::EnterCondition:
            case EStateTreeSlot::TransitionCondition:
                return SStateTreeConditionBase::StaticStruct();
            default:
                return nullptr;
        }
    }

    int32 InsertNode(CStateTree& Tree, const FStateTreeSelection& Node, const FInstancedStruct& Value)
    {
        return VisitNodeList(Tree, Node, Constants::kIndexNone, [&](auto& List) -> int32
        {
            using TSlot = std::decay_t<decltype(List[0])>;
            const int32 Index = List.IsValidIndex(Node.Index) ? Node.Index : List.Num();
            List.Insert(TSlot(Value), Index);
            return Index;
        });
    }

    int32 AddNodeOfType(CStateTree& Tree, const FStateTreeSelection& Node, CStruct* Type)
    {
        FInstancedStruct Value(Type);
        if (SStateTreeNodeBase* Base = Value.GetMutablePtr<SStateTreeNodeBase>())
        {
            // Bindings find a node by name, so every new node gets one no other node in the tree has.
            Base->Name = MakeUniqueName(FName(Type->MakeDisplayName()), [&](FName Name) { return FindNodeNamed(Tree, Name, [](const FStateTreeSelection&) { return true; }) != nullptr; });
        }
        return InsertNode(Tree, { Node.State, Node.Slot, Node.Transition }, Value);
    }

    void RemoveNode(CStateTree& Tree, const FStateTreeSelection& Node)
    {
        VisitNodeList(Tree, Node, 0, [&](auto& List)
        {
            if (List.IsValidIndex(Node.Index))
            {
                List.RemoveAt(Node.Index);
            }
            return 0;
        });
    }

    TVector<SStateTreeBinding>* FindBindings(CStateTree& Tree, const FStateTreeSelection& Reader, CStruct*& OutTargetType)
    {
        OutTargetType = nullptr;
        if (Reader.Slot == EStateTreeSlot::State && Tree.States.IsValidIndex(Reader.State))
        {
            SStateTreeState& State = Tree.States[Reader.State];
            const CStateTree* Linked = State.LinkedSubtree.Get();
            OutTargetType = State.Kind == EStateTreeStateKind::LinkedSubtree && Linked != nullptr ? Linked->Parameters.GetScriptStruct() : nullptr;
            return OutTargetType != nullptr ? &State.ParameterBindings : nullptr;
        }
        FInstancedStruct* Node = Reader.IsNode() ? FindNode(Tree, Reader) : nullptr;
        SStateTreeNodeBase* Base = Node != nullptr ? Node->GetMutablePtr<SStateTreeNodeBase>() : nullptr;
        OutTargetType = Base != nullptr ? Base->GetInstanceDataType() : nullptr;
        return OutTargetType != nullptr ? &Base->Bindings : nullptr;
    }

    void GatherBindingSources(const CStateTree& Tree, const FStateTreeSelection& Reader, const FProperty& Target, bool bAllowFunctions, TVector<FStateTreeBindingSource>& Out)
    {
        Out.Reset();
        auto AddSource = [&](EStateTreeBindingSource Source, FName Node, FName Field, const FProperty& Value, const FString& Category, const char* Icon)
        {
            FStateTreeBindingSource& Added = Out.AddDefaultedGetRef();
            Added.Path.Source = Source;
            Added.Path.SourceNode = Node;
            Added.Path.SourceField = Field;
            Added.Item.Name = Field.ToString();
            Added.Item.Key = DescribeBinding(Added.Path);
            Added.Item.Category = Category;
            Added.Item.Tooltip = Format("{} ({})", Added.Item.Key, Value.GetTypeName().ToString());
            Added.Item.Icon = Icon;
        };
        auto AddStruct = [&](EStateTreeBindingSource Source, FName Node, const CStruct* Type, const FString& Category)
        {
            if (Type == nullptr)
            {
                return;
            }
            const bool bFunctionSource = Source == EStateTreeBindingSource::Function;
            for (const FProperty* Field : bFunctionSource ? TSpan<FProperty* const>() : Type->GetProperties())
            {
                if (FStateTreeCompiler::CanBind(Target, *Field))
                {
                    AddSource(Source, Node, Field->GetPropertyName(), *Field, Category, LE_ICON_CIRCLE_MEDIUM);
                }
            }
            for (const FFunction* Function : bAllowFunctions ? Type->GetFunctions() : TSpan<FFunction* const>())
            {
                // An argument the compiler cannot copy into the frame makes the function unbindable whatever feeds it.
                const FProperty* Return = Function->GetReturnParam();
                const bool bArgumentsBindable = Algo::AllOf(Function->GetArguments(), [](const FProperty* Argument) { return FStateTreeCompiler::CanBind(*Argument, *Argument); });
                if (Return != nullptr && bArgumentsBindable && (!bFunctionSource || Function->IsStatic()) && FStateTreeCompiler::CanBind(Target, *Return))
                {
                    AddSource(Source, Node, Function->GetFunctionName(), *Return, Category, LE_ICON_FUNCTION);
                }
            }
        };
        auto AddNodes = [&](EStateTreeBindingSource Source, const auto& List, int32 Count, const FString& Category)
        {
            for (int32 Index = 0; Index < Math::Min(Count, List.Num()); ++Index)
            {
                const SStateTreeNodeBase* Node = List[Index].template GetPtr<SStateTreeNodeBase>();
                if (Node != nullptr && !Node->Name.IsNone())
                {
                    AddStruct(Source, Node->Name, Node->GetInstanceDataType(), Format("{}/{}", Category, Node->Name.ToString()));
                }
            }
        };

        AddStruct(EStateTreeBindingSource::Parameter, FName(), Tree.Parameters.GetScriptStruct(), "Parameters");
        AddStruct(EStateTreeBindingSource::Context, FName(), SStateTreeContextData::StaticStruct(), "Context");
        const bool bEvaluatorReader = Reader.Slot == EStateTreeSlot::Evaluator;
        AddNodes(EStateTreeBindingSource::Evaluator, Tree.Evaluators, bEvaluatorReader ? Reader.Index : Tree.Evaluators.Num(), "Evaluators");

        if (!bEvaluatorReader && Tree.States.IsValidIndex(Reader.State))
        {
            // A state's own tasks run after its enter conditions and before its transitions and linked tree.
            const SStateTreeState& Owner = Tree.States[Reader.State];
            const int32 OwnTasks = Reader.Slot == EStateTreeSlot::Task ? Reader.Index : Reader.Slot == EStateTreeSlot::EnterCondition ? 0 : Owner.Tasks.Num();
            AddNodes(EStateTreeBindingSource::Task, Owner.Tasks, OwnTasks, Format("Tasks/{}", Owner.Name.ToString()));
            for (int32 Ancestor : GatherAncestors(Tree, Reader.State))
            {
                const SStateTreeState& State = Tree.States[Ancestor];
                AddNodes(EStateTreeBindingSource::Task, State.Tasks, State.Tasks.Num(), Format("Tasks/{}", State.Name.ToString()));
            }
        }

        if (bAllowFunctions)
        {
            for (TObjectIterator<CClass> It; It; ++It)
            {
                CClass* Library = *It;
                if (Library != CFunctionLibrary::StaticClass() && Library->IsChildOf(CFunctionLibrary::StaticClass()))
                {
                    AddStruct(EStateTreeBindingSource::Function, Library->GetName(), Library, Format("Functions/{}", Library->GetName().ToString()));
                }
            }
        }
    }

    SStateTreeBinding& SetBinding(const CStateTree& Tree, TVector<SStateTreeBinding>& Bindings, FName TargetField, const SStateTreeBindingPath& Path)
    {
        const int32 Existing = Algo::IndexOf(Bindings, TargetField, &SStateTreeBinding::TargetField);
        SStateTreeBinding& Binding = Existing != Constants::kIndexNone ? Bindings[Existing] : Bindings.AddDefaultedGetRef();
        static_cast<SStateTreeBindingPath&>(Binding) = Path;
        Binding.TargetField = TargetField;
        const FFunction* Function = FindBoundFunction(Tree, Binding);
        Binding.Arguments.Reset();
        Binding.Arguments.SetNum(Function != nullptr ? Function->GetArguments().Num() : 0);
        return Binding;
    }

    const FFunction* FindBoundFunction(const CStateTree& Tree, const SStateTreeBindingPath& Path)
    {
        const CStruct* Type = nullptr;
        switch (Path.Source)
        {
            case EStateTreeBindingSource::Parameter: Type = Tree.Parameters.GetScriptStruct(); break;
            case EStateTreeBindingSource::Context:   Type = SStateTreeContextData::StaticStruct(); break;
            case EStateTreeBindingSource::Function:  Type = FindObject<CStruct>(Path.SourceNode); break;
            default:                                 Type = FindNodeDataType(Tree, Path.Source, Path.SourceNode); break;
        }
        // A field wins over a function of the same name, as it does in the compiler.
        const bool bReadsField = Type == nullptr || (Path.Source != EStateTreeBindingSource::Function && Type->GetProperty(Path.SourceField) != nullptr);
        return bReadsField ? nullptr : Type->FindFunction(Path.SourceField);
    }

    FString DescribeBinding(const SStateTreeBindingPath& Path)
    {
        const FString Field = Path.SourceField.ToString();
        switch (Path.Source)
        {
            case EStateTreeBindingSource::Parameter: return Format("Parameters.{}", Field);
            case EStateTreeBindingSource::Context:   return Format("Context.{}", Field);
            case EStateTreeBindingSource::Function:  return Format("{}.{}()", Path.SourceNode.ToString(), Field);
            default:                                 return Format("{}.{}", Path.SourceNode.ToString(), Field);
        }
    }

    TVector<FStateTreeIssue> Validate(CStateTree& Tree)
    {
        Tree.Compile(false);
        TVector<FStateTreeIssue> Issues;
        for (const FString& Error : Tree.GetCompileErrors())
        {
            Issues.Add({ Error, FindIssueTarget(Tree, Error), true });
        }
        for (int32 Index = 0; Index < Tree.States.Num(); ++Index)
        {
            const SStateTreeState& State = Tree.States[Index];
            if (State.Kind == EStateTreeStateKind::Group && !Algo::Contains(Tree.States, State.Name, &SStateTreeState::Parent))
            {
                Issues.Add({ Format("Group '{}' has no children, so it is never entered.", State.Name.ToString()), { Index }, false });
            }
        }
        return Issues;
    }
}
