#include "DataAssetEditorTool.h"

#include "UI/Properties/PropertyTable.h"
#include "imgui.h"

namespace Lumina
{
    // Neutral, since this tool also serves assets that are not CDataAsset.
    static const char* DataAssetWindowName = "Properties";

    FDataAssetEditorTool::FDataAssetEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FDataAssetEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        // The base ctor already bound the table and wired the callbacks, so this only adds a window.
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
