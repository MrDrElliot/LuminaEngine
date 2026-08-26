#pragma once
#include "UI/Tools/EditorTool.h"
#include "Memory/MemoryTracking.h"
#include "Platform/Process/PlatformProcess.h"
#include "Renderer/RHI.h"

namespace Lumina
{
    // Unified CPU + GPU memory tool. CPU = always-on category tracker (baseline, watch Delta,
    // then capture call-stacks). GPU = per-heap allocator stats via RHI::GetGPUMemoryStats, plus
    // the editor-only per-purpose breakdown built from RHI::GetGPUAllocations -- the GPU-side
    // answer to the CPU category table.
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
        void DrawGPUPurpose();
        void DrawGPUAllocations();

        // Pulls the RHI's live-allocation ledger and rolls it up per purpose. Costs the allocator
        // locks plus a copy of every live allocation, so it runs only while the GPU tab is open
        // (and once more when a report is copied).
        void RefreshGPUAllocations();

        // CPU sub-panels.
        void DrawCPUComposition();
        void DrawAddressSpace();
        void RunAddressSpaceScan();
        void DrawControls();
        void DrawCategoryTable(float Height);
        void DrawCallSites();

        // Walks the live-allocation ledger for the selected category; independent of stack capture.
        void CopyLedgerAnalysisToClipboard();

        // GPU snapshot (backend-agnostic). Refreshed on a timer; always available.
        RHI::FGPUMemoryStats    GPUStats;
        RHI::FGPUDeviceInfo     DeviceInfo;
        bool                    bDeviceInfoValid = false;

        // One purpose -- the "Scene" in "Scene.HDR" -- with its buffers and textures kept apart, because
        // "3 GB of textures" and "3 GB of structured buffers" are different problems with different fixes.
        struct FGPUPurposeRow
        {
            FString Name;
            uint64  TextureBytes = 0;
            uint64  BufferBytes  = 0;
            uint32  TextureCount = 0;
            uint32  BufferCount  = 0;

            uint64 Total() const { return TextureBytes + BufferBytes; }
            uint32 Count() const { return TextureCount + BufferCount; }
        };

        TVector<RHI::FGPUAllocationInfo> GPUAllocations;   // one row per live RHI allocation
        TVector<FGPUPurposeRow>      GPUPurposes;      // rolled up, sorted by total descending
        uint64                       GPUTextureBytes    = 0;
        uint64                       GPUBufferBytes     = 0;
        bool                         bGPUAllocationsValid = false;

        // Substring filter for the allocation list; matches name or purpose.
        char                         GPUFilter[64] = {};
        // Textures and buffers can be hidden separately -- the list is otherwise all textures.
        bool                         bShowTextures = true;
        bool                         bShowBuffers  = true;

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

        // Category row picked in the table; empty means rank call sites across every category.
        FString CallSiteCategory;

        // Whether -memcallstacks put the process in capture mode; closing the window must not undo that.
        bool  bCaptureAtStartup = false;

        // Drops the frames every stack ends in (CRT entry, thread start thunks) and the container /
        // allocator plumbing at the top, leaving the frames that name actual engine code.
        bool  bHideNoiseFrames = true;

        // Row text derives purely from Function, so it is built once per symbol, not per row per frame.
        static constexpr size_t kRowLabelMaxLen   = 96;
        static constexpr size_t kFrameLabelMaxLen = 110;

        // One resolved frame, split into what you read and where it lives.
        struct FResolvedFrame
        {
            FString Function;       // demangled name, or "Module.dll+0x..." when there is no PDB
            FString Location;       // "File.cpp:1234", empty when the frame has no line info
            FString RowLabel;       // Function shortened to kRowLabelMaxLen for the headline row
            FString FrameLabel;     // Function shortened to kFrameLabelMaxLen for the expanded stack
            bool    bPlumbing = false;   // container/allocator internals: HOW it allocated, not WHO asked
            bool    bNoise    = false;   // CRT/OS entry frames, identical on every stack
        };

        // Symbol resolution goes through DbgHelp under a process-wide lock, so resolving a panel's
        // worth of frames every UI frame is far too slow to do live. Addresses are stable for the
        // process, so each one is resolved exactly once.
        THashMap<void*, FResolvedFrame> SymbolCache;

        // One row per category, rebuilt every frame; held as a member so the buffer is reused.
        struct FCategoryRow
        {
            const Memory::FMemoryCategoryStats* S;
            int64                               DeltaBytes;
            int64                               DeltaCount;
        };

        TVector<FCategoryRow> CategoryRows;

        const FResolvedFrame& ResolveCached(void* Address);
#endif
    };
}
