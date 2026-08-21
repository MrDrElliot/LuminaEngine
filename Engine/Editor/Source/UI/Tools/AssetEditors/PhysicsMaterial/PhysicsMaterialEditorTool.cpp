#include "PhysicsMaterialEditorTool.h"

#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "UI/Properties/PropertyTable.h"
#include "imgui.h"

namespace Lumina
{
    static const char* PhysicsMaterialWindowName = "Physics Material";

    FPhysicsMaterialEditorTool::FPhysicsMaterialEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FPhysicsMaterialEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        // The base ctor already bound the table and marks the package dirty, so this only adds a window.
        CreateToolWindow(PhysicsMaterialWindowName, [this](bool /*bFocused*/)
        {
            PropertyTable.DrawTree();
        });
    }

    void FPhysicsMaterialEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& /*InDockspaceSize*/) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(PhysicsMaterialWindowName).c_str(), InDockspaceID);
    }
}
