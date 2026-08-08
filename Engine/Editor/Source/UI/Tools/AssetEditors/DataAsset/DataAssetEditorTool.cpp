#include "DataAssetEditorTool.h"

#include "UI/Properties/PropertyTable.h"
#include "imgui.h"

namespace Lumina
{
    static const char* DataAssetWindowName = "Data Asset";

    FDataAssetEditorTool::FDataAssetEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FDataAssetEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        // FAssetEditorTool's ctor already bound PropertyTable to the asset's class and wired the
        // dirty/undo callbacks; all this tool adds is a window to draw it in.
        CreateToolWindow(DataAssetWindowName, [this](bool /*bFocused*/)
        {
            PropertyTable.DrawTree();
        });
    }

    void FDataAssetEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& /*InDockspaceSize*/) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(DataAssetWindowName).c_str(), InDockspaceID);
    }
}
