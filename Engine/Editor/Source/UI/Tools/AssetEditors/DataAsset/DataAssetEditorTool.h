#pragma once

#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "imgui.h"

namespace Lumina
{
    // A property grid over an asset's reflected properties, registered per asset class that wants one.
    class FDataAssetEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FDataAssetEditorTool)

        FDataAssetEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_DATABASE; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override {}
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
    };
}
