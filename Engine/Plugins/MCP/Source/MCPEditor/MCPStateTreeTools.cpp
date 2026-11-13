#include "MCPStateTreeTools.h"

#include "Agent/AgentEntityToken.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Assets/Factories/Factory.h"
#include "Containers/Algorithm.h"
#include "MCPServer.h"
#include "MCPToolTypes.h"
#include "Session/SessionOps.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/StateTreeComponent.h"

namespace Lumina::MCP
{
    namespace MCPStateTreeToolsInternal
    {
        SStateTreeState* FindState(CStateTree* Tree, FName Name, FString& OutError)
        {
            const int32 Index = Algo::IndexOf(Tree->States, Name, &SStateTreeState::Name);
            if (Index == Constants::kIndexNone)
            {
                OutError = Lumina::Format("The tree has no state named '{}'.", Name);
                return nullptr;
            }
            return &Tree->States[Index];
        }

        SStateTreeNodeBase* FindNode(CStateTree* Tree, FName Name)
        {
            SStateTreeNodeBase* Found = nullptr;
            const auto Search = [&](auto& Slots)
            {
                for (FInstancedStruct& Slot : Slots)
                {
                    SStateTreeNodeBase* Node = Slot.GetMutablePtr<SStateTreeNodeBase>();
                    if (Found == nullptr && Node != nullptr && Node->Name == Name)
                    {
                        Found = Node;
                    }
                }
            };

            Search(Tree->Evaluators);
            for (SStateTreeState& State : Tree->States)
            {
                Search(State.EnterConditions);
                Search(State.Tasks);
                for (SStateTreeTransition& Transition : State.Transitions)
                {
                    Search(Transition.Conditions);
                }
            }
            return Found;
        }

        template<typename TBase>
        int32 AppendNode(TVector<TInstancedStruct<TBase>>& Slots, const FInstancedStruct& Node)
        {
            Slots.Emplace(Node);
            return Slots.Num() - 1;
        }

        // One undo step when an editor has the tree open, then the recompile every authored edit gets.
        void EditStateTree(CStateTree* Tree, FName Label, const TFunction<void()>& Mutate)
        {
            const TFunction<void()> Apply = [&]()
            {
                Mutate();
                Tree->Compile();
            };

            if (!SessionOps::RunObjectTransacted(Tree, Label, Apply))
            {
                Apply();
            }
            MarkAssetDirty(Tree);
        }

        void ReportStateTree(CStateTree* Tree, SStateTreeReport& Out)
        {
            Out.Name = FString(*Tree->GetName().ToString());

            FJsonValue Authored;
            if (Agent::WriteStruct(Tree->GetClass(), Tree, Authored).IsValid())
            {
                Out.Tree = Authored.ToString();
            }

            Out.bCompiled = Tree->GetRuntime()->IsValid();
            Out.Errors    = Tree->GetCompileErrors();
        }

        void RegisterStateTreeCreate(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCreateStateTreeParams, SAssetInfo>(
                Owner, "statetree.create",
                "Create an empty state tree asset under /Game/Content. Build it with statetree.add_state, add_node, "
                "set_binding and add_transition, check it with statetree.compile and save it with assets.save.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCreateStateTreeParams& In, SAssetInfo& Out)
                {
                    FFixedString Path;
                    FString Error;
                    if (!MakeNewAssetPath(In.Folder, In.Name, Path, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const TStrongObjectPtr<CObject> Tree = CFactory::CreateAsset(nullptr, CStateTree::StaticClass(), FStringView(*Path, Path.Len()), Error);
                    if (!Tree)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("State tree at {}: {}.", Path, Error));
                    }

                    KeepAssetLoaded(Tree.Get());

                    Out.Name       = FString(*Tree->GetName().ToString());
                    Out.Path       = FString(*Path);
                    Out.AssetClass = FString(*CStateTree::StaticClass()->GetName().ToString());
                    Out.Guid       = FString(*Tree->GetGUID().ToString());
                    return Agent::FToolResult::Ok(Lumina::Format("Created {}.", Out.Path));
                });
        }

        void RegisterStateTreeAddState(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddStateTreeStateParams, SStateTreeAddResult>(
                Owner, "statetree.add_state",
                "Add a state after the tree's others; list order is sibling order. Kind is State, Group (picks one "
                "child) or LinkedSubtree (runs LinkedSubtree's states as its children).",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddStateTreeStateParams& In, SStateTreeAddResult& Out)
                {
                    CStateTree* Tree = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.StateTree), Tree, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (In.State.Name.IsNone())
                    {
                        return Agent::FToolResult::Error("A state needs a Name.");
                    }
                    if (Algo::IndexOf(Tree->States, In.State.Name, &SStateTreeState::Name) != Constants::kIndexNone)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("The tree already has a state named '{}'.", In.State.Name));
                    }
                    if (!In.State.Parent.IsNone() && FindState(Tree, In.State.Parent, Error) == nullptr)
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    EditStateTree(Tree, "Add State Tree State (agent)", [&]() { Tree->States.Add(In.State); });
                    Out.Index = Tree->States.Num() - 1;

