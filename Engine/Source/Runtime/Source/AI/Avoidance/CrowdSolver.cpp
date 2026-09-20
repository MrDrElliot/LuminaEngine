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
            const uint8 Flags = Agents.Flags[(size_t)i];

            if ((Flags & CrowdFlag::Solve) == 0)
            {
                return;
            }

            if ((Flags & CrowdFlag::Passthrough) != 0)
            {
                Agents.OutVelX[(size_t)i]      = Agents.PrefVelX[(size_t)i];
                Agents.OutVelZ[(size_t)i]      = Agents.PrefVelZ[(size_t)i];
                Agents.OutNeighbors[(size_t)i] = 0;
                return;
            }

            FORCAAgent Self;
            Self.PosX     = Agents.PosX[(size_t)i];
            Self.PosZ     = Agents.PosZ[(size_t)i];
            Self.VelX     = Agents.VelX[(size_t)i];
            Self.VelZ     = Agents.VelZ[(size_t)i];
            Self.Radius   = Agents.Radius[(size_t)i];
            Self.MaxSpeed = Agents.MaxSpeed[(size_t)i];

            FNeighborHit Hits[kMaxORCALines];
            const int32  Found = GatherNeighbors(Agents.Grid, i, Self.PosX, Self.PosZ,
                                                 Agents.NeighborRange[(size_t)i],
                                                 Agents.MaxNeighbors[(size_t)i], Hits);

            FORCALine   Lines[kMaxORCALines];
            const float InvHorizon     = Agents.InvTimeHorizon[(size_t)i];
            const float Responsibility = Agents.Responsibility[(size_t)i];

            for (int32 n = 0; n < Found; ++n)
            {
                const int32 Other = Hits[n].Index;

                FORCAAgent Neighbor;
                Neighbor.PosX   = Agents.PosX[(size_t)Other];
                Neighbor.PosZ   = Agents.PosZ[(size_t)Other];
                Neighbor.VelX   = Agents.VelX[(size_t)Other];
                Neighbor.VelZ   = Agents.VelZ[(size_t)Other];
                Neighbor.Radius = Agents.Radius[(size_t)Other];

                // A neighbor that ignores others takes none of the correction, so this agent takes it all.
                const float Share = (Agents.Flags[(size_t)Other] & CrowdFlag::Passthrough) != 0
                    ? 1.0f
                    : Responsibility;

                BuildAgentLine(Self, Neighbor, InvHorizon, InvTimeStep, Share, Lines[n]);
            }

            SolveVelocity(Lines, Found, Self.MaxSpeed,
                          Agents.PrefVelX[(size_t)i], Agents.PrefVelZ[(size_t)i],
                          Agents.OutVelX[(size_t)i], Agents.OutVelZ[(size_t)i]);

            Agents.OutNeighbors[(size_t)i] = Found;
        }
    }

    void FCrowdAgents::Reset(size_t Count)
    {
        PosX.resize(Count);
        PosZ.resize(Count);
        VelX.resize(Count);
        VelZ.resize(Count);
        PrefVelX.resize(Count);
        PrefVelZ.resize(Count);
        Radius.resize(Count);
        MaxSpeed.resize(Count);
        InvTimeHorizon.resize(Count);
        NeighborRange.resize(Count);
        Responsibility.resize(Count);
        MaxNeighbors.resize(Count);
        Flags.resize(Count);
        OutVelX.resize(Count);
        OutVelZ.resize(Count);
        OutNeighbors.resize(Count);
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
            CellSize = Math::Max(CellSize, Agents.NeighborRange[(size_t)i]);
        }

        Agents.Grid.Build(Agents.PosX.data(), Agents.PosZ.data(), Count, CellSize);

        if (!bAllowParallel || Count < kCrowdParallelThreshold || GTaskSystem == nullptr)
        {
            for (int32 i = 0; i < Count; ++i)
            {
                SolveOne(Agents, i, InvTimeStep);
            }
            return;
        }

        Task::ParallelFor((uint32)Count, [&](const Task::FParallelRange& Range)
        {
            for (uint32 i = Range.Start; i < Range.End; ++i)
            {
                SolveOne(Agents, (int32)i, InvTimeStep);
            }
        }, kSolveGrain);
    }
}
