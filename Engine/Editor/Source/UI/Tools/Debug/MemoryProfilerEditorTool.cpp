#include "MemoryProfilerEditorTool.h"

#include <cctype>
#include <cstring>
#include <cstdio>
#include <EASTL/sort.h>
#include "implot.h"
#include "Core/Engine/Engine.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "Platform/Process/PlatformProcess.h"
#include "Renderer/RHI.h"
#include "Renderer/RenderResource.h"
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

#if LUMINA_MEMORY_TRACKING
        bool StartsWith(const char* Text, const char* Prefix)
        {
            return std::strncmp(Text, Prefix, std::strlen(Prefix)) == 0;
        }

        // Frames every stack ends in: the CRT entry point, the OS thread/fiber thunks, and the window
        // proc chain. They carry no information about the allocation and trebled the height of a stack.
        bool IsNoiseFrame(const char* Fn)
        {
            static const char* const kNoise[] = {
                "__scrt_common_main_seh", "invoke_main", "BaseThreadInitThunk", "RtlUserThreadStart",
                "RtlUserFiberStart", "SetSecurityDescriptorControl", "wcsrchr", "CallWindowProcW",
                "IsWindowUnicode", "LuminaMain", "Lumina::FApplication::Run",
            };
            for (const char* N : kNoise)
            {
                if (std::strcmp(Fn, N) == 0) { return true; }
            }
            return false;
        }

        // Container and allocator internals. They say HOW the memory was taken (a vector grew, a
        // hashtable inserted a node); the frame below says WHO wanted it, which is what a reader is
        // looking for -- so these never get to be the headline of a call site.
        bool IsPlumbingFrame(const char* Fn)
        {
            static const char* const kPrefixes[] = {
                "eastl::", "std::", "moodycamel::", "ImVector", "ImGui::MemAlloc",
                "Lumina::Memory::", "Lumina::ImGuiX::ImGuiMemAlloc", "operator new", "malloc", "_malloc",
            };
            for (const char* P : kPrefixes)
            {
                if (StartsWith(Fn, P)) { return true; }
            }
            return false;
        }

        // Template arguments are most of the length of a name like
        // eastl::hashtable<__int64,eastl::pair<...>,...>::insert and none of the meaning at a glance.
        // Collapsed to <...> for the row; the tooltip still carries the full text.
        FString CollapseTemplateArgs(const FString& In)
        {
            FString Out;
            Out.reserve(In.size());
            int32 Depth = 0;
            for (char C : In)
            {
                if (C == '<')
                {
                    if (++Depth == 1) { Out += "<...>"; }
                    continue;
                }
                if (C == '>')
                {
                    if (Depth > 0) { --Depth; }
                    continue;
                }
                if (Depth == 0) { Out += C; }
            }
            return Out;
        }

        // Namespace-qualified names are long and the qualification repeats on every row. The leaf
        // (last "::" segment plus the type it hangs off) is what distinguishes one frame from another.
        FString ShortenForRow(const FString& Full, size_t MaxLen)
        {
            FString Text = Full.find('<') != FString::npos ? CollapseTemplateArgs(Full) : Full;
            if (Text.size() <= MaxLen)
            {
                return Text;
            }
            // Keep the tail: the leaf function is at the end of a qualified name.
            return FString("...") + Text.substr(Text.size() - MaxLen);
        }
