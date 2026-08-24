#include "ProfilerEditorTool.h"
#include "ProfilerViewCommon.h"

#include "imgui.h"

#if defined(LUMINA_WITH_GPU_PROFILING)

#include "Renderer/RHI.h"

namespace Lumina
{
    void FProfilerEditorTool::DrawGPU()
    {
        FGPUProfiler& Profiler = FGPUProfiler::Get();

        if (!RHI::SupportsTimestamps())
        {
            ImGui::TextDisabled("This device reports no timestamp support, so GPU timings are unavailable.");
            return;
        }

        bool bEnabled = Profiler.IsEnabled();
        if (ImGui::Checkbox("Collect (r.GPUProfiler.Enabled)", &bEnabled))
        {
            Profiler.SetEnabled(bEnabled);
        }

        ImGui::SameLine();
        ImGui::Checkbox("Freeze", &bFrozen);

        if (!bEnabled)
        {
            ImGui::TextDisabled("Collection is off. Nothing is recorded and the markers cost nothing.");
            return;
        }

        if (!bFrozen && Profiler.HasResolvedFrame())
        {
            SmoothTowards(Profiler.GetLatest());
            const TSpan<const float> History = Profiler.GetFrameHistory();
            GPUHistory.assign(History.begin(), History.end());
        }

        if (GPUFrame.Scopes.empty())
        {
            ImGui::TextDisabled("Waiting for a resolved frame.");
            return;
        }

        DrawGPUSummary();
        ImGui::Separator();
        DrawGPUFrameGraph();
        ImGui::Separator();
        DrawGPUFlameGraph();
    }

    // A matching name at the same index is the same pass; anything else snaps rather than blending two bars.
    void FProfilerEditorTool::SmoothTowards(const FGPUProfileFrame& Live)
    {
        const double Keep = (double)Math::Clamp(Smoothing, 0.0f, 0.98f);
        const double Take = 1.0 - Keep;

        GPUFrame.FrameNumber = Live.FrameNumber;
        GPUFrame.Scopes.resize(Live.Scopes.size());

        for (size_t Index = 0; Index < Live.Scopes.size(); ++Index)
        {
            const FGPUProfileScope& In = Live.Scopes[Index];
            FGPUProfileScope& Out = GPUFrame.Scopes[Index];

            const bool bSamePass = Out.Name == In.Name && Out.Depth == In.Depth;

            Out.Name        = In.Name;
            Out.ParentIndex = In.ParentIndex;
            Out.Depth       = In.Depth;
            Out.StartMs     = bSamePass ? (Out.StartMs * Keep + In.StartMs * Take) : In.StartMs;
            Out.EndMs       = bSamePass ? (Out.EndMs   * Keep + In.EndMs   * Take) : In.EndMs;
        }

        GPUFrame.TotalMs = GPUFrame.TotalMs > 0.0
            ? GPUFrame.TotalMs * Keep + Live.TotalMs * Take
            : Live.TotalMs;
    }

    void FProfilerEditorTool::DrawGPUSummary()
    {
        ImGui::Text("Frame %llu", (unsigned long long)GPUFrame.FrameNumber);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("GPU: %.3f ms", GPUFrame.TotalMs);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%d passes", (int)GPUFrame.Scopes.size());

        if (SelectedScope >= 0 && SelectedScope < (int32)GPUFrame.Scopes.size())
        {
            const FGPUProfileScope& Scope = GPUFrame.Scopes[SelectedScope];
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text("Selected: %s (%.3f ms)", Scope.Name.c_str(), Scope.DurationMs());
        }
    }

    void FProfilerEditorTool::DrawGPUFrameGraph()
    {
        if (GPUHistory.empty())
        {
            return;
        }

        float Peak = 0.0f;
        for (float Value : GPUHistory)
        {
            Peak = Math::Max(Peak, Value);
        }

        // The line stays raw so a hitch still reads as a spike; only the axis it is drawn against eases.
        const float Keep = bFrozen ? 1.0f : Math::Clamp(Smoothing, 0.0f, 0.98f);
        HistoryPeak = (HistoryPeak > 0.0f) ? (HistoryPeak * Keep + Peak * (1.0f - Keep)) : Peak;

        const float Ceiling = Math::Max(HistoryPeak, Peak) * 1.15f;

        char Overlay[64];
        snprintf(Overlay, sizeof(Overlay), "peak %.2f ms", Peak);
        ImGui::PlotLines("##gpuhistory", GPUHistory.data(), (int)GPUHistory.size(), 0,
            Overlay, 0.0f, Ceiling, ImVec2(-1.0f, 60.0f));
    }

