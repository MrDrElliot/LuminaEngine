#include "RuntimePCH.h"
#include "Systems/NetMovementInterpSystem.h"
#include "World/ECS/Registry.h"

#include "World/Entity/Systems/SystemContext.h"
#include "Config/NetworkSettings.h"
#include "Core/Object/ObjectCore.h"
#include "World/Entity/EntityUtils.h"
#include "Components/RepTransformComponent.h"
#include "Components/NetworkComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/AudioSystem.h"
#include "World/Entity/Systems/EntityScriptSystem.h"

namespace Lumina
{
    void CNetMovementInterpSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PostPhysics);
        // Remote entities are placed before anything reads where they are this frame.
        RunsBefore<CAudioSystem, CEntityScriptSystem>();
    }

    void CNetMovementInterpSystem::OnUpdate(const SSystemContext& Context)
    {
        if (!Clock->bNetworked)
        {
            return;
        }

        //~ Rate-matching avoids the saw-tooth an offset EMA gives on a bursty send cadence.
        const CNetworkSettings* Settings = GetDefault<CNetworkSettings>();
        const double InterpDelay     = Settings ? static_cast<double>(Settings->InterpDelay) : 0.1;
        const bool   bExtrapolate    = Settings ? Settings->bEnableExtrapolation : true;
        const double MaxExtrap       = Settings ? static_cast<double>(Settings->MaxExtrapolation) : 0.25;
        const double BufferIntervals = Settings ? static_cast<double>(Settings->InterpBufferIntervals) : 1.5;
        const float  TeleportDistance = Settings ? Settings->TeleportDistance : 3.0f;

        // Tracks the offset with a gentle EMA so RenderTime stays behind the newest server time.
        FNetInterpClock& State = *Clock;
        const double Dt = Context.GetDeltaTime();
        if (!State.bInitialized)
        {
            State.ServerPlaybackTime = State.LatestServerTime;
            State.bInitialized       = true;
        }
        else
        {
            State.ServerPlaybackTime += Dt;
            const double Error = State.LatestServerTime - State.ServerPlaybackTime;
            if (Error > 0.5 || Error < -0.5)
            {
                State.ServerPlaybackTime = State.LatestServerTime; // join / big hitch -> resync
            }
            else
            {
                const double Rate = Dt * 4.0; // close drift over ~0.25s
                State.ServerPlaybackTime += Error * (Rate < 1.0 ? Rate : 1.0);
            }
        }

        // The per-entity interp delay is subtracted in the parallel body so each proxy stays behind its OWN rate.
        const double ServerRenderNow = State.ServerPlaybackTime;

        Proxies.ParallelForEach([&](ECS::FEntity Entity, ECS::TMut<FRepTransform> RepMut, ECS::TMut<STransformComponent> Transform, const SNetworkComponent& Net)
        {
            const FRepTransform& Rep = RepMut.Get();
            // An AutonomousProxy is locally controlled, and an empty ring has no data yet.
            if (Rep.Ring.Count == 0 || Net.LocalRole == ENetRole::AutonomousProxy)
            {
                return;
            }

            // Eased over time, so a tier change ramps the delay rather than rewinding the render clock.
            const double Interval = Rep.Ring.AverageInterval();
            double TargetDelay = InterpDelay;
            if (Interval > 0.0 && BufferIntervals * Interval > TargetDelay)
            {
                TargetDelay = BufferIntervals * Interval;
            }
            RepMut.SetIfChanged(&FRepTransform::SmoothedInterpDelay, Rep.EasedInterpDelay(TargetDelay, Dt));
            const double RenderTime = ServerRenderNow - Rep.SmoothedInterpDelay;

            FVector3 Pos;
            FQuat    Rot;
            Rep.Ring.Evaluate(RenderTime, Pos, Rot, bExtrapolate, MaxExtrap, TeleportDistance);

            // Scale isn't interpolated; apply the latest replicated value, else keep the local scale.
            const STransformComponent& T = Transform.Get();
            const FVector3 Scale = Rep.bHasScale ? Rep.CurrentScaleQ.ToVector(NetQuantize::ScaleQuantum)
                                                 : T.GetLocalScale();
            // A settled proxy would otherwise queue a transform resolve every frame for a pose that did not move.
            if (Pos == T.GetLocalLocation() && Rot == T.GetLocalRotation() && Scale == T.GetLocalScale())
            {
                return;
            }
            Transform->SetRaw(Pos, Rot, Scale);
            Context.Emplace<FNeedsTransformUpdate>(Entity);
        }, 16u);
    }
}
