#include "RuntimePCH.h"
#include "NetMovementInterpSystem.h"

#include "SystemContext.h"
#include "TaskSystem/TaskSystem.h"
#include "Config/NetworkSettings.h"
#include "Core/Object/ObjectCore.h"
#include "World/Net/NetWorldState.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/RepTransformComponent.h"
#include "World/Entity/Components/NetworkComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    // Disjoint write set so the scheduler can overlap this with other PostPhysics systems that don't touch
    // transforms. Dirty-tagging (FNeedsTransformUpdate) happens in the serial tail, covered by the transform
    // write domain (same convention as SAnimationSystem's root-motion pass). FRelationshipComponent is read
    // by the ResolveAllDirtyTransforms parent-chain walk.
    FSystemAccess SNetMovementInterpSystem::Access = FSystemAccess{}
        .Write<STransformComponent>()
        .Write<FRepTransform>()               // per-entity SmoothedInterpDelay is updated in the parallel body
        .Read<SNetworkComponent, FRelationshipComponent>();

    void SNetMovementInterpSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        entt::registry& Registry = Context.GetRegistry();

        // No net state => not a networked world; nothing to interpolate.
        FNetWorldState* State = Registry.ctx().find<FNetWorldState>();
        if (State == nullptr)
        {
            return;
        }

        auto View   = Registry.view<FRepTransform>();
        auto Handle = View.handle();
        const uint32 Count = static_cast<uint32>(Handle->size());
        if (Count == 0)
        {
            return;
        }

        //~ Serial pre-step. RenderTime and the clock offset are global render-clock state; computing them in
        //  the parallel body would race. Track the server/client offset with a gentle EMA so RenderTime stays
        //  ~InterpDelay behind the newest received server time and advances with the local frame clock.
        const CNetworkSettings* Settings = GetDefault<CNetworkSettings>();
        const double InterpDelay     = Settings ? static_cast<double>(Settings->InterpDelay) : 0.1;
        const bool   bExtrapolate    = Settings ? Settings->bEnableExtrapolation : true;
        const double MaxExtrap       = Settings ? static_cast<double>(Settings->MaxExtrapolation) : 0.25;
        const double BufferIntervals = Settings ? static_cast<double>(Settings->InterpBufferIntervals) : 1.5;

        // Smooth render clock: advance by local dt and gently rate-match the newest server time, so the clock
        // doesn't saw-tooth with the (bursty) send cadence the way an offset-EMA does. Snap only on a big desync.
        const double Dt = Context.GetDeltaTime();
        if (!State->bClockInitialized)
        {
            State->ServerPlaybackTime = State->LatestServerTime;
            State->bClockInitialized  = true;
        }
        else
        {
            State->ServerPlaybackTime += Dt;
            const double Error = State->LatestServerTime - State->ServerPlaybackTime;
            if (Error > 0.5 || Error < -0.5)
            {
                State->ServerPlaybackTime = State->LatestServerTime; // join / big hitch -> resync
            }
            else
            {
                const double Rate = Dt * 4.0; // close drift over ~0.25s
                State->ServerPlaybackTime += Error * (Rate < 1.0 ? Rate : 1.0);
            }
        }
        // The per-entity interp delay is subtracted in the parallel body so each proxy stays behind its OWN rate.
        const double ServerRenderNow = State->ServerPlaybackTime;

        auto&& RepStorage       = Registry.storage<FRepTransform>();
        auto&& NetStorage       = Registry.storage<SNetworkComponent>();
        auto&& TransformStorage = Registry.storage<STransformComponent>();

        // True for any entity whose pose we should drive from its sample ring. AutonomousProxy is locally
        // controlled (its ring is never filled, but skip explicitly); empty rings have no data yet.
        auto ShouldInterp = [&](entt::entity Entity) -> bool
        {
            if (!RepStorage.contains(Entity) || !NetStorage.contains(Entity) || !TransformStorage.contains(Entity))
            {
                return false;
            }
            if (RepStorage.get(Entity).Ring.Count == 0)
            {
                return false;
            }
            return NetStorage.get(Entity).LocalRole != ENetRole::AutonomousProxy;
        };
        
        Task::ParallelFor(Count, [&](const Task::FParallelRange& Range)
        {
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                const entt::entity Entity = (*Handle)[i];
                if (!ShouldInterp(Entity))
                {
                    continue;
                }

                FRepTransform&       Rep = RepStorage.get(Entity);
                STransformComponent& T   = TransformStorage.get(Entity);

                // Adaptive delay: stay ~BufferIntervals send-intervals behind THIS entity's sample rate, so a
                // low-rate (LOD-far) proxy interpolates between real samples instead of extrapolating + snapping.
                // Eased per tick so a tier/rate change ramps the delay rather than rewinding the render clock.
                const double Interval = Rep.Ring.AverageInterval();
                double TargetDelay = InterpDelay;
                if (Interval > 0.0 && BufferIntervals * Interval > TargetDelay)
                {
                    TargetDelay = BufferIntervals * Interval;
                }
                if (Rep.SmoothedInterpDelay < 0.0)
                {
                    Rep.SmoothedInterpDelay = TargetDelay;
                }
                else
                {
                    Rep.SmoothedInterpDelay += (TargetDelay - Rep.SmoothedInterpDelay) * 0.1;
                }
                const double RenderTime = ServerRenderNow - Rep.SmoothedInterpDelay;

                FVector3 Pos;
                FQuat    Rot;
                Rep.Ring.Evaluate(RenderTime, Pos, Rot, bExtrapolate, MaxExtrap);

                // Scale isn't interpolated; apply the latest replicated value, else keep the local scale.
                const FVector3 Scale = Rep.bHasScale ? Rep.CurrentScaleQ.ToVector(NetQuantize::ScaleQuantum)
                                                     : T.GetLocalScale();
                T.SetRaw(Pos, Rot, Scale);
            }
        }, 16);

        //~ Serial tail (Jolt ApplyInterpolatedTransforms pattern). Structural dirty-tagging + one bulk resolve.
        //  Tagging a different pool (FNeedsTransformUpdate) doesn't invalidate the FRepTransform handle.
        for (uint32 i = 0; i < Count; ++i)
        {
            const entt::entity Entity = (*Handle)[i];
            if (ShouldInterp(Entity))
            {
                Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
            }
        }
        ECS::Utils::ResolveAllDirtyTransforms(Registry);
    }
}
