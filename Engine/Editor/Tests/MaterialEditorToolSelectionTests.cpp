#include <gtest/gtest.h>

#include "ImGuiTestHarness.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/UpdateContext.h"
#include "Material/MaterialOps.h"
#include "Settings/EditorSettings.h"
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
        static uint64 GetAutoCompileOutputHash(const FMaterialEditorTool& Tool) { return Tool.AutoCompileOutputHash; }

        static void SettleAutoCompile(FMaterialEditorTool& Tool)
        {
            Tool.UpdateAutoCompile(0.0);
            Tool.UpdateAutoCompile(1.0);
        }

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

TEST(MaterialEditorTool, AutoCompileTracksTheGraphFeedingTheOutput)
{
    FImGuiTestHarness Harness;
    CMaterial* Material = NewObject<CMaterial>(CPackage::CreatePackage("/MaterialEditorToolSelectionTests/AutoCompile"), "AutoCompile");
    FMaterialEditorTool Tool(nullptr, Material);
    Tool.OnInitialize();

    CEdNodeGraph* Graph = Tool.GetActiveNodeGraph();
    THashSet<CEdGraphNode*> Contributing;
    Graph->CollectContributingNodes(Contributing);
    ASSERT_EQ(Contributing.Num(), 1);
    CEdNodeGraphPin* OutputInput = NodeGraphOps::FindPin(*Contributing.begin(), "Roughness", ENodePinDirection::Input);
    ASSERT_NE(OutputInput, nullptr);

    CClass* ConstantClass = FindObject<CClass>(FName("CMaterialExpression_ConstantFloat"));
    ASSERT_NE(ConstantClass, nullptr);
    CEdGraphNode* Constant = Graph->CreateNode(ConstantClass);
    ASSERT_NE(Constant, nullptr);
    ASSERT_FALSE(Constant->GetOutputPins().IsEmpty());
    CEdNodeGraphPin* ConstantOutput = Constant->GetOutputPins()[0].Get();
    FProperty* Value = ConstantClass->GetProperty("Value");
    ASSERT_NE(Value, nullptr);

    CMaterialEditorSettings* Settings = GetMutableDefault<CMaterialEditorSettings>();
    const bool bPreviousAutoCompile = Settings->bAutoCompile;
    const uint64 InitialHash = FMaterialEditorToolTestAccess::GetAutoCompileOutputHash(Tool);
    Settings->bAutoCompile = true;
    Value->SetValue(Constant, FVector4(0.5f));
    Constant->NotifyValueEdited();
    FMaterialEditorToolTestAccess::SettleAutoCompile(Tool);
    EXPECT_EQ(FMaterialEditorToolTestAccess::GetAutoCompileOutputHash(Tool), InitialHash);

    Settings->bAutoCompile = false;
    EXPECT_TRUE(Graph->ConnectPins(ConstantOutput, OutputInput));
    FMaterialEditorToolTestAccess::SettleAutoCompile(Tool);
    EXPECT_EQ(FMaterialEditorToolTestAccess::GetAutoCompileOutputHash(Tool), InitialHash);

    Settings->bAutoCompile = true;
    FMaterialEditorToolTestAccess::SettleAutoCompile(Tool);
    const uint64 ConnectedHash = FMaterialEditorToolTestAccess::GetAutoCompileOutputHash(Tool);
    EXPECT_NE(ConnectedHash, InitialHash);

    Value->SetValue(Constant, FVector4(0.75f));
    Constant->NotifyValueEdited();
    FMaterialEditorToolTestAccess::SettleAutoCompile(Tool);
    EXPECT_NE(FMaterialEditorToolTestAccess::GetAutoCompileOutputHash(Tool), ConnectedHash);

    Graph->BreakLink(ConstantOutput, OutputInput);
    FMaterialEditorToolTestAccess::SettleAutoCompile(Tool);
    EXPECT_EQ(FMaterialEditorToolTestAccess::GetAutoCompileOutputHash(Tool), InitialHash);

    Settings->bAutoCompile = bPreviousAutoCompile;
    Tool.OnDeinitialize(FUpdateContext());
}
