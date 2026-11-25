#include "ProfilerEditorTool.h"
#include "ProfilerViewCommon.h"

#include <cctype>
#include <cmath>
#include <cstdio>

#include "imgui.h"

#if defined(LUMINA_WITH_GPU_PROFILING)

#include "Renderer/RHI.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"

namespace Lumina
{
    namespace
    {
        namespace GPUView
        {
            struct FBudget
            {
                const char* Label;
                double      Ms;
            };

            constexpr FBudget kBudgets[] =
            {
                { "60 fps",  1000.0 / 60.0  },
                { "30 fps",  1000.0 / 30.0  },
                { "90 fps",  1000.0 / 90.0  },
                { "120 fps", 1000.0 / 120.0 },
                { "144 fps", 1000.0 / 144.0 },
            };

            // Lanes top to bottom, so async compute sits right under the graphics work it overlaps.
            constexpr RHI::EQueueType kLaneOrder[] = { RHI::EQueueType::Graphics, RHI::EQueueType::Compute, RHI::EQueueType::Transfer };

            // Below these a parent's uncovered time is timestamp jitter rather than work missing a marker.
            constexpr double kUnmarkedFloorMs = 0.02;
            constexpr double kUnmarkedShare   = 0.10;

            constexpr double kMinViewMs = 0.002;

            bool HasUnmarkedTime(const FGPUProfileScope& Scope, bool bHasChildren)
            {
                return bHasChildren && Scope.SelfMs > kUnmarkedFloorMs && Scope.SelfMs > Scope.DurationMs() * kUnmarkedShare;
            }

            uint32 HashName(const char* Name)
            {
                uint32 H = 2166136261u;
                for (const char* P = Name ? Name : "?"; *P; ++P)
                {
                    H ^= (uint8)*P;
                    H *= 16777619u;
                }
                return H;
            }

            bool ContainsNoCase(const char* Haystack, const char* Needle)
            {
                if (Needle == nullptr || Needle[0] == '\0')
                {
                    return true;
                }
                for (const char* H = Haystack; *H; ++H)
                {
                    const char* A = H;
                    const char* B = Needle;
                    while (*A && *B && std::tolower((uint8)*A) == std::tolower((uint8)*B))
                    {
                        ++A;
                        ++B;
                    }
                    if (*B == '\0')
                    {
                        return true;
                    }
                }
                return false;
            }

            ImVec4 BudgetColor(double Share)
            {
                if (Share > 1.0)
                {
                    return EditorColors::Danger();
                }
                return Share > 0.75 ? EditorColors::Warning() : EditorColors::Success();
            }

            ImVec4 CostColor(double FrameShare)
            {
                if (FrameShare > 0.20)
                {
                    return EditorColors::Danger();
                }
                return FrameShare > 0.08 ? EditorColors::Warning() : EditorColors::TextPrimary();
            }

            ImVec4 QueueColor(RHI::EQueueType Queue)
            {
                switch (Queue)
                {
                case RHI::EQueueType::Compute:  return EditorColors::AccentAlt();
                case RHI::EQueueType::Transfer: return EditorColors::Warning();
                default:                        return EditorColors::Accent();
                }
            }

            // Sub-tenth timings read better in microseconds, where the digits that matter are not after the point.
            void FormatMs(char* Buffer, size_t Size, double Ms)
            {
                if (Ms < 0.1)
                {
                    snprintf(Buffer, Size, "%.0f us", Ms * 1000.0);
                }
                else
                {
                    snprintf(Buffer, Size, "%.2f ms", Ms);
                }
            }

            void TextMs(const ImVec4& Color, double Ms)
            {
                char Buffer[32];
                FormatMs(Buffer, sizeof(Buffer), Ms);
                ImGui::TextColored(Color, "%s", Buffer);
            }

            // Steps of 1, 2 and 5 per decade, so ruler labels stay round at any zoom.
            double NiceStep(double Range, double TargetTicks)
            {
                const double Raw  = Range / Math::Max(TargetTicks, 1.0);
                const double Mag  = std::pow(10.0, std::floor(std::log10(Raw)));
                const double Norm = Raw / Mag;
                const double Nice = Norm < 1.5 ? 1.0 : (Norm < 3.5 ? 2.0 : (Norm < 7.5 ? 5.0 : 10.0));
                return Nice * Mag;
            }

            int32 StepDecimals(double Step)
            {
                if (Step >= 1.0)
                {
                    return 0;
                }
                return Math::Min(4, (int32)std::ceil(-std::log10(Step) - 1e-9));
            }

            ImU32 LabelColorOn(ImU32 Background)
            {
                const ImVec4 C = ImGui::ColorConvertU32ToFloat4(Background);
                const float Luma = 0.299f * C.x + 0.587f * C.y + 0.114f * C.z;
                return Luma > 0.55f ? IM_COL32(18, 18, 22, 240) : IM_COL32(240, 240, 245, 240);
            }

            void Strip()
            {
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::TextColored(EditorColors::TextMuted(), "|");
                ImGui::SameLine(0.0f, 8.0f);
            }

            void ShareBar(double Share, const ImVec4& Color)
            {
                const ImVec2 Min = ImGui::GetCursorScreenPos();
                const float  W   = ImGui::GetContentRegionAvail().x;
                const float  H   = ImGui::GetTextLineHeight();
                ImDrawList* DL = ImGui::GetWindowDrawList();
                DL->AddRectFilled(Min, ImVec2(Min.x + W, Min.y + H), EditorColors::U32(EditorColors::WithAlpha(EditorColors::FrameBg(), 0.6f)), 2.0f);

                // Rounding a fill narrower than its corners draws a stray sliver, so tiny shares get square ends or none.
                const float FillW = W * EditorColors::Clamp01((float)Share);
                if (FillW >= 1.0f)
                {
                    DL->AddRectFilled(Min, ImVec2(Min.x + FillW, Min.y + H), EditorColors::U32(EditorColors::WithAlpha(Color, 0.55f)), FillW > 4.0f ? 2.0f : 0.0f);
                }

                char Text[16];
                snprintf(Text, sizeof(Text), "%.1f%%", Share * 100.0);
                const float TextW = ImGui::CalcTextSize(Text).x;
                DL->AddText(ImVec2(Min.x + W - TextW - 4.0f, Min.y), EditorColors::U32(EditorColors::TextPrimary()), Text);
                ImGui::Dummy(ImVec2(W, H));
            }

            void PushHeadlineFont()
            {
                ImGui::PushFont(Global::GFonts[(int32)ImGuiX::Font::EFont::MediumBold], ImGui::GetFontSize() * 1.8f);
            }

