#include "ProfilerEditorTool.h"
#include "ProfilerViewCommon.h"

#include "imgui.h"
#include "Core/Profiler/GameplayProfiler.h"

namespace Lumina
{
    void FProfilerEditorTool::OnInitialize()
    {
        CreateToolWindow("Profiler", [&] (bool bIsFocused) { DrawWindow(bIsFocused); });
    }

    void FProfilerEditorTool::OnDeinitialize(const FUpdateContext&)
    {
        FGameplayProfiler::Get().SetEnabled(false);
        bGameplayRecording = false;
    }

    void FProfilerEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("GPU",
            "Timings come from the scene's debug markers, so every pass that names itself for RenderDoc is "
            "already a bar. Results are one frame behind, and collection is gated on r.GPUProfiler.Enabled.");
        DrawHelpTextRow("Assets",
            "Every package load request this session, with what it cost. A row marked with a wait count "
            "blocked on another thread's load rather than reading the file itself.");
        DrawHelpTextRow("Freeze",
            "Holds the displayed frame still on every tab, so a bar can be hovered without it moving.");
        DrawHelpTextRow("Smoothing",
            "Eases the displayed values between frames. Drop it to 0 when chasing a single-frame hitch; "
            "raise it to read steady-state cost without the numbers dancing.");
        DrawHelpTextRow("Enable",
            "Span recording is gated on task.Profiler.Enabled (toggle at the top). The fiber grid and "
            "pool stats are live even with recording off; the timeline and the advisor need recording.");
        DrawHelpTextRow("Advisor",
            "While recording, samples shared-queue contention (concurrent poppers), fiber migration and "
            "workload shape, then judges whether per-worker deques + work-stealing would actually help - "
            "or whether the bottleneck is elsewhere (pool size, locality). The sampling adds a little "
            "overhead, so leave recording off in normal use.");
        DrawHelpTextRow("Fiber grid",
            "Each cell is one work fiber: Free (gray), Running (green, worker #), Parked (amber, counter), "
            "Ready (blue). Watch the pool breathe in real time.");
        DrawHelpTextRow("Timeline",
            "By-worker shows core saturation; By-fiber tints each slice by the worker it ran on, so a "
            "fiber migrating between workers is visible directly.");
    }

    void FProfilerEditorTool::DrawSharedControls()
    {
        ImGui::Checkbox("Freeze", &bFrozen);
        if (bFrozen)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f), LE_ICON_PAUSE " FROZEN");
        }

        ProfilerView::Divider();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Smoothing", &Smoothing, 0.0f, 0.98f, "%.2f");

        // A stall while the panel is visible means the window is not being re-submitted.
        ++DrawTicks;
        ProfilerView::Divider();
        const char Spinner[] = { '|', '/', '-', '\\' };
        ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.55f, 1.0f), "live %c", Spinner[(DrawTicks / 6) % 4]);
    }

    void FProfilerEditorTool::DrawWindow(bool)
    {
        DrawSharedControls();
        ImGui::Separator();

        bool bGameplayVisible = false;

        if (ImGui::BeginTabBar("##profiler"))
        {
#if defined(LUMINA_WITH_GPU_PROFILING)
            if (ImGui::BeginTabItem("GPU"))
            {
                DrawGPU();
                ImGui::EndTabItem();
            }
#endif
            if (ImGui::BeginTabItem("Task System"))
            {
                DrawTasks();
                ImGui::EndTabItem();
            }

#if USING(WITH_EDITOR)
            if (ImGui::BeginTabItem("Assets"))
            {
                DrawAssets();
                ImGui::EndTabItem();
            }
#endif

            if (ImGui::BeginTabItem("Gameplay"))
            {
                bGameplayVisible = true;
                DrawGameplay();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        // Recording follows the visible tab rather than the window, so a tab nobody is looking at costs nothing.
        if (bGameplayVisible != bGameplayRecording)
        {
            bGameplayRecording = bGameplayVisible;
            FGameplayProfiler::Get().SetEnabled(bGameplayVisible);
        }
    }
}
