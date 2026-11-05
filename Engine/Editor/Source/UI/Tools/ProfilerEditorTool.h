#pragma once
#include "EditorTool.h"
#include "Containers/Vector.h"
#include "Core/Profiler/AssetLoadTracker.h"
#include "Core/Profiler/GameplayProfiler.h"
#include "TaskSystem/Fork/ForkRuntime.h"
#include "TaskSystem/Scheduler/JobProfiler.h"
#include "World/World.h"

#if defined(LUMINA_WITH_GPU_PROFILING)
#include "Renderer/GPUProfiler.h"
#endif

namespace Lumina
{
    class FProfilerEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FProfilerEditorTool)

        FProfilerEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Profiler", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CHART_BAR; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

    private:

        // One (stage, batch) run of Schedule, drawn as a column on the schedule canvas.
        struct FScheduleColumn
        {
            uint8 Stage = 0;
            uint8 Batch = 0;
            int32 First = 0;
            int32 Count = 0;
        };

        void DrawWindow(bool bIsFocused);

        void DrawSharedControls();

        //~ GPU

#if defined(LUMINA_WITH_GPU_PROFILING)
        void DrawGPU();
        void DrawGPUSummary();
        void DrawGPUFrameGraph();
        void DrawGPUFlameGraph();

        void SmoothTowards(const FGPUProfileFrame& Live);

        FGPUProfileFrame  GPUFrame;
        TVector<float>    GPUHistory;
        float             HistoryPeak   = 0.0f;
        float             GPURowHeight  = 30.0f;
        int32             SelectedScope = Constants::kIndexNone;
#endif

        //~ Task system

        void DrawTasks();
        void DrawTaskStats();
        void DrawTaskTimeline();

        FJobProfFrame       TaskFrame;
        Jobs::FRuntimeStats TaskStats;
        float  RowHeight = 18.0f;
        float  ZoomT     = 1.0f;
        float  PanT      = 0.0f;

        //~ Gameplay

        void DrawGameplay();
        void DrawSchedule();
        void DrawScheduleCanvas();
        void DrawStats();
        void DrawDetail();

        CWorld* ResolveWorld() const;
        void    RefreshSchedule();
        int32   ResolveSelection() const;
        const FGameplayProfileEntry* FindStat(const char* Name) const;

        FGameplayProfileFrame         GameplayFrame;
        TVector<FSystemScheduleEntry> Schedule;
        TVector<FScheduleColumn>      ScheduleColumns;
        char    Filter[64]    = {};
        bool    bShowEdges    = true;
        float   ScheduleZoom  = 1.0f;
        int32   SelectedIndex = Constants::kIndexNone;
        FName   SelectedName;

        //~ Assets

#if USING(WITH_EDITOR)
        void DrawAssets();
        void DrawAssetTable(const char* Id, TVector<FAssetLoadStat>& Stats, bool bShowSize);

        TVector<FAssetLoadRecord> AssetRecent;
        TVector<FAssetLoadStat>   AssetRequests;
        TVector<FAssetLoadStat>   AssetPackages;
        char                      AssetFilter[64] = {};
#endif

        //~ Shared across tabs

        bool   bFrozen   = false;
        // Fraction of the previous displayed value kept each frame; 0 shows the raw timings.
        float  Smoothing = 0.85f;
        uint32 DrawTicks = 0;
        // Gameplay recording is armed only while its tab is the visible one.
        bool   bGameplayRecording = false;
    };
}
