#pragma once

#include "EditorTool.h"
#include "Containers/Vector.h"
#include "Core/Profiler/GameplayProfiler.h"
#include "World/World.h"

namespace Lumina
{
    // A unified inspector for how the world's gameplay logic executes.
    class FGameplayInsightsEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FGameplayInsightsEditorTool)

        FGameplayInsightsEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Gameplay Insights", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_CHART_TIMELINE_VARIANT; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;

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
        void DrawSchedule();
        void DrawScheduleCanvas();
        void DrawStats();
        void DrawDetail();

        // The world whose schedule we inspect: the active gameplay (Game/Simulation) world if one is running,
        // else the editor world.
        CWorld* ResolveWorld() const;
        void    RefreshSchedule();

        // Index into Schedule for the selected system, re-found by name so a reschedule doesn't move
        // the selection onto an unrelated system. INDEX_NONE when nothing is selected.
        int32 ResolveSelection() const;

        const FGameplayProfileEntry* FindStat(const char* Name) const;

        // Display copies, refreshed each frame unless frozen, so Freeze holds a stable frame to inspect.
        FGameplayProfileFrame         DisplayFrame;   // aggregate scope timings (scripts + Sample + C# systems)
        TVector<FSystemScheduleEntry> Schedule;       // batch/access snapshot from the active world
        TVector<FScheduleColumn>      ScheduleColumns;

        char    Filter[64]      = {};
        bool    bFrozen         = false;
        bool    bShowEdges      = true;
        float   ScheduleZoom    = 1.0f;
        uint32  DrawTicks       = 0;

        int32   SelectedIndex   = INDEX_NONE;
        FName   SelectedName;           // None for a managed system; index is the only handle we have
    };
}
