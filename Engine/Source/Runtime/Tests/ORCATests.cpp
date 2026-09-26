#include "gtest/gtest.h"

#include <cmath>
#include <vector>

#include "AI/Avoidance/AvoidanceGrid.h"
#include "AI/Avoidance/CrowdSolver.h"
#include "AI/Avoidance/ORCA.h"
#include "Core/Math/Math.h"
#include "Platform/Time/PlatformTime.h"

// Drives the avoidance math through its public entry points, so no world, ECS or navmesh is involved.

using namespace Lumina;
using namespace Lumina::Avoidance;

namespace
{
    struct FSimAgent
    {
        float PosX = 0.0f;
        float PosZ = 0.0f;
        float VelX = 0.0f;
        float VelZ = 0.0f;
        float GoalX = 0.0f;
        float GoalZ = 0.0f;
        float Radius = 0.5f;
        float MaxSpeed = 2.0f;
    };

    FORCAAgent ToORCA(const FSimAgent& A)
    {
        FORCAAgent Out;
        Out.PosX     = A.PosX;
        Out.PosZ     = A.PosZ;
        Out.VelX     = A.VelX;
        Out.VelZ     = A.VelZ;
        Out.Radius   = A.Radius;
        Out.MaxSpeed = A.MaxSpeed;
        return Out;
    }

    // Seed mirrors the entity index the system feeds in, so the tests break symmetry the same way.
    void PreferredVelocity(const FSimAgent& A, uint32 Seed, float& OutX, float& OutZ)
    {
        const float DX  = A.GoalX - A.PosX;
        const float DZ  = A.GoalZ - A.PosZ;
        const float Len = Math::Sqrt(DX * DX + DZ * DZ);
        if (Len < 1e-4f)
        {
            OutX = 0.0f;
            OutZ = 0.0f;
            return;
        }
        OutX = DX / Len * A.MaxSpeed;
        OutZ = DZ / Len * A.MaxSpeed;

        ApplySymmetryBreak(Seed, A.MaxSpeed * 0.01f, OutX, OutZ);
    }

    // One ORCA step for every agent against every other, then integrate. Returns the closest approach seen.
    float StepAll(std::vector<FSimAgent>& Agents, float TimeStep, int32 Steps)
    {
        const float InvTimeStep    = 1.0f / TimeStep;
        const float InvTimeHorizon = 1.0f / 2.0f;

        float ClosestGap = 1e30f;

        for (int32 s = 0; s < Steps; ++s)
        {
            std::vector<float> NewVelX(Agents.size());
            std::vector<float> NewVelZ(Agents.size());

            for (size_t i = 0; i < Agents.size(); ++i)
            {
                const FORCAAgent Self = ToORCA(Agents[i]);

                FORCALine Lines[kMaxORCALines];
                int32     NumLines = 0;

                for (size_t j = 0; j < Agents.size() && NumLines < kMaxORCALines; ++j)
                {
                    if (i == j)
                    {
                        continue;
                    }
                    BuildAgentLine(Self, ToORCA(Agents[j]), InvTimeHorizon, InvTimeStep, 0.5f, Lines[NumLines++]);
                }

                float PrefX, PrefZ;
                PreferredVelocity(Agents[i], (uint32)i, PrefX, PrefZ);
                SolveVelocity(Lines, NumLines, Self.MaxSpeed, PrefX, PrefZ, NewVelX[i], NewVelZ[i]);
            }

            for (size_t i = 0; i < Agents.size(); ++i)
            {
                Agents[i].VelX = NewVelX[i];
                Agents[i].VelZ = NewVelZ[i];
                Agents[i].PosX += Agents[i].VelX * TimeStep;
                Agents[i].PosZ += Agents[i].VelZ * TimeStep;
            }

            for (size_t i = 0; i < Agents.size(); ++i)
            {
                for (size_t j = i + 1; j < Agents.size(); ++j)
                {
                    const float DX = Agents[i].PosX - Agents[j].PosX;
                    const float DZ = Agents[i].PosZ - Agents[j].PosZ;
                    const float Gap = Math::Sqrt(DX * DX + DZ * DZ) - (Agents[i].Radius + Agents[j].Radius);
                    ClosestGap = Math::Min(ClosestGap, Gap);
                }
            }
        }

        return ClosestGap;
    }

