#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_State.h"
#include "AnimGraphNode_StateRouting.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Containers/HashTable.h"
#include "Containers/Pair.h"
#include "Containers/Vector.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateMachineGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateTransition.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"

#include "Containers/StringFormat.h"

namespace Lumina
{
    void CAnimGraphNode_StateMachine::BuildNode()
    {
        ResultPin = CreateAnimPin("Result", ENodePinDirection::Output, EAnimPinType::Pose);
    }

    FString CAnimGraphNode_StateMachine::GetNodeTitleText() const
    {
        return MachineName.IsNone() ? FString(GetNodeDisplayName()) : MachineName.ToString();
    }

    bool CAnimGraphNode_StateMachine::GetRenameText(FString& OutText) const
    {
        OutText = MachineName.IsNone() ? FString() : MachineName.ToString();
        return true;
    }

    void CAnimGraphNode_StateMachine::SetRenameText(const FString& InText)
    {
        MachineName = InText.empty() ? NAME_None : FName(InText.c_str());
    }

    CAnimStateMachineGraph* CAnimGraphNode_StateMachine::AllocateStateMachineGraph()
    {
        const FString GraphName = FString("StateMachine_") + Format("{}", GetNodeID());
        StateMachineGraph = NewObject<CAnimStateMachineGraph>(GetPackage(), GraphName);
        return StateMachineGraph.Get();
    }

    CAnimStateMachineGraph* CAnimGraphNode_StateMachine::GetOrCreateStateMachineGraph()
    {
        if (!StateMachineGraph.IsValid())
        {
            AllocateStateMachineGraph();
        }

        // Context-free, since the compiler walks machines never opened and Initialize is deferred.
        StateMachineGraph->EnsureSetup();
        return StateMachineGraph.Get();
    }

    void CAnimGraphNode_StateMachine::PostCloneFrom(const CEdGraphNode* Source)
    {
        const CAnimGraphNode_StateMachine* SourceMachine = Cast<CAnimGraphNode_StateMachine>(Source);
        CAnimStateMachineGraph* SourceGraph = SourceMachine != nullptr ? SourceMachine->StateMachineGraph.Get() : nullptr;

        StateMachineGraph = nullptr;
        if (SourceGraph == nullptr)
        {
            return;
        }

        // Cloned before EnsureSetup so the copied Entry node is not joined by a second one.
        AllocateStateMachineGraph()->CloneContentFrom(SourceGraph);
    }

    CEdNodeGraph* CAnimGraphNode_StateMachine::GetOwnedSubGraph() const
    {
        return StateMachineGraph.Get();
    }

    void CAnimGraphNode_StateMachine::ReplaceSubGraphWithCopy()
    {
        CAnimStateMachineGraph* Shared = StateMachineGraph.Get();
        if (Shared != nullptr)
        {
            AllocateStateMachineGraph()->CloneContentFrom(Shared);
        }
    }

    CEdNodeGraph* CAnimGraphNode_StateMachine::GetEnterableSubGraph()
    {
        return GetOrCreateStateMachineGraph();
    }

    void CAnimGraphNode_StateMachine::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        FAnimGraphStateMachine StateMachine;

        if (!StateMachineGraph.IsValid())
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Empty State Machine";
            NodeError.Description = "State Machine has no states; double-click it to add some.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);

