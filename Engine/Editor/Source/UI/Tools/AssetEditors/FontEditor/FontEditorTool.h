#pragma once
#include "UI/Tools/AssetEditors/AssetEditorTool.h"

struct ImFont;

namespace Lumina
{
    class FFontEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FFontEditorTool)

        FFontEditorTool(IEditorToolContext* Context, CObject* InAsset)
            : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
        {}

        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_FORMAT_FONT; }
        // Bulk data, so a whole-asset snapshot per edit would cost more than the undo is worth.
        bool WantsAutomaticAssetUndo() override { return false; }
        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        // Face rasterized from the asset's bytes via ImGui's dynamic atlas. Owned by
        // the atlas registration, not the asset; removed on deinitialize.
        ImFont* PreviewFont = nullptr;

        float PreviewSize = 48.0f;
        char  SampleText[512] = "The quick brown fox jumps over the lazy dog 0123456789";
    };
}
