#pragma once
#include "UI/Tools/EditorTool.h"
#include "Renderer/TextureStreamingManager.h"

namespace Lumina
{
    /**
     * Live view of the texture streamer: the pool against its budget, every registered texture's resident
     * vs full mip range, what is in flight, and the cumulative counters.
     *
     * Built to ANSWER "is streaming actually working", so it deliberately shows the things that are wrong
     * when it isn't: a resident total that never moves, textures stuck at their tail while covering half
     * the screen, promotions that never complete, and CPU bytes that stay held after a demotion.
     */
    class FTextureStreamingEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FTextureStreamingEditorTool)

        FTextureStreamingEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Texture Streaming", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_SWAP_VERTICAL; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

    private:

        void DrawWindow(bool bIsFocused);
        void DrawSummary(const FTextureStreamingManager::FStats& Stats);
        void DrawTextureTable();
        void DrawPendingTable();

        // Refreshed each frame from the manager; members rather than locals so sorting survives the frame.
        TVector<FTextureStreamingManager::FTextureSnapshot>  Snapshot;
        TVector<FTextureStreamingManager::FPendingSnapshot>  Pending;

        // Rolling history of resident bytes, in MiB, for the pool plot. A flat line while the camera moves
        // is the clearest single symptom of feedback not reaching the streamer.
        static constexpr int kHistory = 240;
        float   ResidentHistory[kHistory] = {};
        int     HistoryCursor = 0;

        char    Filter[64] = {};
        bool    bStreamedOnly = false;
        bool    bPinnedOnly = false;
    };
}
