#include "ScriptDiagnosticsEditorTool.h"

#include <cfloat>

#include "imgui.h"
#include "Core/Engine/Engine.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace
    {
        constexpr float  kScriptRefreshSeconds = 0.25f;   // 4 Hz poll while open
        constexpr uint32 kHistoryMax     = 240;     // ~60 s of history
        constexpr double kMB             = 1024.0 * 1024.0;

        void PushCapped(TVector<float>& History, float Value)
        {
            History.push_back(Value);
            if (History.size() > kHistoryMax)
            {
                History.erase(History.begin());
            }
        }

        void Timeline(const char* Id, const TVector<float>& History, const char* Unit, float Last)
        {
            ImGui::TextDisabled("%s", Id + 2); // skip the "##"
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.66f, 0.78f, 0.95f, 1.0f), "%.1f %s", Last, Unit);
            if (!History.empty())
            {
                ImGui::PlotLines(Id, History.data(), (int)History.size(), 0, nullptr, 0.0f, FLT_MAX, ImVec2(-1.0f, 46.0f));
            }
        }
    }

    void FScriptDiagnosticsEditorTool::OnInitialize()
    {
        CreateToolWindow("C# Diagnostics", [this](bool bIsFocused) { DrawWindow(bIsFocused); });
    }

    void FScriptDiagnosticsEditorTool::OnDeinitialize(const FUpdateContext&)
    {
        // Nothing to disable: the managed host is only polled from DrawWindow, so closing the window already
        // stops every cost. (The export itself is dormant unless this tool calls it.)
    }

    void FScriptDiagnosticsEditorTool::Refresh(bool bForceCollect)
    {
        DotNet::FScriptDiagnostics New;
        if (!DotNet::GetRuntimeDiagnostics(New, bForceCollect))
        {
            bAvailable = false;
            return;
        }

        if (!bForceCollect)
        {
            if (bHavePrevAlloc)
            {
                const double Delta = (double)(New.TotalAllocatedBytes - PrevAllocBytes);
                AllocRateMBs = (float)((Delta > 0.0 ? Delta : 0.0) / kMB / kScriptRefreshSeconds);
            }
            PushCapped(HistHeapMB,       (float)(New.ManagedHeapBytes / kMB));
            PushCapped(HistAllocRateMB,  AllocRateMBs);
            PushCapped(HistWorkingSetMB, (float)(New.WorkingSetBytes / kMB));
        }

        PrevAllocBytes = New.TotalAllocatedBytes;
        bHavePrevAlloc = true;
        Snapshot       = New;
        bAvailable     = true;
    }

    void FScriptDiagnosticsEditorTool::DrawWindow(bool)
    {
        // ---- Controls -----------------------------------------------------------------------------
        ImGui::Checkbox("Freeze", &bFrozen);
        if (bFrozen)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f), LE_ICON_PAUSE " FROZEN");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ++DrawTicks;
        const char Spinner[] = { '|', '/', '-', '\\' };
        ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.55f, 1.0f), "live %c", Spinner[(DrawTicks / 6) % 4]);

        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::Button(LE_ICON_RECYCLE " Force GC"))
        {
            Refresh(true); // blocking full collect; settles a pending collectible-ALC unload
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Run a full blocking GC now. If 'Resident generations' drops back to 1, the\n"
                              "previous reload's ALC was only pending async unload -- not a real leak.");
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_REFRESH " Reload Scripts"))
        {
            DotNet::ReloadScripts();
        }

        // ---- Throttled poll -----------------------------------------------------------------------
        RefreshTimer += (float)GEngine->GetDeltaTime();
        if (!bFrozen && (RefreshTimer >= kScriptRefreshSeconds || !bAvailable))
        {
            RefreshTimer = 0.0f;
            Refresh(false);
        }

        ImGui::Separator();

        if (!bAvailable)
        {
            ImGui::TextDisabled("C# scripting is not initialized (no managed runtime). Diagnostics unavailable.");
            return;
        }

        // ---- Headline: ALC / generation health (the hot-reload leak monitor) ----------------------
        ImGui::TextUnformatted("Resident generations");
        ImGui::SameLine();
        if (Snapshot.ScriptsOnline == 0)
        {
            ImGui::TextDisabled("no scripts loaded");
        }
        else if (Snapshot.AliveScriptAlcCount <= 1)
        {
            ImGui::TextColored(ImVec4(0.55f, 0.86f, 0.62f, 1.0f), LE_ICON_CHECK_CIRCLE " %d  (healthy)", Snapshot.AliveScriptAlcCount);
        }
        else
        {
            ImGui::TextColored(ImVec4(0.96f, 0.55f, 0.35f, 1.0f),
                LE_ICON_ALERT " %d resident (oldest Gen%d) -- possible leak, try Force GC",
                Snapshot.AliveScriptAlcCount, Snapshot.OldestAliveGeneration);
        }
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::TextDisabled("Gen %d  |  %d type(s): %d script(s), %d system(s)",
            Snapshot.Generation, Snapshot.LoadedTypeCount, Snapshot.EntityScriptCount, Snapshot.EntitySystemCount);

        ImGui::Separator();

        // ---- Stat table ---------------------------------------------------------------------------
        const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##csstats", 4, Flags))
        {
            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthStretch, 1.0f);

            auto Cell = [](const char* Label, const FString& Value)
            {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Label);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Value.c_str());
            };
            auto Size = [](int64 Bytes) { return ImGuiX::FormatSize((size_t)(Bytes < 0 ? 0 : Bytes)); };

            ImGui::TableNextRow();
            Cell("Managed heap",      Size(Snapshot.ManagedHeapBytes));
            Cell("GC heap size",      Size(Snapshot.HeapSizeBytes));

            ImGui::TableNextRow();
            Cell("Committed",         Size(Snapshot.CommittedBytes));
            Cell("Fragmented",        Size(Snapshot.FragmentedBytes));

            ImGui::TableNextRow();
            Cell("Working set",       Size(Snapshot.WorkingSetBytes));
            Cell("Alloc churn",       Format("{:.1f} MB/s", AllocRateMBs));

            ImGui::TableNextRow();
            Cell("Total allocated",   Size(Snapshot.TotalAllocatedBytes));
            Cell("Pinned objects",    Format("{}", Snapshot.PinnedObjects));

            ImGui::TableNextRow();
            Cell("GC gen0 / gen1 / gen2", Format("{} / {} / {}",
                Snapshot.Gen0Collections, Snapshot.Gen1Collections, Snapshot.Gen2Collections));
            Cell("GC pause",          Format("{:.2f}%", Snapshot.PauseTimePercentage));

            ImGui::EndTable();
        }

        ImGui::Spacing();

        // ---- Timelines ----------------------------------------------------------------------------
        Timeline("##Managed heap (MB)", HistHeapMB,       "MB",   HistHeapMB.empty()       ? 0.0f : HistHeapMB.back());
        Timeline("##Alloc churn (MB/s)", HistAllocRateMB, "MB/s", AllocRateMBs);
        Timeline("##Working set (MB)",  HistWorkingSetMB, "MB",   HistWorkingSetMB.empty() ? 0.0f : HistWorkingSetMB.back());
    }
}
