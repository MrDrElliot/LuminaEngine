#include "MCPAnimGraphTools.h"
#include "MCPToolTestUtil.h"

#include "Animation/AnimGraphOps.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateTransition.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPAnimGraphToolsTestsInternal
{
    class MCPAnimGraphToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPAnimGraphToolsTest() : FRegisteredToolsTest("MCPAnimGraphToolsTests", &RegisterAnimGraphTools) {}

        void SetUp() override
        {
            RegisterPendingTypes();
            FRegisteredToolsTest::SetUp();
            const FString Package = Format("/MCPAnimGraphToolsTests/{}", testing::UnitTest::GetInstance()->current_test_info()->name());
            Graph = NewObject<CAnimationGraph>(CPackage::CreatePackage(Package), "Scratch");
            Guid = FString(*Graph->GetGUID().ToString());
        }

        int64 AddNode(const char* Type, const char* GraphPath = "[]")
        {
            const FJsonValue Result = ExpectSucceeded("animgraph.node",
                FStringView(Format(R"({{"AnimGraph":"{}","GraphPath":{},"NodeType":"{}"}})", Guid, GraphPath, Type)));
            return Result["structuredContent"]["Id"].AsInt64();
        }

        int64 FindNodeOfType(const char* Type)
        {
            const FJsonValue Result = ExpectSucceeded("animgraph.describe", FStringView(Format(R"({{"AnimGraph":"{}"}})", Guid)));
            for (const FJsonValue& Node : Result["structuredContent"]["Nodes"].AsArray())
            {
                if (Node["Type"].AsString() == FStringView(Type))
                {
                    return Node["Id"].AsInt64();
                }
            }
            ADD_FAILURE() << "No " << Type << " on the root canvas.";
            return 0;
        }

        FString LinkArgs(const char* GraphPath, int64 From, const char* FromPin, int64 To, const char* ToPin)
        {
            return Format(R"({{"AnimGraph":"{}","GraphPath":{},"FromNode":{},"FromPin":"{}","ToNode":{},"ToPin":"{}"}})",
                Guid, GraphPath, From, FromPin, To, ToPin);
        }

        FString TransitionArgs(const char* GraphPath, int64 From, int64 To, const char* Path, const char* Value)
        {
            return Format(R"({{"AnimGraph":"{}","GraphPath":{},"FromNode":{},"ToNode":{},"Path":"{}","Value":{}}})",
                Guid, GraphPath, From, To, Path, Value);
        }

        CAnimationGraph* Graph = nullptr;
        FString Guid;
    };
}

using namespace MCPAnimGraphToolsTestsInternal;

TEST_F(MCPAnimGraphToolsTest, NodeAddsAndRemovesOnTheRootCanvas)
{
    const int64 Machine = AddNode("CAnimGraphNode_StateMachine");
    ASSERT_NE(NodeGraphOps::FindNode(AnimGraphOps::FindOrCreateGraph(Graph), Machine), nullptr);

    ExpectSucceeded("animgraph.node", FStringView(Format(R"({{"AnimGraph":"{}","Action":"Remove","Node":{}}})", Guid, Machine)));
    EXPECT_EQ(NodeGraphOps::FindNode(AnimGraphOps::FindOrCreateGraph(Graph), Machine), nullptr);
}

TEST_F(MCPAnimGraphToolsTest, GraphPathThroughAMissingNodeIsRefused)
{
    ExpectRefused("animgraph.node",
        FStringView(Format(R"({{"AnimGraph":"{}","GraphPath":[987654],"NodeType":"CAnimGraphNode_State"}})", Guid)),
        "GraphPath names node 987654");
}

TEST_F(MCPAnimGraphToolsTest, StatesLinkIntoAnEditableTransition)
{
    const int64 Machine = AddNode("CAnimGraphNode_StateMachine");
    const FString Path = Format("[{}]", Machine);
    const int64 Idle = AddNode("CAnimGraphNode_State", *Path);
    const int64 Run  = AddNode("CAnimGraphNode_State", *Path);

    CEdNodeGraph* States = NodeGraphOps::FindNode(AnimGraphOps::FindOrCreateGraph(Graph), Machine)->GetOwnedSubGraph();
    ASSERT_NE(States, nullptr);
    ASSERT_NE(NodeGraphOps::FindNode(States, Idle), nullptr);

    const FJsonValue Renamed = ExpectSucceeded("animgraph.set_node_property", FStringView(Format(
        R"({{"AnimGraph":"{}","GraphPath":{},"Node":{},"Path":"StateName","Value":"Idle"}})", Guid, Path, Idle)));
    EXPECT_NE(FString(Renamed["structuredContent"]["Current"].AsString()).Find("Idle"), Constants::kIndexNone);
    EXPECT_EQ(NodeGraphOps::FindNode(States, Idle)->GetNodeTitleText(), "Idle");

    ExpectSucceeded("animgraph.link", FStringView(LinkArgs(*Path, Idle, "Out", Run, "In")));
    ASSERT_EQ(AnimGraphOps::GetTransitions(States).Num(), 1);

    const uint64 VersionBeforeTransitionEdit = States->GetContentVersion();
    ExpectSucceeded("animgraph.set_transition_property", FStringView(TransitionArgs(*Path, Idle, Run, "BlendDuration", "0.5")));
    EXPECT_EQ(AnimGraphOps::GetTransitions(States)[0]->BlendDuration, 0.5f);
    EXPECT_GT(States->GetContentVersion(), VersionBeforeTransitionEdit);

    ExpectRefused("animgraph.set_transition_property", FStringView(TransitionArgs(*Path, Run, Idle, "BlendDuration", "0.5")),
        "No transition runs from node");

    ExpectSucceeded("animgraph.link", FStringView(Format(
        R"({{"AnimGraph":"{}","GraphPath":{},"Action":"Disconnect","Node":{},"Pin":"In"}})", Guid, Path, Run)));
    EXPECT_TRUE(AnimGraphOps::GetTransitions(States).IsEmpty());
    ExpectRefused("animgraph.set_transition_property", FStringView(TransitionArgs(*Path, Idle, Run, "BlendDuration", "0.5")),
        "No transition runs from node");
}

TEST_F(MCPAnimGraphToolsTest, CompileReportsAnEmptyStateMachine)
{
    const int64 Machine = AddNode("CAnimGraphNode_StateMachine");
    ExpectSucceeded("animgraph.link", FStringView(LinkArgs("[]", Machine, "Result", FindNodeOfType("CAnimGraphNode_Output"), "Result Pose")));

    ExpectRefused("animgraph.compile", FStringView(Format(R"({{"AnimGraph":"{}"}})", Guid)),
        *Format("[Empty State Machine] State Machine has no states; double-click it to add some. (node {})", Machine));
}
