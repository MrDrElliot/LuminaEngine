#pragma once

#include "AvoidanceGrid.h"
#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

// Crowd-wide ORCA solve over a struct of arrays, deliberately free of ECS so it is testable on its own.

namespace Lumina::Avoidance
{
    namespace CrowdFlag
    {
        inline constexpr uint8 Solve       = 1 << 0;
        inline constexpr uint8 Passthrough = 1 << 1;
    }

    // Reused across ticks, so a steady crowd allocates nothing after the first solve.
    struct FCrowdAgents
    {
        TVector<float> PosX;
        TVector<float> PosZ;
        TVector<float> VelX;
        TVector<float> VelZ;
        TVector<float> PrefVelX;
        TVector<float> PrefVelZ;
        TVector<float> Radius;
        TVector<float> MaxSpeed;
        TVector<float> InvTimeHorizon;
        TVector<float> NeighborRange;
        TVector<float> Responsibility;
        TVector<int32> MaxNeighbors;
        TVector<uint8> Flags;

        TVector<float> OutVelX;
        TVector<float> OutVelZ;
        TVector<int32> OutNeighbors;

        FAvoidanceGrid Grid;

        RUNTIME_API void Reset(size_t Count);
    };

    // Agents below this solve on the calling thread, since the dispatch would cost more than the work.
    inline constexpr int32 kCrowdParallelThreshold = 256;

    // Bins then solves [0, Count). Leaves an agent without CrowdFlag::Solve holding its previous output.
    RUNTIME_API void SolveCrowd(FCrowdAgents& Agents, int32 Count, float InvTimeStep, bool bAllowParallel);
}
