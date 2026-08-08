#pragma once

#include "UI/Tools/EditorTool.h"

namespace Lumina
{
    // pImpl: owns the NvPerf periodic sampler + HUD model/renderer. Defined in the .cpp so the heavy
    // (and std::-heavy) Nsight Perf headers never leak into anything that includes this tool.
    struct FNsightPerfState;

    // "Nsight Perf HUD": a live GPU metrics dashboard driven by the NVIDIA Nsight Perf periodic
    // sampler (SM occupancy, throughputs, memory bandwidth, ...). Sampling starts when the window
    // opens and stops when it closes. Requires an NVIDIA GPU and the device created with NvPerf's
    // required extensions (registered by FNsightPerfEditorModule at the Core loading phase). When
    // those aren't available the window opens and explains why. Editor-only singleton.
    class FNsightPerfTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FNsightPerfTool)

        FNsightPerfTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Nsight Perf", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_GAUGE; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;

    private:

        void DrawWindow(bool bIsFocused);

        // The numeric side of the same data the HUD plots: every metric the active preset collects, with
        // its current value and its min/avg/max over the plot window. The plots answer "is it moving";
        // these answer "what is it", which is what you need to write a number down or compare two runs.
        void DrawMetricTable();

        // Session/config facts that make the numbers above interpretable (chip, sample rate, how much
        // history a plot actually covers, how many metrics are live).
        void DrawSessionInfo();

        FNsightPerfState* State = nullptr;
    };
}
