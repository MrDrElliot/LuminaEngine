#include "Platform/Time/PlatformTime.h"
#include "MemoryProfilerEditorTool.h"

#include <cctype>
#include <cstring>
#include <cstdio>
#include "implot.h"
#include "Core/Engine/Engine.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "Platform/Process/PlatformProcess.h"
#include "Renderer/RHI.h"
#include "Renderer/RenderResource.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Containers/StringFormat.h"
#include "UI/Tools/Debug/DebugHistoryTimeline.h"

namespace Lumina
{
    namespace
    {
        constexpr uint32 kHistorySamples = 240;     // ~60s at the 0.25s refresh tick.
        constexpr float  kMemoryRefreshSeconds = 0.25f;

        constexpr ImVec4 kTextureSeries(0.55f, 0.80f, 0.95f, 1.0f);
        constexpr ImVec4 kBufferSeries(0.95f, 0.78f, 0.55f, 1.0f);
        constexpr ImVec4 kVRAMSeries(0.95f, 0.70f, 0.40f, 1.0f);
        constexpr ImVec4 kRSSSeries(0.40f, 0.80f, 0.55f, 1.0f);
        constexpr ImVec4 kTrackedSeries(0.40f, 1.0f, 0.60f, 1.0f);
        constexpr ImVec4 kRetainedSeries(0.85f, 0.72f, 0.45f, 1.0f);
        constexpr ImVec4 kCachedSeries(0.70f, 0.62f, 0.42f, 1.0f);
        constexpr ImVec4 kMappedSeries(0.66f, 0.78f, 0.95f, 1.0f);
        constexpr ImVec4 kExternalSeries(0.90f, 0.55f, 0.55f, 1.0f);
        constexpr ImVec4 kCRTHeapSeries(0.95f, 0.65f, 0.35f, 1.0f);

        // Green under load, amber as it fills, red when nearly out.
        ImVec4 UsageColor(float Fraction)
        {
            if (Fraction > 0.85f) { return EditorColors::Danger(); }
            if (Fraction > 0.65f) { return EditorColors::Warning(); }
            return EditorColors::Success();
        }

        struct FCPUMemorySample
        {
            size_t Process;
            size_t Tracked;
            size_t Mapped;
            size_t Committed;
            size_t Retained;
            size_t External;
        };

        // Retained and External use committed bytes because mapped also counts address space rpmalloc has decommitted.
        FCPUMemorySample SampleCPUMemory()
        {
            FCPUMemorySample S;
            S.Process   = Platform::GetProcessMemoryUsageBytes();
#if LUMINA_MEMORY_TRACKING
            S.Tracked   = Memory::GetTrackedLiveBytes();
#else
            S.Tracked   = 0;
#endif
            S.Mapped    = Memory::GetCurrentMappedMemory();
            S.Committed = Memory::GetCommittedMemory();
            S.Retained  = (S.Committed > S.Tracked) ? (S.Committed - S.Tracked) : 0;
            S.External  = (S.Process > S.Committed) ? (S.Process - S.Committed) : 0;
            return S;
        }

#if LUMINA_MEMORY_TRACKING
        bool StartsWith(const char* Text, const char* Prefix)
        {
            return std::strncmp(Text, Prefix, std::strlen(Prefix)) == 0;
        }

        // The CRT entry point and OS thunks carry no information and trebled a stack's height.
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

        // These say HOW the memory was taken, while the frame below says WHO wanted it.
        bool IsPlumbingFrame(const char* Fn)
        {
            static const char* const kPrefixes[] = {
                "std::", "ImVector", "ImGui::MemAlloc",
                "Lumina::Memory::", "Lumina::ImGuiX::ImGuiMemAlloc", "operator new", "malloc", "_malloc",
            };
            for (const char* P : kPrefixes)
            {
                if (StartsWith(Fn, P)) { return true; }
            }
            return false;
        }

