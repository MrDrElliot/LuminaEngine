#include "RuntimePCH.h"
#include "NetDebugDrawSystem.h"

#include "Components/NetworkComponent.h"
#include "Components/RepTransformComponent.h"
#include "Config/NetworkSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Net/LagCompensation.h"
#include "Net/NetWorldState.h"
#include "Platform/Time/PlatformTime.h"
#include "Systems/NetMovementInterpSystem.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/DebugDrawSystem.h"
#include "World/Subsystems/DebugSubsystem.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        constexpr EDebugDrawNetworking kNetworkViews = EDebugDrawNetworking::LagCompensation | EDebugDrawNetworking::Proxies | EDebugDrawNetworking::Stats
            | EDebugDrawNetworking::Interest;

        constexpr double kRateSampleSeconds = 0.5;

        const FVector4 kNowColor(0.2f, 1.0f, 0.3f, 1.0f);
        const FVector4 kRewoundColor(1.0f, 0.3f, 0.3f, 0.6f);
        const FVector4 kTrailColor(1.0f, 0.8f, 0.2f, 0.8f);
        const FVector4 kSampleColor(0.3f, 0.7f, 1.0f, 1.0f);
        const FVector4 kExtrapolateColor(1.0f, 0.2f, 0.2f, 1.0f);
        const FVector4 kTextColor(0.85f, 0.9f, 1.0f, 1.0f);
        const FVector4 kCellColor(0.30f, 0.55f, 1.0f, 1.0f);
        const FVector4 kEnterColor(0.2f, 1.0f, 0.3f, 1.0f);
        const FVector4 kLeaveColor(1.0f, 0.9f, 0.2f, 1.0f);

        // Near, mid, far and culled, the last never expected among what a client holds.
        const FVector4 kTierColors[4] = { FVector4(1.0f, 0.25f, 0.25f, 1.0f), FVector4(1.0f, 0.85f, 0.2f, 1.0f),
                                          FVector4(0.3f, 1.0f, 0.45f, 1.0f), FVector4(0.5f, 0.5f, 0.5f, 1.0f) };

        FString FormatRate(float BytesPerSecond)
        {
            return BytesPerSecond < 1024.0f ? Format("{:.0f} B/s", BytesPerSecond) : Format("{:.1f} KB/s", BytesPerSecond / 1024.0f);
        }

        const char* NetModeName(ENetMode Mode)
        {
            switch (Mode)
            {
                case ENetMode::Client:          return "client";
                case ENetMode::ListenServer:    return "listen server";
                case ENetMode::DedicatedServer: return "dedicated server";
                default:                        return "standalone";
            }
        }
    }

    void CNetDebugDrawSystem::Configure()
    {
        #if !defined(LE_SHIPPING)
        RequireUpdate(EUpdateStage::FrameEnd);
        #endif
    }

    bool CNetDebugDrawSystem::HasWork(const SSystemContext& Context)
    {
        const FDebugDrawState* State = DebugDraw::GetState(GetOwningWorld());
        return State != nullptr && State->Draws(kNetworkViews);
    }

    void CNetDebugDrawSystem::OnExclusiveUpdate(const SExclusiveContext& Context)
    {
        const FDebugDrawState& State = *DebugDraw::GetState(Context.GetWorld());
        if (State.Draws(EDebugDrawNetworking::Stats))
        {
            DrawStats(Context);
        }
        if (State.Draws(EDebugDrawNetworking::LagCompensation))
        {
            DrawLagCompensation(Context);
        }
        if (State.Draws(EDebugDrawNetworking::Proxies))
        {
            DrawProxies(Context);
        }
        if (State.Draws(EDebugDrawNetworking::Interest))
        {
            DrawInterest(Context);
        }
    }

    void CNetDebugDrawSystem::DrawInterest(const SExclusiveContext& Context)
    {
        CWorld* World = Context.GetWorld();
        CDebugSubsystem* Draw = World->GetDebug();
        ECS::FRegistry& Registry = Context.GetRegistry();
        const FNetWorldState* Net = Registry.Ctx().Find<FNetWorldState>();
        if (Net == nullptr || !World->IsNetServer())
        {
            Draw->DrawText("Interest management is decided on the host", kTextColor);
            return;
        }

        // Only occupied cells, so a large grid costs what its entities do.
        constexpr float kGroundY = 0.05f;
        const FNetGrid& Grid = Net->Grid;
        if (Grid.CellStart.Num() == Grid.NumCells() + 1)
        {
            for (int32 CellZ = 0; CellZ < Grid.DimZ; ++CellZ)
            {
                for (int32 CellX = 0; CellX < Grid.DimX; ++CellX)
                {
                    const int32 Cell = Grid.CellIndex(CellX, CellZ);
                    if (Grid.CellStart[Cell + 1] <= Grid.CellStart[Cell])
                    {
                        continue;
                    }
                    const FVector3 Corner = Grid.CellOrigin(CellX, CellZ);
                    const FVector3 Center(Corner.x + Grid.CellSize * 0.5f, kGroundY, Corner.z + Grid.CellSize * 0.5f);
                    Draw->DrawBox(Center, FVector3(Grid.CellSize * 0.5f, 0.0f, Grid.CellSize * 0.5f), FQuat::Identity(), kCellColor, 1.0f, -1.0f, false);
                }
            }
        }

        const SDefaultWorldSettings& Settings = World->GetDefaultWorldSettings();
        for (const auto& [Owner, Record] : Net->OwnerToRecord)
        {
            if (Record < Net->Extract.Num())
            {
                Draw->DrawCircle(Net->Extract.Pos[Record], FVector3(0.0f, 1.0f, 0.0f), Settings.AOIEnterRadius, kEnterColor, 48, 1.5f, -1.0f, false);
                Draw->DrawCircle(Net->Extract.Pos[Record], FVector3(0.0f, 1.0f, 0.0f), Settings.AOILeaveRadius, kLeaveColor, 48, 1.5f, -1.0f, false);
            }
        }

        uint32 Held = 0;
        for (const auto& [Connection, View] : Net->ClientViews)
        {
            for (const auto& [Guid, Entry] : View.Relevant)
            {
                const ECS::FEntity Entity = Net->GuidTable.Find(SNetGUID{ Guid });
                const STransformComponent* Transform = Registry.IsValid(Entity) ? Registry.TryGet<ECS::Read<STransformComponent>>(Entity) : nullptr;
                if (Transform != nullptr)
                {
                    Draw->DrawPoint(Transform->GetWorldLocationCached(), 0.5f, kTierColors[static_cast<int32>(Entry.Tier) & 3], 2.0f, -1.0f, false);
                    ++Held;
                }
            }
        }
        Draw->DrawText(Format("Interest | {} clients | {} entities held | grid {} x {} of {:.0f} m | enter {:.0f} m, leave {:.0f} m",
            Net->ClientViews.Num(), Held, Grid.DimX, Grid.DimZ, Grid.CellSize, Settings.AOIEnterRadius, Settings.AOILeaveRadius), kTextColor);
    }

    void CNetDebugDrawSystem::DrawLagCompensation(const SExclusiveContext& Context)
    {
        CDebugSubsystem* Draw = Context.GetWorld()->GetDebug();
        const FLagCompensationHistory* History = Context.GetRegistry().Ctx().Find<FLagCompensationHistory>();
        if (History == nullptr || History->Count == 0)
        {
            Draw->DrawText(Context.GetWorld()->IsNetServer() ? "Lag compensation, no hitboxes recorded" : "Lag compensation is judged on the host", kTextColor);
            return;
        }

        const FLagCompensationFrame& Newest = History->FrameAt(0);
        const FLagCompensationFrame& Oldest = History->FrameAt(History->Count - 1);
        Draw->DrawText(Format("Lag compensation | {} hitboxes | {} frames | window {:.0f} ms of {:.0f} ms", Newest.Samples.Num(), History->Count,
            (Newest.Time - Oldest.Time) * 1000.0, History->MaxRewindSeconds * 1000.0), kTextColor);

        // Green where a hitbox is now, red where the furthest shot back would find it, joined so the rewind distance is visible.
        for (const FLagCompensationSample& Now : Newest.Samples)
        {
            Draw->DrawCapsule(Now.A, Now.B, Now.Radius, kNowColor, 1.5f, -1.0f, false);
            if (const FLagCompensationSample* Then = Oldest.Find(Now.Entity))
            {
                Draw->DrawCapsule(Then->A, Then->B, Then->Radius, kRewoundColor, 1.0f, -1.0f, false);
                Draw->DrawLine((Then->A + Then->B) * 0.5f, (Now.A + Now.B) * 0.5f, kTrailColor, 1.0f, -1.0f, false);
            }
        }
    }

    void CNetDebugDrawSystem::DrawProxies(const SExclusiveContext& Context)
    {
        ECS::FRegistry& Registry = Context.GetRegistry();
        CDebugSubsystem* Draw = Context.GetWorld()->GetDebug();
        const FNetInterpClock* Clock = Registry.Ctx().Find<FNetInterpClock>();
        if (Clock == nullptr || !Clock->bInitialized)
        {
            Draw->DrawText(Context.GetWorld()->IsNetServer() ? "Proxies are interpolated on clients" : "Proxies, nothing received from a server yet", kTextColor);
            return;
        }

        const float InterpDelay = GetDefault<CNetworkSettings>()->InterpDelay;
        int32 Proxies = 0;
        int32 Extrapolating = 0;
        double DelaySum = 0.0;
        Registry.View<SNetworkComponent, FRepTransform, STransformComponent>().ForEach(
            [&](ECS::FEntity, const SNetworkComponent& Net, const FRepTransform& Rep, const STransformComponent& Transform)
        {
            if (Net.LocalRole != ENetRole::SimulatedProxy || Rep.Ring.Count == 0)
            {
                return;
            }

            const double Delay = Rep.SmoothedInterpDelay > 0.0 ? Rep.SmoothedInterpDelay : (double)InterpDelay;
            const double RenderTime = Clock->ServerPlaybackTime - Delay;
            const FNetInterpSample& Newest = Rep.Ring.Newest();
            const bool bExtrapolating = RenderTime > Newest.Time;

            // The rendered pose trails the newest sample by the interpolation delay, red once it runs past it.
            const FVector3 Rendered = Transform.GetWorldLocationCached();
            const FVector4 Color = bExtrapolating ? kExtrapolateColor : kSampleColor;
            for (int32 Index = 0; Index < Rep.Ring.Count; ++Index)
            {
                Draw->DrawPoint(Rep.Ring.Logical(Index).Pos, 0.06f, kSampleColor, 1.0f, -1.0f, false);
            }
            Draw->DrawSphere(Newest.Pos, 0.12f, Color, 1.0f, -1.0f, false);
            Draw->DrawLine(Rendered, Newest.Pos, Color, 1.5f, -1.0f, false);

            ++Proxies;
            Extrapolating += bExtrapolating ? 1 : 0;
            DelaySum += Delay;
        });

        Draw->DrawText(Format("Proxies {} | extrapolating {} | mean delay {:.0f} ms | server time {:.3f}", Proxies, Extrapolating,
            Proxies > 0 ? DelaySum / Proxies * 1000.0 : 0.0, Clock->LatestServerTime), Extrapolating > 0 ? kExtrapolateColor : kTextColor);
    }

    void CNetDebugDrawSystem::DrawStats(const SExclusiveContext& Context)
    {
        CWorld* World = Context.GetWorld();
        CDebugSubsystem* Draw = World->GetDebug();
        const FNetWorldState* Net = Context.GetRegistry().Ctx().Find<FNetWorldState>();
        if (Net == nullptr || Net->Transport == nullptr)
        {
            Draw->DrawText(Format("Network {} | not connected", NetModeName(World->GetNetMode())), kTextColor);
            return;
        }

        const FNetworkStats Stats = Net->Transport->GetStats();
        const double Now = PlatformTime::Seconds();
        if (Now - LastSampleTime >= kRateSampleSeconds || Now < LastSampleTime)
        {
            const float Seconds = (float)Math::Max(Now - LastSampleTime, 1e-3);
            const bool bFirstSample = LastSampleTime <= 0.0 || Stats.TotalSentBytes < LastSentBytes;
            SentPerSecond     = bFirstSample ? 0.0f : (float)(Stats.TotalSentBytes - LastSentBytes) / Seconds;
            ReceivedPerSecond = bFirstSample ? 0.0f : (float)(Stats.TotalReceivedBytes - LastReceivedBytes) / Seconds;
            LastSentBytes     = Stats.TotalSentBytes;
            LastReceivedBytes = Stats.TotalReceivedBytes;
            LastSampleTime    = Now;
        }

        Draw->DrawText(Format("Network {} | clients {} | up {} | down {} | property packets lost {}", NetModeName(World->GetNetMode()), Net->ConnectedClients,
            FormatRate(SentPerSecond), FormatRate(ReceivedPerSecond), Net->Stats.PropertyPacketsLost), kTextColor);

        TVector<FConnectionStats> Connections;
        Net->Transport->GetConnectionStats(Connections);
        for (const FConnectionStats& Connection : Connections)
        {
            const FVector4 Color = Connection.PacketLoss > 0.02f || Connection.RoundTripTimeMs > 150 ? kExtrapolateColor : kTextColor;
            Draw->DrawText(Format("  connection {} | rtt {} ms | loss {:.1f}%", Connection.ConnectionId, Connection.RoundTripTimeMs,
                Connection.PacketLoss * 100.0f), Color);
        }
    }
}