            const uint16 BindPose = Compiler.EmitRefPose();
            Compiler.SetPinRegister(ResultPin, BindPose);
            return;
        }

        CAnimStateMachineGraph* SMGraph = GetOrCreateStateMachineGraph();

        // Compiles every state's blend tree into the shared register space and records its pose register.
        THashMap<int64, int32> NodeIDToStateIndex;
        THashSet<int64> AnyStateNodeIDs;
        THashSet<int64> ConduitNodeIDs;
        THashMap<int64, CAnimGraphNode_StateAlias*> AliasNodes;
        THashMap<FName, int64> StateNameToNodeID;
        TVector<TPair<CEdGraphNode*, int32>> StateNodesForDebug;

        // Machine-relative, since a nested machine's entries belong to its own owner.
        const uint16 MachineClockFirst = (uint16)Compiler.GetClockSlots().size();

        for (CEdGraphNode* Node : SMGraph->Nodes)
        {
            if (Node->IsA<CAnimGraphNode_StateAny>())
            {
                AnyStateNodeIDs.insert(Node->GetNodeID());
            }

            if (Node->IsA<CAnimGraphNode_StateConduit>())
            {
                ConduitNodeIDs.insert(Node->GetNodeID());
            }

            if (CAnimGraphNode_StateAlias* AliasNode = Cast<CAnimGraphNode_StateAlias>(Node))
            {
                AliasNodes.emplace(AliasNode->GetNodeID(), AliasNode);
            }

            CAnimGraphNode_State* StateNode = Cast<CAnimGraphNode_State>(Node);
            if (StateNode == nullptr)
            {
                continue;
            }

            if (!StateNode->StateName.IsNone())
            {
                StateNameToNodeID[StateNode->StateName] = StateNode->GetNodeID();
            }

            CAnimationGraphNodeGraph* BlendTree = StateNode->GetOrCreateBlendTree();

            const uint16 ClockSlotFirst   = (uint16)Compiler.GetClockSlots().size() - MachineClockFirst;
            const uint16 ChildMachineFirst = Compiler.GetStateMachineCount();

            Compiler.BeginStateCapture();

            uint16 PoseReg = 0;
            if (!BlendTree->CompileNodes(Compiler, PoseReg))
            {
                // CompileNodes already reported the error, so fall back to a bind pose and keep indices consistent.
                PoseReg = Compiler.EmitRefPose();
            }

            const int32 StateIndex = (int32)StateMachine.StatePoseRegisters.size();
            StateMachine.StatePoseRegisters.push_back(PoseReg);
            StateMachine.StateFinishedRegisters.push_back(Compiler.EndStateCapture());

            // Machines emitted while this state compiled, descendants included, since they are contiguous.
            StateMachine.StateChildMachineFirst.push_back(ChildMachineFirst);
            StateMachine.StateChildMachineEnd.push_back(Compiler.GetStateMachineCount());
            StateMachine.StateClockSlotFirst.push_back(ClockSlotFirst);
            StateMachine.StateClockSlotEnd.push_back((uint16)Compiler.GetClockSlots().size() - MachineClockFirst);
            NodeIDToStateIndex[StateNode->GetNodeID()] = StateIndex;
            StateNodesForDebug.emplace_back(StateNode, StateIndex);
        }

        const TVector<uint16>& CompiledClockSlots = Compiler.GetClockSlots();
        StateMachine.ClockSlots.assign(CompiledClockSlots.begin() + MachineClockFirst, CompiledClockSlots.end());

        if (StateMachine.StatePoseRegisters.empty())
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Empty State Machine";
            NodeError.Description = "State Machine has no State nodes; it will evaluate to the bind pose.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
        }

        // The entry state follows the Entry node's single outgoing wire.
        StateMachine.EntryState = 0;
        for (CEdGraphNode* Node : SMGraph->Nodes)
        {
            CAnimGraphNode_StateEntry* EntryNode = Cast<CAnimGraphNode_StateEntry>(Node);
            if (EntryNode == nullptr || EntryNode->OutPin == nullptr)
            {
                continue;
            }

            if (EntryNode->OutPin->HasConnection())
            {
                CEdNodeGraphPin* TargetPin = EntryNode->OutPin->GetConnection(0);
                CAnimGraphNode_State* TargetState = Cast<CAnimGraphNode_State>(TargetPin->GetOwningNode());
                if (TargetState != nullptr)
                {
                    auto It = NodeIDToStateIndex.find(TargetState->GetNodeID());
                    if (It != NodeIDToStateIndex.end())
                    {
                        StateMachine.EntryState = It->second;
                    }
                }
            }
            else if (!StateMachine.StatePoseRegisters.empty())
            {
                EdNodeGraph::FError NodeError;
                NodeError.Name        = "No Entry State";
                NodeError.Description = "State Machine's Entry node is not wired to a State; defaulting to the first state.";
                NodeError.Node        = this;
                Compiler.AddError(NodeError);
            }
            break;
        }

        // The VM takes the first passing edge, so author Priority decides the emission order.
        TVector<CAnimStateTransition*> SortedTransitions;
        SortedTransitions.reserve(SMGraph->GetTransitions().size());
        for (const TObjectPtr<CAnimStateTransition>& Transition : SMGraph->GetTransitions())
        {
            if (Transition.IsValid())
            {
                SortedTransitions.push_back(Transition.Get());
            }
        }
        Algo::StableSort(SortedTransitions,
            [](const CAnimStateTransition* A, const CAnimStateTransition* B)
        {
            if (A->Priority != B->Priority)
            {
                return A->Priority < B->Priority;
            }
            if (A->FromStateNodeID != B->FromStateNodeID)
            {
                return A->FromStateNodeID < B->FromStateNodeID;
            }
            return A->ToStateNodeID < B->ToStateNodeID;
        });

        // Routing nodes are authoring conveniences, so they are folded into direct edges here.
        struct FResolvedEdge
        {
            int64 FromNodeID = 0;
            int64 ToNodeID = 0;
            TVector<FAnimTransitionCondition> Conditions;
            bool  bRequireAll = true;
            float BlendDuration = 0.2f;
            bool  bCanInterrupt = false;
            bool  bFromAnyState = false;
        };

        // An OR group cannot be ANDed with another rule without a term tree, so it is rejected instead.
        const auto CanCombine = [](const CAnimStateTransition* Edge)
        {
            return Edge->Conditions.size() <= 1 || Edge->bRequireAll;
        };

        TVector<FResolvedEdge> ResolvedEdges;
        ResolvedEdges.reserve(SortedTransitions.size());

        for (CAnimStateTransition* Transition : SortedTransitions)
        {
            Transition->MigrateLegacyCondition();

            // Out-edges of a conduit are consumed when their in-edge is paired with them.
            if (ConduitNodeIDs.find(Transition->FromStateNodeID) != ConduitNodeIDs.end())
            {
                continue;
            }

            // One source, or every state an alias names.
            TVector<int64> Sources;
            bool bFromAnyState = false;

            if (AnyStateNodeIDs.find(Transition->FromStateNodeID) != AnyStateNodeIDs.end())
            {
                bFromAnyState = true;
                Sources.push_back(0);
            }
            else if (AliasNodes.find(Transition->FromStateNodeID) != AliasNodes.end())
            {
                CAnimGraphNode_StateAlias* Alias = AliasNodes[Transition->FromStateNodeID];
                if (Alias->SourceStates.empty())
                {
                    EdNodeGraph::FError Warning;
                    Warning.Name        = "Empty State Alias";
                    Warning.Description = "State Alias names no states, so the transitions drawn out of it compile to nothing.";
                    Warning.Node        = Alias;
                    Compiler.AddWarning(Warning);
                }

                for (const FName& StateName : Alias->SourceStates)
                {
                    auto NameIt = StateNameToNodeID.find(StateName);
                    if (NameIt == StateNameToNodeID.end())
                    {
                        EdNodeGraph::FError Warning;
                        Warning.Name        = "Unknown Aliased State";
                        Warning.Description = FString("State Alias names ") + StateName.ToString() +
                            ", which is not a state on this machine (renamed or removed?). That edge is skipped.";
                        Warning.Node        = Alias;
                        Compiler.AddWarning(Warning);
                        continue;
                    }
                    Sources.push_back(NameIt->second);
                }
            }
            else
            {
                Sources.push_back(Transition->FromStateNodeID);
            }

            // One target, or every exit of the conduit this edge feeds.
            TVector<CAnimStateTransition*> Exits;
            const bool bToConduit = ConduitNodeIDs.find(Transition->ToStateNodeID) != ConduitNodeIDs.end();
            if (bToConduit)
            {
                for (CAnimStateTransition* Exit : SortedTransitions)
                {
                    if (Exit->FromStateNodeID != Transition->ToStateNodeID)
                    {
                        continue;
                    }

                    if (ConduitNodeIDs.find(Exit->ToStateNodeID) != ConduitNodeIDs.end())
                    {
                        EdNodeGraph::FError CompileError;
                        CompileError.Name        = "Chained Conduits";
                        CompileError.Description = "A conduit wired into another conduit is not supported; wire it to a State.";
                        CompileError.Node        = this;
                        Compiler.AddError(CompileError);
                        continue;
                    }
                    Exits.push_back(Exit);
                }

                if (Exits.empty())
                {
                    EdNodeGraph::FError Warning;
                    Warning.Name        = "Conduit Routes Nowhere";
                    Warning.Description = "A conduit with no outgoing transition swallows the edges drawn into it.";
                    Warning.Node        = this;
                    Compiler.AddWarning(Warning);
                }
            }
            else
            {
                Exits.push_back(nullptr);
            }

            for (int64 SourceNodeID : Sources)
            {
                for (CAnimStateTransition* Exit : Exits)
                {
                    if (Exit != nullptr && (!CanCombine(Transition) || !CanCombine(Exit)))
                    {
                        EdNodeGraph::FError CompileError;
                        CompileError.Name        = "Conduit Rule Cannot Be Combined";
                        CompileError.Description = "An edge through a conduit ANDs the two rules, so neither side may be an "
                            "any-of rule with more than one condition. Split it into separate transitions.";
                        CompileError.Node        = this;
                        Compiler.AddError(CompileError);
                        continue;
                    }

                    FResolvedEdge Resolved;
                    Resolved.FromNodeID    = SourceNodeID;
                    Resolved.bFromAnyState = bFromAnyState;
                    Resolved.Conditions    = Transition->Conditions;
                    Resolved.bRequireAll   = Transition->bRequireAll;

                    // The edge that actually reaches the state owns the blend; the conduit only routes.
                    const CAnimStateTransition* Final = (Exit != nullptr) ? Exit : Transition;
                    Resolved.ToNodeID      = Final->ToStateNodeID;
                    Resolved.BlendDuration = Final->BlendDuration;
                    Resolved.bCanInterrupt = Final->bCanInterrupt;

                    if (Exit != nullptr)
                    {
                        Resolved.bRequireAll = true;
                        for (const FAnimTransitionCondition& Condition : Exit->Conditions)
                        {
                            Resolved.Conditions.push_back(Condition);
                        }
                    }

                    ResolvedEdges.push_back(Move(Resolved));
                }
            }
        }

        for (const FResolvedEdge& Edge : ResolvedEdges)
        {
            auto ToIt = NodeIDToStateIndex.find(Edge.ToNodeID);
            if (ToIt == NodeIDToStateIndex.end())
            {
                // The endpoint state is missing, so skip rather than emit a bad index.
                continue;
            }

            int32 FromIndex = -1;
            if (!Edge.bFromAnyState)
            {
                auto FromIt = NodeIDToStateIndex.find(Edge.FromNodeID);
                if (FromIt == NodeIDToStateIndex.end())
                {
                    continue;
                }
                FromIndex = FromIt->second;
            }

            FAnimGraphTransition Runtime;
            Runtime.FromState     = FromIndex;
            Runtime.ToState       = ToIt->second;
            Runtime.bRequireAll   = Edge.bRequireAll;
            Runtime.BlendDuration = Edge.BlendDuration;
            Runtime.bCanInterrupt = Edge.bCanInterrupt;

            for (const FAnimTransitionCondition& Condition : Edge.Conditions)
            {
                FAnimGraphTransitionTerm Term;
                Term.ConditionSource = Condition.ConditionSource;
                Term.Compare         = Condition.Compare;
                Term.CompareValue    = Condition.CompareValue;

                switch (Condition.ConditionSource)
                {
                case EAnimTransitionSource::Parameter:
                    Term.Name = Condition.ParameterName;

                    // Warns when the name does not match a parameter struct field, renamed or retyped.
                    // An empty name is the unconditional term, so it registers no parameter of its own.
                    if (!Term.Name.IsNone())
                    {
                        Compiler.ValidateParameterKey(Term.Name, this);
                        Compiler.AddParameter(Term.Name, EAnimGraphParamType::Float, 0.0f);
                    }
                    break;

                case EAnimTransitionSource::Curve:
                    Term.Name = Condition.CurveName;

                    // Reserves the slot, so a curve only a transition reads still rides on the pose.
                    if (!Term.Name.IsNone())
                    {
                        Compiler.AddCurve(Term.Name);
                    }
                    break;

                default:
                    break;
                }

                Runtime.Terms.push_back(Term);
            }

            StateMachine.Transitions.push_back(Runtime);
        }

        StateMachine.bResetOnEntry = bResetOnEntry;

        // Kept in locals, since the machine is moved below.
        const uint16 CurrentStateSlot = Compiler.AllocStateSlot();
        const uint16 FromStateSlot    = Compiler.AllocStateSlot();
        StateMachine.CurrentStateSlot = CurrentStateSlot;
        StateMachine.FromStateSlot    = FromStateSlot;
        StateMachine.TimeInStateSlot  = Compiler.AllocStateSlot();
        StateMachine.DurationSlot     = Compiler.AllocStateSlot();

        const uint16 ResultReg    = Compiler.EmitEvalStateMachine(Move(StateMachine));
        const uint16 MachineIndex = Compiler.GetStateMachineCount() - 1;

        // What lets the debug overlay find the live state and the transition being blended through.
        for (const TPair<CEdGraphNode*, int32>& Entry : StateNodesForDebug)
        {
            Compiler.AddDebugStateNode(Entry.first, CurrentStateSlot, FromStateSlot, MachineIndex, Entry.second);
        }

        Compiler.SetPinRegister(ResultPin, ResultReg);
    }
}
