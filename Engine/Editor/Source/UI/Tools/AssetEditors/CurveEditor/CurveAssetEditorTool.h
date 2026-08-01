#pragma once

#include "UI/CurveEditor/CurveEditorWidget.h"
#include "UI/Tools/AssetEditors/AssetEditorTool.h"
#include "imgui.h"

namespace Lumina
{
    // Editor for CCurveAsset: the curve canvas in the dominant window, the reflected
    // properties in a side panel. Both edit the same SKeyedCurve live.
    class FCurveAssetEditorTool : public FAssetEditorTool
    {
    public:

        LUMINA_EDITOR_TOOL(FCurveAssetEditorTool)

        FCurveAssetEditorTool(IEditorToolContext* Context, CObject* InAsset);

        bool IsSingleWindowTool() const override { return false; }
        // Minted by the CPU curve painter registered in AssetTilePainters, not a viewport grab -- this tool
        // has no 3D viewport to grab. Embeds the preview in the .lasset so a curve shows its shape in the
        // content browser (and asset pickers) without having been loaded first.
        bool ShouldGenerateThumbnailOnSave() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CHART_BELL_CURVE; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override {}
        void InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const override;

    private:

        FCurveEditorWidget CurveWidget;
    };
}
