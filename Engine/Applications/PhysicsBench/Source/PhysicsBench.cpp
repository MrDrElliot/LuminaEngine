#include "Platform/Time/PlatformTime.h"
#include "Physics/API/Box3D/Box3DTaskBridge.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
#include "Core/Threading/Thread.h"
#include "Core/Threading/Atomic.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/TransformComponent.h"

#include <box3d/box3d.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

namespace PhysicsBench
{
    using namespace Lumina;

    struct FStepStats
    {
        double AverageMs = 0.0;
        double WorstMs   = 0.0;
        double SolveMs   = 0.0;
        double CollideMs = 0.0;
        int32  AwakeBodies = 0;
        int32  TaskCount = 0;
    };

    struct FWorldConfig
    {
        uint32 WorkerCount = 1;
        bool   bUseEngineBridge = false;
    };

    static b3WorldId CreateWorld(const FWorldConfig& Config, Physics::FBox3DTaskBridge* Bridge)
    {
        b3WorldDef Def = b3DefaultWorldDef();
        Def.workerCount = Config.WorkerCount;

        if (Config.bUseEngineBridge)
        {
            Def.enqueueTask = &Physics::FBox3DTaskBridge::EnqueueTask;
            Def.finishTask = &Physics::FBox3DTaskBridge::FinishTask;
            Def.userTaskContext = Bridge;
        }

        return b3CreateWorld(&Def);
    }

    // A grid of spheres over a static floor, which is the cheapest shape pair Box3D has to solve.
    static void PopulateWorld(b3WorldId WorldId, uint32 BodyCount, float Spacing)
    {
        {
            b3BodyDef GroundDef = b3DefaultBodyDef();
            GroundDef.type = b3_staticBody;
            GroundDef.position = { 0.0f, -1.0f, 0.0f };

            const b3BodyId Ground = b3CreateBody(WorldId, &GroundDef);
            const b3BoxHull Floor = b3MakeBoxHull(4000.0f, 1.0f, 4000.0f);
            b3ShapeDef ShapeDef = b3DefaultShapeDef();
            b3CreateHullShape(Ground, &ShapeDef, &Floor.base);
        }

        const uint32 SideCount = (uint32)(sqrt((double)BodyCount) + 1.0);
        const b3Sphere Sphere = { { 0.0f, 0.0f, 0.0f }, 0.5f };

        for (uint32 Index = 0; Index < BodyCount; ++Index)
        {
            const uint32 X = Index % SideCount;
            const uint32 Z = Index / SideCount;

            b3BodyDef BodyDef = b3DefaultBodyDef();
            BodyDef.type = b3_dynamicBody;
            BodyDef.position = { (float)X * Spacing - (float)SideCount * Spacing * 0.5f,
                                 2.0f + (float)((Index * 7919u) % 13u) * 0.05f,
                                 (float)Z * Spacing - (float)SideCount * Spacing * 0.5f };

            const b3BodyId Body = b3CreateBody(WorldId, &BodyDef);
            b3ShapeDef ShapeDef = b3DefaultShapeDef();
            b3CreateSphereShape(Body, &ShapeDef, &Sphere);
        }
    }

    static FStepStats RunSteps(b3WorldId WorldId, uint32 StepCount, uint32 SubStepCount)
    {
        FStepStats Stats;

        double Total = 0.0;
        double SolveTotal = 0.0;
        double CollideTotal = 0.0;

        for (uint32 Step = 0; Step < StepCount; ++Step)
        {
            const double Start = PlatformTime::Seconds();
            b3World_Step(WorldId, 1.0f / 60.0f, (int)SubStepCount);
            const double Elapsed = (PlatformTime::Seconds() - Start) * 1000.0;

            Total += Elapsed;
            Stats.WorstMs = Elapsed > Stats.WorstMs ? Elapsed : Stats.WorstMs;

            const b3Profile Profile = b3World_GetProfile(WorldId);
            SolveTotal += Profile.solve;
            CollideTotal += Profile.collide;
        }

        Stats.AverageMs = Total / (double)StepCount;
        Stats.SolveMs = SolveTotal / (double)StepCount;
        Stats.CollideMs = CollideTotal / (double)StepCount;
        const b3Counters Counters = b3World_GetCounters(WorldId);
        Stats.AwakeBodies = Counters.bodyCount;
        Stats.TaskCount = Counters.taskCount;

        return Stats;
    }

