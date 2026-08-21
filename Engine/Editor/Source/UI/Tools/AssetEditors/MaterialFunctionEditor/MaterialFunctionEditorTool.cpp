#include "MaterialFunctionEditorTool.h"

#include "imgui.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialFunctionGraph.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Function.h"

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
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FMaterialFunctionEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CreateToolWindow(FunctionGraphWindowName,      [&](bool) { DrawGraphWindow(); });
        CreateToolWindow(FunctionPropertiesWindowName, [&](bool) { DrawPropertiesWindow(); });
        CreateToolWindow(FunctionSignatureWindowName,  [&](bool) { DrawSignatureWindow(); });

        NodeGraph = Cast<CMaterialFunctionGraph>(Asset->GetPackage()->LoadObjectByName(FName(GMaterialFunctionGraphObjectName)));
        if (NodeGraph == nullptr)
        {
            NodeGraph = NewObject<CMaterialFunctionGraph>(Asset->GetPackage(), FName(GMaterialFunctionGraphObjectName));
        }

        NodeGraph->Initialize();
        NodeGraph->SetNodeSelectedCallback([this](CEdGraphNode* Node)
        {
            if (Node != SelectedNode)
            {
                SelectedNode = Node;
                if (SelectedNode == nullptr)
                {
                    GetPropertyTable()->SetObject(Asset, Asset->GetClass());
                }
                else
                {
                    GetPropertyTable()->SetObject(Node, Node->GetClass());
                }
            }
        });

        NodeGraph->SetPreNodeDeletedCallback([this](const CEdGraphNode* Node)
        {
            if (Node == SelectedNode)
            {
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
            CompileAndSyncSignature();
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
            ImGui::TextWrapped("Last compile reported errors:\n%s", CompilationLog.c_str());
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
        if (Fn->GetInputs().empty())
        {
            ImGui::TextDisabled("  (none)");
        }
        for (const FMaterialFunctionInput& In : Fn->GetInputs())
        {
            ImGui::BulletText("%s : %s", In.Name.c_str(), ValueTypeName(In.Type));
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, Header);
        ImGui::TextUnformatted("Outputs");
        ImGui::PopStyleColor();
        ImGui::Separator();
        if (Fn->GetOutputs().empty())
        {
            ImGui::TextDisabled("  (none)");
        }
        for (const FMaterialFunctionOutput& Out : Fn->GetOutputs())
        {
            ImGui::BulletText("%s : %s", Out.Name.c_str(), ValueTypeName(Out.Type));
        }
    }

    void FMaterialFunctionEditorTool::CompileAndSyncSignature()
    {
        CMaterialFunction* Fn = GetAsset<CMaterialFunction>();
        if (Fn == nullptr || NodeGraph == nullptr)
        {
            return;
        }

        TVector<CMaterialExpression_FunctionInput*> InputNodes;
        TVector<CMaterialFunctionOutput*>           OutputNodes;
        for (const TObjectPtr<CEdGraphNode>& N : NodeGraph->Nodes)
        {
            if (!N.IsValid())
            {
                continue;
            }
            if (CMaterialExpression_FunctionInput* In = Cast<CMaterialExpression_FunctionInput>(N.Get()))
            {
                InputNodes.push_back(In);
            }
            else if (CMaterialFunctionOutput* Out = Cast<CMaterialFunctionOutput>(N.Get()))
            {
                OutputNodes.push_back(Out);
            }
        }

        // A stable sort keeps a predictable pin order while leaving equal-priority nodes in graph order.
        Algo::StableSort(InputNodes.begin(), InputNodes.end(), [](CMaterialExpression_FunctionInput* A, CMaterialExpression_FunctionInput* B)
        {
            return A->SortPriority < B->SortPriority;
        });
        Algo::StableSort(OutputNodes.begin(), OutputNodes.end(), [](CMaterialFunctionOutput* A, CMaterialFunctionOutput* B)
        {
            return A->SortPriority < B->SortPriority;
        });

        Fn->Inputs.clear();
        Fn->Inputs.reserve(InputNodes.size());
        for (CMaterialExpression_FunctionInput* In : InputNodes)
        {
            FMaterialFunctionInput Decl;
            Decl.Name         = In->InputName;
            Decl.Type         = In->InputType;
            Decl.DefaultValue = In->DefaultValue;
            Decl.Description  = In->Description;
            Fn->Inputs.push_back(Move(Decl));
        }

        Fn->Outputs.clear();
        Fn->Outputs.reserve(OutputNodes.size());
        for (CMaterialFunctionOutput* Out : OutputNodes)
        {
            FMaterialFunctionOutput Decl;
            Decl.Name        = Out->OutputName;
            Decl.Type        = Out->OutputType;
            Decl.Description  = Out->Description;
            Fn->Outputs.push_back(Move(Decl));
        }

        // Runs every node into a throwaway compiler to surface type errors.
        FMaterialCompiler Compiler;
        NodeGraph->CompileForValidation(Compiler);

        bHasErrors = Compiler.HasErrors();
        CompilationLog.clear();
        for (const EdNodeGraph::FError& Error : Compiler.GetErrors())
        {
            CompilationLog += "[" + Error.Name + "]: " + Error.Description + "\n";
        }

        Fn->GetPackage()->MarkDirty();
    }

    void FMaterialFunctionEditorTool::OnSave()
    {
        // Always resync the signature from the graph before the asset is written.
        CompileAndSyncSignature();
        FAssetEditorTool::OnSave();
    }

    void FMaterialFunctionEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID leftDockID = 0, rightDockID = 0, rightBottomDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.30f, &rightDockID, &leftDockID);
        ImGui::DockBuilderSplitNode(rightDockID, ImGuiDir_Down, 0.40f, &rightBottomDockID, &rightDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(FunctionGraphWindowName).c_str(),      leftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(FunctionPropertiesWindowName).c_str(), rightDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(FunctionSignatureWindowName).c_str(),  rightBottomDockID);
    }
}