            void Sparkline(const float* Values, int32 Count, int32 Offset, double PeakMs, const ImVec4& Color)
            {
                // A floor on the scale keeps a pass that costs almost nothing flat instead of magnifying its jitter.
                const float Ceiling = (float)Math::Max(PeakMs * 1.1, 0.05);
                ImGui::PushStyleColor(ImGuiCol_PlotLines, EditorColors::WithAlpha(Color, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
                ImGui::PlotLines("##h", Values, Count, Offset, nullptr, 0.0f, Ceiling, ImVec2(-1.0f, ImGui::GetTextLineHeight()));
                ImGui::PopStyleColor(2);
            }
        }
    }

    double FProfilerEditorTool::FGPUPassStats::AverageMs() const
    {
        double Sum = 0.0;
        for (int32 i = 0; i < Count; ++i)
        {
            Sum += History[i];
        }
        return Count > 0 ? Sum / Count : 0.0;
    }

    double FProfilerEditorTool::FGPUPassStats::AverageSelfMs() const
    {
        double Sum = 0.0;
        for (int32 i = 0; i < Count; ++i)
        {
            Sum += SelfHistory[i];
        }
        return Count > 0 ? Sum / Count : 0.0;
    }

    double FProfilerEditorTool::FGPUPassStats::PeakMs() const
    {
        float Peak = 0.0f;
        for (int32 i = 0; i < Count; ++i)
        {
            Peak = Math::Max(Peak, History[i]);
        }
        return Peak;
    }

    double FProfilerEditorTool::GetGPUBudgetMs() const
    {
        return GPUView::kBudgets[Math::Clamp(GPUBudgetIndex, 0, (int32)std::size(GPUView::kBudgets) - 1)].Ms;
    }

    void FProfilerEditorTool::DrawGPU()
    {
        FGPUProfiler& Profiler = FGPUProfiler::Get();

        if (!RHI::SupportsTimestamps())
        {
            ImGui::TextColored(EditorColors::TextMuted(), "This device reports no timestamp support, so GPU timings are unavailable.");
            return;
        }

        bool bEnabled = Profiler.IsEnabled();
        if (ImGui::Checkbox("Collect", &bEnabled))
        {
            Profiler.SetEnabled(bEnabled);
        }
        ImGui::SetItemTooltip("Same switch as r.GPUProfiler.Enabled. Off, the markers cost one relaxed load each.");

        if (!bEnabled)
        {
            ImGui::Spacing();
            ImGui::TextColored(EditorColors::TextMuted(), "Collection is off. Tick Collect to time every marked GPU pass.");
            return;
        }

        if (!bFrozen && Profiler.HasResolvedFrame())
        {
            const FGPUProfileFrame& Live = Profiler.GetLatest();
            SmoothTowards(Live);
            AccumulatePassStats(Live);
            const TSpan<const float> History = Profiler.GetFrameHistory();
            GPUHistory.Reset();
            GPUHistory.Append(History.begin(), History.end());
        }

        if (GPUFrame.Scopes.IsEmpty())
        {
            ImGui::Spacing();
            ImGui::TextColored(EditorColors::TextMuted(), "Waiting for a resolved frame.");
            return;
        }

        if (SelectedScope >= GPUFrame.Scopes.Num() || (SelectedScope != Constants::kIndexNone && GPUFrame.Scopes[SelectedScope].Name != SelectedPass))
        {
            SelectedScope = Constants::kIndexNone;
        }

        DrawGPUSummary();
        ImGui::Spacing();
        DrawGPUFrameGraph();
        ImGui::Spacing();
        DrawGPUTimeline();
        ImGui::Spacing();

        if (ImGui::RadioButton("Tree", !bGPUFlatView))
        {
            bGPUFlatView = false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("By name", bGPUFlatView))
        {
            bGPUFlatView = true;
        }
        ImGui::SetItemTooltip("Sums every pass of one name, such as one per shadow view, into a single row.");

        GPUView::Strip();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##gpufilter", LE_ICON_MAGNIFY " Filter passes", GPUFilter, sizeof(GPUFilter));

        if (!bGPUFlatView)
        {
            GPUView::Strip();
            ImGui::Checkbox("Unmarked time", &bGPUShowUnmarked);
            ImGui::SetItemTooltip("Adds a row under each pass whose children leave a real share of its time uncovered. That is work with no marker of its own.");
        }

        if (!SelectedPass.IsEmpty())
        {
            GPUView::Strip();
            ImGui::TextColored(EditorColors::Accent(), "%s", *SelectedPass);
            ImGui::SameLine();
            if (ImGui::SmallButton(LE_ICON_CLOSE "##clearsel"))
            {
                SelectedPass.Reset();
                SelectedScope = Constants::kIndexNone;
            }
            ImGui::SetItemTooltip("Clear the selection.");
        }

        if (bGPUFlatView)
        {
            DrawGPUPassTable();
        }
        else
        {
            DrawGPUPassTree();
        }
    }

    // A matching name, depth and queue at the same index is the same pass; anything else snaps rather than blending two bars.
    void FProfilerEditorTool::SmoothTowards(const FGPUProfileFrame& Live)
    {
        const double Keep = (double)Math::Clamp(Smoothing, 0.0f, 0.98f);
        const double Take = 1.0 - Keep;
        auto Blend = [&](double Current, double Target) { return Current > 0.0 ? Current * Keep + Target * Take : Target; };

        GPUFrame.FrameNumber   = Live.FrameNumber;
        GPUFrame.DroppedScopes = Live.DroppedScopes;
        GPUFrame.Scopes.SetNum(Live.Scopes.Num());

        for (int32 Index = 0; Index < Live.Scopes.Num(); ++Index)
        {
            const FGPUProfileScope& In = Live.Scopes[Index];
            FGPUProfileScope& Out = GPUFrame.Scopes[Index];

            const bool bSamePass = Out.Name == In.Name && Out.Depth == In.Depth && Out.Queue == In.Queue;

            Out.Name        = In.Name;
            Out.ParentIndex = In.ParentIndex;
            Out.Depth       = In.Depth;
            Out.Queue       = In.Queue;
            Out.StartMs     = bSamePass ? (Out.StartMs * Keep + In.StartMs * Take) : In.StartMs;
            Out.EndMs       = bSamePass ? (Out.EndMs   * Keep + In.EndMs   * Take) : In.EndMs;
            Out.SelfMs      = bSamePass ? (Out.SelfMs  * Keep + In.SelfMs  * Take) : In.SelfMs;
        }

        GPUFrame.TotalMs = Blend(GPUFrame.TotalMs, Live.TotalMs);
        for (uint32 Queue = 0; Queue < kGPUProfileQueueCount; ++Queue)
        {
            // A queue that went idle drops out at once instead of fading through the smoothing.
            GPUFrame.QueueBusyMs[Queue] = Live.QueueBusyMs[Queue] > 0.0 ? Blend(GPUFrame.QueueBusyMs[Queue], Live.QueueBusyMs[Queue]) : 0.0;
        }
        GPUFrame.AsyncOverlapMs = Live.AsyncOverlapMs > 0.0 ? Blend(GPUFrame.AsyncOverlapMs, Live.AsyncOverlapMs) : 0.0;
    }

