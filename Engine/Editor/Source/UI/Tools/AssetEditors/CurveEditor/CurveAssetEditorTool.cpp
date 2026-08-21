#include "CurveAssetEditorTool.h"

#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "UI/Properties/PropertyTable.h"
#include "imgui.h"

namespace Lumina
{
    static const char* CurveWindowName = "Curve";
    static const char* CurveDetailsWindowName = "Details";

    FCurveAssetEditorTool::FCurveAssetEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FCurveAssetEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CCurveAsset* CurveAsset = GetAsset<CCurveAsset>();
        if (CurveAsset != nullptr)
        {
            CurveWidget.SetCurve(&CurveAsset->Curve);
        }

        // Canvas edits bypass the PropertyTable, so they dirty the package and rebuild the tree here.
        CurveWidget.SetOnModified([this]()
        {
            if (Asset.IsValid() && Asset->GetPackage() != nullptr)
            {
                Asset->GetPackage()->MarkDirty();
            }

            PropertyTable.MarkDirty();
        });

        CreateToolWindow(CurveWindowName, [this](bool /*bFocused*/)
        {
            CurveWidget.Draw("CurveEditor");
        });

        CreateToolWindow(CurveDetailsWindowName, [this](bool /*bFocused*/)
        {
            PropertyTable.DrawTree();
        });
    }

    void FCurveAssetEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& /*InDockspaceSize*/) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftDockID = 0;
        ImGuiID RightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.25f, &RightDockID, &LeftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(CurveWindowName).c_str(), LeftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(CurveDetailsWindowName).c_str(), RightDockID);
    }
}
