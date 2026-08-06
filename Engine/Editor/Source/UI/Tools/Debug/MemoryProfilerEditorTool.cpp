#include "MemoryProfilerEditorTool.h"

#include <cstring>
#include <cstdio>
#include <EASTL/sort.h>
#include "implot.h"
#include "Core/Engine/Engine.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "Platform/Process/PlatformProcess.h"
#include "Renderer/RHI.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"

namespace Lumina
{
    namespace
    {
        constexpr uint32 kHistorySamples = 240;     // ~60s at the 0.25s refresh tick.
        constexpr float  kMemoryRefreshSeconds = 0.25f;

        // Green under load, amber as it fills, red when nearly out.
        ImVec4 UsageColor(float Fraction)
        {
            if (Fraction > 0.85f) { return ImVec4(0.90f, 0.32f, 0.28f, 1.0f); }
            if (Fraction > 0.65f) { return ImVec4(0.92f, 0.70f, 0.25f, 1.0f); }
            return ImVec4(0.30f, 0.78f, 0.45f, 1.0f);
        }

        // "12.3 MB (12884901 bytes)" -- human-readable plus exact, so an AI gets both the
        // gestalt and a parseable number.
        FString SizeBoth(uint64 Bytes)
        {
            return FString().sprintf("%s (%llu bytes)", ImGuiX::FormatSize((size_t)Bytes).c_str(), (unsigned long long)Bytes);
        }

        void PushHistory(TVector<float>& History, float Value)
        {
            History.push_back(Value);
            if (History.size() > kHistorySamples)
            {
                History.erase(History.begin());
            }
        }

        // Compact line+fill plot for a rolling MB history.
        void DrawTimeline(const char* Id, const TVector<float>& History, const ImVec4& Color, float Height)
        {
            if (ImPlot::BeginPlot(Id, ImVec2(-1, Height), ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend | ImPlotFlags_NoTitle))
            {
                ImPlot::SetupAxes(nullptr, "MB",
                    ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines,
                    ImPlotAxisFlags_AutoFit);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, (double)kHistorySamples, ImGuiCond_Always);

                if (!History.empty())
                {
                    ImPlot::PushStyleColor(ImPlotCol_Line, Color);
                    ImPlot::PushStyleColor(ImPlotCol_Fill, ImVec4(Color.x, Color.y, Color.z, 0.25f));
                    ImPlot::PlotShaded(Id, History.data(), (int)History.size(), 0.0);
                    ImPlot::PlotLine(Id, History.data(), (int)History.size());
                    ImPlot::PopStyleColor(2);
                }
                ImPlot::EndPlot();
            }
        }
    }

    void FMemoryProfilerEditorTool::OnInitialize()
    {
        CreateToolWindow("Memory", [this](bool bIsFocused)
        {
            DrawWindow(bIsFocused);
        });
    }

    void FMemoryProfilerEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
#if LUMINA_MEMORY_TRACKING
        // Base category tracking is always on; just stop the heavy per-alloc call-stack
        // capture so it doesn't keep walking stacks after the window is closed.
        Memory::SetCaptureCallstacks(false);