    // Raw rather than smoothed values, so the average and peak describe what the GPU really did.
    void FProfilerEditorTool::AccumulatePassStats(const FGPUProfileFrame& Live)
    {
        if (Live.FrameNumber == GPUStatsFrame)
        {
            return;
        }
        GPUStatsFrame = Live.FrameNumber;

        for (const FGPUProfileScope& Scope : Live.Scopes)
        {
            FGPUPassStats& Stats = GPUPassStats.FindOrAdd(FName(*Scope.Name));
            if (Stats.LastFrame != GPUStatsFrame)
            {
                Stats.LastFrame  = GPUStatsFrame;
                Stats.LastMs     = 0.0;
                Stats.LastSelfMs = 0.0;
                Stats.Calls      = 0;
                Stats.Depth      = Scope.Depth;
                Stats.Queue      = (uint8)Scope.Queue;
            }
            Stats.LastMs     += Scope.DurationMs();
            Stats.LastSelfMs += Scope.SelfMs;
            ++Stats.Calls;
        }

        TVector<FName> Expired;
        for (auto& [Name, Stats] : GPUPassStats)
        {
            const bool bPresent = Stats.LastFrame == GPUStatsFrame;
            if (!bPresent && GPUStatsFrame - Stats.LastFrame > (uint64)kGPUPassHistory)
            {
                Expired.Add(Name);
                continue;
            }
            Stats.History[Stats.Head]     = bPresent ? (float)Stats.LastMs : 0.0f;
            Stats.SelfHistory[Stats.Head] = bPresent ? (float)Stats.LastSelfMs : 0.0f;
            Stats.Head  = (Stats.Head + 1) % kGPUPassHistory;
            Stats.Count = Math::Min(Stats.Count + 1, kGPUPassHistory);
        }
        for (const FName& Name : Expired)
        {
            GPUPassStats.Remove(Name);
        }
    }

    // Hue follows the top-level pass, so a family of children reads as one block, and each child shifts it slightly.
    uint32 FProfilerEditorTool::GPUScopeColor(int32 ScopeIndex) const
    {
        const FGPUProfileScope& Scope = GPUFrame.Scopes[ScopeIndex];
        int32 Root = ScopeIndex;
        while (GPUFrame.Scopes[Root].ParentIndex != Constants::kIndexNone)
        {
            Root = GPUFrame.Scopes[Root].ParentIndex;
        }

        const int32 Level = Math::Min(Scope.Depth - GPUFrame.Scopes[Root].Depth, 5);
        const float Jitter = Level > 0 ? ((GPUView::HashName(*Scope.Name) % 64) / 63.0f - 0.5f) * 0.09f : 0.0f;
        float Hue = (GPUView::HashName(*GPUFrame.Scopes[Root].Name) % 360) / 360.0f + Jitter;
        Hue -= std::floor(Hue);

        float R, G, B;
        ImGui::ColorConvertHSVtoRGB(Hue, 0.40f + 0.07f * Level, 0.88f - 0.06f * Level, R, G, B);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(R, G, B, 1.0f));
    }

    bool FProfilerEditorTool::IsGPUScopeHighlighted(int32 ScopeIndex) const
    {
        const FGPUProfileScope& Scope = GPUFrame.Scopes[ScopeIndex];
        if (!SelectedPass.IsEmpty() && Scope.Name != SelectedPass)
        {
            return false;
        }
        return GPUView::ContainsNoCase(*Scope.Name, GPUFilter);
    }