    // Best of several fresh worlds, since interference only ever makes a pass slower.
    static void RunCase(const char* Label, uint32 BodyCount, const FWorldConfig& Config,
                        Physics::FBox3DTaskBridge* Bridge, uint32 SubStepCount)
    {
        constexpr uint32 Passes = 5;

        FStepStats Active;
        Active.AverageMs = 1e9;
        FStepStats Settled;
        double BuildMs = 0.0;

        for (uint32 Pass = 0; Pass < Passes; ++Pass)
        {
            b3WorldId PassWorld = CreateWorld(Config, Bridge);

            const double BuildStart = PlatformTime::Seconds();
            PopulateWorld(PassWorld, BodyCount, 1.25f);
            BuildMs = (PlatformTime::Seconds() - BuildStart) * 1000.0;

            // Falling and colliding, which is the worst case every body is awake for.
            const FStepStats PassActive = RunSteps(PassWorld, 60, SubStepCount);
            if (PassActive.AverageMs < Active.AverageMs)
            {
                Active = PassActive;
            }

            if (Pass == 0)
            {
                // Long enough for the island sleep to take the pile out of the solver.
                RunSteps(PassWorld, 240, SubStepCount);
                Settled = RunSteps(PassWorld, 60, SubStepCount);
            }

            b3DestroyWorld(PassWorld);
        }

        std::printf("%-34s %8u %10.2f %10.3f %10.3f %10.3f %10.3f %8d\n",
            Label, BodyCount, BuildMs, Active.AverageMs, Active.WorstMs, Settled.AverageMs, Settled.WorstMs, Active.TaskCount);
    }
}

namespace PhysicsBench
{
    // Fork/join latency rather than throughput, which is what a solver's many small stages actually pay.
    static void RunFanOutCase()
    {
        constexpr uint32 Calls = 2000;
        constexpr uint32 Items = 64;

        TAtomic<uint64> Sink{ 0 };

        double Best = 1e9;
        for (uint32 Pass = 0; Pass < 10; ++Pass)
        {
            const double Start = PlatformTime::Seconds();
            for (uint32 Call = 0; Call < Calls; ++Call)
            {
                uint64 Local = 0;
                Task::ParallelFor(Items, [&](uint32 Index)
                {
                    Local += Index;
                });
                Sink.fetch_add(Local, std::memory_order_relaxed);
            }
            const double Elapsed = (PlatformTime::Seconds() - Start) * 1e6 / (double)Calls;
            Best = Elapsed < Best ? Elapsed : Best;
        }

        std::printf("ParallelFor fan-out latency: %.2f us per call (%u items, %u calls)\n", Best, Items, Calls);
    }

    // The engine half of a step: what the interpolation write-back costs per moved body, with no solver in it.
    static void RunWriteBackCase(uint32 BodyCount)
    {
        ECS::FRegistry Registry;

        TVector<ECS::FEntity> Entities;
        Entities.reserve(BodyCount);

        for (uint32 Index = 0; Index < BodyCount; ++Index)
        {
            const ECS::FEntity Entity = Registry.Create();
            STransformComponent& Transform = Registry.Emplace<STransformComponent>(Entity);
            Transform.Bind(Registry, Entity);
            Entities.push_back(Entity);
        }

        auto TransformStorage = Registry.GetStorage<STransformComponent>();

        auto WriteOne = [&](uint32 Index)
        {
            const ECS::FEntity Entity = Entities[Index];
            const float Wave = (float)(Index % 128) * 0.01f;

            STransformComponent& Transform = TransformStorage.Get(Entity);
            Transform.SetFromPhysics(FVector3(Wave, Wave + 1.0f, Wave), FQuat::Identity());
        };

        constexpr uint32 Passes = 20;

        double SerialBest = 1e9;
        for (uint32 Pass = 0; Pass < Passes; ++Pass)
        {
            const double Start = PlatformTime::Seconds();
            for (uint32 Index = 0; Index < BodyCount; ++Index)
            {
                WriteOne(Index);
            }
            const double Elapsed = (PlatformTime::Seconds() - Start) * 1000.0;
            SerialBest = Elapsed < SerialBest ? Elapsed : SerialBest;
        }

        double ParallelBest = 1e9;
        for (uint32 Pass = 0; Pass < Passes; ++Pass)
        {
            const double Start = PlatformTime::Seconds();
            Task::ParallelFor(BodyCount, WriteOne, 256);
            const double Elapsed = (PlatformTime::Seconds() - Start) * 1000.0;
            ParallelBest = Elapsed < ParallelBest ? Elapsed : ParallelBest;
        }

        std::printf("ECS write-back for %u bodies: serial %.3f ms (%.1f ns/body), parallel %.3f ms (%.1f ns/body)\n",
            BodyCount, SerialBest, SerialBest * 1e6 / (double)BodyCount,
            ParallelBest, ParallelBest * 1e6 / (double)BodyCount);
    }
}