#endif
    }

    void FMemoryProfilerEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Overview",
            "One picture of both memory pools: CPU (process heap, tracked by category) and GPU "
            "(device memory per heap). Backend-agnostic -- no API specifics.");
        DrawHelpTextRow("GPU breakdown",
            "Heap totals are device truth from the allocator (VMA budgets), including driver "
            "overhead and fragmentation.");
        DrawHelpTextRow("Finding a CPU leak",
            "On the CPU tab: click Set Baseline at a known-good moment, let the suspect run, then watch "
            "the Delta column. The category that keeps climbing is your leak. Tick Capture call stacks "
            "and read Top Call Sites for the exact line.");
        DrawHelpTextRow("Memory the tracker can't see",
            "The category tracker only sees Memory::Malloc, so anything a foreign DLL allocates from "
            "the CRT heap (Slang, the GPU driver, basisu) or the driver takes straight from "
            "VirtualAlloc is invisible to it. Hit Scan under Address Space to split that bucket into "
            "rpmalloc / NT-CRT heap / unattributed, then run BuildScripts/MemoryTrace.ps1 in the mode "
            "matching whichever is largest to get call stacks.");
        DrawHelpTextRow("Cost",
            "CPU category tracking is always on in Debug/Development and compiled out in Shipping. "
            "Call-stack capture is a heavier, separate toggle, switched off when this window closes. "
            "The address-space scan is on-demand: the heap walk locks every heap process-wide.");
    }

    void FMemoryProfilerEditorTool::RefreshSnapshot()
    {
#if LUMINA_MEMORY_TRACKING
        Categories.resize(256);
        const uint32 N = Memory::GetCategoryStats(Categories.data(), (uint32)Categories.size());
        Categories.resize(N);
#endif

        RHI::GetGPUMemoryStats(GPUStats);
        if (!bDeviceInfoValid)
        {
            DeviceInfo = RHI::GetDeviceInfo();
            bDeviceInfoValid = !DeviceInfo.Name.empty();
        }

        const float ToMB = 1.0f / (1024.0f * 1024.0f);
        const size_t Process = Platform::GetProcessMemoryUsageBytes();
        const size_t Mapped  = Memory::GetCurrentMappedMemory();
        const size_t External = (Process > Mapped) ? (Process - Mapped) : 0;
        PushHistory(HistRSS, (float)Process * ToMB);
        PushHistory(HistMapped, (float)Mapped * ToMB);
        PushHistory(HistExternal, (float)External * ToMB);
        PushHistory(HistVRAM, (float)GPUStats.TotalUsage * ToMB);
#if LUMINA_MEMORY_TRACKING
        PushHistory(HistCPUTracked, (float)Memory::GetTrackedLiveBytes() * ToMB);
#endif
    }

    void FMemoryProfilerEditorTool::DrawWindow(bool bIsFocused)
    {
        RefreshTimer += (float)GEngine->GetDeltaTime();
        if (RefreshTimer >= kMemoryRefreshSeconds || HistVRAM.empty())
        {
            RefreshTimer = 0.0f;
            RefreshSnapshot();
        }

        DrawHeaderCards();
        ImGui::Spacing();

        if (ImGui::Button(LE_ICON_CONTENT_COPY " Copy All Stats"))
        {
            CopyAllStatsToClipboard();
        }
        ImGuiX::TextTooltip("Copies a full structured memory report (CPU + GPU heaps + memory-by-purpose + "
                            "live resources + CPU categories + call sites) to the clipboard");
        ImGui::Spacing();

        if (ImGui::BeginTabBar("##MemoryTabs"))
        {
            if (ImGui::BeginTabItem(LE_ICON_GAUGE " Overview"))
            {
                DrawOverviewTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(LE_ICON_EXPANSION_CARD " GPU"))
            {
                DrawGPUTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(LE_ICON_CPU_64_BIT " CPU"))
            {
                DrawCPUTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    void FMemoryProfilerEditorTool::DrawHeaderCards()
    {
        const size_t Process = Platform::GetProcessMemoryUsageBytes();
#if LUMINA_MEMORY_TRACKING
        const size_t Tracked = Memory::GetTrackedLiveBytes();
#else
        const size_t Tracked = 0;
#endif
        const size_t Untracked = (Process > Tracked) ? (Process - Tracked) : 0;

        const size_t Mapped    = Memory::GetCurrentMappedMemory();
        const size_t Retained  = (Mapped > Tracked) ? (Mapped - Tracked) : 0;
        const size_t External  = (Process > Mapped) ? (Process - Mapped) : 0;
        (void)Untracked;

        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        const float CardW = (ImGui::GetContentRegionAvail().x - Spacing) * 0.5f;
        const float CardH = 104.0f;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.14f, 0.16f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

        // CPU card
        ImGui::BeginChild("##CPUCard", ImVec2(CardW, CardH), true);
        {
            ImGui::TextColored(ImVec4(0.66f, 0.78f, 0.95f, 1.0f), LE_ICON_CPU_64_BIT " CPU MEMORY");
            ImGui::Separator();
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::TextUnformatted(ImGuiX::FormatSize(Process).c_str());
            ImGuiX::Font::PopFont();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("process (RSS)");

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s tracked", ImGuiX::FormatSize(Tracked).c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "  %s retained", ImGuiX::FormatSize(Retained).c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.55f, 0.55f, 1.0f), "  %s external", ImGuiX::FormatSize(External).c_str());
            ImGuiX::TextTooltip("tracked = ledger live bytes\n"
                                "retained = rpmalloc mapped - tracked (caches + fragmentation; freed, not returned to OS)\n"
                                "external = RSS - rpmalloc mapped (GPU driver host memory, CRT malloc, code/stacks)");
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // GPU card
        ImGui::BeginChild("##GPUCard", ImVec2(CardW, CardH), true);
        {
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.55f, 1.0f), LE_ICON_EXPANSION_CARD " GPU MEMORY");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", bDeviceInfoValid ? DeviceInfo.Name.c_str() : "");

            ImGui::Separator();

            const float Frac = (GPUStats.TotalBudget > 0)
                ? (float)((double)GPUStats.TotalUsage / (double)GPUStats.TotalBudget) : 0.0f;

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::TextUnformatted(ImGuiX::FormatSize(GPUStats.TotalUsage).c_str());
            ImGuiX::Font::PopFont();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("/ %s  (%.0f%%)", ImGuiX::FormatSize(GPUStats.TotalBudget).c_str(), Frac * 100.0f);

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, UsageColor(Frac));
            ImGui::ProgressBar(Frac, ImVec2(-1, 14), "");
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    // Overview

    void FMemoryProfilerEditorTool::DrawOverviewTab()
    {
        ImGui::Spacing();

        // Side-by-side rolling timelines.
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        const float HalfW = (ImGui::GetContentRegionAvail().x - Spacing) * 0.5f;

        ImGui::BeginChild("##VRAMTL", ImVec2(HalfW, 200), false);
        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "VRAM usage");
        DrawTimeline("##VRAMPlot", HistVRAM, ImVec4(0.95f, 0.70f, 0.40f, 1.0f), 160.0f);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##RSSTL", ImVec2(0, 200), false);
        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "Process RSS");
        DrawTimeline("##RSSPlot", HistRSS, ImVec4(0.40f, 0.80f, 0.55f, 1.0f), 160.0f);
        ImGui::EndChild();
    }

    // GPU

    void FMemoryProfilerEditorTool::DrawGPUHeaps()
    {
        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "Device heaps");
        ImGui::Spacing();

        const ImGuiTableFlags Flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
        if (ImGui::BeginTable("##Heaps", 5, Flags))
        {
            ImGui::TableSetupColumn("Heap",   ImGuiTableColumnFlags_WidthFixed, 150);
            ImGui::TableSetupColumn("Usage",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Used",   ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Budget", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Allocs", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableHeadersRow();

            for (const RHI::FGPUMemoryHeapStats& Heap : GPUStats.Heaps)
            {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s %u", Heap.bDeviceLocal ? LE_ICON_EXPANSION_CARD : LE_ICON_MEMORY, Heap.HeapIndex);
                ImGui::SameLine();
                const char* Kind = Heap.bReBAR ? "Device (ReBAR)"
                                 : Heap.bDeviceLocal ? (Heap.bHostVisible ? "Device (BAR)" : "Device")
                                 : "Host";
                if (Heap.bReBAR)
                {
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "%s", Kind);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("CPU-writable VRAM.");
                    }
                }
                else
                {
                    ImGui::TextDisabled("%s", Kind);
                }

                ImGui::TableSetColumnIndex(1);
                const float Frac = (Heap.BudgetBytes > 0)
                    ? (float)((double)Heap.UsageBytes / (double)Heap.BudgetBytes) : 0.0f;
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, UsageColor(Frac));
                ImGui::ProgressBar(Frac, ImVec2(-1, 0), FString().sprintf("%.1f%%", Frac * 100.0f).c_str());
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(ImGuiX::FormatSize((size_t)Heap.UsageBytes).c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(ImGuiX::FormatSize((size_t)Heap.BudgetBytes).c_str());

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%u", Heap.AllocationCount);
            }

            ImGui::EndTable();
        }

        ImGui::TextDisabled("Allocator: %s in %u allocations across %u blocks.",
            ImGuiX::FormatSize((size_t)GPUStats.TotalAllocated).c_str(),
            GPUStats.TotalAllocations, GPUStats.TotalBlocks);
    }

    void FMemoryProfilerEditorTool::DrawGPUTab()
    {
        ImGui::Spacing();
        if (bDeviceInfoValid)
        {
            ImGui::TextDisabled(LE_ICON_CHIP " %s   %s   %s",
                DeviceInfo.Name.c_str(), DeviceInfo.APIName.c_str(),
                DeviceInfo.bDiscrete ? "Discrete" : "Integrated");
        }
        ImGui::Spacing();

        DrawGPUHeaps();
    }

    // CPU

    void FMemoryProfilerEditorTool::DrawCPUComposition()
    {
        const size_t Process = Platform::GetProcessMemoryUsageBytes();
#if LUMINA_MEMORY_TRACKING
        const size_t Tracked = Memory::GetTrackedLiveBytes();
#else
        const size_t Tracked = 0;
#endif
        const size_t Mapped   = Memory::GetCurrentMappedMemory();
        const size_t Cached   = Memory::GetCachedMemory();
        const size_t Retained = (Mapped > Tracked) ? (Mapped - Tracked) : 0;
        const size_t External = (Process > Mapped) ? (Process - Mapped) : 0;

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "Composition  " LE_ICON_INFORMATION);
        ImGuiX::TextTooltip("Where Process RSS lives. If 'retained' climbs, it's allocation churn -- rpmalloc "
                            "holding freed/fragmented spans (rank Top Call Sites by Total Allocs to find it). "
                            "If 'external' climbs, it's outside rpmalloc: GPU driver host memory or the CRT allocator.");
        ImGui::Spacing();

        auto Row = [](const char* Label, size_t Bytes, const ImVec4& Color, const char* Note)
        {
            ImGui::TextColored(Color, "%14s", ImGuiX::FormatSize(Bytes).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Label);
            if (Note && Note[0])
            {
                ImGui::SameLine();
                ImGui::TextDisabled("- %s", Note);
            }
        };

        Row("Process RSS", Process, ImVec4(0.85f, 0.92f, 1.0f, 1.0f), "total resident");
        Row("rpmalloc mapped", Mapped, ImVec4(0.66f, 0.78f, 0.95f, 1.0f), "allocator's OS footprint");
        Row("tracked live", Tracked, ImVec4(0.40f, 1.0f, 0.60f, 1.0f), "category ledger");
        Row("retained", Retained, ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "caches + fragmentation (freed, not returned)");
        Row("of which cached", Cached, ImVec4(0.70f, 0.62f, 0.42f, 1.0f), "rpmalloc global span cache");
        Row("external", External, ImVec4(0.90f, 0.55f, 0.55f, 1.0f), "driver / CRT / code+stacks");

        if (Mapped == 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                LE_ICON_ALERT " rpmalloc statistics are zero -- build with ENABLE_STATISTICS (Debug/Development).");
        }

        ImGui::Spacing();
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        const float HalfW = (ImGui::GetContentRegionAvail().x - Spacing) * 0.5f;

        ImGui::BeginChild("##MappedTL", ImVec2(HalfW, 150), false);
        ImGui::TextColored(ImVec4(0.66f, 0.78f, 0.95f, 1.0f), "rpmalloc mapped (retained churn)");
        DrawTimeline("##MappedPlot", HistMapped, ImVec4(0.66f, 0.78f, 0.95f, 1.0f), 110.0f);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##ExternalTL", ImVec2(0, 150), false);
        ImGui::TextColored(ImVec4(0.90f, 0.55f, 0.55f, 1.0f), "external (driver / CRT)");
        DrawTimeline("##ExternalPlot", HistExternal, ImVec4(0.90f, 0.55f, 0.55f, 1.0f), 110.0f);
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
    }

    void FMemoryProfilerEditorTool::RunAddressSpaceScan()
    {
        const double Start = Platform::GetTime();
        Platform::GetAddressSpaceStats(AddressSpace, bScanHeaps);
        LastScanCostMs = (Platform::GetTime() - Start) * 1000.0;
        LastScanTime = Platform::GetTime();
        bAddressSpaceValid = true;
    }

    // Splits the "external" number above into buckets the OS can actually name. This is the only
    // panel that sees allocations no engine allocator made -- the GPU driver's raw VirtualAlloc and
    // the CRT heap that every foreign DLL (Slang, the driver, basisu) allocates from.
    void FMemoryProfilerEditorTool::DrawAddressSpace()
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "Address Space  " LE_ICON_INFORMATION);
        ImGuiX::TextTooltip("Asks the OS what's actually mapped, so it covers memory the category "
                            "tracker structurally cannot see. 'NT / CRT heap' is the bucket for every "
                            "DLL that doesn't route through Memory::Malloc.");
        ImGui::Spacing();

        if (ImGui::Button(LE_ICON_MAGNIFY " Scan"))
        {
            RunAddressSpaceScan();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Include heap walk", &bScanHeaps);
        ImGuiX::TextTooltip("Walks every NT heap to size the CRT bucket. Takes a process-wide heap "
                            "lock and is O(live blocks), so a multi-GB heap can stall the process "
                            "for seconds. Untick for a fast scan of the page tables only.");

        if (!bAddressSpaceValid)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Not scanned yet.");
            ImGui::Spacing();
            ImGui::Separator();
            return;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("| %u regions, %.1f ms, %.0fs ago",
            AddressSpace.RegionCount, LastScanCostMs, Platform::GetTime() - LastScanTime);

        auto Row = [](const char* Label, uint64 Bytes, const ImVec4& Color, const char* Note)
        {
            ImGui::TextColored(Color, "%14s", ImGuiX::FormatSize((size_t)Bytes).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Label);
            if (Note && Note[0])
            {
                ImGui::SameLine();
                ImGui::TextDisabled("- %s", Note);
            }
        };

        ImGui::Spacing();
        ImGui::TextDisabled("Committed");
        Row("private", AddressSpace.PrivateCommitted, ImVec4(0.85f, 0.92f, 1.0f, 1.0f), "this process only");
        Row("image", AddressSpace.ImageCommitted, ImVec4(0.62f, 0.68f, 0.78f, 1.0f), "code + data of loaded modules");
        Row("mapped", AddressSpace.MappedCommitted, ImVec4(0.62f, 0.68f, 0.78f, 1.0f), "file mappings and shared sections");
        Row("reserved", AddressSpace.Reserved, ImVec4(0.55f, 0.58f, 0.62f, 1.0f), "address space only, costs no RAM");

        // Every byte of private commit belongs to exactly one of these three. Whichever is largest
        // tells you which trace to run: rpmalloc -> the category table below; heap -> a heap ETW
        // trace; unattributed -> a VirtualAlloc ETW trace (see BuildScripts/MemoryTrace.ps1).
        const uint64 Rpmalloc = Memory::GetCurrentMappedMemory();
        const uint64 Heap     = AddressSpace.bHeapWalkValid ? AddressSpace.HeapCommitted : 0;
        const uint64 Accounted = Rpmalloc + Heap;
        const uint64 Unattributed = AddressSpace.PrivateCommitted > Accounted
                                  ? AddressSpace.PrivateCommitted - Accounted : 0;

        ImGui::Spacing();
        ImGui::TextDisabled("Private commit, by owner");
        Row("rpmalloc", Rpmalloc, ImVec4(0.66f, 0.78f, 0.95f, 1.0f), "engine allocator (the category table below)");

        if (AddressSpace.bHeapWalkValid)
        {
            Row("NT / CRT heap", Heap, ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "foreign DLLs: Slang, GPU driver, basisu, ucrtbase");
            ImGui::TextDisabled("%14s of that live, %s block overhead",
                ImGuiX::FormatSize((size_t)AddressSpace.HeapAllocated).c_str(),
                ImGuiX::FormatSize((size_t)AddressSpace.HeapOverhead).c_str());
        }
        else if (bScanHeaps)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                LE_ICON_ALERT " Heap walk failed (a heap refused to lock); CRT bytes are folded into unattributed.");
        }
        else
        {
            ImGui::TextDisabled("%14s NT / CRT heap - not measured (heap walk off)", "?");
        }

        Row("unattributed", Unattributed, ImVec4(0.90f, 0.55f, 0.55f, 1.0f), "raw VirtualAlloc: GPU driver, thread + fiber stacks");

        ImGui::Spacing();
        ImGui::Separator();
    }

    void FMemoryProfilerEditorTool::DrawCPUTab()
    {
        DrawCPUComposition();
        DrawAddressSpace();

#if !LUMINA_MEMORY_TRACKING
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
            LE_ICON_ALERT " CPU category tracking is compiled out in Shipping builds.");
