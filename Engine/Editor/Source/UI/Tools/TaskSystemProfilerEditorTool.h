#pragma once
#include "EditorTool.h"
#include "Containers/Vector.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
#include "TaskSystem/Scheduler/JobProfiler.h"

namespace Lumina
{
    // Visualizes the fiber job system: a live fiber-state grid, per-worker / per-fiber timeline, the
    // wait/counter graph and pool stats. Span recording is gated on task.Profiler.Enabled (zero cost
    // off); the fiber grid + dashboard are live regardless. Editor-only.
    class FTaskSystemProfilerEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FTaskSystemProfilerEditorTool)

        FTaskSystemProfilerEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Task System", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CHART_TIMELINE; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

    private:

        void DrawWindow(bool bIsFocused);
        void DrawAdvisor();
        void DrawDashboard();
        void DrawCores();
        void DrawFiberGrid();
        void DrawTimeline();
        void DrawCounters();

        // Display copies, refreshed each frame only while not frozen, so Freeze holds a stable frame to
        // inspect (hover spans / read the grid without it changing underneath you).
        FJobProfFrame                  DisplayFrame;
        TVector<Jobs::FFiberState>     FiberStates;
        TVector<Jobs::FCounterState>   Counters;
        TVector<Jobs::FWorkerCoreState> WorkerCores;

        bool   bFrozen   = false;
        bool   bByFiber  = false;
        uint32 DrawTicks = 0;     // ++ each DrawWindow call; drives the liveness heartbeat
        float ZoomT     = 1.0f;   // fraction of the captured frame window shown
        float PanT      = 0.0f;   // 0..1 scroll within the window
        float RowHeight = 16.0f;
    };
}
