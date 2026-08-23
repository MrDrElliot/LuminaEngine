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
#include "Nodes/AnimGraphNode_CachedPose.h"
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
        // Context-free, so it is safe on a graph the compiler readies but never opens.
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
    }

    void CAnimationGraphNodeGraph::Initialize()
    {
        // This graph type is re-entered, and a repeat Super::Initialize leaks a node-editor context.
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
        // PostLoad reads this pair list back to rewire the pins on open.
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

        // Cached pose branches emit ahead of everything else, so a state machine compiled later in this
        // walk can resolve a Use Cached Pose sitting inside one of its states.
        TVector<CEdGraphNode*> CachedPoseNodes;
        CEdGraphNode* CyclicCachedPoseNode = GraphAlgorithms::TopologicalSortFromRoots(Nodes, CachedPoseNodes, [](CEdGraphNode* Node)
        {
            return Cast<CAnimGraphNode_SaveCachedPose>(Node) != nullptr;
        });

        if (CyclicCachedPoseNode != nullptr)
        {
            EdNodeGraph::FError Error;
            Error.Name        = "Cyclic";
            Error.Description = "Cycle detected feeding a Save Cached Pose node! Graph must be acyclic.";
            Error.Node        = CyclicCachedPoseNode;
            Compiler.AddError(Error);
            return false;
        }

        THashSet<CEdGraphNode*> EmittedNodes;
        for (CEdGraphNode* Node : CachedPoseNodes)
        {
            if (CAnimGraphNode* AnimNode = Cast<CAnimGraphNode>(Node))
            {
                AnimNode->GenerateBytecode(Compiler);
                EmittedNodes.insert(Node);
            }
        }

        // SortedNodes is dependency-ordered, so each node's input registers exist by the time it emits.
        for (uint32 i = 0; i < (uint32)SortedNodes.size(); ++i)
        {
            CEdGraphNode* Node = SortedNodes[i];
            Node->SetDebugExecutionOrder(i);

            if (EmittedNodes.find(Node) != EmittedNodes.end())
            {
                continue;
            }

            if (CAnimGraphNode* AnimNode = Cast<CAnimGraphNode>(Node))
            {
                AnimNode->GenerateBytecode(Compiler);
            }
        }

        // The Output node resolves but does not emit, so hand its register back to the caller.
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
                // Registered even when unwired, or the Parameters panel never sees the name.
                Compiler.AddParameter(GetParam->ParameterName, EAnimGraphParamType::Float, GetParam->DefaultValue);
            }
            else if (CAnimGraphNode_StateMachine* StateMachine = Cast<CAnimGraphNode_StateMachine>(Node))
            {
                CAnimStateMachineGraph* SMGraph = StateMachine->StateMachineGraph.Get();
                if (SMGraph == nullptr)
                {
                    continue;
                }

                // Each transition condition declares a parameter the runtime reads at evaluation time.
                for (const TObjectPtr<CAnimStateTransition>& Transition : SMGraph->Transitions)
                {
                    if (!Transition.IsValid())
                    {
                        continue;
                    }

                    Transition->MigrateLegacyCondition();
                    for (const FAnimTransitionCondition& Condition : Transition->Conditions)
                    {
                        if (Condition.ConditionSource == EAnimTransitionSource::Parameter && !Condition.ParameterName.IsNone())
                        {
                            Compiler.AddParameter(Condition.ParameterName, EAnimGraphParamType::Float, 0.0f);
                        }
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

        // Includes errors from nested state blend trees compiled mid-walk.
        for (auto& Error : Compiler.GetErrors())
        {
            if (Error.Node)
            {
                Error.Node->SetError(Error);
            }
        }
    }
}
