#pragma once

#include <mutex>
#include "Core/Math/Math.h"
#include "Containers/Array.h"
#include "AI/Perception/PerceptionTypes.h"
#include "GameplayTags/GameplayTag.h"
#include "World/Entity/EntityHandle.h"

namespace Lumina
{
    // One perceivable source, gathered into the grid each tick. Tags points into the live
    // SAIStimuliSourceComponent (valid only for the tick the grid was built).
    struct FPerceptionSource
    {
        entt::entity                    Entity              = entt::null;
        FVector3                        AimPoint            = FVector3(0.0f);   // sight target point (origin + SightTargetOffset).
        const FGameplayTagContainer*    AffiliationTags     = nullptr;
        uint32                          BodyID              = ~0u;
        uint8                           RegisteredSenses    = 0;                // EAISenseChannel bits.
    };

    // Uniform XZ grid over the stimulus sources, CSR (counting-sort) layout. Cell c owns SortedIndices in
    // [CellStart[c], CellStart[c+1]). Built serially each tick, queried read-only from the parallel sense
    // pass. Modeled on FNetGrid (NetReplicationGraph.h) but decoupled from the net module.
    struct FPerceptionGrid
    {
        FVector3            Origin   = FVector3(0.0f);
        float               CellSize = 32.0f;
        int32               DimX     = 0;
        int32               DimZ     = 0;
        TVector<int32>      CellStart;       // size NumCells()+1.
        TVector<int32>      SortedIndices;   // into Sources, in cell order.
        TVector<FPerceptionSource> Sources;

        int32 NumCells() const { return DimX * DimZ; }

        void CellCoords(const FVector3& P, int32& OutX, int32& OutZ) const
        {
            int32 cx = (int32)((P.x - Origin.x) / CellSize);
            int32 cz = (int32)((P.z - Origin.z) / CellSize);
            OutX = cx < 0 ? 0 : (cx >= DimX ? DimX - 1 : cx);
            OutZ = cz < 0 ? 0 : (cz >= DimZ ? DimZ - 1 : cz);
        }

        int32 CellIndex(int32 cx, int32 cz) const { return cz * DimX + cx; }

        // CSR build from the already-populated Sources array. Vectors are reused across ticks (cleared, not freed).
        void Build(float InCellSize)
        {
            CellSize = InCellSize > 1.0f ? InCellSize : 1.0f;
            const int32 N = (int32)Sources.size();
            if (N == 0)
            {
                DimX = DimZ = 0;
                CellStart.clear();
                SortedIndices.clear();
                return;
            }

            FVector3 Min = Sources[0].AimPoint;
            FVector3 Max = Sources[0].AimPoint;
            for (int32 i = 1; i < N; ++i)
            {
                const FVector3& P = Sources[i].AimPoint;
                if (P.x < Min.x) Min.x = P.x;
                if (P.z < Min.z) Min.z = P.z;
                if (P.x > Max.x) Max.x = P.x;
                if (P.z > Max.z) Max.z = P.z;
            }

            Origin = Min;
            DimX = (int32)((Max.x - Min.x) / CellSize) + 1;
            DimZ = (int32)((Max.z - Min.z) / CellSize) + 1;
            if (DimX < 1) DimX = 1;
            if (DimZ < 1) DimZ = 1;

            const int32 Cells = DimX * DimZ;
            CellStart.assign((size_t)Cells + 1, 0);

            for (int32 i = 0; i < N; ++i)
            {
                int32 cx, cz;
                CellCoords(Sources[i].AimPoint, cx, cz);
                ++CellStart[(size_t)CellIndex(cx, cz) + 1];
            }
            for (int32 c = 1; c <= Cells; ++c)
            {
                CellStart[c] += CellStart[c - 1];
            }

            SortedIndices.resize((size_t)N);
            TVector<int32> Cursor = CellStart; // running write offset per cell.
            for (int32 i = 0; i < N; ++i)
            {
                int32 cx, cz;
                CellCoords(Sources[i].AimPoint, cx, cz);
                SortedIndices[(size_t)Cursor[CellIndex(cx, cz)]++] = i;
            }
        }

        // Invoke Func(const FPerceptionSource&) for every source whose cell overlaps the XZ disc of Radius
        // around Center. Read-only; safe to call concurrently from the parallel sense pass.
        template<typename Fn>
        void ForEachInRadius(const FVector3& Center, float Radius, Fn&& Func) const
        {
            if (DimX <= 0 || DimZ <= 0)
            {
                return;
            }
            int32 minx, minz, maxx, maxz;
            CellCoords(FVector3(Center.x - Radius, 0.0f, Center.z - Radius), minx, minz);
            CellCoords(FVector3(Center.x + Radius, 0.0f, Center.z + Radius), maxx, maxz);
            for (int32 cz = minz; cz <= maxz; ++cz)
            {
                for (int32 cx = minx; cx <= maxx; ++cx)
                {
                    const int32 Cell = CellIndex(cx, cz);
                    for (int32 i = CellStart[Cell]; i < CellStart[Cell + 1]; ++i)
                    {
                        Func(Sources[(size_t)SortedIndices[i]]);
                    }
                }
            }
        }
    };

    // Per-world perception scratch, stored in the entt registry context. Holds the per-tick source grid and
    // the queue of event-driven hearing/damage reports. Stored out-of-line by entt (std::mutex is non-movable).
    struct FPerceptionWorldState
    {
        FPerceptionGrid             SourceGrid;
        TVector<FAIStimulusEvent>   PendingStimuli;   // drained (swap-and-clear) by SPerceptionSystem each tick.
        std::mutex                  StimuliMutex;     // guards PendingStimuli across the (rare) cross-thread push.
    };

    namespace Perception
    {
        // Get the per-world perception state, creating it on first use (ReportNoise can run before the system ticks).
        inline FPerceptionWorldState& GetOrCreateState(entt::registry& Registry)
        {
            auto& Ctx = Registry.ctx();
            if (FPerceptionWorldState* State = Ctx.find<FPerceptionWorldState>())
            {
                return *State;
            }
            return Ctx.emplace<FPerceptionWorldState>();
        }

        // Thread-safe enqueue of a hearing/damage report.
        inline void EnqueueStimulus(FPerceptionWorldState& State, const FAIStimulusEvent& Event)
        {
            std::lock_guard<std::mutex> Lock(State.StimuliMutex);
            State.PendingStimuli.push_back(Event);
        }
    }
}
