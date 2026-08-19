#pragma once

#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "imgui.h"

namespace Lumina
{
    // Every field is reflected, so this only hosts FAssetEditorTool's PropertyTable in a docked window.
    class FSoundAttenuationEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FSoundAttenuationEditorTool)

        FSoundAttenuationEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_SPEAKER; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override {}
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
    };
}
