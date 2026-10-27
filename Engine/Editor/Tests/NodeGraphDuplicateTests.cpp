#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/EditorAssetActions.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_StaticSwitch.h"

using namespace Lumina;

// Duplicating an asset copies its graph whole, so the copy keeps every link and where each node sits.
TEST(NodeGraphDuplicate, ACopiedGraphKeepsItsLinksAndLayout)
{
    // The editor makes every default object at startup, which links each class's properties for serialization.
    CEdNodeGraph::StaticClass()->GetDefaultObject();
    CMaterialExpression_StaticSwitch::StaticClass()->GetDefaultObject();

    CPackage* Source = CPackage::CreatePackage("/GraphDuplicate/Source");
    CEdNodeGraph* Graph = NewObject<CEdNodeGraph>(Source, "Graph");
    CMaterialExpression_StaticSwitch* Switch = NewObject<CMaterialExpression_StaticSwitch>(Source, "Switch");
    CMaterialExpression_StaticSwitch* Feed   = NewObject<CMaterialExpression_StaticSwitch>(Source, "Feed");
    Graph->AddNode(Switch);
    Graph->AddNode(Feed);
    Switch->SetGridPos(120.0f, 340.0f);
    ASSERT_TRUE(Graph->ConnectPins(Feed->Output, Switch->True));

    // The base graph keeps no saved link list of its own, which the material graph builds in ValidateGraph.
    Graph->Connections = { Switch->True->GetPinGUID(), Feed->Output->GetPinGUID() };

    CObject* Sources[] = { Graph, Switch, Feed };
    const THashMap<CObject*, CObject*> Remap = CopyObjectsInto(Sources, CPackage::CreatePackage("/GraphDuplicate/Copy"),
        [](CObject* Object) { return Object->GetName(); });
    ASSERT_EQ(Remap.size(), 3u);

    const CMaterialExpression_StaticSwitch* CopiedSwitch = static_cast<CMaterialExpression_StaticSwitch*>(Remap.at(Switch));
    const CMaterialExpression_StaticSwitch* CopiedFeed   = static_cast<CMaterialExpression_StaticSwitch*>(Remap.at(Feed));
    ASSERT_NE(CopiedSwitch->True, nullptr);
    ASSERT_TRUE(CopiedSwitch->True->HasConnection());
    EXPECT_EQ(CopiedSwitch->True->GetConnection(0), CopiedFeed->Output);
    EXPECT_EQ(CopiedSwitch->GetNodeX(), 120.0f);
    EXPECT_EQ(CopiedSwitch->GetNodeY(), 340.0f);
}
