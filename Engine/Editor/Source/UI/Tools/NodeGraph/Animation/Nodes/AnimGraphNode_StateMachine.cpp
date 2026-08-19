#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_State.h"
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

    CAnimStateMachineGraph* CAnimGraphNode_StateMachine::GetOrCreateStateMachineGraph()
    {
        if (!StateMachineGraph.IsValid())
        {
            const FString GraphName = FString("StateMachine_") + Format("{}", GetNodeID());
            StateMachineGraph = NewObject<CAnimStateMachineGraph>(GetPackage(), GraphName);
        }

        // Context-free: compiler walks state machines never opened; Initialize() deferred to editor tool.
        StateMachineGraph->EnsureSetup();
        return StateMachineGraph.Get();
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
            // Never opened -> nothing to evaluate. Emit a bind pose so the
            // graph still resolves; the user gets an error to act on.
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

        // Compile every State's blend tree into the shared register space and
        // record which pose register each state resolved to.
        THashMap<int64, int32> NodeIDToStateIndex;
        THashSet<int64> AnyStateNodeIDs;
        TVector<TPair<CEdGraphNode*, int32>> StateNodesForDebug;

        for (CEdGraphNode* Node : SMGraph->Nodes)
        {
            if (Node->IsA<CAnimGraphNode_StateAny>())
            {
                AnyStateNodeIDs.insert(Node->GetNodeID());
            }

            CAnimGraphNode_State* StateNode = Cast<CAnimGraphNode_State>(Node);
            if (StateNode == nullptr)
            {
                continue;
            }

            CAnimationGraphNodeGraph* BlendTree = StateNode->GetOrCreateBlendTree();

            uint16 PoseReg = 0;
            if (!BlendTree->CompileNodes(Compiler, PoseReg))
            {
                // CompileNodes already reported the error; fall back to a bind
                // pose so state indices stay consistent with the canvas.
                PoseReg = Compiler.EmitRefPose();
            }

            const int32 StateIndex = (int32)StateMachine.StatePoseRegisters.size();
            StateMachine.StatePoseRegisters.push_back(PoseReg);
            NodeIDToStateIndex[StateNode->GetNodeID()] = StateIndex;
            StateNodesForDebug.emplace_back(StateNode, StateIndex);
        }

        if (StateMachine.StatePoseRegisters.empty())
        {
            EdNodeGraph::FError NodeError;
            NodeError.Name        = "Empty State Machine";
            NodeError.Description = "State Machine has no State nodes; it will evaluate to the bind pose.";
            NodeError.Node        = this;
            Compiler.AddError(NodeError);
        }

        // Entry state: follow the Entry node's single outgoing wire.
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

        // Transitions: resolve each transition object's endpoint node IDs to state indices and copy
        // the condition through. The VM takes the first passing edge, so author Priority decides the
        // emission order; without it the order would follow the reconcile pass's hash iteration.
        TVector<CAnimStateTransition*> SortedTransitions;
        SortedTransitions.reserve(SMGraph->GetTransitions().size());
        for (const TObjectPtr<CAnimStateTransition>& Transition : SMGraph->GetTransitions())
        {
            if (Transition.IsValid())
            {
                SortedTransitions.push_back(Transition.Get());
            }
        }
        std::stable_sort(SortedTransitions.begin(), SortedTransitions.end(),
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

        for (CAnimStateTransition* Transition : SortedTransitions)
        {
            auto ToIt = NodeIDToStateIndex.find(Transition->ToStateNodeID);
            if (ToIt == NodeIDToStateIndex.end())
            {
                // Endpoint state missing -- skip rather than emit a bad index.
                continue;
            }

            // An Any State source compiles to the runtime's from-anywhere edge (FromState < 0),
            // checked no matter which state is active.
            int32 FromIndex = -1;
            if (AnyStateNodeIDs.find(Transition->FromStateNodeID) == AnyStateNodeIDs.end())
            {
                auto FromIt = NodeIDToStateIndex.find(Transition->FromStateNodeID);
                if (FromIt == NodeIDToStateIndex.end())
                {
                    continue;
                }
                FromIndex = FromIt->second;
            }

            FAnimGraphTransition Runtime;
            Runtime.FromState          = FromIndex;
            Runtime.ToState            = ToIt->second;
            Runtime.ConditionParameter = Transition->ConditionParameter;
            Runtime.Compare            = Transition->Compare;
            Runtime.CompareValue       = Transition->CompareValue;
            Runtime.BlendDuration      = Transition->BlendDuration;
            Runtime.bCanInterrupt      = Transition->bCanInterrupt;

            // Make sure the condition parameter exists in the compiled table,
            // and warn if it doesn't match a blackboard key (renamed / retyped).
            Compiler.ValidateParameterKey(Transition->ConditionParameter, this);
            Compiler.AddParameter(Transition->ConditionParameter, EAnimGraphParamType::Float, 0.0f);

            StateMachine.Transitions.push_back(Runtime);
        }

        // Allocate the four persistent bookkeeping slots.
        StateMachine.CurrentStateSlot = Compiler.AllocStateSlot();
        StateMachine.FromStateSlot    = Compiler.AllocStateSlot();
        StateMachine.TimerSlot        = Compiler.AllocStateSlot();
        StateMachine.DurationSlot     = Compiler.AllocStateSlot();

        // Record which slot/index each State node maps to so the editor's debug
        // overlay can highlight the state the VM is currently in.
        for (const TPair<CEdGraphNode*, int32>& Entry : StateNodesForDebug)
        {
            Compiler.AddDebugStateNode(Entry.first, StateMachine.CurrentStateSlot, Entry.second);
        }

        const uint16 ResultReg = Compiler.EmitEvalStateMachine(Move(StateMachine));
        Compiler.SetPinRegister(ResultPin, ResultReg);
    }
}
