#include "ProfilerEditorTool.h"
#include "ProfilerViewCommon.h"

#include "imgui.h"
#include "implot.h"
#include "Core/Console/ConsoleVariable.h"
#include "Platform/Process/PlatformProcess.h"
#include "TaskSystem/Scheduler/JobProfiler.h"

namespace Lumina
{
    namespace
    {

        ImU32 WorkerColor(uint32 Worker)
        {
            float R, G, B;
            ImGui::ColorConvertHSVtoRGB((Worker * 0.137f) - (float)(int)(Worker * 0.137f), 0.6f, 0.9f, R, G, B);
            return ImGui::ColorConvertFloat4ToU32(ImVec4(R, G, B, 1.0f));
        }

        ImU32 StateColor(Jobs::EFiberState S)
        {
            switch (S)
            {
            case Jobs::EFiberState::Free:    return IM_COL32(64, 66, 72, 255);
            case Jobs::EFiberState::Running: return IM_COL32(60, 175, 95, 255);
            case Jobs::EFiberState::Parked:  return IM_COL32(214, 162, 52, 255);
            case Jobs::EFiberState::Ready:   return IM_COL32(72, 132, 214, 255);
            }
            return IM_COL32(120, 120, 120, 255);
        }

        const char* StateName(Jobs::EFiberState S)
        {
            switch (S)
            {
            case Jobs::EFiberState::Free:    return "Free";
            case Jobs::EFiberState::Running: return "Running";
            case Jobs::EFiberState::Parked:  return "Parked";
            case Jobs::EFiberState::Ready:   return "Ready";
            }
            return "?";
        }

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
            if (H.empty()) { ImGui::Dummy(ImVec2(-1, 50)); return; }
            float MaxV = 1.0f;
            for (float V : H) MaxV = Math::Max(MaxV, V);
            if (ImPlot::BeginPlot(Id, ImVec2(-1, 60), ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend))
            {
                ImPlot::SetupAxes(nullptr, nullptr,
                    ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, MaxV * 1.15f, ImGuiCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, (double)H.size(), ImGuiCond_Always);
                ImPlot::SetNextLineStyle(Col, 1.5f);
                ImPlot::SetNextFillStyle(ImVec4(Col.x, Col.y, Col.z, 0.20f), 1.0f);
                ImPlot::PlotShaded(Id, H.data(), (int)H.size(), 0.0);
                ImPlot::PlotLine(Id, H.data(), (int)H.size());
                ImPlot::EndPlot();
            }
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
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Checkbox("By fiber", &bByFiber);

        // Refresh the display copies only while live, so Freeze holds a stable frame to inspect.
        if (!bFrozen)
        {
            TaskFrame = FJobProfiler::Get().GetLatest();
            Jobs::SnapshotFiberStates(FiberStates);
            Jobs::SnapshotActiveCounters(Counters);
            Jobs::SnapshotWorkerCores(WorkerCores);
        }

        ImGui::Separator();
        DrawDashboard();

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("CPU cores", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawCores();
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Scheduling advisor", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawAdvisor();
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Fibers", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawFiberGrid();
        }
        if (ImGui::CollapsingHeader("Timeline", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawTimeline();
        }
        if (ImGui::CollapsingHeader("Waiting counters"))
        {
            DrawCounters();
        }
    }