    float DistanceToGoal(const FSimAgent& A)
    {
        const float DX = A.GoalX - A.PosX;
        const float DZ = A.GoalZ - A.PosZ;
        return Math::Sqrt(DX * DX + DZ * DZ);
    }
}

TEST(ORCA, UnconstrainedAgentTakesItsPreferredVelocity)
{
    float OutX = 0.0f;
    float OutZ = 0.0f;
    SolveVelocity(nullptr, 0, 3.0f, 1.5f, -0.5f, OutX, OutZ);

    EXPECT_FLOAT_EQ(OutX, 1.5f);
    EXPECT_FLOAT_EQ(OutZ, -0.5f);
}

TEST(ORCA, PreferredVelocityIsClampedToMaxSpeed)
{
    float OutX = 0.0f;
    float OutZ = 0.0f;
    SolveVelocity(nullptr, 0, 1.0f, 30.0f, 40.0f, OutX, OutZ);

    EXPECT_NEAR(Math::Sqrt(OutX * OutX + OutZ * OutZ), 1.0f, 1e-4f);
    EXPECT_NEAR(OutX, 0.6f, 1e-4f);
    EXPECT_NEAR(OutZ, 0.8f, 1e-4f);
}

TEST(ORCA, HeadOnPairPassesWithoutOverlapping)
{
    std::vector<FSimAgent> Agents(2);
    Agents[0] = FSimAgent{-5.0f, 0.0f, 0.0f, 0.0f,  5.0f, 0.0f, 0.5f, 2.0f};
    Agents[1] = FSimAgent{ 5.0f, 0.0f, 0.0f, 0.0f, -5.0f, 0.0f, 0.5f, 2.0f};

    const float ClosestGap = StepAll(Agents, 1.0f / 30.0f, 300);

    EXPECT_GT(ClosestGap, -0.02f);
    EXPECT_LT(DistanceToGoal(Agents[0]), 0.5f);
    EXPECT_LT(DistanceToGoal(Agents[1]), 0.5f);
}

TEST(ORCA, PerpendicularCrossingClears)
{
    std::vector<FSimAgent> Agents(2);
    Agents[0] = FSimAgent{-5.0f,  0.0f, 0.0f, 0.0f, 5.0f,  0.0f, 0.5f, 2.0f};
    Agents[1] = FSimAgent{ 0.0f, -5.0f, 0.0f, 0.0f, 0.0f,  5.0f, 0.5f, 2.0f};

    const float ClosestGap = StepAll(Agents, 1.0f / 30.0f, 300);

    EXPECT_GT(ClosestGap, -0.02f);
    EXPECT_LT(DistanceToGoal(Agents[0]), 0.5f);
    EXPECT_LT(DistanceToGoal(Agents[1]), 0.5f);
}

// The antipodal circle is the standard ORCA benchmark, and the case a naive solver deadlocks on.
TEST(ORCA, AntipodalCircleResolvesWithoutDeadlock)
{
    constexpr int32 Count  = 8;
    constexpr float Circle = 6.0f;

    std::vector<FSimAgent> Agents((size_t)Count);
    for (int32 i = 0; i < Count; ++i)
    {
        const float Angle = (float)i * (2.0f * 3.14159265f / (float)Count);
        FSimAgent& A = Agents[(size_t)i];
        A.PosX  =  Math::Cos(Angle) * Circle;
        A.PosZ  =  Math::Sin(Angle) * Circle;
        A.GoalX = -A.PosX;
        A.GoalZ = -A.PosZ;
        A.Radius = 0.5f;
        A.MaxSpeed = 2.0f;
    }

    const float ClosestGap = StepAll(Agents, 1.0f / 30.0f, 600);

    EXPECT_GT(ClosestGap, -0.05f);

    for (int32 i = 0; i < Count; ++i)
    {
        EXPECT_LT(DistanceToGoal(Agents[(size_t)i]), 1.0f) << "agent " << i << " never reached its antipode";
    }
}

