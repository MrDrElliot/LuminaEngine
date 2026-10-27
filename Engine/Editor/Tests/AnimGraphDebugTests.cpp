#include <gtest/gtest.h>

#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/Animation/AnimGraphDebugView.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_Blend.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_ClipPlayer.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_Output.h"

using namespace Lumina;

namespace
{
    template<typename T>
    T* AddAnimNode(CAnimationGraphNodeGraph* Graph, CPackage* Package, const char* Name)
    {
        T* Node = NewObject<T>(Package, Name);
        Graph->AddNode(Node);
        return Node;
    }
}

// Each pose node is weighted by how much of the output it supplies, which is what the wires and the fade show.
TEST(AnimGraphDebug, PoseWeightsFollowTheBlendAlpha)
{
    CAnimationGraphNodeGraph::StaticClass()->GetDefaultObject();
    CAnimGraphNode_Output::StaticClass()->GetDefaultObject();
    CAnimGraphNode_Blend::StaticClass()->GetDefaultObject();
    CAnimGraphNode_ClipPlayer::StaticClass()->GetDefaultObject();

    CPackage* Package = CPackage::CreatePackage("/AnimGraphDebug/Weights");
    CAnimationGraphNodeGraph* Graph = NewObject<CAnimationGraphNodeGraph>(Package, "Graph");
    Graph->EnsureSetup();

    CAnimGraphNode_Output* Output = nullptr;
    for (const TStrongObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
    {
        Output = Output != nullptr ? Output : Cast<CAnimGraphNode_Output>(Node.Get());
    }
    ASSERT_NE(Output, nullptr);

    CAnimGraphNode_Blend* Blend = AddAnimNode<CAnimGraphNode_Blend>(Graph, Package, "Blend");
    CAnimGraphNode_ClipPlayer* Walk = AddAnimNode<CAnimGraphNode_ClipPlayer>(Graph, Package, "Walk");
    CAnimGraphNode_ClipPlayer* Run = AddAnimNode<CAnimGraphNode_ClipPlayer>(Graph, Package, "Run");
    CAnimGraphNode_ClipPlayer* Unused = AddAnimNode<CAnimGraphNode_ClipPlayer>(Graph, Package, "Unused");

    ASSERT_TRUE(Graph->ConnectPins(Walk->PosePin, Blend->PoseAPin));
    ASSERT_TRUE(Graph->ConnectPins(Run->PosePin, Blend->PoseBPin));
    ASSERT_TRUE(Graph->ConnectPins(Blend->ResultPin, Output->ResultPosePin));
    Blend->SetValuePinDefault(Blend->AlphaPin, 0.25f);

    FAnimGraphDebugView View;
    Graph->ComputePoseWeights(View, 1.0f);
    EXPECT_NEAR(View.GetNodeWeight(Blend), 1.0f, 1e-5f);
    EXPECT_NEAR(View.GetNodeWeight(Walk), 0.75f, 1e-5f);
    EXPECT_NEAR(View.GetNodeWeight(Run), 0.25f, 1e-5f);
    EXPECT_NEAR(View.GetNodeWeight(Unused), 0.0f, 1e-5f);

    // A live alpha wins over the pin's default.
    View.bLive = true;
    View.Scalars = { 0.9f };
    View.PinRegisters[Blend->AlphaPin] = 0;
    Graph->ComputePoseWeights(View, 1.0f);
    EXPECT_NEAR(View.GetNodeWeight(Walk), 0.1f, 1e-5f);
    EXPECT_NEAR(View.GetNodeWeight(Run), 0.9f, 1e-5f);

    // A state that is not current supplies nothing, however its own blend is set.
    Graph->ComputePoseWeights(View, 0.0f);
    EXPECT_NEAR(View.GetNodeWeight(Run), 0.0f, 1e-5f);
}
