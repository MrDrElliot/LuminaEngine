#include "MaterialFunctionEditorTool.h"

#include "imgui.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Log/Log.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphCompile.h"
#include "UI/Tools/NodeGraph/Material/MaterialFunctionGraph.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Function.h"
#include "Material/MaterialOps.h"
#include "UI/Tools/EditorToolContext.h"

namespace Lumina
{
    static const char* FunctionGraphWindowName      = "Function Graph";
    static const char* FunctionPropertiesWindowName = "Properties";
    static const char* FunctionSignatureWindowName  = "Signature";

    static const char* ValueTypeName(EMaterialValueType Type)
    {
        switch (Type)
        {
            case EMaterialValueType::Float:  return "float";
            case EMaterialValueType::Float2: return "float2";
            case EMaterialValueType::Float3: return "float3";
            case EMaterialValueType::Float4: return "float4";
            case EMaterialValueType::TextureHandle: return "texture handle";
            default:                         return "float";
        }
    }

    FMaterialFunctionEditorTool::FMaterialFunctionEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, *InAsset->GetName(), InAsset)
    {
    }

    void FMaterialFunctionEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CreateToolWindow(FunctionGraphWindowName,      [&](bool) { DrawGraphWindow(); });
        CreateToolWindow(FunctionPropertiesWindowName, [&](bool) { DrawPropertiesWindow(); });
        CreateToolWindow(FunctionSignatureWindowName,  [&](bool) { DrawSignatureWindow(); });

        NodeGraph = static_cast<CMaterialFunctionGraph*>(MaterialOps::FindOrCreateFunctionGraph(Cast<CMaterialFunction>(Asset.Get())));

        NodeGraph->Initialize();
        NodeGraph->SetTransactionManager(&GetTransactionManager());
        NodeGraph->SetNodeSelectedCallback([this](CEdGraphNode* Node)
        {
            if (Node != SelectedNode)
            {
                SelectedNode = Node;
                if (SelectedNode == nullptr)
                {
                    GetPropertyTable()->SetObject(Asset.Get(), Asset->GetClass());
                }
                else
                {
                    GetPropertyTable()->SetObject(Node, Node->GetClass());
                }
            }
        });

        NodeGraph->SetNodeDoubleClickedCallback([this](CEdGraphNode* Node)
        {
            CMaterialExpression_MaterialFunctionCall* Call = Cast<CMaterialExpression_MaterialFunctionCall>(Node);
            if (Call == nullptr || ToolContext == nullptr)
            {
                return;
            }

            if (!Call->Function.IsValid())
            {
                ImGuiX::Notifications::NotifyWarning("This Material Function node has no function assigned.");
                return;
            }

            // A function calling itself would reopen this same tab, which reads as the click doing nothing.
            if (Call->Function.Get() == Asset.Get())
            {
                ImGuiX::Notifications::NotifyWarning("'{0}' is this function.", *Call->Function->GetName());
                return;
            }

            ToolContext->OpenAssetEditor(Call->Function->GetGUID());
        });

        NodeGraph->SetPreNodeDeletedCallback([this](const CEdGraphNode* Node)
        {
            if (Node == SelectedNode)
            {
                SelectedNode = nullptr;
                GetPropertyTable()->SetObject(nullptr, nullptr);
            }
        });
    }

    void FMaterialFunctionEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        if (NodeGraph)
        {
            NodeGraph->Shutdown();
            NodeGraph = nullptr;
        }
    }

    void FMaterialFunctionEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        if (ImGui::MenuItem(LE_ICON_RECEIPT_TEXT " Compile"))
        {
            OnSave();
        }
    }

    void FMaterialFunctionEditorTool::DrawGraphWindow()
    {
        if (NodeGraph)
        {
            NodeGraph->DrawGraph();
        }
    }

    void FMaterialFunctionEditorTool::DrawPropertiesWindow()
    {
        GetPropertyTable()->DrawTree();
    }

    void FMaterialFunctionEditorTool::DrawSignatureWindow()
    {
        CMaterialFunction* Fn = GetAsset<CMaterialFunction>();
        if (Fn == nullptr)
        {
            return;
        }

        if (bHasErrors)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            ImGui::TextWrapped("Last compile reported errors:\n%s", *CompilationLog);
            ImGui::PopStyleColor();
            ImGui::Separator();
        }

        ImGui::TextDisabled("Add FunctionInput / FunctionOutput nodes in the graph to define the signature, then Compile.");
        ImGui::Spacing();

        const ImVec4 Header(0.70f, 0.85f, 1.00f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, Header);
        ImGui::TextUnformatted("Inputs");
        ImGui::PopStyleColor();
        ImGui::Separator();
        if (Fn->GetInputs().IsEmpty())
        {
            ImGui::TextDisabled("  (none)");
        }
        for (const SMaterialFunctionInput& In : Fn->GetInputs())
        {
            ImGui::BulletText("%s : %s", *In.Name, ValueTypeName(In.Type));
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, Header);
        ImGui::TextUnformatted("Outputs");
        ImGui::PopStyleColor();
        ImGui::Separator();
        if (Fn->GetOutputs().IsEmpty())
        {
            ImGui::TextDisabled("  (none)");
        }
        for (const SMaterialFunctionOutput& Out : Fn->GetOutputs())
        {
            ImGui::BulletText("%s : %s", *Out.Name, ValueTypeName(Out.Type));
        }
    }

    void FMaterialFunctionEditorTool::CompileAndSyncSignature()
    {
        CMaterialFunction* Fn = GetAsset<CMaterialFunction>();
        if (Fn == nullptr || NodeGraph == nullptr)
        {
            return;
        }

        MaterialOps::SyncFunctionSignature(Fn, NodeGraph.Get());

        // Runs every node into a throwaway compiler to surface type errors.
        FMaterialCompiler Compiler;
        NodeGraph->CompileForValidation(Compiler);

        bHasErrors = Compiler.HasErrors();
        CompilationLog.Reset();
        for (const EdNodeGraph::FError& Error : Compiler.GetErrors())
        {
            CompilationLog += "[" + Error.Name + "]: " + Error.Description + "\n";
        }

        Fn->GetPackage()->MarkDirty();
    }

    void FMaterialFunctionEditorTool::OnSave()
    {
        CompileAndSyncSignature();
        const bool bHasErrors = !CompilationLog.IsEmpty();
        if (bHasErrors && Asset != nullptr)
        {
            LOG_ERROR("Material function '{}' failed to compile:\n{}", *Asset->GetName(), *CompilationLog);
            ImGuiX::Notifications::NotifyError("'{0}' has compile errors, so materials using it were not recompiled. See the Signature panel.", *Asset->GetName());
        }

        FAssetEditorTool::OnSave();
        if (!bHasErrors && Asset != nullptr && Asset->GetPackage() != nullptr && !Asset->GetPackage()->IsDirty())
        {
            QueueMaterialFunctionRecompiles(Asset->GetGUID());
        }
    }

    void FMaterialFunctionEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0, rightBottomDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.30f, &rightDockID, &leftDockID);
        ImGui::DockBuilderSplitNode(rightDockID, ImGuiDir_Down, 0.40f, &rightBottomDockID, &rightDockID);

        ImGui::DockBuilderDockWindow(*GetToolWindowName(FunctionGraphWindowName),      leftDockID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(FunctionPropertiesWindowName), rightDockID);
        ImGui::DockBuilderDockWindow(*GetToolWindowName(FunctionSignatureWindowName),  rightBottomDockID);
    }

    CEdNodeGraph* FMaterialFunctionEditorTool::GetActiveNodeGraph()
    {
        return NodeGraph.Get();
    }
}
