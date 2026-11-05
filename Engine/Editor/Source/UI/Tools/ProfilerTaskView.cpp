#include "ProfilerEditorTool.h"
#include "ProfilerViewCommon.h"

#include "imgui.h"
#include "implot.h"
#include "Core/Console/ConsoleVariable.h"
#include "TaskSystem/Fork/ForkRuntime.h"
#include "TaskSystem/Scheduler/JobProfiler.h"

namespace Lumina
{
    namespace
    {
        void StatRow(const char* Label, const char* Fmt, ...)
        {
            ImGui::TableNextColumn(); ImGui::TextUnformatted(Label);
            ImGui::TableNextColumn();
            va_list Args; va_start(Args, Fmt);
            char Buf[64]; vsnprintf(Buf, sizeof(Buf), Fmt, Args);
            va_end(Args);
            ImGui::TextUnformatted(Buf);
        }

        void MiniPlot(const char* Id, const TVector<float>& H, const ImVec4& Col)
        {
            if (H.IsEmpty()) { ImGui::Dummy(ImVec2(-1, 50)); return; }
            float MaxV = 1.0f;
            for (float V : H) MaxV = Math::Max(MaxV, V);
            if (ImPlot::BeginPlot(Id, ImVec2(-1, 60), ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend))
            {
                ImPlot::SetupAxes(nullptr, nullptr,
                    ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, MaxV * 1.15f, ImGuiCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, (double)H.Num(), ImGuiCond_Always);
                ImPlot::SetNextLineStyle(Col, 1.5f);
                ImPlot::SetNextFillStyle(ImVec4(Col.x, Col.y, Col.z, 0.20f), 1.0f);
                ImPlot::PlotShaded(Id, H.GetData(), H.Num(), 0.0);
                ImPlot::PlotLine(Id, H.GetData(), H.Num());
                ImPlot::EndPlot();
            }
        }

        void SlotLabel(uint32 Slot, char (&Out)[16])
        {
            snprintf(Out, sizeof(Out), Slot < Jobs::GetNumWorkers() ? "W%u" : "X%u", Slot);
        }
    }

