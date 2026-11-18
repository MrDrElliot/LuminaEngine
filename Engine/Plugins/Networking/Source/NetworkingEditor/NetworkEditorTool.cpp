// Engine headers reach for the ECS; the Runtime PCH supplies it there, not here.
#include "NetworkEditorTool.h"
#include <bit>
#include "World/ECS/Registry.h"


#include "World/WorldManager.h"
#include "World/WorldContext.h"
#include "World/World.h"
#include "Net/NetWorldState.h"
#include "Net/NetReplication.h"
#include "Components/NetworkComponent.h"
#include "Components/RepTransformComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "Networking/INetworkTransport.h"
#include "Config/NetworkSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/InfoTable.h"
#include "Containers/StringFormat.h"
#include "Net/LagCompensation.h"
#include "Simulation/Simulation.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Systems/DebugDrawSystem.h"

namespace Lumina
{
    namespace
    {
        const char* NetModeName(ENetMode Mode)
        {
            switch (Mode)
            {
            case ENetMode::Standalone:      return "Standalone";
            case ENetMode::Client:          return "Client";
            case ENetMode::ListenServer:    return "Listen Server";
            case ENetMode::DedicatedServer: return "Dedicated Server";
            }
            return "Unknown";
        }

        const char* ConnStateName(EConnectionState State)
        {
            switch (State)
            {
            case EConnectionState::Disconnected: return "Disconnected";
            case EConnectionState::Connecting:   return "Connecting";
            case EConnectionState::Connected:    return "Connected";
            }
            return "?";
        }

        FString FormatBytesNice(double Bytes)
        {
            const char* Units[] = { "B", "KB", "MB", "GB" };
            int Unit = 0;
            while (Bytes >= 1024.0 && Unit < 3) { Bytes /= 1024.0; ++Unit; }
            char Buf[64];
            snprintf(Buf, sizeof(Buf), "%.2f %s", Bytes, Units[Unit]);
            return FString(Buf);
        }

        const char* NetRoleName(ENetRole Role)
        {
            switch (Role)
            {
            case ENetRole::None:            return "None";
            case ENetRole::SimulatedProxy:  return "SimulatedProxy";
            case ENetRole::AutonomousProxy: return "AutonomousProxy";
            case ENetRole::Authority:       return "Authority";
            }
            return "?";
        }

        // Severity palette for the warnings panel / threshold coloring.
        const ImVec4 ColGood (0.40f, 0.85f, 0.45f, 1.0f);
        const ImVec4 ColWarn (0.95f, 0.80f, 0.25f, 1.0f);
        const ImVec4 ColBad  (0.95f, 0.35f, 0.30f, 1.0f);

        // A networked world is one whose registry has a live FNetWorldState with a transport.
        FNetWorldState* GetNetState(CWorld* World)
        {
            if (World == nullptr) { return nullptr; }
            FNetWorldState* State = World->TryGetSingleton<FNetWorldState>();
            return (State != nullptr && State->Transport) ? State : nullptr;
        }
    }

    void FNetworkEditorTool::OnInitialize()
    {
        CreateToolWindow("Network", [&](bool bIsFocused)
        {
            DrawNetworkWindow(bIsFocused);
        });
    }