        // Collapsed for the row, and the tooltip still carries the full text.
        FString CollapseTemplateArgs(const FString& In)
        {
            FString Out;
            Out.Reserve(In.Len());
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

        // The leaf segment plus the type it hangs off is what distinguishes one frame from another.
        FString ShortenForRow(const FString& Full, size_t MaxLen)
        {
            FString Text = Full.Find('<') != Constants::kIndexNone ? CollapseTemplateArgs(Full) : Full;
            if (static_cast<size_t>(Text.Len()) <= MaxLen)
            {
                return Text;
            }
            // Keep the tail, since the leaf function is at the end of a qualified name.
            return FString("...") + Text.Mid(IntCastChecked<int32>(Text.Len() - MaxLen));
        }
#endif

        // Every GPU allocation is named Subsystem.Thing, so the segment before the dot is the purpose.
        FString GPUPurposeOf(const RHI::FGPUAllocationInfo& Alloc)
        {
            if (Alloc.Name[0] != '\0')
            {
                const char* Dot = std::strchr(Alloc.Name, '.');
                if (Dot != nullptr && Dot != Alloc.Name)
                {
                    return FString(Alloc.Name, IntCastChecked<int32>(Dot - Alloc.Name));
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

        // What the row is, past its size.
        FString DescribeAllocation(const RHI::FGPUAllocationInfo& Alloc)
        {
            if (Alloc.Kind != RHI::EGPUAllocationKind::Texture)
            {
                return MemoryTypeName(Alloc.Memory);
            }

            const RHI::FTextureDesc& Desc = Alloc.Desc;
            FString Out;
            if (Desc.Type == RHI::ETextureType::Tex3D)
            {
                Out = Format("{}x{}x{} {}", Desc.Dimension.x, Desc.Dimension.y, Desc.Dimension.z, TextureTypeName(Desc.Type));
            }
            else
            {
                Out = Format("{}x{} {}", Desc.Dimension.x, Desc.Dimension.y, TextureTypeName(Desc.Type));
            }
            if (Desc.LayerCount > 1)
            {
                AppendFormat(Out, " x{}", Desc.LayerCount);
            }
            if (Desc.MipCount > 1)
            {
                AppendFormat(Out, ", {} mips", Desc.MipCount);
            }
            if (Desc.SampleCount > 1)
            {
                AppendFormat(Out, ", {}x MSAA", Desc.SampleCount);
            }
            AppendFormat(Out, ", {}", RHI::Format::Info(Desc.Format).Name);
            return Out;
        }

        // Human-readable plus exact, so a reader gets both the gestalt and a parseable number.
        FString SizeBoth(uint64 Bytes)
        {
            return Format("{} ({} bytes)", *ImGuiX::FormatSize((size_t)Bytes), (unsigned long long)Bytes);
        }

        // Compact line+fill plot for a rolling MB history.
        void DrawTimeline(const char* Id, const TRingBuffer<float>& History, const ImVec4& Color, float Height)
        {
            if (ImPlot::BeginPlot(Id, ImVec2(-1, Height), ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend | ImPlotFlags_NoTitle))
            {
                ImPlot::SetupAxes(nullptr, "MB",
                    ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines,
                    ImPlotAxisFlags_AutoFit);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, (double)kHistorySamples, ImGuiCond_Always);

                if (!History.IsEmpty())
                {
                    auto Sample = [](int Index, void* Data) { return ImPlotPoint(Index, HistorySampleAt(Data, Index)); };
                    auto Baseline = [](int Index, void*) { return ImPlotPoint(Index, 0.0); };
                    void* Data = const_cast<TRingBuffer<float>*>(&History);

                    ImPlot::PushStyleColor(ImPlotCol_Line, Color);
                    ImPlot::PushStyleColor(ImPlotCol_Fill, EditorColors::WithAlpha(Color, 0.25f));
                    ImPlot::PlotShadedG(Id, Sample, Data, Baseline, nullptr, History.Num());
                    ImPlot::PlotLineG(Id, Sample, Data, History.Num());
                    ImPlot::PopStyleColor(2);
                }
                ImPlot::EndPlot();
            }
        }
    }

    void FMemoryProfilerEditorTool::OnInitialize()
    {
#if LUMINA_MEMORY_TRACKING
        bCaptureAtStartup = Memory::IsCapturingCallstacks();
#endif
        CreateToolWindow("Memory", [this](bool bIsFocused)
        {
            DrawWindow(bIsFocused);
        });
    }

    void FMemoryProfilerEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
#if LUMINA_MEMORY_TRACKING
        // Stop the heavy per-alloc stack walk, unless -memcallstacks asked for it process-wide.
        Memory::SetCaptureCallstacks(bCaptureAtStartup);
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
        Categories.SetNum(256);
        const uint32 N = Memory::GetCategoryStats(Categories.GetData(), (uint32)Categories.Num());
        Categories.SetNum(N);
#endif

        RHI::GetGPUMemoryStats(GPUStats);

        // Invalidated here, re-pulled by the GPU tab if it is the one on screen.
        bGPUAllocationsValid = false;

        if (!bDeviceInfoValid)
        {
            DeviceInfo = RHI::GetDeviceInfo();
            bDeviceInfoValid = !DeviceInfo.Name.IsEmpty();
        }

        const float ToMB = 1.0f / float(Constants::kMiB);
        const FCPUMemorySample CPU = SampleCPUMemory();
        PushHistorySample(HistRSS, (float)CPU.Process * ToMB, kHistorySamples);
        PushHistorySample(HistMapped, (float)CPU.Mapped * ToMB, kHistorySamples);
        PushHistorySample(HistExternal, (float)CPU.External * ToMB, kHistorySamples);
        PushHistorySample(HistVRAM, (float)GPUStats.TotalUsage * ToMB, kHistorySamples);
#if LUMINA_MEMORY_TRACKING
        PushHistorySample(HistCPUTracked, (float)CPU.Tracked * ToMB, kHistorySamples);
#endif
    }

    void FMemoryProfilerEditorTool::DrawWindow(bool bIsFocused)
    {
        RefreshTimer += (float)Global::GEngine->GetDeltaTime();
        if (RefreshTimer >= kMemoryRefreshSeconds || HistVRAM.IsEmpty())
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
        const FCPUMemorySample CPU = SampleCPUMemory();

        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        const float CardW = (ImGui::GetContentRegionAvail().x - Spacing) * 0.5f;
        const float CardH = 104.0f;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorColors::WindowBg());
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);

        // CPU card
        ImGui::BeginChild("##CPUCard", ImVec2(CardW, CardH), true);
        {
            ImGui::TextColored(EditorColors::SectionHeader(), LE_ICON_CPU_64_BIT " CPU MEMORY");
            ImGui::Separator();
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::TextUnformatted(*ImGuiX::FormatSize(CPU.Process));
            ImGuiX::Font::PopFont();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("process (RSS)");

            ImGui::Spacing();
            ImGui::TextColored(kTrackedSeries, "%s tracked", *ImGuiX::FormatSize(CPU.Tracked));
            ImGui::SameLine();
            ImGui::TextColored(kRetainedSeries, "  %s retained", *ImGuiX::FormatSize(CPU.Retained));
            ImGui::SameLine();
            ImGui::TextColored(kExternalSeries, "  %s external", *ImGuiX::FormatSize(CPU.External));
            ImGuiX::TextTooltip("tracked = ledger live bytes\n"
                                "retained = rpmalloc committed - tracked (caches + fragmentation; freed, not returned to OS)\n"
                                "external = RSS - rpmalloc committed (GPU driver host memory, CRT malloc, code/stacks)");
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // GPU card
        ImGui::BeginChild("##GPUCard", ImVec2(CardW, CardH), true);
        {
            ImGui::TextColored(EditorColors::SectionHeader(), LE_ICON_EXPANSION_CARD " GPU MEMORY");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", bDeviceInfoValid ? *DeviceInfo.Name : "");

            ImGui::Separator();

            const float Frac = (GPUStats.TotalBudget > 0)
                ? (float)((double)GPUStats.TotalUsage / (double)GPUStats.TotalBudget) : 0.0f;

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::TextUnformatted(*ImGuiX::FormatSize(GPUStats.TotalUsage));
            ImGuiX::Font::PopFont();
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("/ %s  (%.0f%%)", *ImGuiX::FormatSize(GPUStats.TotalBudget), Frac * 100.0f);

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
        ImGui::TextColored(EditorColors::SectionHeader(), "VRAM usage");
        DrawTimeline("##VRAMPlot", HistVRAM, kVRAMSeries, 160.0f);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##RSSTL", ImVec2(0, 200), false);
        ImGui::TextColored(EditorColors::SectionHeader(), "Process RSS");
        DrawTimeline("##RSSPlot", HistRSS, kRSSSeries, 160.0f);
        ImGui::EndChild();
    }

    // GPU

    void FMemoryProfilerEditorTool::DrawGPUHeaps()
    {
        ImGui::TextColored(EditorColors::SectionHeader(), "Device heaps");
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
                    ImGui::TextColored(EditorColors::Success(), "%s", Kind);
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
                ImGui::ProgressBar(Frac, ImVec2(-1, 0), *Format("{:.1f}%", Frac * 100.0f));
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(*ImGuiX::FormatSize((size_t)Heap.UsageBytes));

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(*ImGuiX::FormatSize((size_t)Heap.BudgetBytes));

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%u", Heap.AllocationCount);
            }

            ImGui::EndTable();
        }

