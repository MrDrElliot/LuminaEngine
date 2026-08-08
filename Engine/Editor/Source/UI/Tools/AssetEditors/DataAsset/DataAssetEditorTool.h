#pragma once

#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "imgui.h"

namespace Lumina
{
    // Editor for CDataAsset and everything deriving from it: a property grid over the asset's reflected
    // properties. Registered against the base class, and asset-editor lookup walks the hierarchy, so
    // every subclass gets this without registering anything of its own.
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