    void FNetworkEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        Histories.Reset();
    }

    void FNetworkEditorTool::SampleRates(CWorld* InWorld, FNetHistory& History, float DeltaSeconds)
    {
        FNetWorldState* State = GetNetState(InWorld);
        if (State == nullptr) { return; }

        // The raw per-tick value jitters as each tick sends a different subset of due entities.
        const float SnapNow = static_cast<float>(State->Stats.MovementFrameBytes);
        History.SnapshotEMA += (SnapNow - History.SnapshotEMA) * 0.1f;

        // A rate over one editor frame is mostly zero at high frame rates, since most frames carry no packet.
        History.SinceSample += DeltaSeconds;
        if (History.bPrimed && History.SinceSample < FNetHistory::SampleInterval)
        {
            return;
        }

        const FNetworkStats Stats = State->Transport->GetStats();
        if (History.bPrimed)
        {
            History.SendRate[History.Head]      = static_cast<float>(Stats.TotalSentBytes     - History.LastSent) / History.SinceSample;
            History.RecvRate[History.Head]      = static_cast<float>(Stats.TotalReceivedBytes - History.LastRecv) / History.SinceSample;
            History.MovementBytes[History.Head] = static_cast<float>(State->Stats.MovementFrameBytes);
            History.Head = (History.Head + 1) % FNetHistory::Capacity;
        }
        History.LastSent    = Stats.TotalSentBytes;
        History.LastRecv    = Stats.TotalReceivedBytes;
        History.bPrimed     = true;
        History.SinceSample = 0.0f;

    }

    void FNetworkEditorTool::DrawNetworkWindow(bool bIsFocused)
    {
        if (Global::GWorldManager == nullptr)
        {
            ImGui::TextDisabled("World manager not initialized.");
            return;
        }

        // Gather networked worlds, and keep their rate history sampled every frame (even when unselected).
        const float Dt = ImGui::GetIO().DeltaTime;
        TVector<CWorld*> NetWorlds;
        for (const TUniquePtr<FWorldContext>& Ctx : Global::GWorldManager->GetContexts())
        {
            CWorld* ContextWorld = Ctx->World.Get();
            if (GetNetState(ContextWorld) == nullptr)
            {
                continue;
            }
            NetWorlds.Add(ContextWorld);
            SampleRates(ContextWorld, Histories[ContextWorld], Dt);
        }

        // Prune ended sessions so a reused CWorld address never inherits a stale graph.
        Histories.RemoveAll([&NetWorlds](const auto& Pair) { return !NetWorlds.Contains(Pair.Key); });

        if (NetWorlds.IsEmpty())
        {
            ImGui::TextDisabled("No active network sessions.");
            ImGui::TextWrapped("Start a multiplayer PIE session (Play settings -> Net Mode) to see connections and stats here.");
            SelectedWorld = nullptr;
            return;
        }

        // Default / validate the selection.
        bool bSelectedValid = false;
        for (CWorld* W : NetWorlds) { if (W == SelectedWorld) { bSelectedValid = true; break; } }
        if (!bSelectedValid) { SelectedWorld = NetWorlds[0]; }

        // --- World selector ---
        auto WorldLabel = [](CWorld* W) -> FString
        {
            char Buf[128];
            snprintf(Buf, sizeof(Buf), "[%s] %s", NetModeName(W->GetNetMode()), *W->GetName());
            return FString(Buf);
        };

        ImGui::TextUnformatted("Session:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::BeginCombo("##NetWorld", *WorldLabel(SelectedWorld)))
        {
            for (CWorld* W : NetWorlds)
            {
                const bool bSelected = (W == SelectedWorld);
                if (ImGui::Selectable(*WorldLabel(W), bSelected))
                {
                    SelectedWorld = W;
                }
                if (bSelected) { ImGui::SetItemDefaultFocus(); }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d networked world%s)", NetWorlds.Num(), NetWorlds.Num() == 1 ? "" : "s");

        ImGui::Separator();

        FNetWorldState* State = GetNetState(SelectedWorld);
        if (State == nullptr)
        {
            ImGui::TextDisabled("Selected world is no longer networked.");
            return;
        }

        ECS::FRegistry& Registry = SelectedWorld->GetEntityRegistry();
        const bool bIsServer = SelectedWorld->GetNetMode() == ENetMode::ListenServer
                            || SelectedWorld->GetNetMode() == ENetMode::DedicatedServer;

        const FNetReplicationStats& RepStats = State->Stats;

        // Per-connection stats fetched once; the warnings panel and the Connections section both use them.
        TVector<FConnectionStats> Conns;
        State->Transport->GetConnectionStats(Conns);

        // A low average means the buffer is starving, which shows up as visible stutter.
        int RingCount = 0, RingSampleSum = 0, RingMin = 0;
        bool bRingMinSet = false;
        for (ECS::FEntity E : Registry.View<FRepTransform>())
        {
            const FRepTransform& Rep = Registry.Get<FRepTransform>(E);
            if (Rep.Ring.Count > 0)
            {
                ++RingCount;
                RingSampleSum += Rep.Ring.Count;
                if (!bRingMinSet || Rep.Ring.Count < RingMin) { RingMin = Rep.Ring.Count; bRingMinSet = true; }
            }
        }

        // --- Validation / Warnings ---
        if (ImGui::CollapsingHeader("Validation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool bAny = false;
            char Buf[256];
            auto Bullet = [&](const ImVec4& Col, const char* Text) { ImGui::TextColored(Col, "  - %s", Text); bAny = true; };

            const float CapPct = (Net::MaxFramedMessageSize > 0)
                ? static_cast<float>(RepStats.MovementFrameBytes) / static_cast<float>(Net::MaxFramedMessageSize) : 0.0f;

            if (RepStats.OversizedSnapshotDrops > 0)
            {
                snprintf(Buf, sizeof(Buf), "Transform snapshot exceeded the 64 KB frame cap %u time(s) -- updates were DROPPED. "
                    "Reduce replicated entities or split the snapshot.", RepStats.OversizedSnapshotDrops);
                Bullet(ColBad, Buf);
            }
            else if (CapPct >= 0.75f)
            {
                snprintf(Buf, sizeof(Buf), "Transform snapshot at %.0f%% of the 64 KB frame cap (%u entities).",
                    CapPct * 100.0f, RepStats.MovementEntityCount);
                Bullet(ColWarn, Buf);
            }

            for (const FConnectionStats& C : Conns)
            {
                if (C.PacketLoss > 0.15f)
                {
                    snprintf(Buf, sizeof(Buf), "Connection %u packet loss %.0f%%.", C.ConnectionId, C.PacketLoss * 100.0f);
                    Bullet(ColBad, Buf);
                }
                else if (C.PacketLoss > 0.05f)
                {
                    snprintf(Buf, sizeof(Buf), "Connection %u packet loss %.1f%%.", C.ConnectionId, C.PacketLoss * 100.0f);
                    Bullet(ColWarn, Buf);
                }

                if (C.RoundTripTimeMs > 300)
                {
                    snprintf(Buf, sizeof(Buf), "Connection %u RTT %u ms (high).", C.ConnectionId, C.RoundTripTimeMs);
                    Bullet(ColBad, Buf);
                }
                else if (C.RoundTripTimeMs > 150)
                {
                    snprintf(Buf, sizeof(Buf), "Connection %u RTT %u ms.", C.ConnectionId, C.RoundTripTimeMs);
                    Bullet(ColWarn, Buf);
                }
            }

            if (RingCount > 0)
            {
                const float Avg = static_cast<float>(RingSampleSum) / static_cast<float>(RingCount);
                if (Avg < 2.0f)
                {
                    snprintf(Buf, sizeof(Buf), "Proxy interpolation buffers starving (avg %.1f samples) -- expect stutter.", Avg);
                    Bullet(ColWarn, Buf);
                }
            }

            if (const FSimulation* Sim = SelectedWorld->GetSimulation(); Sim != nullptr && Sim->HasDesynced())
            {
                snprintf(Buf, sizeof(Buf), "Deterministic simulation desynced at tick %lld. The log names the component that diverged.", (long long)Sim->GetDesyncTick());
                Bullet(ColBad, Buf);
            }

            if (!bAny)
            {
                ImGui::TextColored(ColGood, "  - No issues detected.");
            }
        }

        // Per world, so a client preview window with no viewport menu of its own can still be inspected.
        if (FDebugDrawState* DrawState = Registry.Ctx().Find<FDebugDrawState>(); DrawState != nullptr && ImGui::CollapsingHeader("Debug Draw"))
        {
            for (const FDebugDrawNetworkingView& View : kDebugDrawNetworkingViews)
            {
                bool bOn = EnumHasAnyFlags(DrawState->Networking, View.View);
                if (ImGui::Checkbox(View.Label, &bOn))
                {
                    DrawState->Networking ^= View.View;
                }
            }
        }

        // --- Overview ---
        if (ImGui::CollapsingHeader("Overview", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int NetworkedEntities = 0;
            for (auto E : Registry.View<SNetworkComponent>()) { (void)E; ++NetworkedEntities; }

            ImGui::BulletText("Role: %s", NetModeName(SelectedWorld->GetNetMode()));
            ImGui::BulletText("Local Peer Id: %u%s", State->LocalPeerId, bIsServer ? " (server)" : "");
            if (bIsServer)
            {
                ImGui::BulletText("Connected Clients: %d", State->ConnectedClients);
            }
            else
            {
                ImGui::BulletText("Connected to server: %s", State->bClientConnected ? "yes" : "no");
                const FNetPropertyAcks& Acks = State->PropertyAcks;
                ImGui::BulletText("Property packets: newest %u, %d of the 32 before it arrived, fence %u",
                    Acks.Latest, std::popcount(Acks.Bits), Acks.Floor);
            }
            ImGui::BulletText("Networked Entities: %d", NetworkedEntities);
            ImGui::BulletText("NetGUID Table: %d entries", State->GuidTable.GuidToEntity.Num());
            uint32 DynamicSpawns = 0;
            for (const auto& [Guid, Entity] : State->GuidTable.GuidToEntity)
            {
                if (Guid >= NetGUID_DynamicStart) { ++DynamicSpawns; }
            }
            ImGui::BulletText("Dynamic Spawns Tracked: %d", (int)DynamicSpawns);
        }

        // --- Transport totals + throughput ---
        const FNetworkStats Stats = State->Transport->GetStats();
        if (ImGui::CollapsingHeader("Transport", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::BeginTable("##Totals", 2, ImGuiTableFlags_SizingFixedFit))
            {
                InfoTable::Row("Sent",         FormatBytesNice((double)Stats.TotalSentBytes));
                InfoTable::Row("Received",     FormatBytesNice((double)Stats.TotalReceivedBytes));
                InfoTable::Row("Packets Sent", Format("{}", Stats.TotalSentPackets));
                InfoTable::Row("Packets Recv", Format("{}", Stats.TotalReceivedPackets));
                ImGui::EndTable();
            }

            // Throughput graph (bytes/sec), ring buffer plotted from Head.
            FNetHistory& H = Histories[SelectedWorld];
            const float CurSend = H.SendRate[(H.Head + FNetHistory::Capacity - 1) % FNetHistory::Capacity];
            const float CurRecv = H.RecvRate[(H.Head + FNetHistory::Capacity - 1) % FNetHistory::Capacity];

            ImGui::Spacing();
            ImGui::Text("Send: %s/s", *FormatBytesNice(CurSend));
            ImGui::PlotLines("##Send", H.SendRate, FNetHistory::Capacity, H.Head, nullptr, 0.0f, FLT_MAX, ImVec2(-1, 60));
            ImGui::Text("Recv: %s/s", *FormatBytesNice(CurRecv));
            ImGui::PlotLines("##Recv", H.RecvRate, FNetHistory::Capacity, H.Head, nullptr, 0.0f, FLT_MAX, ImVec2(-1, 60));
        }

        // --- Replication (what the net layer actually sent this tick) ---
        if (ImGui::CollapsingHeader("Replication", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int MoveRepl = 0, RoleAuth = 0, RoleSim = 0, RoleAuto = 0, RoleNone = 0;
            for (ECS::FEntity E : Registry.View<SNetworkComponent>())
            {
                const SNetworkComponent& N = Registry.Get<SNetworkComponent>(E);
                if (N.bReplicates && N.bReplicatesMovement && N.bNetLoadOnClient) { ++MoveRepl; }
                switch (N.LocalRole)
                {
                case ENetRole::Authority:       ++RoleAuth; break;
                case ENetRole::SimulatedProxy:  ++RoleSim;  break;
                case ENetRole::AutonomousProxy: ++RoleAuto; break;
                default:                        ++RoleNone; break;
                }
            }

            // The smoothed bytes make the bar read as a steady level rather than flickering per tick.
            FNetHistory& H = Histories[SelectedWorld];
            const float SmoothBytes = H.SnapshotEMA;
            float CapPct = (Net::MaxFramedMessageSize > 0)
                ? SmoothBytes / static_cast<float>(Net::MaxFramedMessageSize) : 0.0f;
            if (CapPct > 1.0f) { CapPct = 1.0f; }
            const ImVec4 BarCol = (RepStats.OversizedSnapshotDrops > 0) ? ColBad : (CapPct >= 0.75f ? ColWarn : ColGood);

            char Overlay[96];
            snprintf(Overlay, sizeof(Overlay), "%s / %s  (largest frame)",
                *FormatBytesNice((double)SmoothBytes),
                *FormatBytesNice((double)Net::MaxFramedMessageSize));

            ImGui::TextUnformatted("Largest transform frame vs 64 KB cap:");
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, BarCol);
            ImGui::ProgressBar(CapPct, ImVec2(-1, 0), Overlay);
            ImGui::PopStyleColor();

            ImGui::BulletText("Movement-replicated entities: %d", MoveRepl);
            ImGui::BulletText("Relevant per client: %u avg, %u max  (of %d) -- interest culling",
                RepStats.RelevantAvg, RepStats.RelevantMax, MoveRepl);
            // The snapshot is split into as many <=64 KB frames as needed, so any entity count replicates.
            const float BytesPerEnt = RepStats.MovementEntityCount
                ? static_cast<float>(RepStats.MovementTotalBytes) / static_cast<float>(RepStats.MovementEntityCount) : 0.0f;
            ImGui::BulletText("Snapshot total: %s across %u frame%s  (%u entities, %.1f B/entity)",
                *FormatBytesNice((double)RepStats.MovementTotalBytes),
                RepStats.MovementChunks, RepStats.MovementChunks == 1 ? "" : "s",
                RepStats.MovementEntityCount, BytesPerEnt);
            ImGui::BulletText("Peak frame: %s", *FormatBytesNice((double)RepStats.PeakMovementFrameBytes));
            if (RepStats.OversizedSnapshotDrops > 0)
            {
                ImGui::TextColored(ColBad, "  Oversized frame drops: %u", RepStats.OversizedSnapshotDrops);
            }
            ImGui::BulletText("Datagrams: unreliable %s, reliable %s",
                *FormatBytesNice((double)RepStats.UnreliableBatchBytes),
                *FormatBytesNice((double)RepStats.ReliableBatchBytes));
            ImGui::BulletText("Last tick: %u spawns, %u despawns, %u property updates%s",
                RepStats.SpawnsSent, RepStats.DespawnsSent, RepStats.PropertyUpdatesSent,
                RepStats.bKeyframeThisTick ? "  (keyframe)" : "");
            ImGui::BulletText("Roles: Authority %d, Simulated %d, Autonomous %d, None %d",
                RoleAuth, RoleSim, RoleAuto, RoleNone);

            // With chunking this stays under the cap, since the snapshot just splits into more frames.
            const float CurSnap = H.MovementBytes[(H.Head + FNetHistory::Capacity - 1) % FNetHistory::Capacity];
            ImGui::Spacing();
            ImGui::Text("Largest frame (cap = top of plot): %s", *FormatBytesNice(CurSnap));
            ImGui::PlotLines("##Snap", H.MovementBytes, FNetHistory::Capacity, H.Head, nullptr,
                0.0f, static_cast<float>(Net::MaxFramedMessageSize), ImVec2(-1, 60));
        }

        // --- Interpolation (client-side smoothing health) ---
        if (ImGui::CollapsingHeader("Interpolation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const CNetworkSettings* NS = GetDefault<CNetworkSettings>();
            ImGui::BulletText("Interp delay: %.0f ms", (NS ? NS->InterpDelay : 0.1f) * 1000.0f);
            ImGui::BulletText("Latest server time: %.2f s", State->LatestServerTime);
            if (RingCount > 0)
            {
                ImGui::BulletText("Proxy rings: %d active, avg %.1f samples (min %d, cap %d)",
                    RingCount, (float)RingSampleSum / (float)RingCount, RingMin, FNetInterpState::Capacity);
            }
            else
            {
                ImGui::BulletText("Proxy rings: none active");
            }
        }

        // --- Per-connection stats ---
        if (ImGui::CollapsingHeader("Connections", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (Conns.IsEmpty())
            {
                ImGui::TextDisabled("No peers.");
            }
            else if (ImGui::BeginTable("##Conns", bIsServer ? 11 : 8,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Id");
                ImGui::TableSetupColumn("State");
                ImGui::TableSetupColumn("RTT");
                ImGui::TableSetupColumn("Loss");
                ImGui::TableSetupColumn("Sent");
                ImGui::TableSetupColumn("Recv");
                ImGui::TableSetupColumn("Pkts");
                ImGui::TableSetupColumn("Lost");
                if (bIsServer)
                {
                    ImGui::TableSetupColumn("Relevant");
                    ImGui::TableSetupColumn("Props In Flight");
                    ImGui::TableSetupColumn("Resends Queued");
                }
                ImGui::TableHeadersRow();

                for (const FConnectionStats& C : Conns)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%u", C.ConnectionId);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(ConnStateName(C.State));
                    ImGui::TableNextColumn(); ImGui::Text("%u ms", C.RoundTripTimeMs);
                    ImGui::TableNextColumn(); ImGui::Text("%.1f%%", C.PacketLoss * 100.0f);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(*FormatBytesNice((double)C.SentBytes));
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(*FormatBytesNice((double)C.ReceivedBytes));
                    ImGui::TableNextColumn(); ImGui::Text("%u", C.PacketsSent);
                    ImGui::TableNextColumn(); ImGui::Text("%u", C.PacketsLost);
                    if (bIsServer)
                    {
                        const FNetClientView* View = State->ClientViews.Find(C.ConnectionId);
                        ImGui::TableNextColumn(); ImGui::Text("%d", View != nullptr ? View->Relevant.Num() : 0);
                        ImGui::TableNextColumn(); ImGui::Text("%d", View != nullptr ? View->Properties.InFlight.Num() : 0);
                        ImGui::TableNextColumn(); ImGui::Text("%d", View != nullptr ? View->Properties.Resends.Num() : 0);
                    }
                }
                ImGui::EndTable();
            }
            if (bIsServer)
            {
                ImGui::TextDisabled("Property packets lost and sent again: %u", RepStats.PropertyPacketsLost);
            }
        }

        if (const FLagCompensationHistory* History = Registry.Ctx().Find<FLagCompensationHistory>();
            History != nullptr && History->Count > 0 && ImGui::CollapsingHeader("Lag Compensation"))
        {
            const FLagCompensationFrame& Newest = History->FrameAt(0);
            const CNetworkSettings* NS = GetDefault<CNetworkSettings>();
            ImGui::BulletText("Hitboxes: %d", Newest.Samples.Num());
            ImGui::BulletText("History: %d frames covering %.0f of %.0f ms, recorded at up to %.0f Hz",
                History->Count, (History->GetNewestTime() - History->GetOldestTime()) * 1000.0, History->MaxRewindSeconds * 1000.0,
                NS->LagCompensationRecordRate);
            ImGui::BulletText("Fastest hitbox between the last two frames: %.2f m", Newest.MaxMoveFromPrevious);
        }

        if (const FSimulation* Sim = SelectedWorld->GetSimulation(); Sim != nullptr && Sim->IsRunning() && ImGui::CollapsingHeader("Simulation"))
        {
            ImGui::BulletText("Mode: %s, player %u of %u", Sim->GetMode() == ESimulationMode::Rollback ? "Rollback" : "Lockstep",
                Sim->GetLocalPlayer(), Sim->GetNumPlayers());
            ImGui::BulletText("Tick %u, confirmed %lld", Sim->GetCurrentTick(), (long long)Sim->GetConfirmedTick());
            ImGui::BulletText("Rollbacks %u, replayed ticks %u", Sim->GetRollbackCount(), Sim->GetResimulatedTicks());
            ImGui::BulletText("Checksums verified %u", Sim->GetVerifiedChecksums());
        }

        // --- Per-entity replication (collapsed; capped + filterable so big sessions stay responsive) ---
        if (ImGui::CollapsingHeader("Entities"))
        {
            ImGuiTextFilter& Filter = EntityFilter;
            Filter.Draw("Filter (name)", 180.0f);

            constexpr int MaxRows = 256;
            int Shown = 0, Total = 0;
            if (ImGui::BeginTable("##Ents", 7,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
                | ImGuiTableFlags_ScrollY, ImVec2(0, 220)))
            {
                ImGui::TableSetupColumn("NetGUID");
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Role");
                ImGui::TableSetupColumn("Owner");
                ImGui::TableSetupColumn("Move");
                ImGui::TableSetupColumn("Ring");
                ImGui::TableSetupColumn("Networking");
                ImGui::TableHeadersRow();

                for (ECS::FEntity E : Registry.View<SNetworkComponent>())
                {
                    ++Total;
                    const SNetworkComponent& N = Registry.Get<SNetworkComponent>(E);

                    const char* Name = "";
                    if (const SNameComponent* NC = Registry.TryGet<SNameComponent>(E)) { Name = *NC->Name; }
                    if (!ImGuiX::PassSearchFilter(Filter, Name)) { continue; }
                    if (Shown >= MaxRows) { continue; }
                    ++Shown;

                    int Ring = 0;
                    if (const FRepTransform* R = Registry.TryGet<FRepTransform>(E)) { Ring = R->Ring.Count; }

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%u", N.NetGUID.Value);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(Name);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(NetRoleName(N.LocalRole));
                    ImGui::TableNextColumn(); ImGui::Text("%u", N.OwningConnectionId);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(N.bReplicatesMovement ? "yes" : "no");
                    ImGui::TableNextColumn(); ImGui::Text("%d", Ring);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(N.bInherited ? "inherited" : "own");
                    if (Registry.HasAny<SNetProxyTag>(E))
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled(Registry.HasAny<SNetDrivenTransformTag>(E) ? "mirror, streamed" : "mirror, flown here");
                    }
                }
                ImGui::EndTable();
            }
            if (Shown < Total)
            {
                ImGui::TextDisabled("Showing %d of %d networked entities (cap/filter).", Shown, Total);
            }
        }
    }

    void FNetworkEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Sessions",
            "Each networked world (the listen server and every client preview) is a separate session. "
            "Pick one to inspect its transport and connections.");
        DrawHelpTextRow("Transport",
            "Cumulative bytes/packets the ENet host has sent/received, plus bytes/sec graphs sampled every quarter second over the last minute.");
        DrawHelpTextRow("Validation",
            "Surfaces problems detected this tick: a transform snapshot approaching or exceeding the 64 KB "
            "frame cap (oversized snapshots are dropped wholesale), high packet loss / RTT, and starving "
            "client interpolation buffers. Green means nothing flagged.");
        DrawHelpTextRow("Replication",
            "What the server actually sent this tick: the transform-snapshot size vs the 64 KB frame cap "
            "(the bar goes yellow at 75%, red once frames drop), peak size, datagram sizes, spawn/despawn/"
            "property-update counts, the role breakdown, and a snapshot-size graph (plot top = the cap).");
        DrawHelpTextRow("Interpolation",
            "Client-side smoothing health: render interp delay, server/client clock offset, and the proxy "
            "sample-ring fill (a low average means stutter under loss).");
        DrawHelpTextRow("Connections",
            "Per-peer round-trip time, measured packet loss, and throughput. On a host, also what each client holds, its property "
            "packets awaiting acknowledgement, and entities queued to go again after a lost packet. Use the Play settings' Network "
            "Simulation to inject latency/loss.");
        DrawHelpTextRow("Debug Draw",
            "Turns the networking views on in the selected world's viewport, including a client preview window that has no View menu of its own.");
        DrawHelpTextRow("Lag Compensation",
            "The host's hitbox history: how many hitboxes, how much of the rewind window is recorded, and the fastest hitbox between frames.");
        DrawHelpTextRow("Simulation",
            "The deterministic simulation's tick, confirmed tick, rollbacks, replayed ticks and verified checksums, when a session runs.");
        DrawHelpTextRow("Entities",
            "Per-entity replication state (NetGUID, name, role, owner, movement flag, interpolation sample count), and whether its "
            "networking is its own or inherited from a parent. A mirror is a host entity on a client, streamed or flown locally. "
            "Capped to 256 rows and name-filterable so large sessions stay responsive.");
    }
}
