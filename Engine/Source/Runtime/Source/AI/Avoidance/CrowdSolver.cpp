#include "RuntimePCH.h"
#include "CrowdSolver.h"

#include "ORCA.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina::Avoidance
{
    namespace
    {
        constexpr uint32 kSolveGrain = 64;
        constexpr float  kMinCellSize = 0.5f;

        void SolveOne(FCrowdAgents& Agents, int32 i, float InvTimeStep)
        {
            const uint8 Flags = Agents.Flags[i];

            if ((Flags & CrowdFlag::Solve) == 0)
            {
                return;
            }

            if ((Flags & CrowdFlag::Passthrough) != 0)
            {
                Agents.OutVelX[i]      = Agents.PrefVelX[i];
                Agents.OutVelZ[i]      = Agents.PrefVelZ[i];
                Agents.OutNeighbors[i] = 0;
                return;
            }

            FORCAAgent Self;
            Self.PosX     = Agents.PosX[i];
            Self.PosZ     = Agents.PosZ[i];
            Self.VelX     = Agents.VelX[i];
            Self.VelZ     = Agents.VelZ[i];
            Self.Radius   = Agents.Radius[i];
            Self.MaxSpeed = Agents.MaxSpeed[i];

            FNeighborHit Hits[kMaxORCALines];
            const int32  MaxNeighbors = Math::Min(Agents.MaxNeighbors[i], kMaxORCALines);
            const int32  Found = GatherNeighbors(Agents.Grid, i, Self.PosX, Self.PosZ,
                                                 Agents.NeighborRange[i],
                                                 MaxNeighbors, Hits);

            FORCALine   Lines[kMaxORCALines];
            const float InvHorizon     = Agents.InvTimeHorizon[i];
            const float Responsibility = Agents.Responsibility[i];

            for (int32 n = 0; n < Found; ++n)
            {
                const int32 Other = Hits[n].Index;

                FORCAAgent Neighbor;
                Neighbor.PosX   = Agents.PosX[Other];
                Neighbor.PosZ   = Agents.PosZ[Other];
                Neighbor.VelX   = Agents.VelX[Other];
                Neighbor.VelZ   = Agents.VelZ[Other];
                Neighbor.Radius = Agents.Radius[Other];

                // A neighbor that ignores others takes none of the correction, so this agent takes it all.
                const float Share = (Agents.Flags[Other] & CrowdFlag::Passthrough) != 0
                    ? 1.0f
                    : Responsibility;

                BuildAgentLine(Self, Neighbor, InvHorizon, InvTimeStep, Share, Lines[n]);
            }

            SolveVelocity(Lines, Found, Self.MaxSpeed,
                          Agents.PrefVelX[i], Agents.PrefVelZ[i],
                          Agents.OutVelX[i], Agents.OutVelZ[i]);

            Agents.OutNeighbors[i] = Found;
        }
    }

    void FCrowdAgents::Reset(int32 Count)
    {
        PosX.SetNum(Count);
        PosZ.SetNum(Count);
        VelX.SetNum(Count);
        VelZ.SetNum(Count);
        PrefVelX.SetNum(Count);
        PrefVelZ.SetNum(Count);
        Radius.SetNum(Count);
        MaxSpeed.SetNum(Count);
        InvTimeHorizon.SetNum(Count);
        NeighborRange.SetNum(Count);
        Responsibility.SetNum(Count);
        MaxNeighbors.SetNum(Count);
        Flags.SetNum(Count);
        OutVelX.SetNum(Count);
        OutVelZ.SetNum(Count);
        OutNeighbors.SetNum(Count);
    }

    void SolveCrowd(FCrowdAgents& Agents, int32 Count, float InvTimeStep, bool bAllowParallel)
    {
        if (Count <= 0)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        float CellSize = kMinCellSize;
        for (int32 i = 0; i < Count; ++i)
        {
            CellSize = Math::Max(CellSize, Agents.NeighborRange[i]);
        }

        Agents.Grid.Build(Agents.PosX.GetData(), Agents.PosZ.GetData(), Count, CellSize);

        if (!bAllowParallel || Count < kCrowdParallelThreshold || !Jobs::IsRuntimeRunning())
        {
            for (int32 i = 0; i < Count; ++i)
            {
                SolveOne(Agents, i, InvTimeStep);
            }
            return;
        }

        Jobs::ParallelFor((uint32)Count, [&](const Jobs::FParallelRange& Range)
        {
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                SolveOne(Agents, (int32)i, InvTimeStep);
            }
        }, kSolveGrain);
    }
}
