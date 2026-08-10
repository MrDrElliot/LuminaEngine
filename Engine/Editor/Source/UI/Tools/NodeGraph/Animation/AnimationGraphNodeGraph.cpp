#include "AnimationGraphNodeGraph.h"
#include "AnimGraphSchema.h"
#include "AnimationGraphCompiler.h"
#include "AnimGraphNode.h"
#include "Nodes/AnimGraphNode_Output.h"
#include "Nodes/AnimGraphNode_BlendSpace.h"
#include "Nodes/AnimGraphNode_ClipPlayer.h"
#include "Nodes/AnimGraphNode_Blend.h"
#include "Nodes/AnimGraphNode_GetParameter.h"
#include "Nodes/AnimGraphNode_ScalarOps.h"
#include "Nodes/AnimGraphNode_Remap.h"
#include "Nodes/AnimGraphNode_FloatConstant.h"
#include "Nodes/AnimGraphNode_State.h"
#include "Nodes/AnimGraphNode_StateMachine.h"
#include "Nodes/AnimGraphNode_Additive.h"
#include "Nodes/AnimGraphNode_LayeredBlendPerBone.h"
#include "Nodes/AnimGraphNode_BoneTransform.h"
#include "Nodes/AnimGraphNode_Curve.h"
#include "Nodes/AnimGraphNode_TwoBoneIK.h"
#include "AnimStateMachineGraph.h"
#include "AnimStateTransition.h"
#include "AnimationGraphCompiler.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "UI/Tools/NodeGraph/GraphAlgorithms.h"

namespace Lumina
{
    void CAnimationGraphNodeGraph::EnsureSetup()
    {
        // Context-free: only touches nodes / the creatable-node registry, so it
        // is safe to call on a graph the compiler readies but never opens.
        if (bSetupDone)
        {
            return;
        }
        bSetupDone = true;

        bool bHasOutputNode = false;
        for (const TObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (Node.IsValid() && Node->IsA<CAnimGraphNode_Output>())
            {
                bHasOutputNode = true;
                break;
            }
        }

        if (!bHasOutputNode)
        {
            CreateNode(CAnimGraphNode_Output::StaticClass());
        }

        RegisterGraphNode(CAnimGraphNode_ClipPlayer::StaticClass());
        RegisterGraphNode(CAnimGraphNode_BlendSpace::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Blend::StaticClass());
        RegisterGraphNode(CAnimGraphNode_GetParameter::StaticClass());
        RegisterGraphNode(CAnimGraphNode_FloatConstant::StaticClass());

        // Scalar math: discrete binary and unary op nodes.
        RegisterGraphNode(CAnimGraphNode_Add::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Subtract::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Multiply::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Divide::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Min::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Max::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Clamp01::StaticClass());
        RegisterGraphNode(CAnimGraphNode_OneMinus::StaticClass());
        RegisterGraphNode(CAnimGraphNode_AbsoluteValue::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Sine::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Cosine::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Modulo::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Power::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Atan2::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Less::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Greater::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Floor::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Ceil::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Frac::StaticClass());
        RegisterGraphNode(CAnimGraphNode_SquareRoot::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Negate::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Sign::StaticClass());
        RegisterGraphNode(CAnimGraphNode_Remap::StaticClass());

        // State machine nodes can be placed in any blend-tree graph; the State node
        // itself is only creatable on the state machine canvas (CAnimStateMachineGraph).
        RegisterGraphNode(CAnimGraphNode_StateMachine::StaticClass());

        // Layered / additive blending: standard AAA building blocks for
        // procedural overlays (lean, look-at) and upper/lower body splits.
        RegisterGraphNode(CAnimGraphNode_MakeAdditive::StaticClass());
        RegisterGraphNode(CAnimGraphNode_ApplyAdditive::StaticClass());
        RegisterGraphNode(CAnimGraphNode_LayeredBlendPerBone::StaticClass());

        // Per-bone procedural override: hand-IK / aim offsets / look-at.
        RegisterGraphNode(CAnimGraphNode_BoneTransform::StaticClass());
        RegisterGraphNode(CAnimGraphNode_TwoBoneIK::StaticClass());

        // Animation curves: read a clip-authored value off a pose, or write one onto it.
        RegisterGraphNode(CAnimGraphNode_GetCurve::StaticClass());
        RegisterGraphNode(CAnimGraphNode_SetCurve::StaticClass());
    }

    void CAnimationGraphNodeGraph::Initialize()
    {
        // Must run exactly once: this graph type is re-entered (nested state blend trees), and a repeat
        // Super::Initialize() would leak a second node-editor context.
        if (bInitialized)
        {
            return;
        }
        bInitialized = true;

        Super::Initialize();
        EnsureSetup();
        ValidateGraph();
    }

    void CAnimationGraphNodeGraph::Shutdown()
    {
        Super::Shutdown();
    }

