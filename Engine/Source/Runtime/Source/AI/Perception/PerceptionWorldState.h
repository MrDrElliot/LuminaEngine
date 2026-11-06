#pragma once

#include "World/ECS/Registry.h"

#include "Core/Threading/Thread.h"
#include "Core/Math/Math.h"
#include "Containers/Vector.h"
#include "AI/Avoidance/AvoidanceGrid.h"
#include "AI/Perception/PerceptionTypes.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "World/Entity/Systems/SystemResources.h"

namespace Lumina
{
    // A perceivable source gathered each tick, whose AffiliationTags point into the live SAIStimuliSourceComponent for that tick only.
    struct FPerceptionSource
    {
        ECS::FEntity                    Entity              = ECS::NullEntity;
        FVector3                        AimPoint            = FVector3(0.0f);   // sight target point (origin + SightTargetOffset).
        const SHierarchicalTagContainer*    AffiliationTags     = nullptr;
        uint8                           RegisteredSenses    = 0;                // EAISenseChannel bits.
    };

    // Built serially each tick over the sources' aim points, then queried read-only from the parallel sense pass.
    struct FPerceptionGrid
    {
        Avoidance::FAvoidanceGrid  Grid;
        TVector<float>             AimX;
        TVector<float>             AimZ;
        TVector<FPerceptionSource> Sources;

        void Build(float CellSize)
        {
            const int32 N = Sources.Num();
            AimX.SetNum(N);
            AimZ.SetNum(N);
            for (int32 i = 0; i < N; ++i)
            {
                AimX[i] = Sources[i].AimPoint.x;
                AimZ[i] = Sources[i].AimPoint.z;
            }
            Grid.Build(AimX.GetData(), AimZ.GetData(), N, CellSize);
        }

        // Invoke Func(const FPerceptionSource&) for every source whose cell overlaps the XZ disc of Radius around Center.
        template<typename Fn>
        void ForEachInRadius(const FVector3& Center, float Radius, Fn&& Func) const
        {
            Grid.ForEachRowSpan(Center.x, Center.z, Radius, [&](int32 Begin, int32 End)
            {
                for (int32 i = Begin; i < End; ++i)
                {
                    Func(Sources[Grid.SortedIndices[i]]);
                }
            });
        }
    };

    // Per-world perception scratch in the registry context, out-of-line because FMutex is non-movable.
    struct FPerceptionWorldState : ECS::FResource
    {
        FPerceptionGrid             SourceGrid;
        TVector<FAIStimulusEvent>   PendingStimuli;   // drained (swap-and-clear) by CPerceptionSystem each tick.
        FMutex                  StimuliMutex;     // guards PendingStimuli across the (rare) cross-thread push.
    };

    namespace Perception
    {
        // Get the per-world perception state, creating it on first use (ReportNoise can run before the system ticks).
        inline FPerceptionWorldState& GetOrCreateState(ECS::FRegistry& Registry)
        {
            auto& Ctx = Registry.Ctx();
            if (FPerceptionWorldState* State = Ctx.Find<FPerceptionWorldState>())
            {
                return *State;
            }
            return Ctx.Emplace<FPerceptionWorldState>();
        }

        // Thread-safe enqueue of a hearing/damage report.
        inline void EnqueueStimulus(FPerceptionWorldState& State, const FAIStimulusEvent& Event)
        {
            FScopeLock Lock(State.StimuliMutex);
            State.PendingStimuli.Add(Event);
        }
    }
}