TEST(ORCA, OverlappingPairIsPushedApart)
{
    FSimAgent A{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 2.0f};
    FSimAgent B{0.1f, 0.0f, 0.0f, 0.0f, 0.1f, 0.0f, 0.5f, 2.0f};

    FORCALine Line;
    BuildAgentLine(ToORCA(A), ToORCA(B), 1.0f / 2.0f, 30.0f, 0.5f, Line);

    float OutX = 0.0f;
    float OutZ = 0.0f;
    SolveVelocity(&Line, 1, A.MaxSpeed, 0.0f, 0.0f, OutX, OutZ);

    // B sits at +X, so the only way out of the overlap is away from it.
    EXPECT_LT(OutX, 0.0f);
}

TEST(AvoidanceGrid, EmptyGridFindsNothing)
{
    FAvoidanceGrid Grid;
    Grid.Build(nullptr, nullptr, 0, 4.0f);

    FNeighborHit Hits[kMaxORCALines];
    EXPECT_EQ(GatherNeighbors(Grid, 0, 0.0f, 0.0f, 10.0f, 8, Hits), 0);
}

TEST(AvoidanceGrid, GathersEveryNeighborInRangeAndExcludesSelf)
{
    constexpr int32 Side = 8;
    std::vector<float> PosX, PosZ;
    for (int32 z = 0; z < Side; ++z)
    {
        for (int32 x = 0; x < Side; ++x)
        {
            PosX.push_back((float)x);
            PosZ.push_back((float)z);
        }
    }

    FAvoidanceGrid Grid;
    Grid.Build(PosX.data(), PosZ.data(), (int32)PosX.size(), 3.0f);

    constexpr int32 Self  = 3 * Side + 3;
    constexpr float Range = 1.5f;

    FNeighborHit Hits[kMaxORCALines];
    const int32 Found = GatherNeighbors(Grid, Self, PosX[Self], PosZ[Self], Range, kMaxORCALines, Hits);

    int32 Expected = 0;
    for (size_t i = 0; i < PosX.size(); ++i)
    {
        if ((int32)i == Self)
        {
            continue;
        }
        const float DX = PosX[i] - PosX[Self];
        const float DZ = PosZ[i] - PosZ[Self];
        if (DX * DX + DZ * DZ <= Range * Range)
        {
            ++Expected;
        }
    }

    EXPECT_EQ(Found, Expected);
    for (int32 i = 0; i < Found; ++i)
    {
        EXPECT_NE(Hits[i].Index, Self);
    }
}

TEST(AvoidanceGrid, NeighborsComeBackNearestFirstAndCapAtMaxCount)
{
    constexpr int32 Side = 10;
    std::vector<float> PosX, PosZ;
    for (int32 z = 0; z < Side; ++z)
    {
        for (int32 x = 0; x < Side; ++x)
        {
            PosX.push_back((float)x * 0.5f);
            PosZ.push_back((float)z * 0.5f);
        }
    }

    FAvoidanceGrid Grid;
    Grid.Build(PosX.data(), PosZ.data(), (int32)PosX.size(), 4.0f);

    constexpr int32 Self = 5 * Side + 5;
    constexpr int32 Cap  = 6;

    FNeighborHit Hits[kMaxORCALines];
    const int32 Found = GatherNeighbors(Grid, Self, PosX[Self], PosZ[Self], 4.0f, Cap, Hits);

    ASSERT_EQ(Found, Cap);
    for (int32 i = 1; i < Found; ++i)
    {
        EXPECT_LE(Hits[i - 1].DistSq, Hits[i].DistSq) << "neighbor " << i << " is nearer than the one before it";
    }

    // The cap must keep the nearest, so nothing outside the kept set may be closer than the furthest kept.
    const float Furthest = Hits[Found - 1].DistSq;
    int32 Nearer = 0;
    for (size_t i = 0; i < PosX.size(); ++i)
    {
        if ((int32)i == Self)
        {
            continue;
        }
        const float DX = PosX[i] - PosX[Self];
        const float DZ = PosZ[i] - PosZ[Self];
        if (DX * DX + DZ * DZ < Furthest - 1e-5f)
        {
            ++Nearer;
        }
    }
    EXPECT_LE(Nearer, Cap);
}

