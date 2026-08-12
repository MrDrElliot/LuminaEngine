#pragma once
#include "UI/Tools/EditorTool.h"
#include "Renderer/RHI.h"

namespace Lumina
{
    // Live view of the global bindless texture heap: every occupied sampled slot with its
    // description and a preview; click a preview to inspect it enlarged.
    class FTextureHeapEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FTextureHeapEditorTool)

        FTextureHeapEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Texture Heap", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_IMAGE_ALBUM; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

    private:

        void DrawWindow(bool bIsFocused);
        void DrawStreamingStats();
        void DrawTextureTable(const TVector<RHI::FHeapTextureInfo>& Textures);
        void DrawInspector(const TVector<RHI::FHeapTextureInfo>& Textures);

        char    Filter[64]   = {};
        uint32  SelectedSlot = RHI::kInvalidHeapSlot;
    };
}