namespace PhysicsBench
{
    enum class ESingleScene : uint8
    {
        BoxPile,
        MixedPile,
        Debris,
    };

    static const char* SceneName(ESingleScene Scene)
    {
        switch (Scene)
        {
        case ESingleScene::BoxPile:   return "box pile";
        case ESingleScene::MixedPile: return "mixed pile";
        case ESingleScene::Debris:    return "debris";
        }
        return "?";
    }

    static void AddStaticBox(b3WorldId WorldId, b3Vec3 Center, b3Vec3 HalfExtent)
    {
        b3BodyDef Def = b3DefaultBodyDef();
        Def.type = b3_staticBody;
        Def.position = Center;
        const b3BodyId Body = b3CreateBody(WorldId, &Def);
        const b3BoxHull Hull = b3MakeBoxHull(HalfExtent.x, HalfExtent.y, HalfExtent.z);
        b3ShapeDef ShapeDef = b3DefaultShapeDef();
        b3CreateHullShape(Body, &ShapeDef, &Hull.base);
    }

    // Sleep is off for every body, so the whole set stays in the solver however long it rests.
    static void PopulateSingleScene(b3WorldId WorldId, ESingleScene Scene, uint32 BodyCount)
    {
        AddStaticBox(WorldId, { 0.0f, -1.0f, 0.0f }, { 400.0f, 1.0f, 400.0f });

        const b3BoxHull Box = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
        const b3Sphere Sphere = { { 0.0f, 0.0f, 0.0f }, 0.5f };
        const b3Capsule Capsule = { { 0.0f, -0.35f, 0.0f }, { 0.0f, 0.35f, 0.0f }, 0.35f };

        const bool bPile = Scene != ESingleScene::Debris;
        constexpr uint32 LayersInPile = 10;
        const uint32 Side = bPile
            ? (uint32)std::ceil(std::sqrt((double)BodyCount / LayersInPile))
            : (uint32)std::ceil(std::sqrt((double)BodyCount));
        const float Spacing = bPile ? 1.1f : 3.0f;
        const float Half = (float)Side * Spacing * 0.5f;

        // Walls keep the pile a pile, so every body stays in contact with several neighbors for the whole run.
        if (bPile)
        {
            const float Wall = Half + 0.6f;
            const float Height = LayersInPile * 1.2f + 2.0f;
            AddStaticBox(WorldId, {  Wall, Height * 0.5f, 0.0f }, { 0.5f, Height * 0.5f, Wall });
            AddStaticBox(WorldId, { -Wall, Height * 0.5f, 0.0f }, { 0.5f, Height * 0.5f, Wall });
            AddStaticBox(WorldId, { 0.0f, Height * 0.5f,  Wall }, { Wall, Height * 0.5f, 0.5f });
            AddStaticBox(WorldId, { 0.0f, Height * 0.5f, -Wall }, { Wall, Height * 0.5f, 0.5f });
        }

        for (uint32 Index = 0; Index < BodyCount; ++Index)
        {
            const uint32 Column = Index % (Side * Side);
            const uint32 Layer = Index / (Side * Side);
            const float Jitter = (float)((Index * 7919u) % 97u) / 97.0f - 0.5f;

            b3BodyDef Def = b3DefaultBodyDef();
            Def.type = b3_dynamicBody;
            Def.enableSleep = false;
            Def.position = { (float)(Column % Side) * Spacing - Half + 0.5f + Jitter * 0.1f,
                             0.6f + (float)Layer * 1.15f,
                             (float)(Column / Side) * Spacing - Half + 0.5f - Jitter * 0.1f };
            if (!bPile)
            {
                // Skidding debris, so each box keeps a live ground contact instead of settling into rest.
                Def.linearVelocity = { Jitter * 8.0f, 0.0f, -Jitter * 6.0f };
                Def.angularVelocity = { 0.0f, Jitter * 4.0f, 0.0f };
            }

            const b3BodyId Body = b3CreateBody(WorldId, &Def);
            b3ShapeDef ShapeDef = b3DefaultShapeDef();
            const uint32 Kind = Scene == ESingleScene::MixedPile ? Index % 3u : 0u;
            if (Kind == 1)
            {
                b3CreateSphereShape(Body, &ShapeDef, &Sphere);
            }
            else if (Kind == 2)
            {
                b3CreateCapsuleShape(Body, &ShapeDef, &Capsule);
            }
            else
            {
                b3CreateHullShape(Body, &ShapeDef, &Box.base);
            }
        }
    }

