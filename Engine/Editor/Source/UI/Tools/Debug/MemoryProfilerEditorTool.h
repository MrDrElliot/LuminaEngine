#pragma once
#include "UI/Tools/EditorTool.h"
#include "Memory/MemoryTracking.h"
#include "Platform/Process/PlatformProcess.h"
#include "Renderer/RHI.h"

namespace Lumina
{
    // Unified CPU + GPU memory tool. CPU = always-on category tracker (baseline, watch Delta,
    // then capture call-stacks). GPU = per-heap allocator stats via RHI::GetGPUMemoryStats.
    class FMemoryProfilerEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FMemoryProfilerEditorTool)

        FMemoryProfilerEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Memory", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_MEMORY; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

    private:

        void DrawWindow(bool bIsFocused);
        void RefreshSnapshot();

        // Serializes the full snapshot (CPU + GPU + resources + call sites) to the clipboard
        // as a structured text report, formatted for pasting into an AI assistant.
        void CopyAllStatsToClipboard();

        void DrawHeaderCards();
        void DrawOverviewTab();
        void DrawGPUTab();
        void DrawCPUTab();

        // GPU sub-panels.
        void DrawGPUHeaps();

        // CPU sub-panels.
        void DrawCPUComposition();
        void DrawAddressSpace();
        void RunAddressSpaceScan();
        void DrawControls();
        void DrawCategoryTable(float Height);
        void DrawCallSites();

        // GPU snapshot (backend-agnostic). Refreshed on a timer; always available.
        RHI::FGPUMemoryStats    GPUStats;
        RHI::FGPUDeviceInfo     DeviceInfo;
        bool                    bDeviceInfoValid = false;

        // Rolling timelines in MB, advanced once per refresh tick.
        TVector<float>          HistRSS;
        TVector<float>          HistCPUTracked;
        TVector<float>          HistMapped;     // rpmalloc's OS footprint (mapped bytes)
        TVector<float>          HistExternal;   // RSS - mapped (driver / CRT)
        TVector<float>          HistVRAM;

        float                   RefreshTimer = 0.0f;

        // On-demand OS-level scan: the only view that sees memory no engine allocator touched.
        // Held between scans (never per-tick -- the heap walk locks every heap process-wide).
        Platform::FAddressSpaceStats AddressSpace;
        bool                    bAddressSpaceValid = false;
        bool                    bScanHeaps         = true;
        double                  LastScanTime       = 0.0;
        double                  LastScanCostMs     = 0.0;

#if LUMINA_MEMORY_TRACKING
        // CPU category snapshot, refreshed on the timer so the table reads steady.
        TVector<Memory::FMemoryCategoryStats> Categories;
        TVector<Memory::FMemoryCategoryStats> Baseline;
        bool  bHasBaseline = false;

        // Top Call Sites ranking: false = live bytes (leaks), true = total allocs (churn).
        bool  bSortCallSitesByAllocs = false;
#endif
    };
}