#else
        DrawControls();
        ImGui::Separator();
        ImGui::Spacing();

        const bool bCapturing = Memory::IsCapturingCallstacks();
        const float Avail = ImGui::GetContentRegionAvail().y;
        const float TableHeight = bCapturing ? Avail * 0.55f : Avail;
        DrawCategoryTable(TableHeight);

        if (bCapturing)
        {
            ImGui::Spacing();
            DrawCallSites();
        }
#endif
    }

#if LUMINA_MEMORY_TRACKING

    void FMemoryProfilerEditorTool::DrawControls()
    {
        if (ImGui::Button(LE_ICON_FLAG " Set Baseline"))
        {
            Baseline = Categories;
            bHasBaseline = true;
        }
        ImGuiX::TextTooltip("Snapshot current live bytes per category. The Delta column then shows "
                            "growth since this moment -- the anchor for leak hunting.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_FLAG_OUTLINE " Clear Baseline"))
        {
            bHasBaseline = false;
        }

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_REFRESH " Reset"))
        {
            Memory::ResetTracking();
            Baseline.clear();
            bHasBaseline = false;
        }
        ImGuiX::TextTooltip("Zero all counters and start a fresh capture.");

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(16, 0));
        ImGui::SameLine();

        bool bCapture = Memory::IsCapturingCallstacks();
        if (ImGui::Checkbox("Capture call stacks", &bCapture))
        {
            Memory::SetCaptureCallstacks(bCapture);
        }
        ImGuiX::TextTooltip("Record the stack of every allocation so Top Call Sites can name the "
                            "exact leaking line. Heavier -- turn on once a category is confirmed leaking.");
    }

    void FMemoryProfilerEditorTool::DrawCategoryTable(float Height)
    {
        auto FindBaseline = [this](const char* Name) -> const Memory::FMemoryCategoryStats*
        {
            if (bHasBaseline)
            {
                for (const Memory::FMemoryCategoryStats& B : Baseline)
                {
                    if (std::strcmp(B.Name, Name) == 0)
                    {
                        return &B;
                    }
                }
            }
            return nullptr;
        };

        struct FRow
        {
            const Memory::FMemoryCategoryStats* S;
            int64                               DeltaBytes;
            int64                               DeltaCount;
        };

        TVector<FRow> Rows;
        Rows.reserve(Categories.size());
        for (const Memory::FMemoryCategoryStats& S : Categories)
        {
            const Memory::FMemoryCategoryStats* B = FindBaseline(S.Name);
            FRow Row;
            Row.S          = &S;
            Row.DeltaBytes = B ? (int64)S.LiveBytes - (int64)B->LiveBytes : 0;
            Row.DeltaCount = B ? (int64)S.LiveCount - (int64)B->LiveCount : 0;
            Rows.push_back(Row);
        }

        eastl::sort(Rows.begin(), Rows.end(), [this](const FRow& A, const FRow& B)
        {
            if (bHasBaseline) { return A.DeltaBytes > B.DeltaBytes; }
            return A.S->LiveBytes > B.S->LiveBytes;
        });

        if (bHasBaseline && !Rows.empty() && Rows[0].DeltaBytes > 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                LE_ICON_ALERT " Growing fastest: %s  (+%s)",
                Rows[0].S->Name, ImGuiX::FormatSize((size_t)Rows[0].DeltaBytes).c_str());
        }
        else if (!bHasBaseline)
        {
            ImGui::TextDisabled(LE_ICON_INFORMATION " Set a baseline, then watch the Delta column.");
        }
        else
        {
            ImGui::TextDisabled(LE_ICON_INFORMATION " No category has grown since the baseline.");
        }
        ImGui::Spacing();

        const ImGuiTableFlags Flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                    | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("Categories", 6, Flags, ImVec2(0, Height)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Category",   ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Live",       ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("Allocs",     ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Delta",      ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Peak",       ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("Alloc/Free", ImGuiTableColumnFlags_WidthFixed, 150);
            ImGui::TableHeadersRow();

            for (const FRow& Row : Rows)
            {
                const Memory::FMemoryCategoryStats& S = *Row.S;
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(S.Name[0] ? S.Name : "Default");

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(ImGuiX::FormatSize(S.LiveBytes).c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", (unsigned long long)S.LiveCount);

                ImGui::TableSetColumnIndex(3);
                if (!bHasBaseline)
                {
                    ImGui::TextDisabled("-");
                }
                else if (Row.DeltaBytes > 0)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "+%s",
                        ImGuiX::FormatSize((size_t)Row.DeltaBytes).c_str());
                }
                else if (Row.DeltaBytes < 0)
                {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f), "-%s",
                        ImGuiX::FormatSize((size_t)(-Row.DeltaBytes)).c_str());
                }
                else
                {
                    ImGui::TextDisabled("0");
                }

                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(ImGuiX::FormatSize(S.PeakBytes).c_str());

                ImGui::TableSetColumnIndex(5);
                ImGui::TextDisabled("%llu / %llu",
                    (unsigned long long)S.TotalAllocs, (unsigned long long)S.TotalFrees);
            }

            ImGui::EndTable();
        }
    }

    void FMemoryProfilerEditorTool::DrawCallSites()
    {
        if (!ImGui::CollapsingHeader(LE_ICON_LIST_BOX " Top Call Sites", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        // Live bytes finds leaks/persistent; total allocs finds transient churn -- different sites.
        ImGui::TextUnformatted("Rank by:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Live bytes", !bSortCallSitesByAllocs)) { bSortCallSitesByAllocs = false; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Total allocs (churn)", bSortCallSitesByAllocs)) { bSortCallSitesByAllocs = true; }

        const Memory::ECallSiteSort Sort = bSortCallSitesByAllocs
            ? Memory::ECallSiteSort::TotalAllocs : Memory::ECallSiteSort::LiveBytes;

        static constexpr uint32 kMaxSites = 64;
        Memory::FCallSiteStat Sites[kMaxSites];
        const uint32 NumSites = Memory::GetTopCallSites(Sites, kMaxSites, Sort);

        if (NumSites == 0)
        {
            ImGui::TextDisabled("No call sites captured yet -- give it a few frames.");
            return;
        }

        char SymBuf[512];

        ImGui::SameLine();
        if (ImGui::SmallButton(LE_ICON_CONTENT_COPY " Copy all"))
        {
            FString Report;
            char Line[640];
            std::snprintf(Line, sizeof(Line),
                "=== Memory: top %u call sites by %s (tracked live %s, %llu allocs) ===\n",
                NumSites, bSortCallSitesByAllocs ? "total allocs" : "live bytes",
                ImGuiX::FormatSize(Memory::GetTrackedLiveBytes()).c_str(),
                (unsigned long long)Memory::GetTrackedLiveCount());
            Report += Line;

            for (uint32 i = 0; i < NumSites; ++i)
            {
                const Memory::FCallSiteStat& S = Sites[i];
                std::snprintf(Line, sizeof(Line),
                    "\n[%u] live %s (%llu live) | %llu total allocs | [%s]\n",
                    i + 1,
                    ImGuiX::FormatSize(S.LiveBytes).c_str(),
                    (unsigned long long)S.LiveCount,
                    (unsigned long long)S.TotalAllocs,
                    S.CatName[0] ? S.CatName : "Default");
                Report += Line;

                for (uint32 f = 0; f < S.FrameCount; ++f)
                {
                    Memory::ResolveSymbol(S.Frames[f], SymBuf, sizeof(SymBuf));
                    Report += "    ";
                    Report += SymBuf;
                    Report += "\n";
                }
            }
            ImGui::SetClipboardText(Report.c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%u sites)", NumSites);

        for (uint32 i = 0; i < NumSites; ++i)
        {
            const Memory::FCallSiteStat& Site = Sites[i];

            SymBuf[0] = '\0';
            if (Site.FrameCount > 0)
            {
                Memory::ResolveSymbol(Site.Frames[0], SymBuf, sizeof(SymBuf));
            }

            ImGui::PushID((int)i);
            const bool bOpen = ImGui::TreeNode("site", "%s live (%llu allocs) - %s [%s]",
                ImGuiX::FormatSize(Site.LiveBytes).c_str(),
                (unsigned long long)Site.TotalAllocs,
                SymBuf[0] ? SymBuf : "(no frames)",
                Site.CatName[0] ? Site.CatName : "Default");

            if (bOpen)
            {
                if (ImGui::SmallButton(LE_ICON_CONTENT_COPY " Copy stack"))
                {
                    char Clip[8192];
                    int Off = std::snprintf(Clip, sizeof(Clip), "live %s (%llu live, %llu total allocs) [%s]\n",
                        ImGuiX::FormatSize(Site.LiveBytes).c_str(),
                        (unsigned long long)Site.LiveCount,
                        (unsigned long long)Site.TotalAllocs,
                        Site.CatName[0] ? Site.CatName : "Default");

                    for (uint32 f = 0; f < Site.FrameCount && Off > 0 && Off < (int)sizeof(Clip); ++f)
                    {
                        Memory::ResolveSymbol(Site.Frames[f], SymBuf, sizeof(SymBuf));
                        const int N = std::snprintf(Clip + Off, sizeof(Clip) - Off, "  %s\n", SymBuf);
                        if (N < 0) { break; }
                        Off += N;
                    }
                    ImGui::SetClipboardText(Clip);
                }

                for (uint32 f = 0; f < Site.FrameCount; ++f)
                {
                    Memory::ResolveSymbol(Site.Frames[f], SymBuf, sizeof(SymBuf));
                    ImGui::TextUnformatted(SymBuf);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

#else // !LUMINA_MEMORY_TRACKING

    void FMemoryProfilerEditorTool::DrawControls() {}
    void FMemoryProfilerEditorTool::DrawCategoryTable(float) {}
    void FMemoryProfilerEditorTool::DrawCallSites() {}

#endif

    void FMemoryProfilerEditorTool::CopyAllStatsToClipboard()
    {
        FString R;
        R.reserve(16 * 1024);

        const size_t Process = Platform::GetProcessMemoryUsageBytes();
#if LUMINA_MEMORY_TRACKING
        const size_t Tracked = Memory::GetTrackedLiveBytes();
#else
        const size_t Tracked = 0;
#endif
        const size_t Untracked = (Process > Tracked) ? (Process - Tracked) : 0;
        const size_t Mapped    = Memory::GetCurrentMappedMemory();
        const size_t Cached    = Memory::GetCachedMemory();
        const size_t Retained  = (Mapped > Tracked) ? (Mapped - Tracked) : 0;
        const size_t External  = (Process > Mapped) ? (Process - Mapped) : 0;

        R += "# Lumina Engine - Memory Report\n\n";
        R += "Captured from the in-editor Memory tool. Sizes show human-readable form with exact\n";
        R += "byte counts in parentheses. Counts are live (currently outstanding) unless noted.\n";
        R += "Use this to find which purpose/category/call-site is driving memory growth.\n\n";

        // System
        R += "## System\n";
        if (bDeviceInfoValid)
        {
            R += FString().sprintf("- GPU: %s (%s)\n", DeviceInfo.Name.c_str(), DeviceInfo.bDiscrete ? "Discrete" : "Integrated");
            R += FString().sprintf("- API: %s\n", DeviceInfo.APIName.c_str());
        }
        else
        {
            R += "- GPU: (device info unavailable)\n";
        }
        R += "\n";

        // CPU
        R += "## CPU memory\n";
        R += FString().sprintf("- Process RSS:     %s\n", SizeBoth(Process).c_str());
        R += FString().sprintf("- rpmalloc mapped: %s (allocator's OS footprint)\n", SizeBoth(Mapped).c_str());
        R += FString().sprintf("- Tracked:         %s (category ledger, live)\n", SizeBoth(Tracked).c_str());
        R += FString().sprintf("- Retained:        %s (mapped - tracked: rpmalloc caches + fragmentation, freed but not returned to OS)\n", SizeBoth(Retained).c_str());
        R += FString().sprintf("- ...cached:       %s (rpmalloc global span cache)\n", SizeBoth(Cached).c_str());
        R += FString().sprintf("- External:        %s (RSS - mapped: GPU driver host memory, CRT malloc, code + stacks)\n", SizeBoth(External).c_str());
        R += FString().sprintf("- Untracked:       %s (RSS - tracked; = retained + external)\n\n", SizeBoth(Untracked).c_str());

        // Address space -- the OS-level view, which is the only one that covers allocators the
        // engine never sees. Omitted rather than faked when the user hasn't run a scan.
        R += "## Address space (OS scan)\n";
        if (!bAddressSpaceValid)
        {
            R += "- Not scanned. Run 'Scan' on the CPU tab to attribute memory outside rpmalloc.\n\n";
        }
        else
        {
            const uint64 Heap = AddressSpace.bHeapWalkValid ? AddressSpace.HeapCommitted : 0;
            const uint64 Accounted = Mapped + Heap;
            const uint64 Unattributed = AddressSpace.PrivateCommitted > Accounted
                                      ? AddressSpace.PrivateCommitted - Accounted : 0;

            R += FString().sprintf("- Private commit:  %s (this process only)\n", SizeBoth(AddressSpace.PrivateCommitted).c_str());
            R += FString().sprintf("- Image commit:    %s (code + data of loaded modules)\n", SizeBoth(AddressSpace.ImageCommitted).c_str());
            R += FString().sprintf("- Mapped commit:   %s (file mappings, shared sections)\n", SizeBoth(AddressSpace.MappedCommitted).c_str());
            R += FString().sprintf("- Reserved:        %s (address space only, no RAM)\n", SizeBoth(AddressSpace.Reserved).c_str());
            R += "\n  Private commit by owner:\n";
            R += FString().sprintf("  - rpmalloc:      %s (engine allocator; see category table)\n", SizeBoth(Mapped).c_str());
            if (AddressSpace.bHeapWalkValid)
            {
                R += FString().sprintf("  - NT/CRT heap:   %s committed, %s live in %u heaps (foreign DLLs: Slang, GPU driver, basisu, ucrtbase)\n",
                    SizeBoth(AddressSpace.HeapCommitted).c_str(), SizeBoth(AddressSpace.HeapAllocated).c_str(), AddressSpace.HeapCount);
            }
            else
            {
                R += "  - NT/CRT heap:   not measured (heap walk off or failed; folded into unattributed)\n";
            }
            R += FString().sprintf("  - Unattributed:  %s (raw VirtualAlloc: GPU driver, thread + fiber stacks)\n\n", SizeBoth(Unattributed).c_str());
        }

        // GPU summary
        const float VRAMFrac = (GPUStats.TotalBudget > 0)
            ? (float)((double)GPUStats.TotalUsage / (double)GPUStats.TotalBudget) : 0.0f;
        R += "## GPU summary\n";
        R += FString().sprintf("- VRAM usage: %s of %s (%.1f%%)\n",
            SizeBoth(GPUStats.TotalUsage).c_str(), SizeBoth(GPUStats.TotalBudget).c_str(), VRAMFrac * 100.0f);
        R += FString().sprintf("- Allocator allocated: %s\n", SizeBoth(GPUStats.TotalAllocated).c_str());
        R += FString().sprintf("- Allocator blocks:    %s in %u blocks\n",
            SizeBoth(GPUStats.TotalBlockBytes).c_str(), GPUStats.TotalBlocks);
        R += FString().sprintf("- Live allocations:    %u\n\n", GPUStats.TotalAllocations);

        // GPU heaps
        R += "## GPU heaps\n";
        R += "| Heap | Type | Used | Budget | Used% | Allocated | Blocks | Allocs |\n";
        R += "|------|------|------|--------|-------|-----------|--------|--------|\n";
        for (const RHI::FGPUMemoryHeapStats& H : GPUStats.Heaps)
        {
            const float Frac = (H.BudgetBytes > 0) ? (float)((double)H.UsageBytes / (double)H.BudgetBytes) : 0.0f;
            R += FString().sprintf("| %u | %s | %s | %s | %.1f%% | %s | %u | %u |\n",
                H.HeapIndex, H.bReBAR ? "Device (ReBAR)" : H.bDeviceLocal ? (H.bHostVisible ? "Device (BAR)" : "Device") : "Host",
                SizeBoth(H.UsageBytes).c_str(), SizeBoth(H.BudgetBytes).c_str(), Frac * 100.0f,
                SizeBoth(H.AllocatedBytes).c_str(), H.BlockCount, H.AllocationCount);
        }
        R += "\n";

#if LUMINA_MEMORY_TRACKING
        // CPU categories
        {
            struct FRow { const Memory::FMemoryCategoryStats* S; int64 DeltaBytes; int64 DeltaCount; };

            auto FindBaseline = [this](const char* Name) -> const Memory::FMemoryCategoryStats*
            {
                if (bHasBaseline)
                {
                    for (const Memory::FMemoryCategoryStats& B : Baseline)
                    {
                        if (std::strcmp(B.Name, Name) == 0) { return &B; }
                    }
                }
                return nullptr;
            };

            TVector<FRow> Rows;
            Rows.reserve(Categories.size());
            for (const Memory::FMemoryCategoryStats& S : Categories)
            {
                const Memory::FMemoryCategoryStats* B = FindBaseline(S.Name);
                Rows.push_back({ &S,
                    B ? (int64)S.LiveBytes - (int64)B->LiveBytes : 0,
                    B ? (int64)S.LiveCount - (int64)B->LiveCount : 0 });
            }
            eastl::sort(Rows.begin(), Rows.end(), [](const FRow& A, const FRow& B) { return A.S->LiveBytes > B.S->LiveBytes; });

            R += "## CPU memory by category (sorted by live bytes)\n";
            R += FString().sprintf("Baseline set: %s\n\n", bHasBaseline ? "yes (Delta = growth since baseline)" : "no");
            R += "| Category | Live | Count | Peak | Total allocs | Total frees | Delta bytes | Delta count |\n";
            R += "|----------|------|-------|------|--------------|-------------|-------------|-------------|\n";
            for (const FRow& Row : Rows)
            {
                const Memory::FMemoryCategoryStats& S = *Row.S;
                R += FString().sprintf("| %s | %s | %llu | %s | %llu | %llu | %s%lld | %lld |\n",
                    S.Name[0] ? S.Name : "Default",
                    SizeBoth(S.LiveBytes).c_str(), (unsigned long long)S.LiveCount,
                    SizeBoth(S.PeakBytes).c_str(),
                    (unsigned long long)S.TotalAllocs, (unsigned long long)S.TotalFrees,
                    Row.DeltaBytes > 0 ? "+" : "", (long long)Row.DeltaBytes, (long long)Row.DeltaCount);
            }
            R += "\n";
        }

        // Top call sites (only when capturing)
        R += "## Top call sites\n";
        if (!Memory::IsCapturingCallstacks())
        {
            R += "(Call-stack attribution is OFF. Enable 'Capture call stacks' on the CPU tab, reproduce\n";
            R += "the growth, then copy again to get per-line allocation sources.)\n\n";
        }
        else
        {
            char SymBuf[512];
            const Memory::ECallSiteSort Sorts[2] = { Memory::ECallSiteSort::LiveBytes, Memory::ECallSiteSort::TotalAllocs };
            const char* SortNames[2] = { "live bytes (leaks / persistent)", "total allocs (churn)" };

            for (int s = 0; s < 2; ++s)
            {
                static constexpr uint32 kMaxSites = 24;
                Memory::FCallSiteStat Sites[kMaxSites];
                const uint32 NumSites = Memory::GetTopCallSites(Sites, kMaxSites, Sorts[s]);

                R += FString().sprintf("### Ranked by %s\n", SortNames[s]);
                for (uint32 i = 0; i < NumSites; ++i)
                {
                    const Memory::FCallSiteStat& Site = Sites[i];
                    R += FString().sprintf("\n[%u] live %s | %llu live allocs | %llu total allocs | category [%s]\n",
                        i + 1, SizeBoth(Site.LiveBytes).c_str(),
                        (unsigned long long)Site.LiveCount, (unsigned long long)Site.TotalAllocs,
                        Site.CatName[0] ? Site.CatName : "Default");
                    for (uint32 f = 0; f < Site.FrameCount; ++f)
                    {
                        Memory::ResolveSymbol(Site.Frames[f], SymBuf, sizeof(SymBuf));
                        R += "    ";
                        R += SymBuf;
                        R += "\n";
                    }
                }
                R += "\n";
            }
        }
#else
        R += "## CPU categories / call sites\n(Compiled out in this build -- CPU tracking is Debug/Development only.)\n";
#endif

        ImGui::SetClipboardText(R.c_str());
    }
}
