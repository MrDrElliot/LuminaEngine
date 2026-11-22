#pragma once

#include "UI/CurveEditor/CurveEditorWidget.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "imgui.h"

namespace Lumina
{
    // Editor for CCurveAsset where the canvas and the side panel edit the same SKeyedCurve live.
    class FCurveAssetEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FCurveAssetEditorTool)

        FCurveAssetEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        // Embeds a CPU-painted preview so the curve shows in the content browser without being loaded, since there is no viewport to grab.
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CHART_BELL_CURVE; }

        void OnInitialize() override;
        void OnPropertyEditFinished(const FPropertyChangedEvent& Event) override;
        void OnPostUndoRedo() override;
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        FCurveEditorWidget CurveWidget;
    };
}