    static void RunSingleThreadCase(ESingleScene Scene, uint32 BodyCount, uint32 SubStepCount, bool bForceSSE2)
    {
        constexpr uint32 SettleSteps = 90;
        constexpr uint32 MeasuredSteps = 240;
        constexpr uint32 Passes = 3;

        double BestAverage = 1e9;
        double BestP95 = 0.0;
        double BestWorst = 0.0;
        b3Profile BestProfile{};
        int32 Contacts = 0;

        for (uint32 Pass = 0; Pass < Passes; ++Pass)
        {
            const b3WorldId WorldId = CreateWorld(FWorldConfig{ 1, false }, nullptr);
            b3World_EnableSSE2Fallback(WorldId, bForceSSE2);
            PopulateSingleScene(WorldId, Scene, BodyCount);

            for (uint32 Step = 0; Step < SettleSteps; ++Step)
            {
                b3World_Step(WorldId, 1.0f / 60.0f, (int)SubStepCount);
            }

            std::vector<double> StepMs;
            StepMs.reserve(MeasuredSteps);
            b3Profile Sum{};
            for (uint32 Step = 0; Step < MeasuredSteps; ++Step)
            {
                const double Start = PlatformTime::Seconds();
                b3World_Step(WorldId, 1.0f / 60.0f, (int)SubStepCount);
                StepMs.push_back((PlatformTime::Seconds() - Start) * 1000.0);

                const b3Profile Profile = b3World_GetProfile(WorldId);
                Sum.pairs += Profile.pairs;
                Sum.collide += Profile.collide;
                Sum.solve += Profile.solve;
                Sum.prepareConstraints += Profile.prepareConstraints;
                Sum.solveImpulses += Profile.solveImpulses;
                Sum.integrateVelocities += Profile.integrateVelocities + Profile.integratePositions;
                Sum.refit += Profile.refit + Profile.transforms;
            }

            double Total = 0.0;
            for (double Ms : StepMs)
            {
                Total += Ms;
            }
            const double Average = Total / (double)StepMs.size();
            std::sort(StepMs.begin(), StepMs.end());

            if (Average < BestAverage)
            {
                BestAverage = Average;
                BestP95 = StepMs[(size_t)(StepMs.size() * 0.95)];
                BestWorst = StepMs.back();
                const float Inv = 1.0f / (float)MeasuredSteps;
                BestProfile = Sum;
                BestProfile.pairs *= Inv;
                BestProfile.collide *= Inv;
                BestProfile.solve *= Inv;
                BestProfile.prepareConstraints *= Inv;
                BestProfile.solveImpulses *= Inv;
                BestProfile.integrateVelocities *= Inv;
                BestProfile.refit *= Inv;
                Contacts = b3World_GetCounters(WorldId).contactCount;
            }

            b3DestroyWorld(WorldId);
        }

        std::printf("%-11s %6u %8d %8.3f %8.3f %8.3f   %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f\n",
            SceneName(Scene), BodyCount, Contacts, BestAverage, BestP95, BestWorst,
            BestProfile.pairs, BestProfile.collide, BestProfile.solve, BestProfile.prepareConstraints,
            BestProfile.solveImpulses, BestProfile.integrateVelocities, BestProfile.refit);
    }