    void CAnimationGraphNodeGraph::ValidateGraph()
    {
        // Flatten live pin connections into the serialized (InputID, OutputID)
        // pair list. PostLoad reads this back to rewire the pins on open.
        Connections.clear();
        Connections.reserve(16);

        for (CEdGraphNode* Node : Nodes)
        {
            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    Connections.push_back(InputPin->PinID);
                    Connections.push_back(Connection->PinID);
                }
            }
        }
    }

    const FEdGraphSchema& CAnimationGraphNodeGraph::GetSchema() const
    {
        return GetAnimGraphSchema();
    }

    bool CAnimationGraphNodeGraph::CompileNodes(FAnimationGraphCompiler& Compiler, uint16& OutPoseReg)
    {
        OutPoseReg = 0;

        for (CEdGraphNode* Node : Nodes)
        {
            Node->ClearError();
        }

        if (Nodes.empty())
        {
            EdNodeGraph::FError Error;
            Error.Name        = "No Output";
            Error.Description = "Animation graph has no Output Pose node to compile from.";
            Error.Node        = nullptr;
            Compiler.AddError(Error);
            return false;
        }

        TVector<CEdGraphNode*> SortedNodes;
        CEdGraphNode* CyclicNode = GraphAlgorithms::TopologicalSortFromRoot(Nodes, SortedNodes, [](CEdGraphNode* Node)
        {
            return Cast<CAnimGraphNode_Output>(Node) != nullptr;
        });

        if (CyclicNode != nullptr)
        {
            EdNodeGraph::FError Error;
            Error.Name        = "Cyclic";
            Error.Description = "Cycle detected in animation node graph! Graph must be acyclic.";
            Error.Node        = CyclicNode;
            Compiler.AddError(Error);
            return false;
        }

        if (SortedNodes.empty())
        {
            EdNodeGraph::FError Error;
            Error.Name        = "No Output";
            Error.Description = "Animation graph has no Output Pose node to compile from.";
            Error.Node        = nullptr;
            Compiler.AddError(Error);
            return false;
        }

        // SortedNodes is dependency-ordered (inputs before consumers, output last),
        // so each node's input registers are populated by the time it emits.
        for (uint32 i = 0; i < (uint32)SortedNodes.size(); ++i)
        {
            CEdGraphNode* Node = SortedNodes[i];
            Node->SetDebugExecutionOrder(i);

            if (CAnimGraphNode* AnimNode = Cast<CAnimGraphNode>(Node))
            {
                AnimNode->GenerateBytecode(Compiler);
            }
        }

        // The Output node resolves (but does not emit) the pose feeding the
        // graph; hand that register back so the caller can wire it up.
        for (CEdGraphNode* Node : SortedNodes)
        {
            if (CAnimGraphNode_Output* Output = Cast<CAnimGraphNode_Output>(Node))
            {
                OutPoseReg = Output->GetResolvedPoseRegister();
                break;
            }
        }

        return true;
    }

    void CAnimationGraphNodeGraph::CollectAllParameters(FAnimationGraphCompiler& Compiler)
    {
        for (CEdGraphNode* Node : Nodes)
        {
            if (CAnimGraphNode_GetParameter* GetParam = Cast<CAnimGraphNode_GetParameter>(Node))
            {
                // Register even when this node isn't wired anywhere, else the Parameters panel / Lua
                // bindings never see the name.
                Compiler.AddParameter(GetParam->ParameterName, EAnimGraphParamType::Float, GetParam->DefaultValue);
            }
            else if (CAnimGraphNode_StateMachine* StateMachine = Cast<CAnimGraphNode_StateMachine>(Node))
            {
                CAnimStateMachineGraph* SMGraph = StateMachine->StateMachineGraph.Get();
                if (SMGraph == nullptr)
                {
                    continue;
                }

                // Transition conditions: each declares a parameter the runtime
                // reads at evaluation time.
                for (const TObjectPtr<CAnimStateTransition>& Transition : SMGraph->Transitions)
                {
                    if (Transition.IsValid() && !Transition->ConditionParameter.IsNone())
                    {
                        Compiler.AddParameter(Transition->ConditionParameter, EAnimGraphParamType::Float, 0.0f);
                    }
                }

                // Recurse into each state's blend tree.
                for (CEdGraphNode* SubNode : SMGraph->Nodes)
                {
                    if (CAnimGraphNode_State* State = Cast<CAnimGraphNode_State>(SubNode))
                    {
                        if (CAnimationGraphNodeGraph* BlendTree = State->BlendTree.Get())
                        {
                            BlendTree->CollectAllParameters(Compiler);
                        }
                    }
                }
            }
        }
    }

    void CAnimationGraphNodeGraph::CompileGraph(FAnimationGraphCompiler& Compiler)
    {
        // Collect parameters before topo-sort; unconnected Get Parameter nodes would otherwise be invisible to Lua SetFloat().
        CollectAllParameters(Compiler);

        uint16 ResultRegister = 0;
        if (CompileNodes(Compiler, ResultRegister))
        {
            Compiler.EmitOutput(ResultRegister);
        }

        // Attach every accumulated error (including those from nested state
        // blend trees compiled mid-walk) back to its originating node.
        for (auto& Error : Compiler.GetErrors())
        {
            if (Error.Node)
            {
                Error.Node->SetError(Error);
            }
        }
    }
}