#endif

        // Every GPU allocation the engine makes is named "Subsystem.Thing" -- Scene.HDR, Texture.Rock_D,
        // Upload.StagingSlice -- so the segment before the first dot already is the purpose, and no
        // parallel category enum has to be kept in sync with the allocation sites.
        //
        // Unnamed allocations fall back to what the allocation structurally is, which is all the RHI
        // knows about one. A growing "unnamed" bucket means a site that needs a SetDebugName, not a
        // limitation of the breakdown.
        FString GPUPurposeOf(const RHI::FGPUAllocation& Alloc)
        {
            if (Alloc.Name[0] != '\0')
            {
                const char* Dot = std::strchr(Alloc.Name, '.');
                if (Dot != nullptr && Dot != Alloc.Name)
                {
                    return FString(Alloc.Name, (size_t)(Dot - Alloc.Name));
                }
                return FString(Alloc.Name);
            }

            if (Alloc.Kind == RHI::EGPUAllocationKind::Texture)
            {
                using EUsage = RHI::EImageUsageFlags;
                if (EnumHasAnyFlags(Alloc.Desc.Usage, EUsage::DepthAttachment)) { return "<depth targets>"; }
                if (EnumHasAnyFlags(Alloc.Desc.Usage, EUsage::ColorAttachment)) { return "<color targets>"; }
                if (EnumHasAnyFlags(Alloc.Desc.Usage, EUsage::Storage))         { return "<storage images>"; }
                return "<sampled textures>";
            }

            switch (Alloc.Memory)
            {
            case RHI::EMemoryType::CPUWrite: return "<unnamed upload>";
            case RHI::EMemoryType::CPURead:  return "<unnamed readback>";
            default:                         return "<unnamed buffers>";
            }
        }

        const char* TextureTypeName(RHI::ETextureType Type)
        {
            switch (Type)
            {
            case RHI::ETextureType::Tex1D:        return "1D";
            case RHI::ETextureType::Tex2D:        return "2D";
            case RHI::ETextureType::Tex3D:        return "3D";
            case RHI::ETextureType::TexCube:      return "Cube";
            case RHI::ETextureType::Tex2DArray:   return "2DArray";
            case RHI::ETextureType::TexCubeArray: return "CubeArray";
            }
            return "?";
        }

        const char* MemoryTypeName(RHI::EMemoryType Type)
        {
            switch (Type)
            {
            case RHI::EMemoryType::CPUWrite: return "CPU-write";
            case RHI::EMemoryType::CPURead:  return "CPU-read";
            case RHI::EMemoryType::GPUOnly:  return "GPU-only";
            }
            return "?";
        }

        // "2048x2048 2DArray x6, 11 mips, BC7_UNORM" / "GPU-only" -- what the row is, past its size.
        FString DescribeAllocation(const RHI::FGPUAllocation& Alloc)
        {
            if (Alloc.Kind != RHI::EGPUAllocationKind::Texture)
            {
                return MemoryTypeName(Alloc.Memory);
            }

            const RHI::FTextureDesc& Desc = Alloc.Desc;
            FString Out;
            if (Desc.Type == RHI::ETextureType::Tex3D)
            {
                Out.sprintf("%ux%ux%u %s", Desc.Dimension.x, Desc.Dimension.y, Desc.Dimension.z, TextureTypeName(Desc.Type));
            }
            else
            {
                Out.sprintf("%ux%u %s", Desc.Dimension.x, Desc.Dimension.y, TextureTypeName(Desc.Type));
            }
            if (Desc.LayerCount > 1)
            {
                Out += FString().sprintf(" x%u", Desc.LayerCount);
            }
            if (Desc.MipCount > 1)
            {
                Out += FString().sprintf(", %u mips", Desc.MipCount);
            }
            if (Desc.SampleCount > 1)
            {
                Out += FString().sprintf(", %ux MSAA", Desc.SampleCount);
            }
            Out += FString().sprintf(", %s", RHI::Format::Info(Desc.Format).Name);
            return Out;
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
            "overhead and fragmentation. Memory by purpose is the other half: every live buffer "
            "and texture the RHI owns, grouped by the name its creating site gave it, so VRAM "
            "growth resolves to a subsystem the same way the CPU category table does. It always "
            "totals less than the heaps -- the difference is memory the engine never allocated.");
        DrawHelpTextRow("Unnamed GPU memory",
            "An allocation with no RHI::SetDebugName lands in an angle-bracketed bucket "
            "(<sampled textures>, <unnamed upload>) and is invisible in a device-lost report too. "
            "If one of those buckets is large, name the site rather than working around it here.");
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
            "The address-space scan is on-demand: the heap walk locks every heap process-wide. "
            "The GPU ledger is editor-only (WITH_EDITOR) and is walked only while the GPU tab is "
            "open -- a game build carries neither the allocation names nor the texture ledger.");
    }

    void FMemoryProfilerEditorTool::RefreshSnapshot()
    {
#if LUMINA_MEMORY_TRACKING
        Categories.resize(256);
        const uint32 N = Memory::GetCategoryStats(Categories.data(), (uint32)Categories.size());
        Categories.resize(N);
#endif

        RHI::GetGPUMemoryStats(GPUStats);

        // Invalidated here, re-pulled by the GPU tab if it is the one on screen.
        bGPUAllocationsValid = false;

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

    void FMemoryProfilerEditorTool::RefreshGPUAllocations()
    {
        RHI::GetGPUAllocations(GPUAllocations);
        bGPUAllocationsValid = true;

        GPUTextureBytes = 0;
        GPUBufferBytes  = 0;

        // Rebuilt rather than accumulated: the ledger is a snapshot of what is live right now, and a
        // purpose that dropped to zero has to disappear from the table rather than linger at its last value.
        GPUPurposes.clear();

        auto FindOrAdd = [this](const FString& Name) -> FGPUPurposeRow&
        {
            for (FGPUPurposeRow& Row : GPUPurposes)
            {
                if (Row.Name == Name)
                {
                    return Row;
                }
            }
            GPUPurposes.push_back(FGPUPurposeRow{ Name });
            return GPUPurposes.back();
        };

        for (const RHI::FGPUAllocation& Alloc : GPUAllocations)
        {
            FGPUPurposeRow& Row = FindOrAdd(GPUPurposeOf(Alloc));
            if (Alloc.Kind == RHI::EGPUAllocationKind::Texture)
            {
                Row.TextureBytes += Alloc.Size;
                Row.TextureCount++;
                GPUTextureBytes  += Alloc.Size;
            }
            else
            {
                Row.BufferBytes += Alloc.Size;
                Row.BufferCount++;
                GPUBufferBytes  += Alloc.Size;
            }
        }

        eastl::sort(GPUPurposes.begin(), GPUPurposes.end(),
            [](const FGPUPurposeRow& A, const FGPUPurposeRow& B) { return A.Total() > B.Total(); });

        eastl::sort(GPUAllocations.begin(), GPUAllocations.end(),
            [](const RHI::FGPUAllocation& A, const RHI::FGPUAllocation& B) { return A.Size > B.Size; });
    }

    void FMemoryProfilerEditorTool::DrawGPUPurpose()
    {
        const uint64 Attributed = GPUTextureBytes + GPUBufferBytes;

        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "Memory by purpose  " LE_ICON_INFORMATION);
        ImGuiX::TextTooltip("Every live buffer and texture the RHI owns, grouped by the name its creating "
                            "site gave it (the part before the first dot). The heap totals above are device "
                            "truth and stay larger: they also carry the driver's own allocations, descriptor "
                            "pools, swapchain images and allocator fragmentation, none of which the engine owns.\n\n"
                            "Editor only -- game builds keep neither the names nor the texture ledger.");
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), "%s", ImGuiX::FormatSize(GPUTextureBytes).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("textures");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.55f, 1.0f), "  %s", ImGuiX::FormatSize(GPUBufferBytes).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("buffers");
        ImGui::SameLine();
        ImGui::TextDisabled("  =  %s attributed of %s the allocator holds",
            ImGuiX::FormatSize(Attributed).c_str(),
            ImGuiX::FormatSize((size_t)GPUStats.TotalAllocated).c_str());

        ImGui::Spacing();

        const ImGuiTableFlags Flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                    | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("##GPUPurpose", 5, Flags, ImVec2(0, 220)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Purpose",  ImGuiTableColumnFlags_WidthFixed, 180);
            ImGui::TableSetupColumn("Share",    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Total",    ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Textures", ImGuiTableColumnFlags_WidthFixed, 140);
            ImGui::TableSetupColumn("Buffers",  ImGuiTableColumnFlags_WidthFixed, 140);
            ImGui::TableHeadersRow();

            const uint64 Largest = GPUPurposes.empty() ? 0 : GPUPurposes.front().Total();

            for (const FGPUPurposeRow& Row : GPUPurposes)
            {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(Row.Name.c_str());

                // Against the largest purpose, not the total: with one purpose dominating, bars scaled
                // to the total are all invisible and the column says nothing.
                ImGui::TableSetColumnIndex(1);
                const float Frac = Largest > 0 ? (float)((double)Row.Total() / (double)Largest) : 0.0f;
                const float Share = Attributed > 0 ? (float)((double)Row.Total() / (double)Attributed) : 0.0f;
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.45f, 0.62f, 0.85f, 1.0f));
                ImGui::ProgressBar(Frac, ImVec2(-1, 0), FString().sprintf("%.1f%%", Share * 100.0f).c_str());
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(ImGuiX::FormatSize(Row.Total()).c_str());

                ImGui::TableSetColumnIndex(3);
                if (Row.TextureCount > 0)
                {
                    ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), "%s", ImGuiX::FormatSize(Row.TextureBytes).c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%u)", Row.TextureCount);
                }
                else
                {
                    ImGui::TextDisabled("-");
                }

                ImGui::TableSetColumnIndex(4);
                if (Row.BufferCount > 0)
                {
                    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.55f, 1.0f), "%s", ImGuiX::FormatSize(Row.BufferBytes).c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%u)", Row.BufferCount);
                }
                else
                {
                    ImGui::TextDisabled("-");
                }
            }

            ImGui::EndTable();
        }
    }

    void FMemoryProfilerEditorTool::DrawGPUAllocations()
    {
        if (!ImGui::CollapsingHeader(LE_ICON_LIST_BOX " Allocations", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        ImGui::SetNextItemWidth(240);
        ImGui::InputTextWithHint("##GPUFilter", LE_ICON_MAGNIFY " Filter by name", GPUFilter, sizeof(GPUFilter));
        ImGui::SameLine();
        ImGui::Checkbox("Textures", &bShowTextures);
        ImGui::SameLine();
        ImGui::Checkbox("Buffers", &bShowBuffers);

        // Filter once into an index list: the table body runs through a clipper, and re-testing the
        // filter per visible row would make scrolling depend on where you are in the list.
        TVector<const RHI::FGPUAllocation*> Visible;
        Visible.reserve(GPUAllocations.size());

        char FilterLower[sizeof(GPUFilter)];
        for (size_t i = 0; i < sizeof(GPUFilter); ++i)
        {
            FilterLower[i] = (char)std::tolower((unsigned char)GPUFilter[i]);
        }

        for (const RHI::FGPUAllocation& Alloc : GPUAllocations)
        {
            const bool bTexture = Alloc.Kind == RHI::EGPUAllocationKind::Texture;
            if ((bTexture && !bShowTextures) || (!bTexture && !bShowBuffers))
            {
                continue;
            }

            if (FilterLower[0] != '\0')
            {
                char NameLower[RHI::kMaxGPUAllocationName];
                size_t n = 0;
                for (; n + 1 < sizeof(NameLower) && Alloc.Name[n] != '\0'; ++n)
                {
                    NameLower[n] = (char)std::tolower((unsigned char)Alloc.Name[n]);
                }
                NameLower[n] = '\0';
                if (std::strstr(NameLower, FilterLower) == nullptr)
                {
                    continue;
                }
            }

            Visible.push_back(&Alloc);
        }

        uint64 VisibleBytes = 0;
        for (const RHI::FGPUAllocation* Alloc : Visible)
        {
            VisibleBytes += Alloc->Size;
        }

        ImGui::TextDisabled("%zu allocations, %s", Visible.size(), ImGuiX::FormatSize(VisibleBytes).c_str());

        const ImGuiTableFlags Flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                    | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("##GPUAllocations", 4, Flags, ImVec2(0, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Kind",  ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Size",  ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthFixed, 280);
            ImGui::TableHeadersRow();

            ImGuiListClipper Clipper;
            Clipper.Begin((int)Visible.size());
            while (Clipper.Step())
            {
                for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                {
                    const RHI::FGPUAllocation& Alloc = *Visible[(size_t)i];
                    const bool bTexture = Alloc.Kind == RHI::EGPUAllocationKind::Texture;

                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    if (Alloc.Name[0] != '\0')
                    {
                        ImGui::TextUnformatted(Alloc.Name);
                    }
                    else
                    {
                        ImGui::TextDisabled("<unnamed>");
                        ImGuiX::TextTooltip("Nothing called RHI::SetDebugName on this allocation, so it "
                                            "cannot be attributed to a subsystem here or to a faulting "
                                            "address in a device-lost report.");
                    }

                    ImGui::TableSetColumnIndex(1);
                    if (bTexture)
                    {
                        ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), "Texture");
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.55f, 1.0f), "Buffer");
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(ImGuiX::FormatSize(Alloc.Size).c_str());

                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextDisabled("%s", DescribeAllocation(Alloc).c_str());
                }
            }

            ImGui::EndTable();
        }
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

        // Ledger walk is on the tab, not the refresh tick: it takes the allocator locks and copies
        // every live allocation, which is not something to do behind a tab nobody is looking at.
        if (!bGPUAllocationsValid)
        {
            RefreshGPUAllocations();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        DrawGPUPurpose();

        ImGui::Spacing();
        DrawGPUAllocations();
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

    const FMemoryProfilerEditorTool::FResolvedFrame& FMemoryProfilerEditorTool::ResolveCached(void* Address)
    {
        auto It = SymbolCache.find(Address);
        if (It != SymbolCache.end())
        {
            return It->second;
        }

        char SymBuf[512];
        Memory::ResolveSymbol(Address, SymBuf, sizeof(SymBuf));

        FResolvedFrame Frame;

        // ResolveSymbol formats a located frame as "Function  (File.cpp:1234)"; everything else
        // (no line info, no PDB, a bare address) arrives as a single token.
        const char* Open = std::strstr(SymBuf, "  (");
        const size_t Len = std::strlen(SymBuf);
        if (Open != nullptr && Len > 0 && SymBuf[Len - 1] == ')')
        {
            Frame.Function.assign(SymBuf, (size_t)(Open - SymBuf));
            Frame.Location.assign(Open + 3, Len - (size_t)(Open + 3 - SymBuf) - 1);
        }
        else
        {
            Frame.Function.assign(SymBuf, Len);
        }

        Frame.bNoise    = IsNoiseFrame(Frame.Function.c_str());
        Frame.bPlumbing = IsPlumbingFrame(Frame.Function.c_str());

        return SymbolCache.emplace(Address, Move(Frame)).first->second;
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
        ImGui::Checkbox("Trim boilerplate", &bHideNoiseFrames);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Hide the CRT/thread-entry frames every stack ends in.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%u sites)", NumSites);

        const ImGuiTableFlags Flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                    | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY
                                    | ImGuiTableFlags_SizingFixedFit;

        if (!ImGui::BeginTable("CallSites", 5, Flags, ImVec2(0, 0)))
        {
            return;
        }

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Call site", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Live",      ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Count",     ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Allocs",    ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Category",  ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        // The ranking metric is what the eye should land on first; the other column stays plain.
        const ImVec4 RankedColor  = ImVec4(0.98f, 0.78f, 0.35f, 1.0f);
        const ImVec4 SourceColor  = ImVec4(0.60f, 0.80f, 1.00f, 1.0f);   // frames with a file:line
        const ImVec4 ForeignColor = ImVec4(0.60f, 0.60f, 0.65f, 1.0f);   // no source: system/plumbing

        for (uint32 i = 0; i < NumSites; ++i)
        {
            const Memory::FCallSiteStat& Site = Sites[i];

            // The headline is the first frame that names engine code rather than the container that
            // happened to do the allocating -- "STerrainControllerSystem::CollectChunkCandidates",
            // not "eastl::hashtable<...>::insert". The plumbing frame is still there when expanded.
            uint32 HeadlineFrame = 0;
            for (uint32 f = 0; f < Site.FrameCount; ++f)
            {
                const FResolvedFrame& Candidate = ResolveCached(Site.Frames[f]);
                if (!Candidate.bPlumbing && !Candidate.bNoise)
                {
                    HeadlineFrame = f;
                    break;
                }
            }

            // By pointer: the cache is node-based, so entries stay put as later frames resolve, and a
            // conditional-expression reference would have copied the whole struct once per row.
            static const FResolvedFrame EmptyFrame;
            const FResolvedFrame* Headline = &EmptyFrame;
            if (Site.FrameCount > 0)
            {
                Headline = &ResolveCached(Site.Frames[HeadlineFrame]);
            }

            ImGui::TableNextRow();
            ImGui::PushID((int)i);

            ImGui::TableSetColumnIndex(0);
            const FString RowLabel = FString().sprintf("%u. %s", i + 1,
                Site.FrameCount > 0 ? ShortenForRow(Headline->Function, 96).c_str() : "(no frames)");

            const bool bOpen = ImGui::TreeNodeEx("##site", ImGuiTreeNodeFlags_SpanAllColumns
                | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick,
                "%s", RowLabel.c_str());

            if (ImGui::IsItemHovered() && !Headline->Location.empty())
            {
                ImGui::SetTooltip("%s\n%s", Headline->Function.c_str(), Headline->Location.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            if (bSortCallSitesByAllocs) { ImGui::TextUnformatted(ImGuiX::FormatSize(Site.LiveBytes).c_str()); }
            else { ImGui::TextColored(RankedColor, "%s", ImGuiX::FormatSize(Site.LiveBytes).c_str()); }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%llu", (unsigned long long)Site.LiveCount);

            ImGui::TableSetColumnIndex(3);
            if (bSortCallSitesByAllocs) { ImGui::TextColored(RankedColor, "%llu", (unsigned long long)Site.TotalAllocs); }
            else { ImGui::Text("%llu", (unsigned long long)Site.TotalAllocs); }

            ImGui::TableSetColumnIndex(4);
            ImGui::TextDisabled("%s", Site.CatName[0] ? Site.CatName : "Default");

            if (bOpen)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

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

                // Innermost frame first, matching every debugger's call-stack pane.
                for (uint32 f = 0; f < Site.FrameCount; ++f)
                {
                    const FResolvedFrame& Frame = ResolveCached(Site.Frames[f]);
                    if (bHideNoiseFrames && Frame.bNoise)
                    {
                        continue;
                    }

                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("%2u", f);
                    ImGui::SameLine();

                    // Frames carrying a file:line are the ones worth reading; the rest recede.
                    const bool bHasSource = !Frame.Location.empty();
                    ImGui::PushStyleColor(ImGuiCol_Text, bHasSource ? SourceColor : ForeignColor);
                    ImGui::TextUnformatted(ShortenForRow(Frame.Function, 110).c_str());
                    ImGui::PopStyleColor();

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", Frame.Function.c_str());
                    }

                    // Right-aligned at the far edge of the (stretching) call-site column, the way a
                    // debugger's stack pane reads. It lives here rather than in a stat column because
                    // no 90px column can hold a path, and dropping it when it does not fit keeps the
                    // function name -- the more important half -- from being pushed out.
                    if (bHasSource)
                    {
                        const float Available = ImGui::GetContentRegionAvail().x;
                        const float LocWidth  = ImGui::CalcTextSize(Frame.Location.c_str()).x;
                        if (Available > LocWidth + ImGui::GetStyle().ItemSpacing.x)
                        {
                            ImGui::SameLine(0.0f, Available - LocWidth);
                            ImGui::TextDisabled("%s", Frame.Location.c_str());
                        }
                    }
                }

                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
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

        // GPU by purpose -- the attribution the heap numbers above cannot give.
        RefreshGPUAllocations();
        {
            const uint64 Attributed = GPUTextureBytes + GPUBufferBytes;
            const uint64 Unattributed = GPUStats.TotalAllocated > Attributed
                                      ? GPUStats.TotalAllocated - Attributed : 0;

            R += "## GPU memory by purpose (sorted by total)\n";
            R += FString().sprintf("- Textures:   %s\n", SizeBoth(GPUTextureBytes).c_str());
            R += FString().sprintf("- Buffers:    %s\n", SizeBoth(GPUBufferBytes).c_str());
            R += FString().sprintf("- Attributed: %s of %s the allocator holds\n",
                SizeBoth(Attributed).c_str(), SizeBoth(GPUStats.TotalAllocated).c_str());
            R += FString().sprintf("- Remainder:  %s (driver allocations, descriptor pools, swapchain images, allocator fragmentation -- not engine-owned)\n\n",
                SizeBoth(Unattributed).c_str());

            R += "| Purpose | Total | Textures | Texture count | Buffers | Buffer count |\n";
            R += "|---------|-------|----------|---------------|---------|--------------|\n";
            for (const FGPUPurposeRow& Row : GPUPurposes)
            {
                R += FString().sprintf("| %s | %s | %s | %u | %s | %u |\n",
                    Row.Name.c_str(), SizeBoth(Row.Total()).c_str(),
                    SizeBoth(Row.TextureBytes).c_str(), Row.TextureCount,
                    SizeBoth(Row.BufferBytes).c_str(), Row.BufferCount);
            }
            R += "\n";

            // The tail is a long list of small textures and says nothing the purpose table did not.
            static constexpr size_t kMaxAllocationRows = 64;
            const size_t Shown = Math::Min(GPUAllocations.size(), kMaxAllocationRows);

            R += FString().sprintf("## Largest GPU allocations (top %zu of %zu live)\n",
                Shown, GPUAllocations.size());
            R += "| Name | Kind | Size | Detail |\n";
            R += "|------|------|------|--------|\n";
            for (size_t i = 0; i < Shown; ++i)
            {
                const RHI::FGPUAllocation& Alloc = GPUAllocations[i];
                R += FString().sprintf("| %s | %s | %s | %s |\n",
                    Alloc.Name[0] ? Alloc.Name : "(unnamed)",
                    Alloc.Kind == RHI::EGPUAllocationKind::Texture ? "Texture" : "Buffer",
                    SizeBoth(Alloc.Size).c_str(), DescribeAllocation(Alloc).c_str());
            }
            R += "\n";
        }

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
