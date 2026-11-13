#include <gtest/gtest.h>

#include "ImGuiTestHarness.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/UpdateContext.h"
#include "Material/MaterialOps.h"
#include "UI/Tools/AssetEditors/MaterialEditor/MaterialEditorTool.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"

namespace Lumina
{
    struct FMaterialEditorToolTestAccess
    {
        static void SelectNode(FMaterialEditorTool& Tool, CEdGraphNode* Node)
        {
            Tool.SelectedNode = Node;
            Tool.GetPropertyTable()->SetObject(Node, Node->GetClass());
        }

        static CEdGraphNode* GetSelectedNode(const FMaterialEditorTool& Tool) { return Tool.SelectedNode; }
        static CEdGraphNode* GetCodeEditorBoundNode(const FMaterialEditorTool& Tool) { return Tool.CodeEditorBoundNode; }

        static void DrawCustomCodeEditor(FMaterialEditorTool& Tool)
        {
            Tool.DrawCustomCodeEditor();
        }
    };
}

using namespace Lumina;

// Deleting the selected node used to leave SelectedNode dangling, which DrawCustomCodeEditor reads every frame.
TEST(MaterialEditorTool, DeletingTheSelectedNodeClearsSelectionAndCodeEditorBinding)
{
    FImGuiTestHarness Harness;

    CMaterial* Material = NewObject<CMaterial>(CPackage::CreatePackage("/MaterialEditorToolSelectionTests/Scratch"), "Scratch");
    FMaterialEditorTool Tool(nullptr, Material);
    Tool.OnInitialize();

    CMaterialNodeGraph* Graph = MaterialOps::FindOrCreateGraph(Material);
    ASSERT_NE(Graph, nullptr);

    // The class is not exported from the editor module, so the suite finds it through reflection.
    CClass* CustomSlangClass = FindObject<CClass>(FName("CMaterialExpression_CustomSlang"));
    ASSERT_NE(CustomSlangClass, nullptr);

    CEdGraphNode* Node = Graph->CreateNode(CustomSlangClass);
    ASSERT_NE(Node, nullptr);

    FMaterialEditorToolTestAccess::SelectNode(Tool, Node);
    ASSERT_EQ(FMaterialEditorToolTestAccess::GetSelectedNode(Tool), Node);

    FImGuiTestHarness::BeginFrame();
    FMaterialEditorToolTestAccess::DrawCustomCodeEditor(Tool);
    FImGuiTestHarness::EndFrame();

    EXPECT_EQ(FMaterialEditorToolTestAccess::GetCodeEditorBoundNode(Tool), Node);

    FString Error;
    ASSERT_TRUE(NodeGraphOps::RemoveNode(Graph, Node, Error));

    EXPECT_EQ(FMaterialEditorToolTestAccess::GetSelectedNode(Tool), nullptr);
    EXPECT_EQ(Tool.GetPropertyTableObject(), nullptr);

    FImGuiTestHarness::BeginFrame();
    FMaterialEditorToolTestAccess::DrawCustomCodeEditor(Tool);
    FImGuiTestHarness::EndFrame();

    EXPECT_EQ(FMaterialEditorToolTestAccess::GetCodeEditorBoundNode(Tool), nullptr);

    Tool.OnDeinitialize(FUpdateContext());
}
