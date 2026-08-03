#include "CPUProfilerEditorTool.h"

#include "implot.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Profiler/CPUProfiler.h"
#include "World/WorldContext.h"
#include "World/WorldTypes.h"

namespace Lumina
{
    static const char* NetModeLabel(ENetMode Mode)
    {
        switch (Mode)
        {
        case ENetMode::Standalone:       return "Standalone";
        case ENetMode::Client:           return "Client";
        case ENetMode::ListenServer:     return "Listen";
        case ENetMode::DedicatedServer:  return "Dedicated";
        }
        return "?";
    }

    static const char* WorldTypeLabel(EWorldType Type)
    {
        switch (Type)
        {
        case EWorldType::None:        return "None";
        case EWorldType::Game:        return "Game";
        case EWorldType::Simulation:  return "Sim";
        case EWorldType::Editor:      return "Editor";
        }
        return "?";
    }

    void FCPUProfilerEditorTool::OnInitialize()
    {
        CreateToolWindow("CPU Profiler", [&] (bool bIsFocused)
        {
            DrawProfilerWindow(bIsFocused);
        });
    }

    void FCPUProfilerEditorTool::OnDeinitialize(const FUpdateContext&)
    {
    }

    void FCPUProfilerEditorTool::DrawToolMenu(const FUpdateContext&)
    {
        FConsoleVariable* CVar = FConsoleRegistry::Get().Find("cpu.Profiler.Enabled");
        if (CVar != nullptr)
        {
            bool bEnabled = eastl::get<bool>(*CVar->ValuePtr);
            if (ImGui::MenuItem(LE_ICON_BUG " Profiling Enabled", nullptr, bEnabled))
            {
                FConsoleRegistry::Get().SetAs<bool>("cpu.Profiler.Enabled", !bEnabled);
            }
        }

        ImGui::Separator();
        ImGui::MenuItem("Expand All", nullptr, &bExpandAll);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("Min ms filter", &FilterMinTimeMs, 0.005f, 0.0f, 50.0f, "%.3f");
    }