    void FProfilerEditorTool::DrawTasks()
    {
        FConsoleVariable* CVar = FConsoleRegistry::Get().Find("task.Profiler.Enabled");
        bool bEnabled = (CVar != nullptr) ? Containers::Get<bool>(*CVar->ValuePtr) : false;
        if (ImGui::Checkbox("Record spans (task.Profiler.Enabled)", &bEnabled) && CVar != nullptr)
        {
            FConsoleRegistry::Get().SetAs<bool>("task.Profiler.Enabled", bEnabled);
        }

        if (!bFrozen)
        {
            TaskFrame = FJobProfiler::Get().GetLatest();
            Jobs::GetRuntimeStats(TaskStats);
        }

        ImGui::Separator();
        DrawTaskStats();

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Timeline", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawTaskTimeline();
        }
    }

    void FProfilerEditorTool::DrawTaskStats()
    {
        const FJobProfFrame&       F = TaskFrame;
        const Jobs::FRuntimeStats& S = TaskStats;

        const uint32 Submits    = F.WorkerJobs + F.ExternalJobs;
        const float  WorkerFrac = Submits ? (float)F.WorkerJobs / Submits : 0.0f;

        ImGui::Columns(2, "##dashcols", false);

        if (ImGui::BeginTable("##live", 2, ImGuiTableFlags_SizingStretchProp))
        {
            StatRow("Workers",        "%u", Jobs::GetNumWorkers());
            StatRow("Sleeping",       "%u / %u", S.Sleepers, Jobs::GetNumWorkers());
            StatRow("Jobs / frame",   "%u", F.JobsRun);
            StatRow("Run helping",    "%u", F.JobsHelped);
            StatRow("Submits W / X",  "%u / %u  (%.0f%% worker)", F.WorkerJobs, F.ExternalJobs, WorkerFrac * 100.0f);
            StatRow("Roots H/N/L/B",  "%u / %u / %u / %u", S.QueuedRoots[0], S.QueuedRoots[1], S.QueuedRoots[2], S.QueuedRoots[3]);
            StatRow("Forks H/N/L/B",  "%u / %u / %u / %u", S.QueuedForks[0], S.QueuedForks[1], S.QueuedForks[2], S.QueuedForks[3]);
            StatRow("Live roots",     "%u", S.LiveRoots);
            ImGui::EndTable();
        }

        ImGui::NextColumn();
        FJobProfiler& P = FJobProfiler::Get();
        ImGui::TextDisabled("Jobs / frame");
        MiniPlot("##jobs", P.JobsHistory(), ImVec4(0.40f, 0.75f, 0.95f, 1.0f));
        ImGui::TextDisabled("Jobs run while helping / frame");
        MiniPlot("##helped", P.HelpedHistory(), ImVec4(0.90f, 0.65f, 0.30f, 1.0f));

        ImGui::Columns(1);
    }

    void FProfilerEditorTool::DrawTaskTimeline()
    {
        const FJobProfFrame& F = TaskFrame;
        const double T0 = F.FrameStartMs;
        const double T1 = F.FrameEndMs;
        if (T1 <= T0 || F.Spans.IsEmpty())
        {
            ImGui::TextDisabled("Enable span recording to capture the per-thread timeline.");
            return;
        }

        ImGui::SetNextItemWidth(160); ImGui::SliderFloat("Zoom", &ZoomT, 0.05f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160); ImGui::SliderFloat("Pan", &PanT, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::Text("Frame: %.2f ms", T1 - T0);

        const double WinDur  = T1 - T0;
        const double ViewDur = WinDur * ZoomT;
        const double ViewT0  = T0 + (double)PanT * (WinDur - ViewDur);

        const int32 Slots = (int32)Jobs::GetNumThreadSlots();
        TVector<int32> LaneCount;
        LaneCount.SetNum(Slots, 0);
        for (const FJobProfSpan& S : F.Spans)
        {
            if (S.Worker < Slots)
            {
                LaneCount[S.Worker] = Math::Max(LaneCount[S.Worker], (int32)S.Depth + 1);
            }
        }

        TVector<int32> LaneTop;
        LaneTop.SetNum(Slots, 0);
        int32 TotalLanes = 0;
        for (int32 Slot = 0; Slot < Slots; ++Slot)
        {
            LaneTop[Slot] = TotalLanes;
            TotalLanes += LaneCount[Slot];
        }
        if (TotalLanes == 0) { ImGui::TextDisabled("No spans."); return; }

        const float LabelW = 52.0f;
        const float RowH   = ProfilerView::RowHeight(RowHeight);
        const float Height = TotalLanes * RowH + 8.0f;
        ImGui::BeginChild("##timeline", ImVec2(0, Math::Min(Height + 4.0f, 360.0f)), true, ImGuiWindowFlags_HorizontalScrollbar);

        ImDrawList* DL = ImGui::GetWindowDrawList();
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const float  Width  = ImGui::GetContentRegionAvail().x - LabelW;
        const ImVec2 Mouse  = ImGui::GetIO().MousePos;
        const FJobProfSpan* Hover = nullptr;

        int32 Band = 0;
        for (int32 Slot = 0; Slot < Slots; ++Slot)
        {
            if (LaneCount[Slot] == 0) continue;
            const float Y = Origin.y + LaneTop[Slot] * RowH;
            DL->AddRectFilled(ImVec2(Origin.x, Y), ImVec2(Origin.x + LabelW + Width, Y + LaneCount[Slot] * RowH - 1),
                (Band++ & 1) ? IM_COL32(36, 38, 42, 255) : IM_COL32(30, 32, 36, 255));
            char Lbl[16];
            SlotLabel((uint32)Slot, Lbl);
            DL->AddText(ImVec2(Origin.x + 4, ProfilerView::LabelY(Y, RowH)), IM_COL32(180, 180, 185, 255), Lbl);
        }

        const float PlotX = Origin.x + LabelW;
        for (const FJobProfSpan& S : F.Spans)
        {
            if (S.Worker >= Slots) continue;
            const double A = (S.StartMs - ViewT0) / ViewDur;
            const double B = (S.EndMs   - ViewT0) / ViewDur;
            if (B < 0.0 || A > 1.0) continue;
            const float X0 = PlotX + (float)Math::Max(0.0, A) * Width;
            const float X1 = PlotX + (float)Math::Min(1.0, B) * Width;
            const float Y  = Origin.y + (LaneTop[S.Worker] + S.Depth) * RowH;
            const ImVec2 Min(X0, Y + 1), Max(Math::Max(X1, X0 + 1.0f), Y + RowH - 2);

            const bool bIdle = S.Kind == EJobSpanKind::Idle;
            DL->AddRectFilled(Min, Max, bIdle ? IM_COL32(48, 50, 54, 160) : ProfilerView::ScopeColor(S.Name), 2.0f);
            if (!bIdle && S.Name && (Max.x - Min.x) > 28.0f)
            {
                DL->PushClipRect(Min, Max, true);
                DL->AddText(ImVec2(Min.x + 3, ProfilerView::LabelY(Y, RowH)), IM_COL32(15, 15, 18, 255), S.Name);
                DL->PopClipRect();
            }
            if (Mouse.x >= Min.x && Mouse.x < Max.x && Mouse.y >= Min.y && Mouse.y < Max.y)
            {
                Hover = &S;
            }
        }

        ImGui::Dummy(ImVec2(LabelW + Width, TotalLanes * RowH));

        if (Hover && ImGui::IsWindowHovered())
        {
            char Lbl[16];
            SlotLabel(Hover->Worker, Lbl);
            ImGui::BeginTooltip();
            if (Hover->Kind == EJobSpanKind::Idle)
            {
                ImGui::TextDisabled("idle");
            }
            else
            {
                ImGui::TextUnformatted(Hover->Name ? Hover->Name : "job");
                ImGui::Text("Depth %u", (uint32)Hover->Depth);
            }
            ImGui::Text("Thread %s", Lbl);
            ImGui::Text("%.4f ms", Hover->EndMs - Hover->StartMs);
            ImGui::TextDisabled("at +%.3f ms", Hover->StartMs - T0);
            ImGui::EndTooltip();
        }

        ImGui::EndChild();
    }
}
