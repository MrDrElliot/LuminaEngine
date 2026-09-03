#pragma once

#include "Assets/AssetTypes/SpriteSheet/SpriteSheet.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "imgui.h"

namespace Lumina
{
    // Sheet canvas in the dominant window, clip list and frame strip beside it, all editing the asset live.
    class FSpriteSheetEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FSpriteSheetEditorTool)

        FSpriteSheetEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_ANIMATION; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override {}
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        void DrawSheetWindow();
        void DrawAnimationsWindow();
        void DrawPreviewWindow();

        /** Canvas and strip edits bypass the PropertyTable, so both have to be told. */
        void MarkEdited();

        NODISCARD SSpriteAnimation* GetSelectedAnimation();

        /** UV rect of one grid cell, matching the slicing the sprite component does at extract. */
        NODISCARD ImVec4 CellUVs(int32 Cell);

        int32 SelectedAnimation = 0;
        int32 SelectedFrameSlot = -1;

        FSpritePlayback Preview;
        bool            bPreviewPlaying = true;
    };
}