        ImGui::TextDisabled("Allocator: %s in %u allocations across %u blocks.",
            *ImGuiX::FormatSize((size_t)GPUStats.TotalAllocated),
            GPUStats.TotalAllocations, GPUStats.TotalBlocks);
    }

    void FMemoryProfilerEditorTool::RefreshGPUAllocations()
    {
        RHI::GetGPUAllocations(GPUAllocations);
        bGPUAllocationsValid = true;

        GPUTextureBytes = 0;
        GPUBufferBytes  = 0;

        // Rebuilt rather than accumulated, so a purpose that dropped to zero disappears from the table.
        GPUPurposes.Reset();

        auto FindOrAdd = [this](const FString& Name) -> FGPUPurposeRow&
        {
            for (FGPUPurposeRow& Row : GPUPurposes)
            {
                if (Row.Name == Name)
                {
                    return Row;
                }
            }
            GPUPurposes.Emplace(Name);
            return GPUPurposes.Last();
        };

        for (const RHI::FGPUAllocationInfo& Alloc : GPUAllocations)
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

        Algo::Sort(GPUPurposes,
            [](const FGPUPurposeRow& A, const FGPUPurposeRow& B) { return A.Total() > B.Total(); });

        Algo::Sort(GPUAllocations,
            [](const RHI::FGPUAllocationInfo& A, const RHI::FGPUAllocationInfo& B) { return A.Size > B.Size; });
    }

    void FMemoryProfilerEditorTool::DrawGPUPurpose()
    {
        const uint64 Attributed = GPUTextureBytes + GPUBufferBytes;

        ImGui::TextColored(EditorColors::SectionHeader(), "Memory by purpose  " LE_ICON_INFORMATION);
        ImGuiX::TextTooltip("Every live buffer and texture the RHI owns, grouped by the name its creating "
                            "site gave it (the part before the first dot). The heap totals above are device "
                            "truth and stay larger: they also carry the driver's own allocations, descriptor "
                            "pools, swapchain images and allocator fragmentation, none of which the engine owns.\n\n"
                            "Editor only -- game builds keep neither the names nor the texture ledger.");
        ImGui::Spacing();

        ImGui::TextColored(kTextureSeries, "%s", *ImGuiX::FormatSize(GPUTextureBytes));
        ImGui::SameLine();
        ImGui::TextDisabled("textures");
        ImGui::SameLine();
        ImGui::TextColored(kBufferSeries, "  %s", *ImGuiX::FormatSize(GPUBufferBytes));
        ImGui::SameLine();
        ImGui::TextDisabled("buffers");
        ImGui::SameLine();
        ImGui::TextDisabled("  =  %s attributed of %s the allocator holds",
            *ImGuiX::FormatSize(Attributed),
            *ImGuiX::FormatSize((size_t)GPUStats.TotalAllocated));

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

            const uint64 Largest = GPUPurposes.IsEmpty() ? 0 : GPUPurposes[0].Total();

            for (const FGPUPurposeRow& Row : GPUPurposes)
            {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(*Row.Name);

                // Against the largest purpose, since bars scaled to the total are invisible when one dominates.
                ImGui::TableSetColumnIndex(1);
                const float Frac = Largest > 0 ? (float)((double)Row.Total() / (double)Largest) : 0.0f;
                const float Share = Attributed > 0 ? (float)((double)Row.Total() / (double)Attributed) : 0.0f;
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, EditorColors::Accent());
                ImGui::ProgressBar(Frac, ImVec2(-1, 0), *Format("{:.1f}%", Share * 100.0f));
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(*ImGuiX::FormatSize(Row.Total()));

                ImGui::TableSetColumnIndex(3);
                if (Row.TextureCount > 0)
                {
                    ImGui::TextColored(kTextureSeries, "%s", *ImGuiX::FormatSize(Row.TextureBytes));
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
                    ImGui::TextColored(kBufferSeries, "%s", *ImGuiX::FormatSize(Row.BufferBytes));
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

        // The body runs through a clipper, so re-testing per visible row makes scrolling position-dependent.
        TVector<const RHI::FGPUAllocationInfo*> Visible;
        Visible.Reserve(GPUAllocations.Num());

        char FilterLower[sizeof(GPUFilter)];
        for (size_t i = 0; i < sizeof(GPUFilter); ++i)
        {
            FilterLower[i] = (char)std::tolower((unsigned char)GPUFilter[i]);
        }

        for (const RHI::FGPUAllocationInfo& Alloc : GPUAllocations)
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

            Visible.Add(&Alloc);
        }

        uint64 VisibleBytes = 0;
        for (const RHI::FGPUAllocationInfo* Alloc : Visible)
        {
            VisibleBytes += Alloc->Size;
        }

        ImGui::TextDisabled("%d allocations, %s", Visible.Num(), *ImGuiX::FormatSize(VisibleBytes));

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
            Clipper.Begin(Visible.Num());
            while (Clipper.Step())
            {
                for (int i = Clipper.DisplayStart; i < Clipper.DisplayEnd; ++i)
                {
                    const RHI::FGPUAllocationInfo& Alloc = *Visible[i];
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
                        ImGui::TextColored(kTextureSeries, "Texture");
                    }
                    else
                    {
                        ImGui::TextColored(kBufferSeries, "Buffer");
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(*ImGuiX::FormatSize(Alloc.Size));

                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextDisabled("%s", *DescribeAllocation(Alloc));
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
                *DeviceInfo.Name, *DeviceInfo.APIName,
                DeviceInfo.bDiscrete ? "Discrete" : "Integrated");
        }
        ImGui::Spacing();

        DrawGPUHeaps();

        // The walk takes the allocator locks and copies every live allocation, so it waits for the tab.
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
        const FCPUMemorySample CPU = SampleCPUMemory();
        const size_t Cached = Memory::GetCachedMemory();

        ImGui::Spacing();
        ImGui::TextColored(EditorColors::SectionHeader(), "Composition  " LE_ICON_INFORMATION);
        ImGuiX::TextTooltip("Where Process RSS lives. If 'retained' climbs, it's allocation churn -- rpmalloc "
                            "holding freed/fragmented spans (rank Top Call Sites by Total Allocs to find it). "
                            "If 'external' climbs, it's outside rpmalloc: GPU driver host memory or the CRT allocator.");
        ImGui::Spacing();

        auto Row = [](const char* Label, size_t Bytes, const ImVec4& Color, const char* Note)
        {
            ImGui::TextColored(Color, "%14s", *ImGuiX::FormatSize(Bytes));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Label);
            if (Note && Note[0])
            {
                ImGui::SameLine();
                ImGui::TextDisabled("- %s", Note);
            }
        };

