#pragma once

#include "UI/Tools/EditorTool.h"

namespace Lumina
{
    // pImpl defined in the .cpp so the heavy Nsight Perf headers never leak into includers of this tool.
    struct FNsightPerfState;

    // Live GPU metrics dashboard driven by the Nsight Perf sampler, which needs an NVIDIA GPU and NvPerf's device extensions or the window explains why.
    class FNsightPerfTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FNsightPerfTool)

        FNsightPerfTool(IEditorToolContext* Context);
        ~FNsightPerfTool() override;

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_GAUGE; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;

    private:

        void DrawWindow(bool bIsFocused);

        // The numeric side of the plotted data, with each metric's current value and min, avg and max over the plot window.
        void DrawMetricTable();

        // Writes every live counter to Saved/Profiling/NsightPerf.csv, since the HUD has no other output.
        void ExportMetrics();

        // Session facts that make the numbers interpretable, such as chip, sample rate and plot history covered.
        void DrawSessionInfo();

        TUniquePtr<FNsightPerfState> State;
    };
}
