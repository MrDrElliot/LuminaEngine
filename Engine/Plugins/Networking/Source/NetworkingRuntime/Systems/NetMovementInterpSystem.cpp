#include "RuntimePCH.h"
#include "Systems/NetMovementInterpSystem.h"
#include "World/ECS/Registry.h"

#include "World/Entity/Systems/SystemContext.h"
#include "TaskSystem/TaskSystem.h"
#include "Config/NetworkSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Net/NetWorldState.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "Components/RepTransformComponent.h"
#include "Components/NetworkComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    // Dirty-tagging happens in the serial tail, covered by the transform write domain.
    FSystemAccess SNetMovementInterpSystem::Access = FSystemAccess{}
        .Write<STransformComponent>()
        .Write<FRepTransform>()               // per-entity SmoothedInterpDelay is updated in the parallel body
        .Read<SNetworkComponent, FRelationshipComponent>();

    void SNetMovementInterpSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = Context.GetRegistry();

        // No net state => not a networked world; nothing to interpolate.
        FNetWorldState* State = Registry.Ctx().Find<FNetWorldState>();
        if (State == nullptr)
        {
            return;
        }

        auto View   = Registry.View<FRepTransform>();
        auto Handle = View.GetDriver();
        const uint32 Count = static_cast<uint32>(Handle->GetDenseSize());
        if (Count == 0)
        {
            return;
        }

        //~ Rate-matching avoids the saw-tooth an offset EMA gives on a bursty send cadence.
        const CNetworkSettings* Settings = GetDefault<CNetworkSettings>();
        const double InterpDelay     = Settings ? static_cast<double>(Settings->InterpDelay) : 0.1;
        const bool   bExtrapolate    = Settings ? Settings->bEnableExtrapolation : true;
        const double MaxExtrap       = Settings ? static_cast<double>(Settings->MaxExtrapolation) : 0.25;
        const double BufferIntervals = Settings ? static_cast<double>(Settings->InterpBufferIntervals) : 1.5;

        // Tracks the offset with a gentle EMA so RenderTime stays behind the newest server time.
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

        auto RepStorage       = Registry.GetStorage<FRepTransform>();
        auto NetStorage       = Registry.GetStorage<SNetworkComponent>();
        auto TransformStorage = Registry.GetStorage<STransformComponent>();

        // An AutonomousProxy is locally controlled, and an empty ring has no data yet.
        auto ShouldInterp = [&](ECS::FEntity Entity) -> bool
        {
            const FRepTransform* Rep = RepStorage.TryGet(Entity);
            const SNetworkComponent* Net = NetStorage.TryGet(Entity);
            if (Rep == nullptr || Net == nullptr || !TransformStorage.Contains(Entity))
            {
                return false;
            }
            if (Rep->Ring.Count == 0)
            {
                return false;
            }
            return Net->LocalRole != ENetRole::AutonomousProxy;
        };
        
        Task::ParallelFor(Count, [&](const Task::FParallelRange& Range)
        {
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                const ECS::FEntity Entity = Handle->GetDenseData()[i];
                if (!ShouldInterp(Entity))
                {
                    continue;
                }

                FRepTransform&       Rep = RepStorage.Get(Entity);
                STransformComponent& T   = TransformStorage.Get(Entity);

                // Eased per tick, so a tier change ramps the delay rather than rewinding the render clock.
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

        //~ Tagging a different pool does not invalidate the FRepTransform handle.
        for (uint32 i = 0; i < Count; ++i)
        {
            const ECS::FEntity Entity = Handle->GetDenseData()[i];
            if (ShouldInterp(Entity))
            {
                Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
            }
        }
        ECS::Utils::ResolveAllDirtyTransforms(Registry);
    }
}
