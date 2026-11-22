#pragma once
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Tools/Import/ImportHelpers.h"

namespace Lumina
{
    class CTexture;

    class FTextureEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FTextureEditorTool)
        
        FTextureEditorTool(IEditorToolContext* Context, CObject* InAsset)
            : FAssetEditorTool(Context, *InAsset->GetName(), InAsset)
        {}

        
        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_IMAGE; }
        // Bulk data, so a whole-asset snapshot per edit would cost more than the undo is worth.
        bool WantsAutomaticAssetUndo() override { return false; }
        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void OnPropertyEditFinished(const FPropertyChangedEvent& Event) override;
        void OnPostUndoRedo() override;
        void OnAssetDataChangedExternally() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;


    private:

        /** Take or release the streaming pin so it matches whether the preview is actually on screen. */
        void UpdateStreamingPin();

        /** Re-cook now, from the session baseline when the asset has no source file on disk. */
        bool RecookForPropertyChange(CTexture* Texture);

        // Decoded mip 0 for a sourceless texture, so repeated edits re-cook from one fixed image.
        TOptional<Import::Textures::FTextureImportResult> SourcelessBaseline;

        float ZoomFactor = 1.0f;
        ImVec2 PanOffset = ImVec2(0.0f, 0.0f);
        // ImGui does not tone-map, so dimming the HDR preview recovers bright detail that would clip to white.
        float ExposureStops = 0.0f;
        bool bToneMapPreview = true;
        // Clamped to the live layer count each frame so a shortened array cannot leave it past the end.
        uint32 PreviewSlice = 0;

        // Held as the texture rather than a bool so the unpin targets what was pinned even across a reimport.
        TWeakObjectPtr<CTexture> PinnedTexture;

        // Set only when the preview is visible, since a pin exempts the texture from eviction and hidden 4K tabs would push the pool past VRAM.
        bool bPreviewDrawnSinceUpdate = false;
    };
}
