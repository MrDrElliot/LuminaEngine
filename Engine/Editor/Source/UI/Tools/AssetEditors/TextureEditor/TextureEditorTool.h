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
            : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
        {}

        
        bool IsSingleWindowTool() const override { return false; }
        const char* GetTitlebarIcon() const override { return LE_ICON_IMAGE; }
        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;
        void OnPropertyEditFinished(const FPropertyChangedEvent& Event) override;
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
        // Exposure stop tinting HDR previews; ImGui doesn't tone-map, so >1 clips to
        // white. Lets the user dim the preview to recover bright detail.
        float ExposureStops = 0.0f;
        // Array slice shown in the preview. Clamped against the live layer count each frame, so a
        // rebuild that shortens the array cannot leave this pointing past the end.
        uint32 PreviewSlice = 0;

        // The texture this tab currently holds a streaming pin on, or null. Held as the TEXTURE rather than a
        // bool so the unpin always targets whatever was pinned, even across a reimport that swaps the asset.
        TWeakObjectPtr<CTexture> PinnedTexture;

        // Set by the preview window's draw callback. ImGui only invokes that when the window is un-collapsed,
        // its dock tab is the selected one, and the owning tool is visible -- so it is a precise "the user is
        // looking at this texture right now", which is the only time the pin is justified. A pin exempts a
        // texture from budget eviction entirely, so ten 4K tabs left open in background docks would otherwise
        // hold their full chains resident for the rest of the session and push the pool past VRAM.
        bool bPreviewDrawnSinceUpdate = false;
    };
}