    void FCPUProfilerEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Enable",
            "CPU profiling is gated on cpu.Profiler.Enabled, toggle from the gear menu. Costs frames; "
            "leave off when not investigating.");
        DrawHelpTextRow("Targets",
            "Multiple sample targets (engine main thread, asset cooker) can be picked "
            "from the Target menu. Each has its own scope tree.");
        DrawHelpTextRow("Reading the tree",
            "Each node is a CPU scope pushed via FCPUProfileScope (or LUMINA_PROFILE macro). Time is "
            "wall-clock ms; use Min ms filter to hide noise.");
        DrawHelpTextRow("Frame Time Graph",
            "Rolling history of total frame CPU time on the active target. Spikes correlate with surges "
            "in the tree.");
        DrawHelpTextRow("Adding scopes",
            "In C++: LUMINA_PROFILE(\"YourScope\");, scopes nest naturally.");
    }

    void FCPUProfilerEditorTool::DrawProfilerWindow(bool)
    {
        FCPUProfiler& Profiler = FCPUProfiler::Get();

        FConsoleVariable* CVar = FConsoleRegistry::Get().Find("cpu.Profiler.Enabled");
        bool bEnabled = (CVar != nullptr) ? eastl::get<bool>(*CVar->ValuePtr) : false;

        if (ImGui::Checkbox("Enable CPU Profiling", &bEnabled))
        {
            if (CVar != nullptr)
            {
                FConsoleRegistry::Get().SetAs<bool>("cpu.Profiler.Enabled", bEnabled);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(CVar: cpu.Profiler.Enabled)");

        if (!bEnabled)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                LE_ICON_HELP " CPU profiling is disabled. Enable it above to begin collecting per-world scopes.");
            return;
        }

        ImGui::Spacing();
        DrawTargetPicker();

        const FCPUProfileTarget* Target = nullptr;
        if (SelectedTargetKey != nullptr)
        {
            Target = Profiler.FindTargetByKey(SelectedTargetKey);
        }

        // Auto-select first target with resolved data if none selected.
        if (Target == nullptr)
        {
            for (const TUniquePtr<FCPUProfileTarget>& T : Profiler.GetTargets())
            {
                if (T->bHasResolvedFrame)
                {
                    SelectedTargetKey = T->Key;
                    Target = T.get();
                    break;
                }
            }
        }

        if (Target == nullptr || !Target->bHasResolvedFrame)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Waiting for first resolved frame...");
            return;
        }

        ImGui::Separator();
        ImGui::Text("Target: %s  |  Frame #%llu  |  CPU Total: %.3f ms  |  Scopes: %u",
            Target->Name.c_str(),
            (unsigned long long)Target->Latest.FrameNumber,
            Target->Latest.TotalTimeMs,
            (uint32)Target->Latest.Scopes.size());
        ImGui::Separator();

        DrawFrameTimeGraph(*Target);

        ImGui::Spacing();
        ImGui::TextUnformatted("Scopes (this frame):");
        ImGui::Separator();
        DrawScopeTree(*Target);
    }

    void FCPUProfilerEditorTool::DrawTargetPicker()
    {
        FCPUProfiler& Profiler = FCPUProfiler::Get();
        const auto& Targets = Profiler.GetTargets();

        if (Targets.empty())
        {
            ImGui::TextDisabled("No targets yet; scopes appear when a world ticks.");
            return;
        }

        const FCPUProfileTarget* Current = nullptr;
        for (const TUniquePtr<FCPUProfileTarget>& T : Targets)
        {
            if (T->Key == SelectedTargetKey)
            {
                Current = T.get();
                break;
            }
        }

        char Preview[256];
        if (Current != nullptr)
        {
            snprintf(Preview, sizeof(Preview), "%s [%s / %s%s]",
                Current->Name.c_str(),
                WorldTypeLabel(Current->WorldType),
                NetModeLabel(Current->NetMode),
                Current->bPIE ? " / PIE" : "");
        }
        else
        {
            snprintf(Preview, sizeof(Preview), "Select target...");
        }

        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::BeginCombo("Target", Preview))
        {
            for (const TUniquePtr<FCPUProfileTarget>& T : Targets)
            {
                char Label[256];
                snprintf(Label, sizeof(Label), "%s [%s / %s%s]",
                    T->Name.c_str(),
                    WorldTypeLabel(T->WorldType),
                    NetModeLabel(T->NetMode),
                    T->bPIE ? " / PIE" : "");
                const bool bSelected = (T->Key == SelectedTargetKey);
                if (ImGui::Selectable(Label, bSelected))
                {
                    SelectedTargetKey = T->Key;
                }
                if (bSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    void FCPUProfilerEditorTool::DrawFrameTimeGraph(const FCPUProfileTarget& Target)
    {
        const TVector<float>& History = Target.FrameTimeHistory;
        if (History.empty())
        {
            return;
        }

        float MaxValue = 0.0f;
        float Sum = 0.0f;
        for (float V : History)
        {
            if (V > MaxValue) MaxValue = V;
            Sum += V;
        }
        const float Average = Sum / (float)History.size();

        ImGui::Text("CPU Frame Time  -  Avg: %.3f ms   Max: %.3f ms   Samples: %d",
            Average, MaxValue, (int)History.size());

        if (ImPlot::BeginPlot("##CPUFrameTime", ImVec2(-1, GraphHeight), ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText))
        {
            ImPlot::SetupAxes(nullptr, "ms",
                ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoMenus,
                ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, eastl::max(MaxValue * 1.1f, 1.0f), ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, (double)History.size(), ImGuiCond_Always);

            ImPlot::SetNextLineStyle(ImVec4(0.95f, 0.70f, 0.25f, 1.0f), 1.5f);
            ImPlot::SetNextFillStyle(ImVec4(0.95f, 0.70f, 0.25f, 0.20f), 1.0f);
            ImPlot::PlotShaded("CPU ms", History.data(), (int)History.size(), 0.0);
            ImPlot::PlotLine("CPU ms", History.data(), (int)History.size());

            ImPlot::EndPlot();
        }
    }

    void FCPUProfilerEditorTool::DrawScopeTree(const FCPUProfileTarget& Target)
    {
        const FCPUProfileFrame& Frame = Target.Latest;
        if (Frame.Scopes.empty())
        {
            ImGui::TextDisabled("No scopes recorded.");
            return;
        }

        const float TableTotal = eastl::max((float)Frame.TotalTimeMs, 0.0001f);

        if (!ImGui::BeginTable("##CPUScopes", 4,
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
        {
            return;
        }

        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("% Frame", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Bar", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        TVector<TVector<int32>> Children;
        Children.resize(Frame.Scopes.size());
        TVector<int32> Roots;
        for (int32 i = 0; i < (int32)Frame.Scopes.size(); ++i)
        {
            const FCPUProfileScope& Scope = Frame.Scopes[i];
            if (Scope.ParentIndex < 0)
            {
                Roots.push_back(i);
            }
            else
            {
                Children[Scope.ParentIndex].push_back(i);
            }
        }

        auto SortByTimeDesc = [&](TVector<int32>& Indices)
        {
            eastl::sort(Indices.begin(), Indices.end(), [&](int32 A, int32 B)
            {
                return Frame.Scopes[A].DurationMs() > Frame.Scopes[B].DurationMs();
            });
        };
        SortByTimeDesc(Roots);
        for (TVector<int32>& List : Children)
        {
            SortByTimeDesc(List);
        }

        struct FStackEntry
        {
            int32 ScopeIndex;
            int32 NextChild;
            bool  bOpen;
        };

        TVector<FStackEntry> Stack;

        for (int32 RootIdx : Roots)
        {
            Stack.clear();
            Stack.push_back({RootIdx, 0, false});

            while (!Stack.empty())
            {
                FStackEntry& Top = Stack.back();
                const FCPUProfileScope& Scope = Frame.Scopes[Top.ScopeIndex];
                const TVector<int32>& Kids = Children[Top.ScopeIndex];
                const float Duration = (float)Scope.DurationMs();

                if (Top.NextChild == 0)
                {
                    if (Duration < FilterMinTimeMs && Kids.empty())
                    {
                        Stack.pop_back();
                        continue;
                    }

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    ImGui::PushID(Top.ScopeIndex);
                    ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen;
                    if (Kids.empty())
                    {
                        Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    }
                    if (bExpandAll)
                    {
                        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                    }

                    const ImVec4 NameColor(Scope.Color.R, Scope.Color.G, Scope.Color.B, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, NameColor);
                    Top.bOpen = ImGui::TreeNodeEx(Scope.Name.c_str(), Flags);
                    ImGui::PopStyleColor();

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f", Duration);

                    ImGui::TableSetColumnIndex(2);
                    const float Pct = (Duration / TableTotal) * 100.0f;
                    ImGui::Text("%5.1f%%", Pct);

                    ImGui::TableSetColumnIndex(3);
                    const float Frac = eastl::min(eastl::max(Duration / TableTotal, 0.0f), 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, NameColor);
                    ImGui::ProgressBar(Frac, ImVec2(-FLT_MIN, 0.0f), "");
                    ImGui::PopStyleColor();

                    ImGui::PopID();

                    if (!Top.bOpen || Kids.empty())
                    {
                        Stack.pop_back();
                        continue;
                    }
                }

                if (Top.NextChild < (int32)Kids.size())
                {
                    int32 ChildIdx = Kids[Top.NextChild++];
                    Stack.push_back({ChildIdx, 0, false});
                }
                else
                {
                    if (Top.bOpen && !Kids.empty())
                    {
                        ImGui::TreePop();
                    }
                    Stack.pop_back();
                }
            }
        }

        ImGui::EndTable();
    }
}