// The SIMD span loop and the scalar tail must agree, so a count straddling eight is the case to pin.
TEST(AvoidanceGrid, SimdAndScalarTailAgree)
{
    for (int32 Count = 1; Count <= 40; ++Count)
    {
        std::vector<float> PosX((size_t)Count), PosZ((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            PosX[(size_t)i] = (float)i * 0.25f;
            PosZ[(size_t)i] = 0.0f;
        }

        FAvoidanceGrid Grid;
        Grid.Build(PosX.data(), PosZ.data(), Count, 1000.0f);

        FNeighborHit Hits[kMaxORCALines];
        const int32 Found = GatherNeighbors(Grid, 0, PosX[0], PosZ[0], 1000.0f, kMaxORCALines, Hits);

        const int32 Expected = Math::Min(Count - 1, kMaxORCALines);
        EXPECT_EQ(Found, Expected) << "count " << Count;
    }
}

// Reports the per-agent cost of bin plus gather plus solve, so "efficient" is a measured number.
TEST(AvoidanceBenchmark, CrowdScaling)
{
    struct FCase { int32 Count; float Spacing; };
    const FCase Cases[] = { {1000, 1.2f}, {5000, 1.2f}, {10000, 1.2f} };

    for (const FCase& Case : Cases)
    {
        const int32 Side = (int32)Math::Ceil(Math::Sqrt((float)Case.Count));

        std::vector<float> PosX, PosZ, VelX, VelZ, PrefX, PrefZ;
        PosX.reserve((size_t)Case.Count);
        for (int32 i = 0; i < Case.Count; ++i)
        {
            const int32 GX = i % Side;
            const int32 GZ = i / Side;
            PosX.push_back((float)GX * Case.Spacing);
            PosZ.push_back((float)GZ * Case.Spacing);
            VelX.push_back(1.0f);
            VelZ.push_back(0.0f);
            PrefX.push_back(1.5f);
            PrefZ.push_back(0.0f);
        }

        constexpr float Radius      = 0.35f;
        constexpr float MaxSpeed    = 5.0f;
        constexpr float TimeHorizon = 2.0f;
        constexpr int32 Neighbors   = 8;
        const float     Range       = Radius * 2.0f + MaxSpeed * TimeHorizon;

        FAvoidanceGrid Grid;

        PlatformTime::FStopwatch BuildWatch;
        Grid.Build(PosX.data(), PosZ.data(), Case.Count, Range);
        const double BuildMs = BuildWatch.ElapsedMilliseconds();

        int64 TotalNeighbors = 0;

        PlatformTime::FStopwatch SolveWatch;
        for (int32 i = 0; i < Case.Count; ++i)
        {
            FORCAAgent Self;
            Self.PosX     = PosX[(size_t)i];
            Self.PosZ     = PosZ[(size_t)i];
            Self.VelX     = VelX[(size_t)i];
            Self.VelZ     = VelZ[(size_t)i];
            Self.Radius   = Radius;
            Self.MaxSpeed = MaxSpeed;

            FNeighborHit Hits[kMaxORCALines];
            const int32 Found = GatherNeighbors(Grid, i, Self.PosX, Self.PosZ, Range, Neighbors, Hits);
            TotalNeighbors += Found;

            FORCALine Lines[kMaxORCALines];
            for (int32 n = 0; n < Found; ++n)
            {
                const int32 Other = Hits[n].Index;
                FORCAAgent Neighbor;
                Neighbor.PosX   = PosX[(size_t)Other];
                Neighbor.PosZ   = PosZ[(size_t)Other];
                Neighbor.VelX   = VelX[(size_t)Other];
                Neighbor.VelZ   = VelZ[(size_t)Other];
                Neighbor.Radius = Radius;
                BuildAgentLine(Self, Neighbor, 1.0f / TimeHorizon, 60.0f, 0.5f, Lines[n]);
            }

            float OutX = 0.0f;
            float OutZ = 0.0f;
            SolveVelocity(Lines, Found, MaxSpeed, PrefX[(size_t)i], PrefZ[(size_t)i], OutX, OutZ);
        }
        const double SolveMs = SolveWatch.ElapsedMilliseconds();

        printf("[ RVO      ] %5d agents  bin %6.3f ms  solve %7.3f ms  (%6.2f us/agent, %.1f neighbors avg)\n",
               Case.Count, BuildMs, SolveMs,
               SolveMs * 1000.0 / (double)Case.Count,
               (double)TotalNeighbors / (double)Case.Count);

        EXPECT_GT(TotalNeighbors, 0);
    }
}

namespace
{
    // Fills one slot with the defaults the avoidance system would derive for a plain character agent.
    void AddCrowdAgent(FCrowdAgents& Agents, int32 i, float PosX, float PosZ, float PrefX, float PrefZ,
                       uint8 Flags = CrowdFlag::Solve)
    {
        constexpr float Radius      = 0.35f;
        constexpr float MaxSpeed    = 5.0f;
        constexpr float TimeHorizon = 2.0f;

        Agents.PosX[(size_t)i]           = PosX;
        Agents.PosZ[(size_t)i]           = PosZ;
        Agents.VelX[(size_t)i]           = PrefX;
        Agents.VelZ[(size_t)i]           = PrefZ;
        Agents.PrefVelX[(size_t)i]       = PrefX;
        Agents.PrefVelZ[(size_t)i]       = PrefZ;
        Agents.Radius[(size_t)i]         = Radius;
        Agents.MaxSpeed[(size_t)i]       = MaxSpeed;
        Agents.InvTimeHorizon[(size_t)i] = 1.0f / TimeHorizon;
        Agents.NeighborRange[(size_t)i]  = Radius * 2.0f + MaxSpeed * TimeHorizon;
        Agents.Responsibility[(size_t)i] = 0.5f;
        Agents.MaxNeighbors[(size_t)i]   = 8;
        Agents.Flags[(size_t)i]          = Flags;
        Agents.OutVelX[(size_t)i]        = 0.0f;
        Agents.OutVelZ[(size_t)i]        = 0.0f;
        Agents.OutNeighbors[(size_t)i]   = 0;
    }
}

TEST(CrowdSolver, ConvergingPairIsDivertedFromItsPreferredVelocity)
{
    FCrowdAgents Agents;
    Agents.Reset(2);

    AddCrowdAgent(Agents, 0, -3.0f, 0.0f,  4.0f, 0.1f);
    AddCrowdAgent(Agents, 1,  3.0f, 0.0f, -4.0f, -0.1f);

    SolveCrowd(Agents, 2, 60.0f, false);

    EXPECT_EQ(Agents.OutNeighbors[0], 1);
    EXPECT_EQ(Agents.OutNeighbors[1], 1);

    // Closing head-on inside the horizon, so neither may keep the velocity it asked for.
    EXPECT_LT(Agents.OutVelX[0], 4.0f);
    EXPECT_GT(Agents.OutVelX[1], -4.0f);
}

TEST(CrowdSolver, IsolatedAgentKeepsItsPreferredVelocity)
{
    FCrowdAgents Agents;
    Agents.Reset(2);

    AddCrowdAgent(Agents, 0,    0.0f, 0.0f, 3.0f, 0.0f);
    AddCrowdAgent(Agents, 1, 5000.0f, 0.0f, 3.0f, 0.0f);

    SolveCrowd(Agents, 2, 60.0f, false);

    EXPECT_EQ(Agents.OutNeighbors[0], 0);
    EXPECT_FLOAT_EQ(Agents.OutVelX[0], 3.0f);
    EXPECT_FLOAT_EQ(Agents.OutVelZ[0], 0.0f);
}

TEST(CrowdSolver, PassthroughAgentIgnoresItsNeighbors)
{
    FCrowdAgents Agents;
    Agents.Reset(2);

    AddCrowdAgent(Agents, 0, -1.0f, 0.0f,  4.0f, 0.0f, CrowdFlag::Solve | CrowdFlag::Passthrough);
    AddCrowdAgent(Agents, 1,  1.0f, 0.0f, -4.0f, 0.0f);

    SolveCrowd(Agents, 2, 60.0f, false);

    EXPECT_FLOAT_EQ(Agents.OutVelX[0], 4.0f);
    EXPECT_EQ(Agents.OutNeighbors[0], 0);

    // The one that does avoid must shoulder the whole correction rather than half of it.
    EXPECT_GT(Agents.OutVelX[1], -4.0f);
}

TEST(CrowdSolver, AgentWithoutTheSolveFlagHoldsItsPreviousOutput)
{
    FCrowdAgents Agents;
    Agents.Reset(2);

    AddCrowdAgent(Agents, 0, -1.0f, 0.0f, 4.0f, 0.0f, 0);
    AddCrowdAgent(Agents, 1,  1.0f, 0.0f, -4.0f, 0.0f);

    Agents.OutVelX[0] = 1.25f;
    Agents.OutVelZ[0] = -2.5f;
    Agents.OutNeighbors[0] = 7;

    SolveCrowd(Agents, 2, 60.0f, false);

    EXPECT_FLOAT_EQ(Agents.OutVelX[0], 1.25f);
    EXPECT_FLOAT_EQ(Agents.OutVelZ[0], -2.5f);
    EXPECT_EQ(Agents.OutNeighbors[0], 7);
}

// The parallel path must not change a single result, which is what makes the threshold safe to tune.
TEST(CrowdSolver, ParallelAndSerialSolvesAgreeExactly)
{
    constexpr int32 Count = 2048;
    constexpr int32 Side  = 46;

    const auto Fill = [](FCrowdAgents& Agents)
    {
        Agents.Reset((size_t)Count);
        for (int32 i = 0; i < Count; ++i)
        {
            const float X = (float)(i % Side) * 0.9f;
            const float Z = (float)(i / Side) * 0.9f;

            float PrefX = ((i & 1) != 0) ? 3.0f : -3.0f;
            float PrefZ = ((i & 2) != 0) ? 2.0f : -2.0f;
            ApplySymmetryBreak((uint32)i, 0.05f, PrefX, PrefZ);

            AddCrowdAgent(Agents, i, X, Z, PrefX, PrefZ);
        }
    };

    FCrowdAgents Serial;
    FCrowdAgents Parallel;
    Fill(Serial);
    Fill(Parallel);

    SolveCrowd(Serial, Count, 60.0f, false);
    SolveCrowd(Parallel, Count, 60.0f, true);

    int32 Mismatches = 0;
    int32 Constrained = 0;
    for (int32 i = 0; i < Count; ++i)
    {
        if (Serial.OutVelX[(size_t)i] != Parallel.OutVelX[(size_t)i]
            || Serial.OutVelZ[(size_t)i] != Parallel.OutVelZ[(size_t)i]
            || Serial.OutNeighbors[(size_t)i] != Parallel.OutNeighbors[(size_t)i])
        {
            ++Mismatches;
        }
        if (Serial.OutNeighbors[(size_t)i] > 0)
        {
            ++Constrained;
        }
    }

    EXPECT_EQ(Mismatches, 0);
    EXPECT_GT(Constrained, Count / 2) << "the crowd was too sparse to exercise the solve";
}

// A dense pack has no feasible velocity for most agents, so this is the relaxed fallback's path.
TEST(CrowdSolver, DensePackStaysFiniteAndWithinMaxSpeed)
{
    constexpr int32 Count = 512;
    constexpr int32 Side  = 23;

    FCrowdAgents Agents;
    Agents.Reset((size_t)Count);
    for (int32 i = 0; i < Count; ++i)
    {
        // Spaced well inside the combined radius, so nearly every pair starts overlapping.
        const float X = (float)(i % Side) * 0.2f;
        const float Z = (float)(i / Side) * 0.2f;
        AddCrowdAgent(Agents, i, X, Z, 3.0f, 0.0f);
    }

    SolveCrowd(Agents, Count, 60.0f, true);

    for (int32 i = 0; i < Count; ++i)
    {
        const float VX = Agents.OutVelX[(size_t)i];
        const float VZ = Agents.OutVelZ[(size_t)i];

        ASSERT_TRUE(std::isfinite(VX)) << "agent " << i << " solved to a non-finite X velocity";
        ASSERT_TRUE(std::isfinite(VZ)) << "agent " << i << " solved to a non-finite Z velocity";

        const float Speed = Math::Sqrt(VX * VX + VZ * VZ);
        EXPECT_LE(Speed, Agents.MaxSpeed[(size_t)i] + 1e-3f) << "agent " << i << " exceeded its speed cap";
    }
}
