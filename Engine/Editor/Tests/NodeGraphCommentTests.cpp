#include <gtest/gtest.h>

#include "Containers/Name.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "UI/Tools/NodeGraph/EdNode_Comment.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"

using namespace Lumina;

namespace
{
    // The graph classes are not exported from the editor module, so the suite finds them through reflection.
    CClass* FindGraphClass(const char* Name)
    {
        return FindObject<CClass>(FName(Name));
    }
}

TEST(NodeGraphComment, EveryGraphTypeOffersComments)
{
    const CEdNode_Comment* Comment = CEdNode_Comment::StaticClass()->GetDefaultObject<CEdNode_Comment>();
    ASSERT_NE(Comment, nullptr);

    const char* GraphClasses[] =
    {
        "CMaterialNodeGraph", "CMaterialFunctionGraph", "CAnimationGraphNodeGraph",
        "CAnimStateMachineGraph", "CAudioNodeGraph", "CParticleNodeGraph",
    };
    for (const char* Name : GraphClasses)
    {
        CClass* GraphClass = FindGraphClass(Name);
        ASSERT_NE(GraphClass, nullptr) << Name;
        EXPECT_TRUE(Comment->IsSupportedInGraph(GraphClass)) << Name;
    }
}

// A material recompiles when its graph's content version moves, and a comment changes nothing it compiles.
TEST(NodeGraphComment, EditingACommentDoesNotAskForARecompile)
{
    CEdNodeGraph* Graph = NewObject<CEdNodeGraph>();
    CEdNode_Comment* Comment = NewObject<CEdNode_Comment>();
    Graph->AddNode(Comment);

    const uint64 Before = Graph->GetContentVersion();
    Comment->Title = "Renamed";
    Comment->NotifyValueEdited();
    EXPECT_EQ(Graph->GetContentVersion(), Before);
}

// Delete Unused Nodes removes whatever reaches no output, and a comment never reaches one.
TEST(NodeGraphComment, ACommentIsNeverAnUnusedNode)
{
    CClass* GraphClass  = FindGraphClass("CMaterialNodeGraph");
    CClass* OutputClass = FindGraphClass("CMaterialOutputNode");
    CClass* SwitchClass = FindGraphClass("CMaterialExpression_StaticSwitch");
    ASSERT_NE(GraphClass, nullptr);
    ASSERT_NE(OutputClass, nullptr);
    ASSERT_NE(SwitchClass, nullptr);

    CEdNodeGraph* Graph = NewObject<CEdNodeGraph>(GraphClass);
    CEdGraphNode* Output = NewObject<CEdGraphNode>(OutputClass);
    CEdGraphNode* Unused = NewObject<CEdGraphNode>(SwitchClass);
    CEdNode_Comment* Comment = NewObject<CEdNode_Comment>();
    Graph->AddNode(Output);
    Graph->AddNode(Unused);
    Graph->AddNode(Comment);

    TVector<CEdGraphNode*> Dead;
    Graph->CollectDeadNodes(Dead);
    ASSERT_EQ(Dead.Num(), 1);
    EXPECT_EQ(Dead[0], Unused);
}