                    return Agent::FToolResult::Ok(Lumina::Format("Added state '{}'.", In.State.Name));
                });
        }

        void RegisterStateTreeAddNode(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddStateTreeNodeParams, SStateTreeAddResult>(
                Owner, "statetree.add_node",
                "Add a task or condition to a state, or an evaluator to the tree, by struct name. A condition goes "
                "among the state's enter conditions, or on one of its transitions when Transition is set.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddStateTreeNodeParams& In, SStateTreeAddResult& Out)
                {
                    CStateTree* Tree = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.StateTree), Tree, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const FName Name(In.Name);
                    if (!Name.IsNone() && FindNode(Tree, Name) != nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("The tree already has a node named '{}'.", In.Name));
                    }

                    CStruct* Type = ResolveInstancedStructType(SStateTreeNodeBase::StaticStruct(), FName(In.Type));
                    if (Type == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No task, condition or evaluator struct is called '{}'.", In.Type));
                    }

                    FInstancedStruct Node(Type);
                    FJsonValue Properties;
                    if (!In.Properties.IsEmpty() && FJsonValue::Parse(In.Properties, Properties))
                    {
                        if (const Agent::FMarshalResult Read = Agent::ReadStruct(Properties, Type, Node.GetMutableMemory()); !Read.IsValid())
                        {
                            return Agent::FToolResult::Error(Read.Error);
                        }
                    }
                    Node.GetMutablePtr<SStateTreeNodeBase>()->Name = Name;

                    TFunction<int32()> Append;
                    if (Type->IsChildOf(SStateTreeEvaluatorBase::StaticStruct()))
                    {
                        Append = [&]() { return AppendNode(Tree->Evaluators, Node); };
                    }
                    else
                    {
                        SStateTreeState* State = FindState(Tree, FName(In.State), Error);
                        if (State == nullptr)
                        {
                            return Agent::FToolResult::Error(Error);
                        }

                        if (Type->IsChildOf(SStateTreeTaskBase::StaticStruct()))
                        {
                            Append = [&, State]() { return AppendNode(State->Tasks, Node); };
                        }
                        else if (!Type->IsChildOf(SStateTreeConditionBase::StaticStruct()))
                        {
                            return Agent::FToolResult::Error(Lumina::Format("{} is not a task, condition or evaluator.", In.Type));
                        }
                        else if (In.Transition < 0)
                        {
                            Append = [&, State]() { return AppendNode(State->EnterConditions, Node); };
                        }
                        else if (In.Transition < State->Transitions.Num())
                        {
                            Append = [&, State]() { return AppendNode(State->Transitions[In.Transition].Conditions, Node); };
                        }
                        else
                        {
                            return Agent::FToolResult::Error(Lumina::Format("State '{}' has {} transition(s), so it has no transition {}.",
                                In.State, State->Transitions.Num(), In.Transition));
                        }
                    }

                    EditStateTree(Tree, "Add State Tree Node (agent)", [&]() { Out.Index = Append(); });

                    return Agent::FToolResult::Ok(Lumina::Format("Added {} '{}'.", In.Type, In.Name));
                });
        }

        void RegisterStateTreeSetBinding(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetStateTreeBindingParams>(
                Owner, "statetree.set_binding",
                "Bind a field of a node's instance data to a tree parameter, an evaluator or earlier task's output, "
                "the context (Owner, DeltaTime) or a reflected function's return value, copied before the node runs.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetStateTreeBindingParams& In)
                {
                    CStateTree* Tree = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.StateTree), Tree, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    SStateTreeNodeBase* Node = In.Node.IsEmpty() ? nullptr : FindNode(Tree, FName(In.Node));
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("The tree has no node named '{}'.", In.Node));
                    }

                    const FName Target = In.Binding.TargetField;
                    if (Target.IsNone())
                    {
                        return Agent::FToolResult::Error("Binding.TargetField names the instance data field to fill.");
                    }

                    const int32 Existing = Algo::IndexOf(Node->Bindings, Target, &SStateTreeBinding::TargetField);
                    const bool bRemove = In.Binding.SourceField.IsNone();
                    if (bRemove && Existing == Constants::kIndexNone)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Node '{}' has no binding to '{}' to remove.", In.Node, Target));
                    }

                    EditStateTree(Tree, "Set State Tree Binding (agent)", [&]()
                    {
                        if (bRemove)
                        {
                            Node->Bindings.RemoveAt(Existing);
                        }
                        else if (Existing == Constants::kIndexNone)
                        {
                            Node->Bindings.Add(In.Binding);
                        }
                        else
                        {
                            Node->Bindings[Existing] = In.Binding;
                        }
                    });

                    return Agent::FToolResult::Ok(bRemove
                        ? Lumina::Format("Removed the binding to {}.{}.", In.Node, Target)
                        : Lumina::Format("Bound {}.{} to {}.", In.Node, Target, In.Binding.SourceField));
                });
        }

        void RegisterStateTreeAddTransition(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddStateTreeTransitionParams, SStateTreeAddResult>(
                Owner, "statetree.add_transition",
                "Add a transition leaving a state. Trigger is OnCompleted, OnSucceeded, OnFailed, OnEvent (with EventTag) "
                "or OnTick; Target is State (with TargetState), NextSibling, Parent, Succeeded or Failed.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddStateTreeTransitionParams& In, SStateTreeAddResult& Out)
                {
                    CStateTree* Tree = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.StateTree), Tree, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    SStateTreeState* State = FindState(Tree, FName(In.State), Error);
                    if (State == nullptr)
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    EditStateTree(Tree, "Add State Tree Transition (agent)", [&]() { State->Transitions.Add(In.Transition); });
                    Out.Index = State->Transitions.Num() - 1;

                    return Agent::FToolResult::Ok(Lumina::Format("Added transition {} to state '{}'.", Out.Index, In.State));
                });
        }

        void RegisterStateTreeCompile(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SStateTreeRefParams, SStateTreeReport>(
                Owner, "statetree.compile",
                "Compile a state tree into the runtime its entities run, and report every error, such as a missing "
                "transition target or a binding whose types differ.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SStateTreeRefParams& In, SStateTreeReport& Out)
                {
                    CStateTree* Tree = nullptr;
                    FString Error;
                    if (!ResolveEditableAsset(FStringView(In.StateTree), Tree, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Tree->Compile();
                    ReportStateTree(Tree, Out);

                    return Agent::FToolResult::Ok(Out.bCompiled
                        ? FString("Compiled.")
                        : Lumina::Format("Compile failed with {} error(s). {}", Out.Errors.Num(), Out.Errors.IsEmpty() ? FString() : Out.Errors[0]));
                });
        }

        void RegisterStateTreeDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SStateTreeRefParams, SStateTreeReport>(
                Owner, "statetree.describe",
                "Report a whole state tree as JSON, its states with their conditions, tasks and transitions, its "
                "evaluators and parameters, and the errors of its last compile.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SStateTreeRefParams& In, SStateTreeReport& Out)
                {
                    CStateTree* Tree = nullptr;
                    FString Error;
                    if (!Agent::ResolveAsset(FStringView(In.StateTree), Tree, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    ReportStateTree(Tree, Out);

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' has {} state(s) and {} evaluator(s).",
                        Out.Name, Tree->States.Num(), Tree->Evaluators.Num()));
                });
        }

        void RegisterStateTreeInstanceState(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SStateTreeInstanceParams, SStateTreeInstanceResult>(
                Owner, "statetree.instance_state",
                "Report the state tree running on one entity during Play, its status and its active path of states.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SStateTreeInstanceParams& In, SStateTreeInstanceResult& Out)
                {
                    FString Error;
                    ECS::FRegistry* Registry = SessionOps::GetSceneRegistry(Error);
                    ECS::FEntity Entity = ECS::NullEntity;
                    if (Registry == nullptr || !Agent::FEntityTokens::Resolve(*Registry, FStringView(In.Entity), Entity, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    const SStateTreeComponent* Component = Registry->TryGet<SStateTreeComponent>(Entity);
                    if (Component == nullptr)
                    {
                        return Agent::FToolResult::Error("That entity has no state tree component.");
                    }

                    const FStateTreeRunningInstance& Running = Component->Running;
                    if (Running.Instance == nullptr || Running.Tree == nullptr)
                    {
                        return Agent::FToolResult::Error("That entity's state tree is not running. Start Play with editor.play first.");
                    }

                    // Only a superseded instance runs a runtime other than its tree's current one.
                    if (Running.Instance->IsSuperseded())
                    {
                        return Agent::FToolResult::Error("The tree recompiled since this instance started, so it restarts on the next tick.");
                    }

                    const TSharedPtr<const FStateTreeRuntime> Runtime = Running.Tree->GetRuntime();
                    for (const FStateTreeActiveState& Active : Running.Instance->GetActivePath())
                    {
                        Out.ActivePath.Emplace(*Runtime->States[Active.State].Name.ToString());
                    }

                    Out.StateTree = FString(*Running.Tree->GetGUID().ToString());
                    Out.Status    = Running.Instance->GetStatus();
                    Out.bWaiting  = Running.Instance->IsWaiting();

                    return Agent::FToolResult::Ok(Out.ActivePath.IsEmpty()
                        ? FString("The tree has finished, so no state is active.")
                        : Lumina::Format("{} state(s) active, the leaf is '{}'.", Out.ActivePath.Num(), Out.ActivePath.Last()));
                });
        }
    }

    void RegisterStateTreeTools(FStringView Owner)
    {
        MCPStateTreeToolsInternal::RegisterStateTreeCreate(Owner);
        MCPStateTreeToolsInternal::RegisterStateTreeAddState(Owner);
        MCPStateTreeToolsInternal::RegisterStateTreeAddNode(Owner);
        MCPStateTreeToolsInternal::RegisterStateTreeSetBinding(Owner);
        MCPStateTreeToolsInternal::RegisterStateTreeAddTransition(Owner);
        MCPStateTreeToolsInternal::RegisterStateTreeCompile(Owner);
        MCPStateTreeToolsInternal::RegisterStateTreeDescribe(Owner);
        MCPStateTreeToolsInternal::RegisterStateTreeInstanceState(Owner);
    }
}