        Row("Process RSS", CPU.Process, EditorColors::TextPrimary(), "total resident");
        Row("rpmalloc mapped", CPU.Mapped, kMappedSeries, "allocator's address space, decommitted spans included");
        Row("rpmalloc committed", CPU.Committed, kMappedSeries, "allocator's RAM footprint");
        Row("tracked live", CPU.Tracked, kTrackedSeries, "category ledger");
        Row("retained", CPU.Retained, kRetainedSeries, "committed - tracked, caches + fragmentation");
        Row("of which cached", Cached, kCachedSeries, "rpmalloc global span cache");
        Row("external", CPU.External, kExternalSeries, "RSS - committed, driver / CRT / code+stacks");

        if (CPU.Mapped == 0)
        {
            ImGui::TextColored(EditorColors::Warning(),
                LE_ICON_ALERT " rpmalloc statistics are zero -- build with ENABLE_STATISTICS (Debug/Development).");
        }

        ImGui::Spacing();
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        const float HalfW = (ImGui::GetContentRegionAvail().x - Spacing) * 0.5f;

        ImGui::BeginChild("##MappedTL", ImVec2(HalfW, 150), false);
        ImGui::TextColored(kMappedSeries, "rpmalloc mapped (address space)");
        DrawTimeline("##MappedPlot", HistMapped, kMappedSeries, 110.0f);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##ExternalTL", ImVec2(0, 150), false);
        ImGui::TextColored(kExternalSeries, "external (driver / CRT)");
        DrawTimeline("##ExternalPlot", HistExternal, kExternalSeries, 110.0f);
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();
    }

    void FMemoryProfilerEditorTool::RunAddressSpaceScan()
    {
        const double Start = PlatformTime::Seconds();
        Platform::GetAddressSpaceStats(AddressSpace, bScanHeaps);
        LastScanCostMs = (PlatformTime::Seconds() - Start) * 1000.0;
        LastScanTime = PlatformTime::Seconds();
        bAddressSpaceValid = true;
    }

    // The only panel that sees allocations no engine allocator made, such as the driver's own.
    void FMemoryProfilerEditorTool::DrawAddressSpace()
    {
        ImGui::Spacing();
        ImGui::TextColored(EditorColors::SectionHeader(), "Address Space  " LE_ICON_INFORMATION);
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
            AddressSpace.RegionCount, LastScanCostMs, PlatformTime::Seconds() - LastScanTime);

        auto Row = [](const char* Label, uint64 Bytes, const ImVec4& Color, const char* Note)
        {
            ImGui::TextColored(Color, "%14s", *ImGuiX::FormatSize((size_t)Bytes));
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
        Row("private", AddressSpace.PrivateCommitted, EditorColors::TextPrimary(), "this process only");
        Row("image", AddressSpace.ImageCommitted, EditorColors::TextDim(), "code + data of loaded modules");
        Row("mapped", AddressSpace.MappedCommitted, EditorColors::TextDim(), "file mappings and shared sections");
        Row("reserved", AddressSpace.Reserved, EditorColors::TextMuted(), "address space only, costs no RAM");

        // Whichever is largest tells you which trace to run, see BuildScripts/MemoryTrace.ps1.
        const uint64 Rpmalloc = Memory::GetCommittedMemory();
        const uint64 Heap     = AddressSpace.bHeapWalkValid ? AddressSpace.HeapCommitted : 0;
        const uint64 Accounted = Rpmalloc + Heap;
        const uint64 Unattributed = AddressSpace.PrivateCommitted > Accounted
                                  ? AddressSpace.PrivateCommitted - Accounted : 0;

        ImGui::Spacing();
        ImGui::TextDisabled("Private commit, by owner");
        Row("rpmalloc", Rpmalloc, kMappedSeries, "engine allocator (the category table below)");

        if (AddressSpace.bHeapWalkValid)
        {
            Row("NT / CRT heap", Heap, kCRTHeapSeries, "foreign DLLs: Slang, GPU driver, basisu, ucrtbase");
            ImGui::TextDisabled("%14s of that live, %s block overhead",
                *ImGuiX::FormatSize((size_t)AddressSpace.HeapAllocated),
                *ImGuiX::FormatSize((size_t)AddressSpace.HeapOverhead));
        }
        else if (bScanHeaps)
        {
            ImGui::TextColored(EditorColors::Warning(),
                LE_ICON_ALERT " Heap walk failed (a heap refused to lock); CRT bytes are folded into unattributed.");
        }
        else
        {
            ImGui::TextDisabled("%14s NT / CRT heap - not measured (heap walk off)", "?");
        }

        Row("unattributed", Unattributed, kExternalSeries, "raw VirtualAlloc: GPU driver, thread + fiber stacks");

        ImGui::Spacing();
        ImGui::Separator();
    }

    void FMemoryProfilerEditorTool::DrawCPUTab()
    {
        DrawCPUComposition();
        DrawAddressSpace();

#if !LUMINA_MEMORY_TRACKING
        ImGui::Spacing();
        ImGui::TextColored(EditorColors::Warning(),
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
        else if (!CallSiteCategory.IsEmpty())
        {
            ImGui::Spacing();
            ImGui::TextColored(EditorColors::Warning(),
                LE_ICON_ALERT " Enable 'Capture call stacks' to see what allocates inside '%s'.",
                *CallSiteCategory);
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
            Baseline.Reset();
            bHasBaseline = false;
        }
        ImGuiX::TextTooltip("Zero all counters and start a fresh capture.");

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_MAGNIFY " Analyze Ledger"))
        {
            CopyLedgerAnalysisToClipboard();
        }
        ImGuiX::TextTooltip("Walk every live allocation in the selected category (all categories if none "
                            "is selected) and copy a size histogram plus the largest blocks. Reads the "
                            "allocation ledger directly, so it works even where no call stack was captured.");

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

    void FMemoryProfilerEditorTool::CopyLedgerAnalysisToClipboard()
    {
        const char* Cat = CallSiteCategory.IsEmpty() ? nullptr : *CallSiteCategory;

        Memory::FCategoryLedger Summary;
        Memory::FLedgerAlloc    Largest[Memory::kMaxLedgerTop];
        const uint32 NumLargest = Memory::AnalyzeCategory(Cat, Summary, Largest, 24);

        FString Report;
        char    Line[640];

        std::snprintf(Line, sizeof(Line), "=== Memory ledger: [%s] ===\n",
            Cat ? Cat : "all categories");
        Report += Line;

        std::snprintf(Line, sizeof(Line), "live %s across %llu allocations\n",
            *ImGuiX::FormatSize(Summary.LiveBytes),
            (unsigned long long)Summary.LiveCount);
        Report += Line;

        const double UnattribPct = Summary.LiveBytes > 0
            ? (100.0 * (double)Summary.UnattributedBytes / (double)Summary.LiveBytes) : 0.0;
        std::snprintf(Line, sizeof(Line), "no captured stack: %s across %llu allocations (%.1f%%)\n\n",
            *ImGuiX::FormatSize(Summary.UnattributedBytes),
            (unsigned long long)Summary.UnattributedCount, UnattribPct);
        Report += Line;

        const uint64 Dropped = Memory::GetCallSiteOverflowCount();
        if (Dropped > 0)
        {
            std::snprintf(Line, sizeof(Line),
                "WARNING: %llu allocations lost their stack (call-site table full).\n\n",
                (unsigned long long)Dropped);
            Report += Line;
        }

        Report += "Size class            Allocations        Live bytes\n";
        for (uint32 b = 0; b < 64; ++b)
        {
            if (Summary.BucketCount[b] == 0)
            {
                continue;
            }
            const uint64 Low = (b == 0) ? 0ull : (1ull << b);
            std::snprintf(Line, sizeof(Line), "%8s - %-8s %12llu %17s\n",
                *ImGuiX::FormatSize(Low),
                *ImGuiX::FormatSize((1ull << (b + 1)) - 1),
                (unsigned long long)Summary.BucketCount[b],
                *ImGuiX::FormatSize(Summary.BucketBytes[b]));
            Report += Line;
        }

        Report += "\nLargest live allocations\n";
        char SymBuf[512];
        for (uint32 i = 0; i < NumLargest; ++i)
        {
            std::snprintf(Line, sizeof(Line), "\n[%u] %s\n", i + 1,
                *ImGuiX::FormatSize(Largest[i].Size));
            Report += Line;

            if (Largest[i].FrameCount == 0)
            {
                Report += "    (no stack captured)\n";
                continue;
            }
            for (uint32 f = 0; f < Largest[i].FrameCount; ++f)
            {
                Memory::ResolveSymbol(Largest[i].Frames[f], SymBuf, sizeof(SymBuf));
                Report += "    ";
                Report += SymBuf;
                Report += "\n";
            }
        }

        ImGui::SetClipboardText(*Report);
        LOG_INFO("Memory ledger analysis for [{}] copied to clipboard ({} live, {} allocations)",
            Cat ? Cat : "all categories",
            *ImGuiX::FormatSize(Summary.LiveBytes), Summary.LiveCount);
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

        TVector<FCategoryRow>& Rows = CategoryRows;
        Rows.Reset();
        Rows.Reserve(Categories.Num());
        for (const Memory::FMemoryCategoryStats& S : Categories)
        {
            const Memory::FMemoryCategoryStats* B = FindBaseline(S.Name);
            FCategoryRow Row;
            Row.S          = &S;
            Row.DeltaBytes = B ? (int64)S.LiveBytes - (int64)B->LiveBytes : 0;
            Row.DeltaCount = B ? (int64)S.LiveCount - (int64)B->LiveCount : 0;
            Rows.Add(Row);
        }

        Algo::Sort(Rows, [this](const FCategoryRow& A, const FCategoryRow& B)
        {
            if (bHasBaseline) { return A.DeltaBytes > B.DeltaBytes; }
            return A.S->LiveBytes > B.S->LiveBytes;
        });

        if (bHasBaseline && !Rows.IsEmpty() && Rows[0].DeltaBytes > 0)
        {
            ImGui::TextColored(EditorColors::Warning(),
                LE_ICON_ALERT " Growing fastest: %s  (+%s)",
                Rows[0].S->Name, *ImGuiX::FormatSize((size_t)Rows[0].DeltaBytes));
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

            for (const FCategoryRow& Row : Rows)
            {
                const Memory::FMemoryCategoryStats& S = *Row.S;
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                const char* Name = S.Name[0] ? S.Name : "Default";
                const bool bSelected = (CallSiteCategory == Name);
                if (ImGui::Selectable(Name, bSelected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    CallSiteCategory = bSelected ? FString() : FString(Name);
                }
                ImGuiX::TextTooltip("Select to rank Top Call Sites within this category only.");

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(*ImGuiX::FormatSize(S.LiveBytes));

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", (unsigned long long)S.LiveCount);

                ImGui::TableSetColumnIndex(3);
                if (!bHasBaseline)
                {
                    ImGui::TextDisabled("-");
                }
                else if (Row.DeltaBytes > 0)
                {
                    ImGui::TextColored(EditorColors::Danger(), "+%s",
                        *ImGuiX::FormatSize((size_t)Row.DeltaBytes));
                }
                else if (Row.DeltaBytes < 0)
                {
                    ImGui::TextColored(EditorColors::Success(), "-%s",
                        *ImGuiX::FormatSize((size_t)(-Row.DeltaBytes)));
                }
                else
                {
                    ImGui::TextDisabled("0");
                }

                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(*ImGuiX::FormatSize(S.PeakBytes));

                ImGui::TableSetColumnIndex(5);
                ImGui::TextDisabled("%llu / %llu",
                    (unsigned long long)S.TotalAllocs, (unsigned long long)S.TotalFrees);
            }

            ImGui::EndTable();
        }
    }

    const FMemoryProfilerEditorTool::FResolvedFrame& FMemoryProfilerEditorTool::ResolveCached(void* Address)
    {
        auto It = SymbolCache.Find(Address);
        if (It != nullptr)
        {
            return (*It);
        }

        char SymBuf[512];
        Memory::ResolveSymbol(Address, SymBuf, sizeof(SymBuf));

        FResolvedFrame Frame;

        // ResolveSymbol formats a located frame with its file and line, and everything else is one token.
        const char* Open = std::strstr(SymBuf, "  (");
        const size_t Len = std::strlen(SymBuf);
        if (Open != nullptr && Len > 0 && SymBuf[Len - 1] == ')')
        {
            Frame.Function = FStringView(SymBuf, IntCastChecked<int32>(Open - SymBuf));
            Frame.Location = FStringView(Open + 3, IntCastChecked<int32>(Len - (size_t)(Open + 3 - SymBuf) - 1));
        }
        else
        {
            Frame.Function = FStringView(SymBuf, IntCastChecked<int32>(Len));
        }

        Frame.bNoise    = IsNoiseFrame(*Frame.Function);
        Frame.bPlumbing = IsPlumbingFrame(*Frame.Function);
        Frame.RowLabel   = ShortenForRow(Frame.Function, kRowLabelMaxLen);
        Frame.FrameLabel = ShortenForRow(Frame.Function, kFrameLabelMaxLen);

        return SymbolCache.FindOrAdd(Address, Move(Frame));
    }

    void FMemoryProfilerEditorTool::DrawCallSites()
    {
        if (!ImGui::CollapsingHeader(LE_ICON_LIST_BOX " Top Call Sites", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        // Live bytes finds leaks while total allocations finds transient churn, which are different sites.
        ImGui::TextUnformatted("Rank by:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Live bytes", !bSortCallSitesByAllocs)) { bSortCallSitesByAllocs = false; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Total allocs (churn)", bSortCallSitesByAllocs)) { bSortCallSitesByAllocs = true; }

        const Memory::ECallSiteSort Sort = bSortCallSitesByAllocs
            ? Memory::ECallSiteSort::TotalAllocs : Memory::ECallSiteSort::LiveBytes;

        if (!CallSiteCategory.IsEmpty())
        {
            ImGui::SameLine();
            ImGui::TextColored(EditorColors::AccentAlt(), LE_ICON_FILTER " %s", *CallSiteCategory);
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear filter"))
            {
                CallSiteCategory.Reset();
            }
        }

        static constexpr uint32 kMaxSites = 64;
        Memory::FCallSiteStat Sites[kMaxSites];
        const uint32 NumSites = Memory::GetTopCallSites(Sites, kMaxSites, Sort,
            CallSiteCategory.IsEmpty() ? nullptr : *CallSiteCategory);

        if (NumSites == 0)
        {
            if (CallSiteCategory.IsEmpty())
            {
                ImGui::TextDisabled("No call sites captured yet -- give it a few frames.");
            }
            else
            {
                ImGui::TextDisabled("Nothing captured for '%s' yet -- its allocations predate the capture.",
                    *CallSiteCategory);
            }
            return;
        }

        char SymBuf[512];

        ImGui::SameLine();
        if (ImGui::SmallButton(LE_ICON_CONTENT_COPY " Copy all"))
        {
            FString Report;
            char Line[640];
            std::snprintf(Line, sizeof(Line),
                "=== Memory: top %u call sites in [%s] by %s (tracked live %s, %llu allocs) ===\n",
                NumSites, CallSiteCategory.IsEmpty() ? "all categories" : *CallSiteCategory,
                bSortCallSitesByAllocs ? "total allocs" : "live bytes",
                *ImGuiX::FormatSize(Memory::GetTrackedLiveBytes()),
                (unsigned long long)Memory::GetTrackedLiveCount());
            Report += Line;

            for (uint32 i = 0; i < NumSites; ++i)
            {
                const Memory::FCallSiteStat& S = Sites[i];
                std::snprintf(Line, sizeof(Line),
                    "\n[%u] live %s (%llu live) | %llu total allocs | [%s]\n",
                    i + 1,
                    *ImGuiX::FormatSize(S.LiveBytes),
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
            ImGui::SetClipboardText(*Report);
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
        const ImVec4 RankedColor  = EditorColors::AccentAlt();
        const ImVec4 SourceColor  = EditorColors::Accent();    // frames carrying a file and line
        const ImVec4 ForeignColor = EditorColors::TextDim();   // no source, so system or plumbing

        for (uint32 i = 0; i < NumSites; ++i)
        {
            const Memory::FCallSiteStat& Site = Sites[i];

            // The headline is the first frame naming engine code rather than the container that allocated.
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

            // By pointer, since the cache is node-based and a reference copy would clone the struct per row.
            static const FResolvedFrame EmptyFrame;
            const FResolvedFrame* Headline = &EmptyFrame;
            if (Site.FrameCount > 0)
            {
                Headline = &ResolveCached(Site.Frames[HeadlineFrame]);
            }

            ImGui::TableNextRow();
            ImGui::PushID((int)i);

            ImGui::TableSetColumnIndex(0);
            const char* RowLabel = Site.FrameCount > 0 ? *Headline->RowLabel : "(no frames)";

            const bool bOpen = ImGui::TreeNodeEx("##site", ImGuiTreeNodeFlags_SpanAllColumns
                | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick,
                "%u. %s", i + 1, RowLabel);

            if (ImGui::IsItemHovered() && !Headline->Location.IsEmpty())
            {
                ImGui::SetTooltip("%s\n%s", *Headline->Function, *Headline->Location);
            }

            ImGui::TableSetColumnIndex(1);
            if (bSortCallSitesByAllocs) { ImGui::TextUnformatted(*ImGuiX::FormatSize(Site.LiveBytes)); }
            else { ImGui::TextColored(RankedColor, "%s", *ImGuiX::FormatSize(Site.LiveBytes)); }

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
                        *ImGuiX::FormatSize(Site.LiveBytes),
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

                    // Frames carrying a file and line are the ones worth reading, and the rest recede.
                    const bool bHasSource = !Frame.Location.IsEmpty();
                    ImGui::PushStyleColor(ImGuiCol_Text, bHasSource ? SourceColor : ForeignColor);
                    ImGui::TextUnformatted(*Frame.FrameLabel);
                    ImGui::PopStyleColor();

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", *Frame.Function);
                    }

                    // Right-aligned like a debugger's stack pane, and dropped rather than pushing the name out.
                    if (bHasSource)
                    {
                        const float Available = ImGui::GetContentRegionAvail().x;
                        const float LocWidth  = ImGui::CalcTextSize(*Frame.Location).x;
                        if (Available > LocWidth + ImGui::GetStyle().ItemSpacing.x)
                        {
                            ImGui::SameLine(0.0f, Available - LocWidth);
                            ImGui::TextDisabled("%s", *Frame.Location);
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
        R.Reserve(16 * 1024);

        const FCPUMemorySample CPU = SampleCPUMemory();
        const size_t Untracked = (CPU.Process > CPU.Tracked) ? (CPU.Process - CPU.Tracked) : 0;
        const size_t Cached    = Memory::GetCachedMemory();

        R += "# Lumina Engine - Memory Report\n\n";
        R += "Captured from the in-editor Memory tool. Sizes show human-readable form with exact\n";
        R += "byte counts in parentheses. Counts are live (currently outstanding) unless noted.\n";
        R += "Use this to find which purpose/category/call-site is driving memory growth.\n\n";

        // System
        R += "## System\n";
        if (bDeviceInfoValid)
        {
            AppendFormat(R, "- GPU: {} ({})\n", *DeviceInfo.Name, DeviceInfo.bDiscrete ? "Discrete" : "Integrated");
            AppendFormat(R, "- API: {}\n", *DeviceInfo.APIName);
        }
        else
        {
            R += "- GPU: (device info unavailable)\n";
        }
        R += "\n";

        // CPU
        R += "## CPU memory\n";
        AppendFormat(R, "- Process RSS:     {}\n", *SizeBoth(CPU.Process));
        AppendFormat(R, "- rpmalloc mapped: {} (allocator's address space, decommitted spans included)\n", *SizeBoth(CPU.Mapped));
        AppendFormat(R, "- rpmalloc committed: {} (allocator's RAM footprint)\n", *SizeBoth(CPU.Committed));
        AppendFormat(R, "- Tracked:         {} (category ledger, live)\n", *SizeBoth(CPU.Tracked));
        AppendFormat(R, "- Retained:        {} (committed - tracked: rpmalloc caches + fragmentation, freed but not returned to OS)\n", *SizeBoth(CPU.Retained));
        AppendFormat(R, "- ...cached:       {} (rpmalloc global span cache)\n", *SizeBoth(Cached));
        AppendFormat(R, "- External:        {} (RSS - committed: GPU driver host memory, CRT malloc, code + stacks)\n", *SizeBoth(CPU.External));
        AppendFormat(R, "- Untracked:       {} (RSS - tracked; = retained + external)\n\n", *SizeBoth(Untracked));

        // The OS-level view, omitted rather than faked when the user has not run a scan.
        R += "## Address space (OS scan)\n";
        if (!bAddressSpaceValid)
        {
            R += "- Not scanned. Run 'Scan' on the CPU tab to attribute memory outside rpmalloc.\n\n";
        }
        else
        {
            const uint64 Heap = AddressSpace.bHeapWalkValid ? AddressSpace.HeapCommitted : 0;
            const uint64 Accounted = CPU.Committed + Heap;
            const uint64 Unattributed = AddressSpace.PrivateCommitted > Accounted
                                      ? AddressSpace.PrivateCommitted - Accounted : 0;

            AppendFormat(R, "- Private commit:  {} (this process only)\n", *SizeBoth(AddressSpace.PrivateCommitted));
            AppendFormat(R, "- Image commit:    {} (code + data of loaded modules)\n", *SizeBoth(AddressSpace.ImageCommitted));
            AppendFormat(R, "- Mapped commit:   {} (file mappings, shared sections)\n", *SizeBoth(AddressSpace.MappedCommitted));
            AppendFormat(R, "- Reserved:        {} (address space only, no RAM)\n", *SizeBoth(AddressSpace.Reserved));
            R += "\n  Private commit by owner:\n";
            AppendFormat(R, "  - rpmalloc:      {} (engine allocator; see category table)\n", *SizeBoth(CPU.Committed));
            if (AddressSpace.bHeapWalkValid)
            {
                AppendFormat(R, "  - NT/CRT heap:   {} committed, {} live in {} heaps (foreign DLLs: Slang, GPU driver, basisu, ucrtbase)\n",
                    *SizeBoth(AddressSpace.HeapCommitted), *SizeBoth(AddressSpace.HeapAllocated), AddressSpace.HeapCount);
            }
            else
            {
                R += "  - NT/CRT heap:   not measured (heap walk off or failed; folded into unattributed)\n";
            }
            AppendFormat(R, "  - Unattributed:  {} (raw VirtualAlloc: GPU driver, thread stacks)\n\n", *SizeBoth(Unattributed));
        }

        // GPU summary
        const float VRAMFrac = (GPUStats.TotalBudget > 0)
            ? (float)((double)GPUStats.TotalUsage / (double)GPUStats.TotalBudget) : 0.0f;
        R += "## GPU summary\n";
        AppendFormat(R, "- VRAM usage: {} of {} ({:.1f}%)\n",
            *SizeBoth(GPUStats.TotalUsage), *SizeBoth(GPUStats.TotalBudget), VRAMFrac * 100.0f);
        AppendFormat(R, "- Allocator allocated: {}\n", *SizeBoth(GPUStats.TotalAllocated));
        AppendFormat(R, "- Allocator blocks:    {} in {} blocks\n",
            *SizeBoth(GPUStats.TotalBlockBytes), GPUStats.TotalBlocks);
        AppendFormat(R, "- Live allocations:    {}\n\n", GPUStats.TotalAllocations);

        // GPU heaps
        R += "## GPU heaps\n";
        R += "| Heap | Type | Used | Budget | Used% | Allocated | Blocks | Allocs |\n";
        R += "|------|------|------|--------|-------|-----------|--------|--------|\n";
        for (const RHI::FGPUMemoryHeapStats& H : GPUStats.Heaps)
        {
            const float Frac = (H.BudgetBytes > 0) ? (float)((double)H.UsageBytes / (double)H.BudgetBytes) : 0.0f;
            AppendFormat(R, "| {} | {} | {} | {} | {:.1f}% | {} | {} | {} |\n",
                H.HeapIndex, H.bReBAR ? "Device (ReBAR)" : H.bDeviceLocal ? (H.bHostVisible ? "Device (BAR)" : "Device") : "Host",
                *SizeBoth(H.UsageBytes), *SizeBoth(H.BudgetBytes), Frac * 100.0f,
                *SizeBoth(H.AllocatedBytes), H.BlockCount, H.AllocationCount);
        }
        R += "\n";

        // GPU by purpose, the attribution the heap numbers above cannot give.
        RefreshGPUAllocations();
        {
            const uint64 Attributed = GPUTextureBytes + GPUBufferBytes;
            const uint64 Unattributed = GPUStats.TotalAllocated > Attributed
                                      ? GPUStats.TotalAllocated - Attributed : 0;

            R += "## GPU memory by purpose (sorted by total)\n";
            AppendFormat(R, "- Textures:   {}\n", *SizeBoth(GPUTextureBytes));
            AppendFormat(R, "- Buffers:    {}\n", *SizeBoth(GPUBufferBytes));
            AppendFormat(R, "- Attributed: {} of {} the allocator holds\n",
                *SizeBoth(Attributed), *SizeBoth(GPUStats.TotalAllocated));
            AppendFormat(R, "- Remainder:  {} (driver allocations, descriptor pools, swapchain images, allocator fragmentation -- not engine-owned)\n\n",
                *SizeBoth(Unattributed));

            R += "| Purpose | Total | Textures | Texture count | Buffers | Buffer count |\n";
            R += "|---------|-------|----------|---------------|---------|--------------|\n";
            for (const FGPUPurposeRow& Row : GPUPurposes)
            {
                AppendFormat(R, "| {} | {} | {} | {} | {} | {} |\n",
                    *Row.Name, *SizeBoth(Row.Total()),
                    *SizeBoth(Row.TextureBytes), Row.TextureCount,
                    *SizeBoth(Row.BufferBytes), Row.BufferCount);
            }
            R += "\n";

            // The tail is a long list of small textures and says nothing the purpose table did not.
            static constexpr int32 kMaxAllocationRows = 64;
            const int32 Shown = Math::Min(GPUAllocations.Num(), kMaxAllocationRows);

            AppendFormat(R, "## Largest GPU allocations (top {} of {} live)\n",
                Shown, GPUAllocations.Num());
            R += "| Name | Kind | Size | Detail |\n";
            R += "|------|------|------|--------|\n";
            for (int32 i = 0; i < Shown; ++i)
            {
                const RHI::FGPUAllocationInfo& Alloc = GPUAllocations[i];
                AppendFormat(R, "| {} | {} | {} | {} |\n",
                    Alloc.Name[0] ? Alloc.Name : "(unnamed)",
                    Alloc.Kind == RHI::EGPUAllocationKind::Texture ? "Texture" : "Buffer",
                    *SizeBoth(Alloc.Size), *DescribeAllocation(Alloc));
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
            Rows.Reserve(Categories.Num());
            for (const Memory::FMemoryCategoryStats& S : Categories)
            {
                const Memory::FMemoryCategoryStats* B = FindBaseline(S.Name);
                Rows.Emplace(&S,
                    B ? (int64)S.LiveBytes - (int64)B->LiveBytes : 0,
                    B ? (int64)S.LiveCount - (int64)B->LiveCount : 0);
            }
            Algo::Sort(Rows, [](const FRow& A, const FRow& B) { return A.S->LiveBytes > B.S->LiveBytes; });

            R += "## CPU memory by category (sorted by live bytes)\n";
            AppendFormat(R, "Baseline set: {}\n\n", bHasBaseline ? "yes (Delta = growth since baseline)" : "no");
            R += "| Category | Live | Count | Peak | Total allocs | Total frees | Delta bytes | Delta count |\n";
            R += "|----------|------|-------|------|--------------|-------------|-------------|-------------|\n";
            for (const FRow& Row : Rows)
            {
                const Memory::FMemoryCategoryStats& S = *Row.S;
                AppendFormat(R, "| {} | {} | {} | {} | {} | {} | {}{} | {} |\n",
                    S.Name[0] ? S.Name : "Default",
                    *SizeBoth(S.LiveBytes), (unsigned long long)S.LiveCount,
                    *SizeBoth(S.PeakBytes),
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

                AppendFormat(R, "### Ranked by {}\n", SortNames[s]);
                for (uint32 i = 0; i < NumSites; ++i)
                {
                    const Memory::FCallSiteStat& Site = Sites[i];
                    AppendFormat(R, "\n[{}] live {} | {} live allocs | {} total allocs | category [{}]\n",
                        i + 1, *SizeBoth(Site.LiveBytes),
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

        ImGui::SetClipboardText(*R);
    }
}
