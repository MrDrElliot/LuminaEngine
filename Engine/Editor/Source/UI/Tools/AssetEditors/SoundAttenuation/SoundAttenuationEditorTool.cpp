#include "SoundAttenuationEditorTool.h"

#include "Assets/AssetTypes/Audio/SoundAttenuation.h"
#include "UI/Properties/PropertyTable.h"
#include "imgui.h"

namespace Lumina
{
    static const char* SoundAttenuationWindowName = "Sound Attenuation";

    FSoundAttenuationEditorTool::FSoundAttenuationEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FSoundAttenuationEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CreateToolWindow(SoundAttenuationWindowName, [this](bool /*bFocused*/)
        {
            PropertyTable.DrawTree();
        });
    }

    void FSoundAttenuationEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& /*InDockspaceSize*/) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(SoundAttenuationWindowName).c_str(), InDockspaceID);
    }
}