    void FProfilerEditorTool::DrawGPUFlameGraph()
    {
        const double FrameMs = GPUFrame.TotalMs;
        if (FrameMs <= 0.0)
        {
            ImGui::TextDisabled("The frame resolved with no measurable GPU work.");
            return;
        }

        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Zoom", &ZoomT, 0.02f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Pan", &PanT, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Row height", &GPURowHeight, 18.0f, 56.0f, "%.0f px");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Smoothing", &Smoothing, 0.0f, 0.98f, "%.2f");

        const double ViewDur = FrameMs * (double)ZoomT;
        const double ViewT0  = (double)PanT * (FrameMs - ViewDur);

        int32 MaxDepth = 0;
        for (const FGPUProfileScope& Scope : GPUFrame.Scopes)
        {
            MaxDepth = Math::Max(MaxDepth, Scope.Depth);
        }

        // Derived from the font rather than a fixed pixel count, so a label always fits at any DPI.
        const float TextHeight = ImGui::GetTextLineHeight();
        const float RowH       = ProfilerView::RowHeight(GPURowHeight);
        const float Height     = (MaxDepth + 1) * RowH + 8.0f;

        ImGui::BeginChild("##flame", ImVec2(0.0f, 0.0f), true);

        ImDrawList* DL = ImGui::GetWindowDrawList();
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const float  Width  = ImGui::GetContentRegionAvail().x;
        const ImVec2 Mouse  = ImGui::GetIO().MousePos;

        const FGPUProfileScope* Hovered = nullptr;
        int32 HoveredIndex = INDEX_NONE;

        for (int32 Index = 0; Index < (int32)GPUFrame.Scopes.size(); ++Index)
        {
            const FGPUProfileScope& Scope = GPUFrame.Scopes[Index];

            const double A = (Scope.StartMs - ViewT0) / ViewDur;
            const double B = (Scope.EndMs   - ViewT0) / ViewDur;
            if (B < 0.0 || A > 1.0)
            {
                continue;
            }

            const float X0 = Origin.x + (float)Math::Max(0.0, A) * Width;
            const float X1 = Origin.x + (float)Math::Min(1.0, B) * Width;
            const float Y  = Origin.y + Scope.Depth * RowH;

            // A sub-pixel pass still gets a visible sliver rather than vanishing from the graph.
            const ImVec2 Min(X0, Y + 1.0f);
            const ImVec2 Max(Math::Max(X1, X0 + 2.0f), Y + RowH - 2.0f);

            const bool bHover = Mouse.x >= Min.x && Mouse.x <= Max.x && Mouse.y >= Min.y && Mouse.y <= Max.y;
            if (bHover)
            {
                Hovered = &Scope;
                HoveredIndex = Index;
            }

            const ImU32 Base = ProfilerView::ScopeColor(Scope.Name.c_str());
            DL->AddRectFilled(Min, Max, bHover ? Base : ProfilerView::DimColor(Base, 0.82f), 2.0f);

            if (Index == SelectedScope)
            {
                DL->AddRect(Min, Max, IM_COL32(255, 255, 255, 220), 2.0f, 0, 1.5f);
            }

            if (Max.x - Min.x > 26.0f)
            {
                const float TextY = Min.y + ((Max.y - Min.y) - TextHeight) * 0.5f;
                DL->PushClipRect(Min, Max, true);
                DL->AddText(ImVec2(Min.x + 5.0f, TextY), IM_COL32(16, 16, 18, 240), Scope.Name.c_str());
                DL->PopClipRect();
            }
        }

        ImGui::Dummy(ImVec2(Width, Height));

        if (Hovered != nullptr)
        {
            const double Share = (Hovered->DurationMs() / FrameMs) * 100.0;
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(Hovered->Name.c_str());
            ImGui::Separator();
            ImGui::Text("Duration  %.3f ms", Hovered->DurationMs());
            ImGui::Text("Share     %.1f%% of frame", Share);
            ImGui::Text("Start     %.3f ms", Hovered->StartMs);
            ImGui::Text("Depth     %d", Hovered->Depth);
            ImGui::EndTooltip();

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                SelectedScope = (SelectedScope == HoveredIndex) ? INDEX_NONE : HoveredIndex;
            }
        }

        ImGui::EndChild();
    }
}

#endif
