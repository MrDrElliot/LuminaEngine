#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_StaticSwitch.h"
#include "UI/Tools/Transactions/EditorTransaction.h"

using namespace Lumina;

// A tie changes only the layout, so nothing but its undo step marks the asset as needing a save.
TEST(NodeGraphZipTie, TyingAWireMarksTheGraphUnsaved)
{
    CEdNodeGraph::StaticClass()->GetDefaultObject();
    CMaterialExpression_StaticSwitch::StaticClass()->GetDefaultObject();

    CPackage* Package = CPackage::CreatePackage("/GraphZipTie/Dirty");
    CEdNodeGraph* Graph = NewObject<CEdNodeGraph>(Package, "Graph");
    CMaterialExpression_StaticSwitch* Switch = NewObject<CMaterialExpression_StaticSwitch>(Package, "Switch");
    CMaterialExpression_StaticSwitch* Feed   = NewObject<CMaterialExpression_StaticSwitch>(Package, "Feed");
    Graph->AddNode(Switch);
    Graph->AddNode(Feed);
    ASSERT_TRUE(Graph->ConnectPins(Feed->Output, Switch->True));

    FTransactionManager Manager;
    Graph->SetTransactionManager(&Manager);
    Package->ClearDirty();

    Graph->AddZipTie({ { Switch->True, Feed->Output } }, ImVec2(55.0f, 77.0f));
    Graph->CommitEdit(FName("Zip Tie"));

    EXPECT_TRUE(Package->IsDirty());
    Graph->SetTransactionManager(nullptr);
}
