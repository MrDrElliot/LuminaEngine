#pragma once

#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "imgui.h"

namespace Lumina
{
    // Editor for CPhysicsMaterial: all fields are reflected, so this just hosts
    // FAssetEditorTool's PropertyTable in a docked window. No 3D preview.
    class FPhysicsMaterialEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FPhysicsMaterialEditorTool)

        FPhysicsMaterialEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        bool ShouldGenerateThumbnailOnSave() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_BOWLING; }

        void OnInitialize() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;
    };
}