    void FProfilerEditorTool::DrawAdvisor()
    {
        const FJobProfFrame& F = TaskFrame;
        Jobs::FJobLiveStats LS;
        Jobs::GetLiveStats(LS);

        const uint32 Workers    = Math::Max(1u, LS.NumWorkers);
        const uint32 TotalJobs  = F.WorkerJobs + F.ExternalJobs;
        const float  WorkerFrac = TotalJobs ? (float)F.WorkerJobs / TotalJobs : 0.0f;
        const float  MigRate    = F.Resumes ? (float)F.Migrations / F.Resumes : 0.0f;
        const float  AffRate    = F.Resumes ? (float)F.AffinityOpps / F.Resumes : 0.0f;
        // High raw migration is the baseline, so only affinity opportunities are actionable.
        const float  MigBaseline = Workers > 1 ? (float)(Workers - 1) / Workers : 0.0f;

        // A handful of cache-cold resumes costs microseconds, and this also guards against tiny samples.
        constexpr uint32 kMinAffinityOpps = 16;

        const bool Contended = F.AvgPoppers >= Math::Max(2.0f, 0.5f * Workers);
        const bool ForkJoin  = TotalJobs > 0 && WorkerFrac >= 0.5f;
        const bool Migrating = F.AffinityOpps >= kMinAffinityOpps && AffRate >= 0.25f;

        ImU32       Col;
        const char* Icon;
        const char* Headline;
        char        Detail[360];

        if (TotalJobs == 0)
        {
            Col = IM_COL32(120, 122, 128, 255); Icon = LE_ICON_INFORMATION; Headline = "Awaiting data";
            snprintf(Detail, sizeof Detail,
                "Enable \"Record spans\" above and exercise a workload. The advisor then weighs shared-queue "
                "contention, fiber migration and workload shape to judge whether work-stealing would help.");
        }
        else if (F.Starvations > 0)
        {
            Col = IM_COL32(214, 72, 72, 255); Icon = LE_ICON_ALERT_OCTAGON; Headline = "Fiber pool starved";
            snprintf(Detail, sizeof Detail,
                "%u episode(s): every one of the %u fibers (of %u max) was blocked while jobs were still pending, "
                "so the pool had to grow. Not something work-stealing would fix - it means jobs are blocking inside "
                "jobs. Raise NumWorkFibers to skip the growth, or stop blocking in bulk-spawned work.",
                F.Starvations, LS.NumWorkFibers, LS.MaxWorkFibers);
        }
        else if (Contended && ForkJoin)
        {
            Col = IM_COL32(224, 156, 52, 255); Icon = LE_ICON_ALERT; Headline = "Work-stealing would likely help";
            snprintf(Detail, sizeof Detail,
                "On average %.1f of %u workers dequeue the shared queue at once (peak %u), and %.0f%% of jobs are "
                "worker-spawned (fork-join). Per-worker LIFO deques + stealing would cut that contention and keep "
                "child jobs cache-hot.", F.AvgPoppers, LS.NumWorkers, F.MaxPoppers, WorkerFrac * 100.0f);
        }
        else if (Contended)
        {
            Col = IM_COL32(180, 168, 72, 255); Icon = LE_ICON_INFORMATION; Headline = "Work-stealing: marginal";
            snprintf(Detail, sizeof Detail,
                "The shared queue is contended (avg %.1f of %u workers), but %.0f%% of jobs are submitted from "
                "external threads and reach the global queue regardless. Work-stealing mainly helps locally "
                "spawned work - limited upside until worker-spawned work dominates.",
                F.AvgPoppers, LS.NumWorkers, (1.0f - WorkerFrac) * 100.0f);
        }
        else if (Migrating)
        {
            Col = IM_COL32(72, 132, 214, 255); Icon = LE_ICON_INFORMATION; Headline = "Locality, not contention";
            snprintf(Detail, sizeof Detail,
                "The shared queue is uncontended, but %.0f%% of resumes (%u of %u) had their original worker sitting "
                "idle - affinity could have kept them on their warm core for free. Resume affinity would pay off more "
                "than work-stealing here. (Raw migration %.0f%% is near the %.0f%% baseline and not itself the issue.)",
                AffRate * 100.0f, F.AffinityOpps, F.Resumes, MigRate * 100.0f, MigBaseline * 100.0f);
        }
        else
        {
            Col = IM_COL32(60, 175, 95, 255); Icon = LE_ICON_CHECK_CIRCLE; Headline = "Scheduler healthy";
            if (F.AffinityOpps < kMinAffinityOpps)
            {
                // Too little park/resume activity for locality to matter, whatever the rate reads.
                snprintf(Detail, sizeof Detail,
                    "Shared queue is not contended (avg %.1f of %u workers at once) and only %u fiber resume(s) this "
                    "frame - jobs mostly run to completion without parking, so continuation locality is a non-issue. "
                    "Neither work-stealing nor resume affinity would pay off here.",
                    F.AvgPoppers, LS.NumWorkers, F.Resumes);
            }
            else
            {
                snprintf(Detail, sizeof Detail,
                    "Shared queue is not contended (avg %.1f of %u workers at once). Migration is %.0f%% but that is ~the "
                    "%.0f%% baseline for a shared ready queue, and only %.0f%% of resumes could have stayed local for free "
                    "- neither work-stealing nor resume affinity would pay off here.",
                    F.AvgPoppers, LS.NumWorkers, MigRate * 100.0f, MigBaseline * 100.0f, AffRate * 100.0f);
            }
        }

        // A verdict banner, a colored accent bar beside a colored headline with wrapped reasoning.
        ImGui::Indent(10.0f);
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, Col);
        ImGui::Text("%s  %s", Icon, Headline);
        ImGui::PopStyleColor();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX());
        ImGui::TextUnformatted(Detail);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        const ImVec2 Mn = ImGui::GetItemRectMin();
        const ImVec2 Mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(Mn.x - 8.0f, Mn.y), ImVec2(Mn.x - 5.0f, Mx.y), Col, 1.0f);
        ImGui::Unindent(10.0f);

        ImGui::Spacing();
        ImGui::Columns(2, "##advcols", false);
        if (ImGui::BeginTable("##advmetrics", 2, ImGuiTableFlags_SizingStretchProp))
        {
            StatRow("Workload",       "%u worker / %u external  (%.0f%% fork-join)", F.WorkerJobs, F.ExternalJobs, WorkerFrac * 100.0f);
            StatRow("Queue pressure", "avg %.1f / peak %u of %u workers", F.AvgPoppers, F.MaxPoppers, LS.NumWorkers);
            StatRow("Migration",      "%.0f%%  (~%.0f%% expected)", MigRate * 100.0f, MigBaseline * 100.0f);
            StatRow("Affinity opp.",  "%u / %u resumes  (%.0f%%)", F.AffinityOpps, F.Resumes, AffRate * 100.0f);
            StatRow("Starvation",     "%u episodes", F.Starvations);
            ImGui::EndTable();
        }
        ImGui::NextColumn();
        ImGui::TextDisabled("Shared-queue pressure (avg concurrent poppers)");
        MiniPlot("##poppers", FJobProfiler::Get().PoppersHistory(), ImVec4(0.82f, 0.48f, 0.85f, 1.0f));
        ImGui::Columns(1);
    }

    void FProfilerEditorTool::DrawDashboard()
    {
        Jobs::FJobLiveStats LS;
        Jobs::GetLiveStats(LS);
        const FJobProfFrame& F = TaskFrame;

        ImGui::Columns(2, "##dashcols", false);

        if (ImGui::BeginTable("##live", 2, ImGuiTableFlags_SizingStretchProp))
        {
            StatRow("Workers",      "%u", LS.NumWorkers);
            StatRow("Fibers total", "%u / %u", LS.NumWorkFibers, LS.MaxWorkFibers);
            StatRow("In use",       "%u", LS.FibersInUse);
            StatRow("Free",         "%u", LS.FibersFree);
            StatRow("Ready",        "%u", LS.FibersReady);
            // A deep Background queue is normal, while a deep Low queue during a hitch is not.
            StatRow("Queue H/N/L",  "%u / %u / %u", LS.QueueDepth[0], LS.QueueDepth[1], LS.QueueDepth[2]);
            StatRow("Queue Bkgd",   "%u", LS.QueueDepth[3]);
            StatRow("In flight",    "%lld", (long long)LS.InFlight);
            StatRow("Jobs / frame", "%u", F.JobsRun);
            StatRow("Parks",        "%u", F.Parks);
            StatRow("Migrations",   "%u", F.Migrations);
            ImGui::EndTable();
        }

        ImGui::NextColumn();
        FJobProfiler& P = FJobProfiler::Get();
        ImGui::TextDisabled("Jobs / frame");
        MiniPlot("##jobs", P.JobsHistory(), ImVec4(0.40f, 0.75f, 0.95f, 1.0f));
        ImGui::TextDisabled("Fibers in use");
        MiniPlot("##fibers", P.FibersInUseHistory(), ImVec4(0.40f, 0.85f, 0.50f, 1.0f));
        ImGui::TextDisabled("Migrations / frame");
        MiniPlot("##migs", P.MigrationsHistory(), ImVec4(0.90f, 0.65f, 0.30f, 1.0f));

        ImGui::Columns(1);
    }

    void FProfilerEditorTool::DrawCores()
    {
        const Platform::FCpuTopology& Topo = Platform::GetCpuTopology();
        if (Topo.NumLogicalCores == 0)
        {
            ImGui::TextDisabled("CPU topology unavailable.");
            return;
        }

        // Which worker (if any) currently runs on each logical core, plus how many landed there.
        int    BusyWorker[256];
        uint8  BusyCount[256];
        for (int i = 0; i < 256; ++i) { BusyWorker[i] = -1; BusyCount[i] = 0; }

        uint32 PActive = 0, EActive = 0;
        for (const Jobs::FWorkerCoreState& W : WorkerCores)
        {
            if (!W.bBusy || W.Core >= 256) continue;
            if (BusyCount[W.Core] == 0) BusyWorker[W.Core] = (int)W.Worker;
            ++BusyCount[W.Core];
            if (Topo.CoreTypes[W.Core] == Platform::ECpuCoreType::Efficiency) ++EActive; else ++PActive;
        }

        // Summary line.
        ImGui::Text("%u logical cores", Topo.NumLogicalCores);
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.40f, 0.62f, 0.92f, 1.0f), "%u P-core%s",
            Topo.NumPerformance, Topo.NumPerformance == 1 ? "" : "s");
        ImGui::SameLine();
        if (Topo.bHybrid)
        {
            ImGui::TextColored(ImVec4(0.86f, 0.66f, 0.28f, 1.0f), "  %u E-core%s",
                Topo.NumEfficiency, Topo.NumEfficiency == 1 ? "" : "s");
        }
        else
        {
            ImGui::TextDisabled("  (no efficiency cores)");
        }

        ImGui::TextDisabled("Workers running: %u on P / %u on E  (of %u workers)",
            PActive, EActive, (uint32)WorkerCores.size());
        // Enough work to fill the P-cores while one sits idle and an E-core runs is scheduling waste.
        if (Topo.bHybrid && PActive < Topo.NumPerformance && (PActive + EActive) >= Topo.NumPerformance)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.90f, 0.55f, 0.30f, 1.0f),
                LE_ICON_ALERT " work on E-cores while a P-core is idle");
        }

        ImGui::Spacing();

        // Legend.
        auto Swatch = [](ImU32 Col, const char* Txt)
        {
            ImGui::ColorButton(Txt, ImGui::ColorConvertU32ToFloat4(Col),
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(12, 12));
            ImGui::SameLine(); ImGui::TextUnformatted(Txt); ImGui::SameLine(0, 14);
        };
        Swatch(IM_COL32(46, 62, 86, 255),  "P-core");
        Swatch(IM_COL32(74, 66, 46, 255),  "E-core");
        Swatch(IM_COL32(60, 175, 95, 255), "running");
        ImGui::NewLine();

        // One cell per logical core, where fill encodes type and busy and the worker number shows when running.
        const float Cell  = 30.0f;
        const float Pad   = 4.0f;
        const float Avail = ImGui::GetContentRegionAvail().x;
        const int   Cols  = Math::Max(1, (int)(Avail / (Cell + Pad)));
        ImDrawList* DL     = ImGui::GetWindowDrawList();
        const ImVec2 Origin = ImGui::GetCursorScreenPos();

        int Drawn = 0;
        for (uint32 c = 0; c < 256; ++c)
        {
            const Platform::ECpuCoreType T = Topo.CoreTypes[c];
            if (T == Platform::ECpuCoreType::Unknown) continue;

            const int Col = Drawn % Cols;
            const int Row = Drawn / Cols;
            ++Drawn;

            const ImVec2 P0(Origin.x + Col * (Cell + Pad), Origin.y + Row * (Cell + Pad));
            const ImVec2 P1(P0.x + Cell, P0.y + Cell);

            const bool Busy = BusyCount[c] > 0;
            const bool IsE  = (T == Platform::ECpuCoreType::Efficiency);
            const ImU32 Fill = Busy
                ? (IsE ? IM_COL32(214, 162, 52, 255) : IM_COL32(60, 175, 95, 255))
                : (IsE ? IM_COL32(74, 66, 46, 255)   : IM_COL32(46, 62, 86, 255));
            const ImU32 Border = IsE ? IM_COL32(150, 120, 60, 255) : IM_COL32(90, 130, 190, 255);

            DL->AddRectFilled(P0, P1, Fill, 3.0f);
            DL->AddRect(P0, P1, Border, 3.0f, 0, 1.5f);

            char Label[8];
            snprintf(Label, sizeof Label, "%u", c);
            DL->AddText(ImVec2(P0.x + 3, P0.y + 2), IM_COL32(232, 232, 232, 255), Label);
            if (Busy)
            {
                char Wk[12];
                snprintf(Wk, sizeof Wk, BusyCount[c] > 1 ? "W%d+" : "W%d", BusyWorker[c]);
                DL->AddText(ImVec2(P0.x + 3, P0.y + Cell - 14), IM_COL32(16, 16, 16, 255), Wk);
            }

            if (ImGui::IsMouseHoveringRect(P0, P1))
            {
                ImGui::BeginTooltip();
                ImGui::Text("Core %u  -  %s", c, IsE ? "Efficiency (E)" : "Performance (P)");
                if (Busy) ImGui::Text("Worker %d running%s", BusyWorker[c], BusyCount[c] > 1 ? " (+others)" : "");
                else      ImGui::TextDisabled("idle");
                ImGui::EndTooltip();
            }
        }

        const int Rows = Drawn > 0 ? (Drawn + Cols - 1) / Cols : 0;
        ImGui::Dummy(ImVec2(Avail, Rows * (Cell + Pad)));
    }

    void FProfilerEditorTool::DrawFiberGrid()
    {
        if (FiberStates.empty())
        {
            ImGui::TextDisabled("No fibers (job system not initialized).");
            return;
        }

        // Legend.
        const Jobs::EFiberState Legend[] = { Jobs::EFiberState::Running, Jobs::EFiberState::Parked,
                                             Jobs::EFiberState::Ready,   Jobs::EFiberState::Free };
        for (Jobs::EFiberState S : Legend)
        {
            // desc_id must be unique per swatch or ImGui collides their IDs, key on the state name.
            ImGui::ColorButton(StateName(S), ImGui::ColorConvertU32ToFloat4(StateColor(S)),
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(12, 12));
            ImGui::SameLine(); ImGui::TextUnformatted(StateName(S)); ImGui::SameLine(0, 14);
        }
        ImGui::NewLine();

        const float Cell = 13.0f;
        const float Pad  = 2.0f;
        const float Avail = ImGui::GetContentRegionAvail().x;
        const int   Cols  = Math::Max(1, (int)(Avail / (Cell + Pad)));

        ImDrawList* DL = ImGui::GetWindowDrawList();
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const ImVec2 Mouse  = ImGui::GetIO().MousePos;
        int HoverIndex = -1;

        const int N = (int)FiberStates.size();
        const int Rows = (N + Cols - 1) / Cols;
        for (int i = 0; i < N; ++i)
        {
            const int Cx = i % Cols;
            const int Cy = i / Cols;
            const ImVec2 Min(Origin.x + Cx * (Cell + Pad), Origin.y + Cy * (Cell + Pad));
            const ImVec2 Max(Min.x + Cell, Min.y + Cell);
            const Jobs::FFiberState& FS = FiberStates[i];
            DL->AddRectFilled(Min, Max, StateColor(FS.State), 2.0f);
            if (Mouse.x >= Min.x && Mouse.x < Max.x && Mouse.y >= Min.y && Mouse.y < Max.y)
            {
                HoverIndex = i;
            }
        }

        // Reserve the laid-out area so ImGui scroll/layout accounts for it.
        ImGui::Dummy(ImVec2(Avail, Rows * (Cell + Pad)));

        if (HoverIndex >= 0)
        {
            const Jobs::FFiberState& FS = FiberStates[HoverIndex];
            ImGui::BeginTooltip();
            ImGui::Text("Fiber %u", FS.Index);
            ImGui::Text("State: %s", StateName(FS.State));
            if (FS.State == Jobs::EFiberState::Running) ImGui::Text("Worker: %u", FS.OwnerWorker);
            if (FS.State == Jobs::EFiberState::Parked)  ImGui::Text("Waiting counter: %u", FS.WaitCounterId);
            ImGui::Text("Job: %s", FS.Name ? FS.Name : "-");
            ImGui::EndTooltip();
        }
    }

    void FProfilerEditorTool::DrawTimeline()
    {
        const FJobProfFrame& F = TaskFrame;
        const double T0 = F.FrameStartMs;
        const double T1 = F.FrameEndMs;
        if (T1 <= T0 || F.Spans.empty())
        {
            ImGui::TextDisabled("Enable span recording to capture the per-worker timeline.");
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

        // Map spans → compact rows.
        const uint32 Slots = Jobs::GetNumThreadSlots();
        TVector<int> RowOf;          // index by worker or fiber → row
        const int Key = bByFiber ? 0xFFFF : (int)Slots;
        RowOf.resize((size_t)(bByFiber ? 0x10000 : Slots), -1);
        int RowCount = 0;
        auto RowFor = [&](uint32 Id) -> int
        {
            if (Id >= RowOf.size()) return -1;
            if (RowOf[Id] < 0) RowOf[Id] = RowCount++;
            return RowOf[Id];
        };
        (void)Key;
        for (const FJobProfSpan& S : F.Spans)
        {
            RowFor(bByFiber ? S.Fiber : S.Worker);
        }
        if (RowCount == 0) { ImGui::TextDisabled("No spans."); return; }

        const float LabelW = 52.0f;
        const float RowH   = ProfilerView::RowHeight(RowHeight);
        const float Height = RowCount * RowH + 8.0f;
        ImGui::BeginChild("##timeline", ImVec2(0, Math::Min(Height + 4.0f, 360.0f)), true, ImGuiWindowFlags_HorizontalScrollbar);

        ImDrawList* DL = ImGui::GetWindowDrawList();
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const float  Width  = ImGui::GetContentRegionAvail().x - LabelW;
        const ImVec2 Mouse  = ImGui::GetIO().MousePos;
        const FJobProfSpan* Hover = nullptr;

        // Row backgrounds + labels.
        for (int r = 0; r < RowCount; ++r)
        {
            const float Y = Origin.y + r * RowH;
            DL->AddRectFilled(ImVec2(Origin.x, Y), ImVec2(Origin.x + LabelW + Width, Y + RowH - 1),
                (r & 1) ? IM_COL32(36, 38, 42, 255) : IM_COL32(30, 32, 36, 255));
        }
        // Row labels need the worker/fiber id for each row; invert the map cheaply.
        for (uint32 Id = 0; Id < RowOf.size(); ++Id)
        {
            if (RowOf[Id] < 0) continue;
            const float Y = Origin.y + RowOf[Id] * RowH;
            char Lbl[16];
            snprintf(Lbl, sizeof(Lbl), bByFiber ? "F%u" : "W%u", Id);
            DL->AddText(ImVec2(Origin.x + 4, ProfilerView::LabelY(Y, RowH)), IM_COL32(180, 180, 185, 255), Lbl);
        }

        const float PlotX = Origin.x + LabelW;
        for (const FJobProfSpan& S : F.Spans)
        {
            const int Row = (RowOf[bByFiber ? S.Fiber : S.Worker]);
            if (Row < 0) continue;
            const double A = (S.StartMs - ViewT0) / ViewDur;
            const double B = (S.EndMs   - ViewT0) / ViewDur;
            if (B < 0.0 || A > 1.0) continue;
            const float X0 = PlotX + (float)Math::Max(0.0, A) * Width;
            const float X1 = PlotX + (float)Math::Min(1.0, B) * Width;
            const float Y  = Origin.y + Row * RowH;
            const ImVec2 Min(X0, Y + 1), Max(Math::Max(X1, X0 + 1.0f), Y + RowH - 2);

            ImU32 Col;
            if (S.Kind == (uint8)EJobSpanKind::Idle)       Col = IM_COL32(48, 50, 54, 160);
            else if (bByFiber)                             Col = WorkerColor(S.Worker);
            else                                           Col = ProfilerView::ScopeColor(S.Name);
            DL->AddRectFilled(Min, Max, Col, 2.0f);
            if (S.Kind == (uint8)EJobSpanKind::RanThenParked)
            {
                DL->AddRectFilled(ImVec2(Max.x - 2, Min.y), Max, IM_COL32(214, 162, 52, 255)); // amber park edge
            }
            if ((Max.x - Min.x) > 28.0f && S.Kind != (uint8)EJobSpanKind::Idle && S.Name)
            {
                DL->PushClipRect(Min, Max, true);
                DL->AddText(ImVec2(Min.x + 3, Min.y), IM_COL32(15, 15, 18, 255), S.Name);
                DL->PopClipRect();
            }
            if (Mouse.x >= Min.x && Mouse.x < Max.x && Mouse.y >= Min.y && Mouse.y < Max.y)
            {
                Hover = &S;
            }
        }

        ImGui::Dummy(ImVec2(LabelW + Width, RowCount * RowH));

        if (Hover)
        {
            ImGui::BeginTooltip();
            ImGui::Text("%s", Hover->Kind == (uint8)EJobSpanKind::Idle ? "idle" : (Hover->Name ? Hover->Name : "job"));
            ImGui::Text("Worker %u  Fiber %u", Hover->Worker, Hover->Fiber);
            ImGui::Text("%.4f ms", Hover->EndMs - Hover->StartMs);
            if (Hover->Kind == (uint8)EJobSpanKind::RanThenParked) ImGui::TextDisabled("ran, then parked");
            ImGui::EndTooltip();
        }

        ImGui::EndChild();
    }

    void FProfilerEditorTool::DrawCounters()
    {
        if (Counters.empty())
        {
            ImGui::TextDisabled("No counters with parked waiters right now.");
            return;
        }
        if (ImGui::BeginTable("##counters", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Counter");
            ImGui::TableSetupColumn("Remaining");
            ImGui::TableSetupColumn("Parked fibers");
            ImGui::TableHeadersRow();
            for (const Jobs::FCounterState& C : Counters)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("#%u", C.Id);
                ImGui::TableNextColumn(); ImGui::Text("%d", C.Value);
                ImGui::TableNextColumn(); ImGui::Text("%u", C.ParkedWaiters);
            }
            ImGui::EndTable();
        }
    }
}