    void FProfilerEditorTool::DrawGPUSummary()
    {
        const double BudgetMs = GetGPUBudgetMs();
        const double Share    = GPUFrame.TotalMs / BudgetMs;

        ImGui::BeginGroup();
        GPUView::PushHeadlineFont();
        ImGui::TextColored(GPUView::BudgetColor(Share), "%.2f ms", GPUFrame.TotalMs);
        ImGui::PopFont();
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 14.0f);
        ImGui::BeginGroup();
        ImGui::TextColored(EditorColors::TextDim(), "GPU frame, %.0f%% of the", Share * 100.0);
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("144 fps").x + ImGui::GetFrameHeight() + 12.0f);
        if (ImGui::BeginCombo("##gpubudget", GPUView::kBudgets[GPUBudgetIndex].Label))
        {
            for (int32 i = 0; i < (int32)std::size(GPUView::kBudgets); ++i)
            {
                if (ImGui::Selectable(GPUView::kBudgets[i].Label, i == GPUBudgetIndex))
                {
                    GPUBudgetIndex = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextColored(EditorColors::TextDim(), "budget");

        ImGui::TextColored(EditorColors::TextMuted(), "Frame %llu", (unsigned long long)GPUFrame.FrameNumber);
        GPUView::Strip();
        ImGui::TextColored(EditorColors::TextMuted(), "%d passes", GPUFrame.Scopes.Num());
        if (GPUFrame.DroppedScopes > 0)
        {
            GPUView::Strip();
            ImGui::TextColored(EditorColors::Warning(), LE_ICON_ALERT " %u dropped", GPUFrame.DroppedScopes);
            ImGui::SetItemTooltip("Scopes whose timestamps were unavailable or past the per-frame cap of %u.", FGPUProfiler::MaxScopesPerFrame);
        }
        ImGui::EndGroup();

        // One meter per queue, filled by busy time over the frame span, so idle gaps show as the unfilled part.
        const float MeterH = ImGui::GetTextLineHeight() + 6.0f;
        bool bFirst = true;
        for (RHI::EQueueType Queue : GPUView::kLaneOrder)
        {
            if (!GPUFrame.HasQueue(Queue))
            {
                continue;
            }
            if (!bFirst)
            {
                ImGui::SameLine(0.0f, 12.0f);
            }
            bFirst = false;

            const double BusyMs = GPUFrame.QueueBusyMs[(uint32)Queue];
            const double Fill   = GPUFrame.TotalMs > 0.0 ? BusyMs / GPUFrame.TotalMs : 0.0;
            const ImVec4 Color  = GPUView::QueueColor(Queue);

            char Label[96];
            if (Queue == RHI::EQueueType::Compute)
            {
                const double Overlap = BusyMs > 0.0 ? GPUFrame.AsyncOverlapMs / BusyMs * 100.0 : 0.0;
                snprintf(Label, sizeof(Label), "%s  %.2f ms, %.0f%% overlapped", GPUProfileQueueName(Queue), BusyMs, Overlap);
            }
            else
            {
                snprintf(Label, sizeof(Label), "%s  %.2f ms busy, %.2f ms idle", GPUProfileQueueName(Queue), BusyMs, Math::Max(0.0, GPUFrame.TotalMs - BusyMs));
            }

            // Sized to its label, so a long queue name or a wide number never runs into the next meter.
            const float MeterW = Math::Max(200.0f, ImGui::CalcTextSize(Label).x + 16.0f);

            ImGui::PushID((int)Queue);
            const ImVec2 Min = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##meter", ImVec2(MeterW, MeterH));
            ImDrawList* DL = ImGui::GetWindowDrawList();
            DL->AddRectFilled(Min, ImVec2(Min.x + MeterW, Min.y + MeterH), EditorColors::U32(EditorColors::FrameBg()), 3.0f);
            const float FillW = MeterW * EditorColors::Clamp01((float)Fill);
            if (FillW >= 1.0f)
            {
                DL->AddRectFilled(Min, ImVec2(Min.x + FillW, Min.y + MeterH), EditorColors::U32(EditorColors::WithAlpha(Color, 0.38f)), FillW > 6.0f ? 3.0f : 0.0f);
            }
            DL->AddText(ImVec2(Min.x + 8.0f, Min.y + 3.0f), EditorColors::U32(EditorColors::TextPrimary()), Label);

            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextColored(Color, "%s queue", GPUProfileQueueName(Queue));
                ImGui::Separator();
                ImGui::Text("Busy  %.3f ms (%.0f%% of the frame span)", BusyMs, Fill * 100.0);
                ImGui::Text("Idle  %.3f ms", Math::Max(0.0, GPUFrame.TotalMs - BusyMs));
                if (Queue == RHI::EQueueType::Compute)
                {
                    ImGui::Text("Ran alongside graphics for %.3f ms, which is what async compute saved.", GPUFrame.AsyncOverlapMs);
                }
                else
                {
                    ImGui::TextColored(EditorColors::TextMuted(), "Idle time inside the frame is a wait on another queue, the CPU, or a barrier.");
                }
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    }

    void FProfilerEditorTool::DrawGPUFrameGraph()
    {
        if (GPUHistory.IsEmpty())
        {
            return;
        }

        float Peak = 0.0f;
        double Sum = 0.0;
        for (float Value : GPUHistory)
        {
            Peak = Math::Max(Peak, Value);
            Sum += Value;
        }
        const double Average = Sum / GPUHistory.Num();

        TVector<float> Sorted = GPUHistory;
        Algo::Sort(Sorted);
        const float P95 = Sorted[Math::Min(Sorted.Num() - 1, (int32)(Sorted.Num() * 0.95f))];

        // The bars stay raw so a hitch still reads as a spike; only the axis they are drawn against eases.
        const float Keep = bFrozen ? 1.0f : Math::Clamp(Smoothing, 0.0f, 0.98f);
        HistoryPeak = (HistoryPeak > 0.0f) ? (HistoryPeak * Keep + Peak * (1.0f - Keep)) : Peak;
        const float Ceiling = Math::Max(Math::Max(HistoryPeak, Peak) * 1.2f, 0.25f);

        const double BudgetMs = GetGPUBudgetMs();

        ImGui::TextColored(EditorColors::TextDim(), "History");
        GPUView::Strip();
        ImGui::TextColored(EditorColors::TextMuted(), "avg %.2f ms", Average);
        GPUView::Strip();
        ImGui::TextColored(EditorColors::TextMuted(), "p95 %.2f ms", P95);
        GPUView::Strip();
        ImGui::TextColored(GPUView::BudgetColor(Peak / BudgetMs), "peak %.2f ms", Peak);
        GPUView::Strip();
        ImGui::TextColored(EditorColors::TextMuted(), "%d frames", GPUHistory.Num());

        const float  Height   = 52.0f;
        const ImVec2 Min      = ImGui::GetCursorScreenPos();
        const float  Width    = ImGui::GetContentRegionAvail().x;
        const ImVec2 Max(Min.x + Width, Min.y + Height);

        ImGui::InvisibleButton("##gpuhistory", ImVec2(Width, Height));
        const bool bHovered = ImGui::IsItemHovered();

        ImDrawList* DL = ImGui::GetWindowDrawList();
        DL->AddRectFilled(Min, Max, EditorColors::U32(EditorColors::WithAlpha(EditorColors::PanelBg(), 0.65f)), 4.0f);

        auto YOf = [&](double Ms) { return Max.y - 2.0f - (float)(Ms / Ceiling) * (Height - 4.0f); };

        const int32 Num  = GPUHistory.Num();
        const float BarW = Width / (float)FGPUProfiler::FrameHistorySize;
        const float X0   = Max.x - BarW * Num;
        int32 HoveredBar = Constants::kIndexNone;
        for (int32 i = 0; i < Num; ++i)
        {
            const float  Value = GPUHistory[i];
            const float  BX0   = X0 + BarW * i;
            const ImVec4 Color = GPUView::BudgetColor(Value / BudgetMs);
            const bool   bBarHovered = bHovered && ImGui::GetIO().MousePos.x >= BX0 && ImGui::GetIO().MousePos.x < BX0 + BarW;
            if (bBarHovered)
            {
                HoveredBar = i;
            }
            DL->AddRectFilled(ImVec2(BX0, YOf(Value)), ImVec2(Math::Max(BX0 + BarW - 1.0f, BX0 + 1.0f), Max.y - 2.0f),
                EditorColors::U32(EditorColors::WithAlpha(Color, bBarHovered ? 1.0f : 0.70f)));
        }

        const float AvgY = YOf(Average);
        DL->AddLine(ImVec2(Min.x, AvgY), ImVec2(Max.x, AvgY), EditorColors::U32(EditorColors::WithAlpha(EditorColors::TextDim(), 0.55f)), 1.0f);

        if (BudgetMs < Ceiling)
        {
            const float BudgetY = YOf(BudgetMs);
            DL->AddLine(ImVec2(Min.x, BudgetY), ImVec2(Max.x, BudgetY), EditorColors::U32(EditorColors::WithAlpha(EditorColors::Danger(), 0.8f)), 1.5f);
            DL->AddText(ImVec2(Min.x + 6.0f, BudgetY - ImGui::GetTextLineHeight() - 1.0f), EditorColors::U32(EditorColors::Danger()), "budget");
        }

        if (HoveredBar != Constants::kIndexNone)
        {
            ImGui::BeginTooltip();
            ImGui::TextColored(GPUView::BudgetColor(GPUHistory[HoveredBar] / BudgetMs), "%.3f ms", GPUHistory[HoveredBar]);
            ImGui::TextColored(EditorColors::TextMuted(), "%d frames ago", Num - 1 - HoveredBar);
            ImGui::EndTooltip();
        }
    }

    void FProfilerEditorTool::DrawGPUTimeline()
    {
        const double FrameMs = GPUFrame.TotalMs;
        if (FrameMs <= 0.0)
        {
            ImGui::TextColored(EditorColors::TextMuted(), "The frame resolved with no measurable GPU work.");
            return;
        }

        if (GPUViewDurMs <= 0.0 || GPUViewDurMs > FrameMs)
        {
            GPUViewStartMs = 0.0;
            GPUViewDurMs   = FrameMs;
        }
        GPUViewStartMs = Math::Clamp(GPUViewStartMs, 0.0, FrameMs - GPUViewDurMs);

        int32 MaxDepth[kGPUProfileQueueCount];
        for (int32& Depth : MaxDepth)
        {
            Depth = Constants::kIndexNone;
        }
        for (const FGPUProfileScope& Scope : GPUFrame.Scopes)
        {
            MaxDepth[(uint32)Scope.Queue] = Math::Max(MaxDepth[(uint32)Scope.Queue], Scope.Depth);
        }

        // Derived from the font rather than a fixed pixel count, so a label always fits at any DPI.
        const float TextHeight = ImGui::GetTextLineHeight();
        const float RowH       = ProfilerView::RowHeight(GPURowHeight);
        const float RulerH     = TextHeight + 8.0f;
        const float HeaderH    = TextHeight + 8.0f;

        float LaneTop[kGPUProfileQueueCount] = {};
        float ContentH = RulerH;
        for (RHI::EQueueType Queue : GPUView::kLaneOrder)
        {
            if (MaxDepth[(uint32)Queue] == Constants::kIndexNone)
            {
                continue;
            }
            LaneTop[(uint32)Queue] = ContentH + HeaderH;
            ContentH += HeaderH + (MaxDepth[(uint32)Queue] + 1) * RowH + 6.0f;
        }

        const float ChildH = Math::Min(ContentH + 10.0f, Math::Max(180.0f, ImGui::GetContentRegionAvail().y * 0.5f));
        ImGui::BeginChild("##gputimeline", ImVec2(0.0f, ChildH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollWithMouse);

        ImDrawList* DL     = ImGui::GetWindowDrawList();
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const float  Width  = Math::Max(ImGui::GetContentRegionAvail().x, 1.0f);
        const ImGuiIO& IO   = ImGui::GetIO();

        ImGui::InvisibleButton("##canvas", ImVec2(Width, ContentH));
        const bool bCanvasHovered = ImGui::IsItemHovered();
        const bool bCanvasActive  = ImGui::IsItemActive();

        // Wheel zooms around the cursor, a drag pans, and a double-click fits the whole frame.
        if (bCanvasHovered && IO.MouseWheel != 0.0f)
        {
            const double MouseFrac = Math::Clamp((double)(IO.MousePos.x - Origin.x) / Width, 0.0, 1.0);
            const double MouseMs   = GPUViewStartMs + MouseFrac * GPUViewDurMs;
            const double NewDur    = Math::Clamp(GPUViewDurMs * std::pow(0.8, (double)IO.MouseWheel), Math::Min(GPUView::kMinViewMs, FrameMs), FrameMs);
            GPUViewStartMs = Math::Clamp(MouseMs - MouseFrac * NewDur, 0.0, FrameMs - NewDur);
            GPUViewDurMs   = NewDur;
        }
        if (bCanvasActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
        {
            GPUViewStartMs = Math::Clamp(GPUViewStartMs - (double)IO.MouseDelta.x / Width * GPUViewDurMs, 0.0, FrameMs - GPUViewDurMs);
        }
        if (bCanvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            GPUViewStartMs = 0.0;
            GPUViewDurMs   = FrameMs;
        }

        const double ViewT0  = GPUViewStartMs;
        const double ViewDur = GPUViewDurMs;
        auto XOf = [&](double Ms) { return Origin.x + (float)((Ms - ViewT0) / ViewDur) * Width; };

        DL->PushClipRect(Origin, ImVec2(Origin.x + Width, Origin.y + ContentH), true);

        // Ruler with grid lines that run down through every lane.
        DL->AddRectFilled(Origin, ImVec2(Origin.x + Width, Origin.y + RulerH), EditorColors::U32(EditorColors::WithAlpha(EditorColors::PanelBg(), 0.8f)));
        const double Step     = GPUView::NiceStep(ViewDur, Width / 110.0);
        const int32  Decimals = GPUView::StepDecimals(Step);
        for (double T = std::ceil(ViewT0 / Step) * Step; T <= ViewT0 + ViewDur + 1e-9; T += Step)
        {
            const float X = XOf(T);
            DL->AddLine(ImVec2(X, Origin.y + RulerH - 5.0f), ImVec2(X, Origin.y + RulerH), EditorColors::U32(EditorColors::TextMuted()));
            DL->AddLine(ImVec2(X, Origin.y + RulerH), ImVec2(X, Origin.y + ContentH), EditorColors::U32(EditorColors::WithAlpha(EditorColors::Border(), 0.35f)));
            char Tick[32];
            snprintf(Tick, sizeof(Tick), "%.*f ms", Decimals, T);
            DL->AddText(ImVec2(X + 3.0f, Origin.y + 3.0f), EditorColors::U32(EditorColors::TextMuted()), Tick);
        }

        const double BudgetMs = GetGPUBudgetMs();
        if (BudgetMs >= ViewT0 && BudgetMs <= ViewT0 + ViewDur)
        {
            const float X = XOf(BudgetMs);
            DL->AddLine(ImVec2(X, Origin.y), ImVec2(X, Origin.y + ContentH), EditorColors::U32(EditorColors::WithAlpha(EditorColors::Danger(), 0.8f)), 1.5f);
        }

        // Lane headers, with graphics idle stretches shaded, since an idle async queue is normal but a graphics bubble is lost time.
        for (RHI::EQueueType Queue : GPUView::kLaneOrder)
        {
            if (MaxDepth[(uint32)Queue] == Constants::kIndexNone)
            {
                continue;
            }
            const float HeaderY = Origin.y + LaneTop[(uint32)Queue] - HeaderH;
            const ImVec4 Color  = GPUView::QueueColor(Queue);
            DL->AddRectFilled(ImVec2(Origin.x, HeaderY), ImVec2(Origin.x + Width, HeaderY + HeaderH - 2.0f), EditorColors::U32(EditorColors::WithAlpha(Color, 0.10f)));

            double Cursor = 0.0;
            auto ShadeIdle = [&](double From, double To)
            {
                if (Queue != RHI::EQueueType::Graphics || To - From <= 0.0)
                {
                    return;
                }
                DL->AddRectFilled(ImVec2(XOf(From), HeaderY + HeaderH - 5.0f), ImVec2(Math::Max(XOf(To), XOf(From) + 1.0f), HeaderY + HeaderH - 2.0f),
                    EditorColors::U32(EditorColors::WithAlpha(EditorColors::Danger(), 0.65f)));
            };
            for (const FGPUProfileScope& Scope : GPUFrame.Scopes)
            {
                if (Scope.Queue != Queue || Scope.ParentIndex != Constants::kIndexNone)
                {
                    continue;
                }
                ShadeIdle(Cursor, Scope.StartMs);
                Cursor = Math::Max(Cursor, Scope.EndMs);
            }
            ShadeIdle(Cursor, FrameMs);

            char Label[96];
            snprintf(Label, sizeof(Label), "%s   %.2f ms busy", GPUProfileQueueName(Queue), GPUFrame.QueueBusyMs[(uint32)Queue]);
            DL->AddText(ImVec2(Origin.x + 6.0f, HeaderY + 3.0f), EditorColors::U32(Color), Label);
        }

        int32 HoveredIndex = Constants::kIndexNone;
        const bool bAnyHighlight = !SelectedPass.IsEmpty() || GPUFilter[0] != '\0';

        for (int32 Index = 0; Index < GPUFrame.Scopes.Num(); ++Index)
        {
            const FGPUProfileScope& Scope = GPUFrame.Scopes[Index];
            if (Scope.EndMs < ViewT0 || Scope.StartMs > ViewT0 + ViewDur)
            {
                continue;
            }

            const float X0 = Math::Max(XOf(Scope.StartMs), Origin.x);
            const float X1 = Math::Min(XOf(Scope.EndMs), Origin.x + Width);
            const float Y  = Origin.y + LaneTop[(uint32)Scope.Queue] + Scope.Depth * RowH;

            // A sub-pixel pass still gets a visible sliver rather than vanishing from the graph.
            const ImVec2 Min(X0, Y + 1.0f);
            const ImVec2 Max(Math::Max(X1, X0 + 2.0f), Y + RowH - 1.0f);

            const bool bHover = bCanvasHovered && IO.MousePos.x >= Min.x && IO.MousePos.x <= Max.x && IO.MousePos.y >= Min.y && IO.MousePos.y <= Max.y;
            if (bHover)
            {
                HoveredIndex = Index;
            }

            const bool  bLit = !bAnyHighlight || IsGPUScopeHighlighted(Index);
            const ImU32 Base = GPUScopeColor(Index);
            ImU32 Fill = bHover ? ProfilerView::DimColor(Base, 1.08f) : Base;
            if (!bLit)
            {
                Fill = ProfilerView::DimColor(Fill, 0.38f);
            }
            DL->AddRectFilled(Min, Max, Fill, 2.0f);

            if (Index == SelectedScope || (bHover && bLit))
            {
                DL->AddRect(Min, Max, IM_COL32(255, 255, 255, Index == SelectedScope ? 235 : 140), 2.0f, 1.5f);
            }

            const float BarW = Max.x - Min.x;
            if (BarW > 24.0f)
            {
                char Text[300];
                char Ms[32];
                GPUView::FormatMs(Ms, sizeof(Ms), Scope.DurationMs());
                snprintf(Text, sizeof(Text), "%s  %s", *Scope.Name, Ms);
                const char* Shown = ImGui::CalcTextSize(Text).x + 10.0f <= BarW ? Text : *Scope.Name;

                const float TextY = Min.y + ((Max.y - Min.y) - TextHeight) * 0.5f;
                DL->PushClipRect(Min, Max, true);
                DL->AddText(ImVec2(Min.x + 5.0f, TextY), bLit ? GPUView::LabelColorOn(Fill) : IM_COL32(150, 150, 155, 200), Shown);
                DL->PopClipRect();
            }
        }

        DL->PopClipRect();

        if (HoveredIndex != Constants::kIndexNone && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
        {
            DrawGPUScopeTooltip(HoveredIndex);
        }

        // A release after almost no movement is a click; anything longer was a pan.
        if (bCanvasHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && IO.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 9.0f)
        {
            if (HoveredIndex == Constants::kIndexNone || HoveredIndex == SelectedScope)
            {
                SelectedScope = Constants::kIndexNone;
                SelectedPass.Reset();
            }
            else
            {
                SelectedScope = HoveredIndex;
                SelectedPass  = GPUFrame.Scopes[HoveredIndex].Name;
            }
        }

        ImGui::EndChild();

        ImGui::TextColored(EditorColors::TextMuted(), "Wheel zooms, drag pans, double-click fits the frame. Showing %.3f to %.3f ms.", ViewT0, ViewT0 + ViewDur);
    }

    void FProfilerEditorTool::DrawGPUScopeTooltip(int32 ScopeIndex)
    {
        const FGPUProfileScope& Scope = GPUFrame.Scopes[ScopeIndex];
        const double FrameMs = Math::Max(GPUFrame.TotalMs, 1e-6);

        int32 NumChildren = 0;
        for (const FGPUProfileScope& Other : GPUFrame.Scopes)
        {
            NumChildren += Other.ParentIndex == ScopeIndex ? 1 : 0;
        }

        ImGui::BeginTooltip();
        const ImVec2 Swatch = ImGui::GetCursorScreenPos();
        const float  Size   = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(Swatch, ImVec2(Swatch.x + Size, Swatch.y + Size), GPUScopeColor(ScopeIndex), 3.0f);
        ImGui::Dummy(ImVec2(Size, Size));
        ImGui::SameLine();
        ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
        ImGui::TextUnformatted(*Scope.Name);
        ImGuiX::Font::PopFont();
        ImGui::Separator();

        if (ImGui::BeginTable("##gputip", 2, ImGuiTableFlags_SizingFixedFit))
        {
            auto Row = [](const char* Label)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(EditorColors::TextMuted(), "%s", Label);
                ImGui::TableNextColumn();
            };

            Row("Total");
            ImGui::TextColored(GPUView::CostColor(Scope.DurationMs() / FrameMs), "%.3f ms, %.1f%% of the frame", Scope.DurationMs(), Scope.DurationMs() / FrameMs * 100.0);

            if (NumChildren > 0)
            {
                Row("Self");
                const bool bUnmarked = GPUView::HasUnmarkedTime(Scope, true);
                ImGui::TextColored(bUnmarked ? EditorColors::Warning() : EditorColors::TextPrimary(), "%.3f ms outside its %d children", Scope.SelfMs, NumChildren);
            }

            if (Scope.ParentIndex != Constants::kIndexNone)
            {
                const FGPUProfileScope& Parent = GPUFrame.Scopes[Scope.ParentIndex];
                Row("Parent");
                ImGui::Text("%s, %.0f%% of it", *Parent.Name, Parent.DurationMs() > 0.0 ? Scope.DurationMs() / Parent.DurationMs() * 100.0 : 0.0);
            }

            Row("Queue");
            ImGui::TextColored(GPUView::QueueColor(Scope.Queue), "%s", GPUProfileQueueName(Scope.Queue));
            Row("Runs");
            ImGui::Text("%.3f to %.3f ms", Scope.StartMs, Scope.EndMs);

            if (const FGPUPassStats* Stats = GPUPassStats.Find(FName(*Scope.Name)))
            {
                Row(Stats->Calls > 1 ? "All of name" : "History");
                ImGui::Text("avg %.3f, peak %.3f ms over %d frames", Stats->AverageMs(), Stats->PeakMs(), Stats->Count);
                if (Stats->Calls > 1)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(EditorColors::TextMuted(), "(%d passes this frame)", Stats->Calls);
                }
            }
            ImGui::EndTable();
        }

        if (GPUView::HasUnmarkedTime(Scope, NumChildren > 0))
        {
            ImGui::Spacing();
            ImGui::TextColored(EditorColors::Warning(), LE_ICON_ALERT " %.0f%% of this pass has no marker of its own.", Scope.SelfMs / Math::Max(Scope.DurationMs(), 1e-6) * 100.0);
        }
        ImGui::EndTooltip();
    }

    void FProfilerEditorTool::DrawGPUPassTree()
    {
        const int32 Num = GPUFrame.Scopes.Num();
        TVector<TVector<int32>> Children;
        Children.SetNum(Num);
        TVector<bool> SubtreeMatches;
        SubtreeMatches.SetNum(Num);

        for (int32 i = 0; i < Num; ++i)
        {
            const FGPUProfileScope& Scope = GPUFrame.Scopes[i];
            SubtreeMatches[i] = GPUView::ContainsNoCase(*Scope.Name, GPUFilter);
            if (Scope.ParentIndex != Constants::kIndexNone)
            {
                Children[Scope.ParentIndex].Add(i);
            }
        }
        // Parents sort ahead of their children, so one reverse pass carries a match up to every ancestor.
        for (int32 i = Num - 1; i >= 0; --i)
        {
            const int32 Parent = GPUFrame.Scopes[i].ParentIndex;
            if (SubtreeMatches[i] && Parent != Constants::kIndexNone)
            {
                SubtreeMatches[Parent] = true;
            }
        }

        const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY
            | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;

        if (!ImGui::BeginTable("##gputree", 6, Flags, ImVec2(0.0f, Math::Max(ImGui::GetContentRegionAvail().y, 120.0f))))
        {
            return;
        }

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Pass",    ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Total",   ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Self",    ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Frame",   ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Avg",     ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("History", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        for (RHI::EQueueType Queue : GPUView::kLaneOrder)
        {
            if (!GPUFrame.HasQueue(Queue))
            {
                continue;
            }

            ImGui::TableNextRow(ImGuiTableRowFlags_None);
            ImGui::TableNextColumn();
            ImGui::TextColored(GPUView::QueueColor(Queue), "%s", GPUProfileQueueName(Queue));
            ImGui::TableNextColumn();
            GPUView::TextMs(GPUView::QueueColor(Queue), GPUFrame.QueueBusyMs[(uint32)Queue]);

            for (int32 i = 0; i < Num; ++i)
            {
                const FGPUProfileScope& Scope = GPUFrame.Scopes[i];
                if (Scope.Queue == Queue && Scope.ParentIndex == Constants::kIndexNone)
                {
                    DrawGPUTreeNode(i, Children, SubtreeMatches);
                }
            }
        }

        ImGui::EndTable();
    }

    void FProfilerEditorTool::DrawGPUTreeNode(int32 ScopeIndex, const TVector<TVector<int32>>& Children, const TVector<bool>& SubtreeMatches)
    {
        if (!SubtreeMatches[ScopeIndex])
        {
            return;
        }

        const FGPUProfileScope& Scope = GPUFrame.Scopes[ScopeIndex];
        const double FrameMs   = Math::Max(GPUFrame.TotalMs, 1e-6);
        const double Share     = Scope.DurationMs() / FrameMs;
        const bool bHasChildren = !Children[ScopeIndex].IsEmpty();
        const bool bUnmarked    = bGPUShowUnmarked && GPUView::HasUnmarkedTime(Scope, bHasChildren);
        const bool bFiltering   = GPUFilter[0] != '\0';

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(ScopeIndex);

        ImGuiTreeNodeFlags NodeFlags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_OpenOnArrow;
        if (!bHasChildren && !bUnmarked)
        {
            NodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        if (Scope.Depth < 1 || bFiltering)
        {
            NodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
        }
        if (!SelectedPass.IsEmpty() && Scope.Name == SelectedPass)
        {
            NodeFlags |= ImGuiTreeNodeFlags_Selected;
        }
        if (bFiltering)
        {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }

        // A stripe in the pass's timeline color, so a row and its bar are easy to match up.
        const ImVec2 Stripe = ImGui::GetCursorScreenPos();
        const float  RowH   = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(Stripe.x - 5.0f, Stripe.y + 1.0f), ImVec2(Stripe.x - 2.0f, Stripe.y + RowH - 1.0f), GPUScopeColor(ScopeIndex), 1.0f);

        const bool bOpen = ImGui::TreeNodeEx("##node", NodeFlags, "%s", *Scope.Name);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        {
            SelectedScope = ScopeIndex;
            SelectedPass  = Scope.Name;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                const double Pad = Math::Max(Scope.DurationMs() * 0.1, GPUView::kMinViewMs);
                GPUViewDurMs   = Math::Min(Scope.DurationMs() + Pad * 2.0, GPUFrame.TotalMs);
                GPUViewStartMs = Scope.StartMs - Pad;
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            DrawGPUScopeTooltip(ScopeIndex);
        }

        ImGui::TableNextColumn();
        GPUView::TextMs(GPUView::CostColor(Share), Scope.DurationMs());

        ImGui::TableNextColumn();
        if (bHasChildren)
        {
            GPUView::TextMs(GPUView::HasUnmarkedTime(Scope, true) ? EditorColors::Warning() : EditorColors::TextDim(), Scope.SelfMs);
        }
        else
        {
            GPUView::TextMs(EditorColors::TextDim(), Scope.SelfMs);
        }

        ImGui::TableNextColumn();
        GPUView::ShareBar(Share, GPUView::CostColor(Share));

        const FGPUPassStats* Stats = GPUPassStats.Find(FName(*Scope.Name));
        ImGui::TableNextColumn();
        if (Stats != nullptr && Stats->Calls == 1)
        {
            GPUView::TextMs(EditorColors::TextDim(), Stats->AverageMs());
        }
        else if (Stats != nullptr)
        {
            ImGui::TextColored(EditorColors::TextMuted(), "x%d", Stats->Calls);
            ImGui::SetItemTooltip("%d passes share this name; the By name view sums them.", Stats->Calls);
        }

        ImGui::TableNextColumn();
        if (Stats != nullptr && Stats->Count > 1)
        {
            const int32 Offset = Stats->Count == kGPUPassHistory ? Stats->Head : 0;
            GPUView::Sparkline(Stats->History, Stats->Count, Offset, Stats->PeakMs(), GPUView::CostColor(Share));
        }

        if (bOpen && (bHasChildren || bUnmarked))
        {
            for (int32 Child : Children[ScopeIndex])
            {
                DrawGPUTreeNode(Child, Children, SubtreeMatches);
            }

            if (bUnmarked)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
                ImGui::TextColored(EditorColors::Warning(), LE_ICON_ALERT_OUTLINE " no marker");
                ImGui::SetItemTooltip("Time inside %s that none of its children cover. Add a scope around that work to see what it is.", *Scope.Name);
                ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
                ImGui::TableNextColumn();
                GPUView::TextMs(EditorColors::Warning(), Scope.SelfMs);
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                GPUView::ShareBar(Scope.SelfMs / FrameMs, EditorColors::Warning());
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    void FProfilerEditorTool::DrawGPUPassTable()
    {
        struct FRow
        {
            const FName*         Name;
            const FGPUPassStats* Stats;
            FString              Label;
        };

        TVector<FRow> Rows;
        Rows.Reserve(GPUPassStats.Num());
        for (const auto& [Name, Stats] : GPUPassStats)
        {
            if (Stats.LastFrame != GPUStatsFrame)
            {
                continue;
            }
            FString Label = Name.ToString();
            if (!GPUView::ContainsNoCase(*Label, GPUFilter))
            {
                continue;
            }
            Rows.Add(FRow{ &Name, &Stats, Move(Label) });
        }

        const ImGuiTableFlags Flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
            | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;

        if (!ImGui::BeginTable("##gpuflat", 8, Flags, ImVec2(0.0f, Math::Max(ImGui::GetContentRegionAvail().y, 120.0f))))
        {
            return;
        }

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Pass",    ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Calls",   ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Last",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 72.0f);
        ImGui::TableSetupColumn("Self",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 72.0f);
        ImGui::TableSetupColumn("Avg",     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_DefaultSort, 72.0f);
        ImGui::TableSetupColumn("Peak",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 72.0f);
        ImGui::TableSetupColumn("Frame",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 110.0f);
        ImGui::TableSetupColumn("History", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 1.0f);
        ImGui::TableHeadersRow();

        int32 SortColumn = 4;
        bool  bAscending = false;
        if (ImGuiTableSortSpecs* Specs = ImGui::TableGetSortSpecs())
        {
            if (Specs->SpecsCount > 0)
            {
                SortColumn = Specs->Specs[0].ColumnIndex;
                bAscending = Specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            }
        }

        Algo::Sort(Rows, [&](const FRow& A, const FRow& B)
        {
            double Cmp;
            switch (SortColumn)
            {
            case 0:  Cmp = (double)A.Label.Compare(B.Label); break;
            case 1:  Cmp = (double)A.Stats->Calls - (double)B.Stats->Calls; break;
            case 2:  Cmp = A.Stats->LastMs - B.Stats->LastMs; break;
            case 3:  Cmp = A.Stats->AverageSelfMs() - B.Stats->AverageSelfMs(); break;
            case 5:  Cmp = A.Stats->PeakMs() - B.Stats->PeakMs(); break;
            default: Cmp = A.Stats->AverageMs() - B.Stats->AverageMs(); break;
            }
            return bAscending ? (Cmp < 0.0) : (Cmp > 0.0);
        });

        const double FrameMs = Math::Max(GPUFrame.TotalMs, 1e-6);
        for (const FRow& Row : Rows)
        {
            const FGPUPassStats& Stats = *Row.Stats;
            const double Avg   = Stats.AverageMs();
            const double Share = Avg / FrameMs;
            const ImVec4 Color = GPUView::CostColor(Share);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(Row.Name);
            const bool bSelected = !SelectedPass.IsEmpty() && SelectedPass == *Row.Label;
            if (ImGui::Selectable(*Row.Label, bSelected, ImGuiSelectableFlags_SpanAllColumns))
            {
                if (bSelected)
                {
                    SelectedPass.Reset();
                }
                else
                {
                    SelectedPass = *Row.Label;
                }
                SelectedScope = Constants::kIndexNone;
            }
            ImGui::SetItemTooltip("%s queue, depth %d. Click to highlight every pass of this name in the timeline.",
                GPUProfileQueueName((RHI::EQueueType)Stats.Queue), Stats.Depth);

            ImGui::TableNextColumn();
            ImGui::TextColored(Stats.Calls > 1 ? EditorColors::TextPrimary() : EditorColors::TextMuted(), "%d", Stats.Calls);
            ImGui::TableNextColumn();
            GPUView::TextMs(GPUView::CostColor(Stats.LastMs / FrameMs), Stats.LastMs);
            ImGui::TableNextColumn();
            GPUView::TextMs(EditorColors::TextDim(), Stats.AverageSelfMs());
            ImGui::TableNextColumn();
            GPUView::TextMs(Color, Avg);
            ImGui::TableNextColumn();
            GPUView::TextMs(EditorColors::TextDim(), Stats.PeakMs());
            ImGui::TableNextColumn();
            GPUView::ShareBar(Share, Color);

            ImGui::TableNextColumn();
            if (Stats.Count > 1)
            {
                const int32 Offset = Stats.Count == kGPUPassHistory ? Stats.Head : 0;
                GPUView::Sparkline(Stats.History, Stats.Count, Offset, Stats.PeakMs(), Color);
            }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

#endif
