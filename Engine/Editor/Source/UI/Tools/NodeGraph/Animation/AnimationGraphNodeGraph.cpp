#include "AnimationGraphNodeGraph.h"
#include "AnimGraphSchema.h"
#include "AnimationGraphCompiler.h"
#include "AnimGraphNode.h"
#include "AnimGraphDebugView.h"
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
#include "Containers/Algorithm.h"
#include "UI/Tools/NodeGraph/GraphAlgorithms.h"
#include "Tools/UI/ImGui/EditorColors.h"

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

        const bool bHasOutputNode = Algo::AnyOf(Nodes, [](const TStrongObjectPtr<CEdGraphNode>& Node)
        {
            return Node.IsValid() && Node->IsA<CAnimGraphNode_Output>();
        });

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
        Connections.Reset();
        Connections.Reserve(16);

        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            for (const auto& InputPinRef : Node->GetInputPins())
            {
                CEdNodeGraphPin* InputPin = InputPinRef.Get();
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    Connections.Add(InputPin->PinID);
                    Connections.Add(Connection->PinID);
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

        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            Node->ClearError();
        }

        if (Nodes.IsEmpty())
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

        if (SortedNodes.IsEmpty())
        {
            EdNodeGraph::FError Error;
            Error.Name        = "No Output";
            Error.Description = "Animation graph has no Output Pose node to compile from.";
            Error.Node        = nullptr;
            Compiler.AddError(Error);
            return false;
        }

        // A Save branch can read another cache from inside a nested state graph, which the pin-level
        // sort cannot see. So order the Save nodes by that dependency and emit one branch at a time.
        TVector<CAnimGraphNode_SaveCachedPose*> PendingSaves;
        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (CAnimGraphNode_SaveCachedPose* Save = Cast<CAnimGraphNode_SaveCachedPose>(Node))
            {
                PendingSaves.Add(Save);
            }
        }

        THashSet<CEdGraphNode*> EmittedNodes;
        THashSet<FName> WrittenCaches;

        while (!PendingSaves.IsEmpty())
        {
            int32 ReadyIndex = -1;
            for (int32 i = 0; i < PendingSaves.Num(); ++i)
            {
                THashSet<CEdGraphNode*> BranchNodes;
                GraphAlgorithms::CollectReachableFromRoot(PendingSaves[i], BranchNodes);

                THashSet<FName> ReadCaches;
                CollectReadCacheNames(BranchNodes, ReadCaches);

                bool bWaiting = false;
                for (const FName& ReadCache : ReadCaches)
                {
                    if (WrittenCaches.Contains(ReadCache))
                    {
                        continue;
                    }

                    for (CAnimGraphNode_SaveCachedPose* Other : PendingSaves)
                    {
                        if (Other != PendingSaves[i] && Other->CacheName == ReadCache)
                        {
                            bWaiting = true;
                            break;
                        }
                    }

                    if (bWaiting)
                    {
                        break;
                    }
                }

                if (!bWaiting)
                {
                    ReadyIndex = i;
                    break;
                }
            }

            // Every remaining Save waits on another, so let the Use node report the circular read.
            if (ReadyIndex == -1)
            {
                ReadyIndex = 0;
            }

            CAnimGraphNode_SaveCachedPose* ReadySave = PendingSaves[ReadyIndex];
            PendingSaves.RemoveAt(ReadyIndex);

            TVector<CEdGraphNode*> BranchOrder;
            CEdGraphNode* CyclicCachedPoseNode = GraphAlgorithms::TopologicalSortFromRoot(Nodes, BranchOrder,
                [ReadySave](CEdGraphNode* Node) { return Node == ReadySave; });

            if (CyclicCachedPoseNode != nullptr)
            {
                EdNodeGraph::FError Error;
                Error.Name        = "Cyclic";
                Error.Description = "Cycle detected feeding a Save Cached Pose node! Graph must be acyclic.";
                Error.Node        = CyclicCachedPoseNode;
                Compiler.AddError(Error);
                return false;
            }

            for (CEdGraphNode* Node : BranchOrder)
            {
                if (EmittedNodes.Contains(Node))
                {
                    continue;
                }

                if (CAnimGraphNode* AnimNode = Cast<CAnimGraphNode>(Node))
                {
                    AnimNode->GenerateBytecode(Compiler);
                    EmittedNodes.Add(Node);
                }
            }

            WrittenCaches.Add(ReadySave->CacheName);
        }

        // SortedNodes is dependency-ordered, so each node's input registers exist by the time it emits.
        for (int32 i = 0; i < SortedNodes.Num(); ++i)
        {
            CEdGraphNode* Node = SortedNodes[i];
            Node->SetDebugExecutionOrder(i);

            if (EmittedNodes.Contains(Node))
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

    void CAnimationGraphNodeGraph::CollectReadCacheNames(const THashSet<CEdGraphNode*>& SubtreeNodes, THashSet<FName>& OutNames)
    {
        for (CEdGraphNode* Node : SubtreeNodes)
        {
            if (CAnimGraphNode_UseCachedPose* Use = Cast<CAnimGraphNode_UseCachedPose>(Node))
            {
                OutNames.Add(Use->CacheName);
            }

            CAnimGraphNode_StateMachine* Machine = Cast<CAnimGraphNode_StateMachine>(Node);
            if (Machine == nullptr)
            {
                continue;
            }

            CAnimStateMachineGraph* SMGraph = Machine->StateMachineGraph.Get();
            if (SMGraph == nullptr)
            {
                continue;
            }

            for (const auto& SubNodeRef : SMGraph->Nodes)
            {
                CEdGraphNode* SubNode = SubNodeRef.Get();
                if (CAnimGraphNode_State* State = Cast<CAnimGraphNode_State>(SubNode))
                {
                    CollectReadCacheNamesInGraph(State->BlendTree.Get(), OutNames);
                }
            }
        }
    }

    void CAnimationGraphNodeGraph::CollectReadCacheNamesInGraph(CAnimationGraphNodeGraph* Graph, THashSet<FName>& OutNames)
    {
        if (Graph == nullptr)
        {
            return;
        }

        THashSet<CEdGraphNode*> GraphNodes;
        for (const auto& NodeRef : Graph->Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            GraphNodes.Add(Node);
        }

        CollectReadCacheNames(GraphNodes, OutNames);
    }

    void CAnimationGraphNodeGraph::CollectAllParameters(FAnimationGraphCompiler& Compiler)
    {
        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
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
                for (const TStrongObjectPtr<CAnimStateTransition>& Transition : SMGraph->Transitions)
                {
                    if (!Transition.IsValid())
                    {
                        continue;
                    }

                    Transition->MigrateLegacyCondition();
                    for (const SAnimTransitionCondition& Condition : Transition->Conditions)
                    {
                        if (Condition.ConditionSource == EAnimTransitionSource::Parameter && !Condition.ParameterName.IsNone())
                        {
                            Compiler.AddParameter(Condition.ParameterName, EAnimGraphParamType::Float, 0.0f);
                        }
                    }
                }

                // Recurse into each state's blend tree.
                for (const auto& SubNodeRef : SMGraph->Nodes)
                {
                    CEdGraphNode* SubNode = SubNodeRef.Get();
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
        // Collect parameters before topo-sort; unconnected Get Parameter nodes would otherwise be invisible to SetFloat().
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

    namespace
    {
        constexpr float kIdleWeight = 0.005f;

        bool IsPosePin(const CEdNodeGraphPin* Pin)
        {
            const CAnimGraphPin* AnimPin = Cast<CAnimGraphPin>(Pin);
            return AnimPin != nullptr && AnimPin->GetPinType() == EAnimPinType::Pose;
        }

        float ResolvePoseWeight(const CEdGraphNode* Node, float RootWeight, FAnimGraphDebugView& View, THashSet<const CEdGraphNode*>& Visiting)
        {
            if (auto It = View.NodeWeights.Find(Node); It != nullptr)
            {
                return (*It);
            }
            if (Cast<CAnimGraphNode_Output>(Node) != nullptr)
            {
                return RootWeight;
            }
            if (!Visiting.TryAdd(Node))
            {
                return 0.0f;
            }

            // A reroute carries whatever its wire does, so only an anim node's value outputs are left out.
            float Weight = 0.0f;
            for (const auto& OutputRef : Node->GetOutputPins())
            {
                const CEdNodeGraphPin* Output = OutputRef.Get();
                if (Cast<CAnimGraphPin>(Output) != nullptr && !IsPosePin(Output))
                {
                    continue;
                }
                for (CEdNodeGraphPin* Input : Output->GetConnections())
                {
                    const CEdGraphNode* Consumer = Input->GetOwningNode();
                    const CAnimGraphNode* AnimConsumer = Cast<CAnimGraphNode>(Consumer);
                    Weight += ResolvePoseWeight(Consumer, RootWeight, View, Visiting) * (AnimConsumer != nullptr ? AnimConsumer->GetPoseInputShare(Input, View) : 1.0f);
                }
            }

            Weight = Math::Clamp(Weight, 0.0f, 1.0f);
            View.NodeWeights[Node] = Weight;
            return Weight;
        }
    }

    void CAnimationGraphNodeGraph::ComputePoseWeights(FAnimGraphDebugView& View, float RootWeight) const
    {
        View.NodeWeights.Reset();
        THashSet<const CEdGraphNode*> Visiting;
        for (const TStrongObjectPtr<CEdGraphNode>& NodeRef : Nodes)
        {
            if (Algo::AnyOf(NodeRef->GetOutputPins(), [](const auto& Pin) { return IsPosePin(Pin.Get()); }))
            {
                ResolvePoseWeight(NodeRef.Get(), RootWeight, View, Visiting);
            }
        }
    }

    TOptional<float> CAnimationGraphNodeGraph::GetPoseLinkWeight(const CEdNodeGraphPin* InputPin, const CEdNodeGraphPin* OutputPin) const
    {
        if (DebugView == nullptr || !DebugView->bShowWeights || !IsPosePin(OutputPin))
        {
            return {};
        }

        const CEdGraphNode* Consumer = InputPin->GetOwningNode();
        const CAnimGraphNode* AnimConsumer = Cast<CAnimGraphNode>(Consumer);
        const float Share = AnimConsumer != nullptr ? AnimConsumer->GetPoseInputShare(InputPin, *DebugView) : 1.0f;
        const float ConsumerWeight = Cast<CAnimGraphNode_Output>(Consumer) != nullptr
            ? DebugView->GetNodeWeight(OutputPin->GetOwningNode())
            : DebugView->GetNodeWeight(Consumer);
        return Math::Clamp(ConsumerWeight * Share, 0.0f, 1.0f);
    }

    bool CAnimationGraphNodeGraph::HasNodePreview(const CEdGraphNode* Node)
    {
        const CAnimGraphNode* AnimNode = Cast<CAnimGraphNode>(Node);
        return AnimNode != nullptr && AnimNode->HasPreview();
    }

    void CAnimationGraphNodeGraph::DrawNodePreviewContent(CEdGraphNode* Node)
    {
        static_cast<CAnimGraphNode*>(Node)->DrawPreview(DebugView != nullptr && DebugView->bLive ? DebugView : nullptr);
    }

    bool CAnimationGraphNodeGraph::IsNodeIdle(const CEdGraphNode* Node) const
    {
        return DebugView != nullptr && DebugView->bShowWeights && DebugView->NodeWeights.Contains(Node) && DebugView->GetNodeWeight(Node) < kIdleWeight;
    }

    void CAnimationGraphNodeGraph::GetLinkStyle(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, ImU32& OutColor, float& OutThickness) const
    {
        CEdNodeGraph::GetLinkStyle(InputPin, OutputPin, OutColor, OutThickness);
        if (const TOptional<float> Weight = GetPoseLinkWeight(InputPin, OutputPin); Weight.IsSet())
        {
            OutThickness = Math::Lerp(1.0f, OutThickness * 2.0f, Weight.GetValue());
            OutColor = EditorColors::ScaleAlpha(OutColor, 0.25f + 0.75f * Weight.GetValue());
        }
    }

    bool CAnimationGraphNodeGraph::GetLinkBadge(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, FString& OutText, ImU32& OutColor)
    {
        const TOptional<float> Weight = GetPoseLinkWeight(InputPin, OutputPin);
        if (!Weight.IsSet() || Weight.GetValue() < kIdleWeight || Weight.GetValue() > 1.0f - kIdleWeight)
        {
            return false;
        }

        OutText = Format("{:.0f}%", Weight.GetValue() * 100.0f);
        OutColor = EditorColors::U32(EditorColors::Accent());
        return true;
    }

    bool CAnimationGraphNodeGraph::WantsLinkFlow(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin)
    {
        if (DebugView == nullptr || !DebugView->bShowWeights)
        {
            return false;
        }

        if (const TOptional<float> Weight = GetPoseLinkWeight(InputPin, OutputPin); Weight.IsSet())
        {
            return Weight.GetValue() >= kIdleWeight;
        }

        // A value wire runs only while what it carries is changing.
        const FAnimValueHistory* History = DebugView->FindHistory(OutputPin);
        return History != nullptr && History->Num() >= 2 && Math::Abs((*History)[History->Num() - 1] - (*History)[History->Num() - 2]) > 1e-5f;
    }
}
