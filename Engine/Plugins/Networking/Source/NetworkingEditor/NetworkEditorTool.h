#pragma once
#include "UI/Tools/EditorTool.h"
#include "Containers/HashTable.h"

namespace Lumina
{
    class CWorld;

    // Visualizes the live network system per world, with transport counters, per-connection RTT and loss, and replication state.
    class FNetworkEditorTool : public FEditorTool
    {
    public:

        LUMINA_SINGLETON_EDITOR_TOOL(FNetworkEditorTool)

        FNetworkEditorTool(IEditorToolContext* Context)
            : FEditorTool(Context, "Network", nullptr)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_LAN; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override;
        void DrawHelpMenu() override;

    private:

        // Rolling per-world history of throughput and the largest transform frame, a sample every quarter second so a minute fits.
        struct FNetHistory
        {
            static constexpr int   Capacity       = 240;
            static constexpr float SampleInterval = 0.25f;
            float  SinceSample            = 0.0f;
            float  SendRate[Capacity]     = {};
            float  RecvRate[Capacity]     = {};
            float  MovementBytes[Capacity]= {};
            float  SnapshotEMA            = 0.0f; // smoothed snapshot bytes, for a steady progress bar
            int    Head                   = 0;
            uint64 LastSent               = 0;
            uint64 LastRecv               = 0;
            bool   bPrimed                = false;
        };

        void DrawNetworkWindow(bool bIsFocused);
        void SampleRates(CWorld* World, FNetHistory& History, float DeltaSeconds);

        THashMap<CWorld*, FNetHistory> Histories;
        CWorld*                        SelectedWorld = nullptr;
        ImGuiTextFilter                EntityFilter;
    };
}
