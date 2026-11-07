#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "MCPGraphTools.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_StaticSwitch.h"

using namespace Lumina;

TEST(MCPGraphTools, AnOutputFeedingTwoInputsReportsBothConnections)
{
    CEdNodeGraph::StaticClass()->GetDefaultObject();
    CMaterialExpression_StaticSwitch::StaticClass()->GetDefaultObject();

    CPackage* Package = CPackage::CreatePackage("/MCPGraphTools/FanOut");
    CEdNodeGraph* Graph = NewObject<CEdNodeGraph>(Package, "Graph");
    CMaterialExpression_StaticSwitch* Feed   = NewObject<CMaterialExpression_StaticSwitch>(Package, "Feed");
    CMaterialExpression_StaticSwitch* First  = NewObject<CMaterialExpression_StaticSwitch>(Package, "First");
    CMaterialExpression_StaticSwitch* Second = NewObject<CMaterialExpression_StaticSwitch>(Package, "Second");
    Graph->AddNode(Feed);
    Graph->AddNode(First);
    Graph->AddNode(Second);
    ASSERT_TRUE(Graph->ConnectPins(Feed->Output, First->True));
    ASSERT_TRUE(Graph->ConnectPins(Feed->Output, Second->False));

    TVector<SGraphPinInfo> Pins;
    MCP::CollectPins(Feed, Pins);

    const SGraphPinInfo* Output = Pins.FindByPredicate([&](const SGraphPinInfo& Pin) { return Pin.Name == Feed->Output->GetPinName(); });
    ASSERT_NE(Output, nullptr);
    ASSERT_EQ(Output->Connections.Num(), 2);
    EXPECT_EQ(Output->Connections[0].Node, First->GetNodeID());
    EXPECT_EQ(Output->Connections[0].Pin, First->True->GetPinName());
    EXPECT_EQ(Output->Connections[1].Node, Second->GetNodeID());
    EXPECT_EQ(Output->Connections[1].Pin, Second->False->GetPinName());
}