    // One worker and every body awake for the whole measurement, the budget a low-core-count target actually has.
    static void RunSingleThreadSweep(uint32 SubStepCount, bool bForceSSE2)
    {
        std::printf("Box3D on one thread, %u sub-steps at 60 Hz, sleep disabled, best of 3 worlds, %s\n\n",
            SubStepCount, b3IsAVX2Available() && !bForceSSE2 ? "AVX2 kernels" : "SSE2 kernels");
        std::printf("%-11s %6s %8s %8s %8s %8s   %6s %6s %6s %6s %6s %6s %6s\n",
            "scene", "bodies", "contacts", "avg ms", "p95 ms", "worst", "pairs", "collid", "solve", "prep", "impuls", "integr", "refit");
        std::printf("-----------------------------------------------------------------------------------------------------------------\n");

        const uint32 Counts[] = { 1000, 2000, 3000, 4000, 5000, 6000 };
        const ESingleScene Scenes[] = { ESingleScene::BoxPile, ESingleScene::MixedPile, ESingleScene::Debris };
        for (ESingleScene Scene : Scenes)
        {
            for (uint32 Count : Counts)
            {
                RunSingleThreadCase(Scene, Count, SubStepCount, bForceSSE2);
            }
        }
    }
}

int main(int Argc, char** Argv)
{
    using namespace Lumina;
    using namespace PhysicsBench;

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // "--single [substeps] [sse2]" profiles a handful of thousand awake bodies on one worker instead of the throughput table.
    if (Argc > 1 && std::strcmp(Argv[1], "--single") == 0)
    {
        const uint32 SingleSubSteps = Argc > 2 ? (uint32)std::atoll(Argv[2]) : 4u;
        const bool bForceSSE2 = Argc > 3 && std::strcmp(Argv[3], "sse2") == 0;
        RunSingleThreadSweep(SingleSubSteps == 0 ? 4u : SingleSubSteps, bForceSSE2);
        return 0;
    }

    uint32 BodyCount = 50000;
    if (Argc > 1)
    {
        BodyCount = (uint32)std::atoll(Argv[1]);
        if (BodyCount == 0)
        {
            BodyCount = 50000;
        }
    }

    uint32 SubStepCount = 4;
    if (Argc > 2)
    {
        SubStepCount = (uint32)std::atoll(Argv[2]);
    }

    Task::Initialize();

    Physics::FBox3DTaskBridge Bridge;
    const uint32 EngineWorkerCount = Bridge.GetWorkerCount();

    std::printf("Box3D throughput, %u sub-steps, engine worker count %u\n\n", SubStepCount, EngineWorkerCount);
    std::printf("%-34s %8s %10s %10s %10s %10s %10s %8s\n",
        "case", "bodies", "build ms", "act avg", "act worst", "idle avg", "idle worst", "tasks");
    std::printf("---------------------------------------------------------------------------------------------\n");

    const uint32 Counts[] = { 5000, 20000, BodyCount };
    for (uint32 Count : Counts)
    {
        RunCase("serial (1 worker)", Count, FWorldConfig{ 1, false }, nullptr, SubStepCount);
        RunCase("box3d internal scheduler", Count, FWorldConfig{ EngineWorkerCount, false }, nullptr, SubStepCount);
        RunCase("lumina task bridge", Count, FWorldConfig{ EngineWorkerCount, true }, &Bridge, SubStepCount);
    }

    // What the worker count is worth, since Box3D asks for one cache group's physical cores and we give it the machine.
    const uint32 WorkerCounts[] = { 7, 15, Jobs::GetNumWorkers() };
    for (uint32 Workers : WorkerCounts)
    {
        if (Workers == 0 || Workers > (uint32)B3_MAX_WORKERS)
        {
            continue;
        }

        char Label[64];
        snprintf(Label, sizeof(Label), "lumina task bridge, %u workers", Workers);
        RunCase(Label, BodyCount, FWorldConfig{ Workers, true }, &Bridge, SubStepCount);
    }

    std::printf("---------------------------------------------------------------------------------------------\n");

    RunWriteBackCase(BodyCount);
    RunFanOutCase();

    Task::Shutdown();
    return 0;
}
