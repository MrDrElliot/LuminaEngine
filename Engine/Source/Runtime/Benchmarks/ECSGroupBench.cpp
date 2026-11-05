#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/TaskSystem.h"
#include <cstdio>
#include <random>

using namespace Lumina;

namespace ECSGroupBenchInternal
{
    struct FBenchP { float X = 0.f, Y = 0.f, Z = 0.f; };
    struct FBenchV { float X = 1.f, Y = 1.f, Z = 1.f; };
    struct FBenchM { float Mass = 1.f; };
    struct FBenchH { float Health = 100.f; };
    struct FBenchNoise { int32 Value = 0; };

    double NanosPer(uint64 Start, uint64 End, int64 Ops)
    {
        return Ops > 0 ? PlatformTime::ToSeconds(End - Start) * 1e9 / static_cast<double>(Ops) : 0.0;
    }

    // Each type is skipped on a different fraction of entities, so an ungrouped walk pays real membership misses.
    template<typename... Ts>
    void Populate(ECS::FRegistry& Registry, int32 Count)
    {
        std::mt19937 Rng(42);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<FBenchNoise>(Entity);
            ((Rng() % 8 != 0 ? (void)Registry.Emplace<Ts>(Entity) : void()), ...);
        }
    }

    template<typename... Ts>
    double TimeIteration(int32 Count, bool bGrouped, bool bTracked = false)
    {
        ECS::FRegistry Registry;
        if constexpr (sizeof...(Ts) > 1)
        {
            if (bGrouped)
            {
                Registry.DeclareGroup<Ts...>();
            }
        }
        Populate<Ts...>(Registry, Count);
        if (bTracked)
        {
            (Registry.TrackChanges<Ts>(), ...);
        }
        auto View = Registry.View<Ts...>();
        auto WriteView = Registry.View<ECS::Write<Ts>...>();

        constexpr int32 Passes = 50;
        float Sink = 0.f;
        int64 Visits = 0;
        const uint64 Start = PlatformTime::Cycles();
        for (int32 Pass = 0; Pass < Passes; ++Pass)
        {
            if (bTracked)
            {
                WriteView.ForEach([&](ECS::TMut<Ts>... Values)
                {
                    ((Sink += reinterpret_cast<const float&>(Values.Get())), ...);
                    ((reinterpret_cast<float&>(Values.GetMut()) += 1.f), ...);
                    ++Visits;
                });
                continue;
            }
            View.ForEach([&](Ts&... Values)
            {
                ((Sink += reinterpret_cast<const float&>(Values)), ...);
                ((reinterpret_cast<float&>(Values) += 1.f), ...);
                ++Visits;
            });
        }
        const uint64 End = PlatformTime::Cycles();
        EXPECT_GT(Sink, -1.f);
        return NanosPer(Start, End, Visits);
    }

    template<typename... Ts>
    void ReportIteration(const char* Label, bool bTracked = false)
    {
        for (int32 Count : { 10000, 100000 })
        {
            const double Plain = TimeIteration<Ts...>(Count, false, bTracked);
            const double Grouped = TimeIteration<Ts...>(Count, true, bTracked);
            std::printf("[ECSGroupBench] %s%s %7d entities: ungrouped %.2f ns/entity, grouped %.2f ns/entity (%.2fx)\n",
                Label, bTracked ? " tracked" : "", Count, Plain, Grouped, Grouped > 0.0 ? Plain / Grouped : 0.0);
        }
    }

    // Parallel slices of one write view, where handles into an untracked pool must not share a sink line across workers.
    double TimeParallelWrite(bool bHandles, bool bTracked)
    {
        constexpr int32 Count = 1000000;
        ECS::FRegistry Registry;
        Populate<FBenchP, FBenchV>(Registry, Count);
        if (bTracked)
        {
            Registry.TrackChanges<FBenchP>();
            Registry.TrackChanges<FBenchV>();
        }
        auto View = Registry.View<FBenchP, FBenchV>();
        auto WriteView = Registry.View<ECS::Write<FBenchP>, ECS::Write<FBenchV>>();
        const uint32 Slots = static_cast<uint32>(WriteView.NumDenseSlots());

        constexpr int32 Passes = 30;
        const uint64 Start = PlatformTime::Cycles();
        for (int32 Pass = 0; Pass < Passes; ++Pass)
        {
            Jobs::ParallelFor(Slots, [&](const Jobs::FParallelRange& Range)
            {
                if (bHandles)
                {
                    WriteView.ForEachInRange(Range.Start, Range.End, [](ECS::TMut<FBenchP> P, ECS::TMut<FBenchV> V) { P->X += V.Get().X; });
                    return;
                }
                View.ForEachInRange(Range.Start, Range.End, [](FBenchP& P, const FBenchV& V) { P.X += V.X; });
            }, 4096);
        }
        const uint64 End = PlatformTime::Cycles();
        return NanosPer(Start, End, static_cast<int64>(Passes) * Count);
    }

    double TimeChurn(int32 Count, bool bGrouped)
    {
        ECS::FRegistry Registry;
        if (bGrouped)
        {
            Registry.DeclareGroup<FBenchP, FBenchV, FBenchM>();
        }
        TVector<ECS::FEntity> Entities;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<FBenchP>(Entity);
            Registry.Emplace<FBenchV>(Entity);
            Entities.Add(Entity);
        }

        constexpr int32 Rounds = 5;
        const uint64 Start = PlatformTime::Cycles();
        for (int32 Round = 0; Round < Rounds; ++Round)
        {
            for (const ECS::FEntity Entity : Entities)
            {
                Registry.Emplace<FBenchM>(Entity);
            }
            for (const ECS::FEntity Entity : Entities)
            {
                Registry.Remove<FBenchM>(Entity);
            }
        }
        const uint64 End = PlatformTime::Cycles();
        return NanosPer(Start, End, static_cast<int64>(Rounds) * Count * 2);
    }
}

using namespace ECSGroupBenchInternal;

TEST(ECSGroupBench, DISABLED_GroupedVsUngroupedIteration)
{
    ReportIteration<FBenchP, FBenchV>("2 components");
    ReportIteration<FBenchP, FBenchV, FBenchM>("3 components");
    ReportIteration<FBenchP, FBenchV, FBenchM, FBenchH>("4 components");
}

TEST(ECSGroupBench, DISABLED_TrackedWriteIteration)
{
    ReportIteration<FBenchP>("1 component", true);
    ReportIteration<FBenchP, FBenchV>("2 components", true);
    ReportIteration<FBenchP, FBenchV, FBenchM>("3 components", true);
    std::printf("[ECSGroupBench] parallel 2 components 1M entities: bare %.2f, handle untracked %.2f, handle tracked %.2f ns/entity\n",
        TimeParallelWrite(false, false), TimeParallelWrite(true, false), TimeParallelWrite(true, true));
}

TEST(ECSGroupBench, DISABLED_AddRemoveChurn)
{
    for (int32 Count : { 10000, 100000 })
    {
        const double Plain = TimeChurn(Count, false);
        const double Grouped = TimeChurn(Count, true);
        std::printf("[ECSGroupBench] churn %7d entities: ungrouped %.2f ns/op, grouped %.2f ns/op\n", Count, Plain, Grouped);
    }
}
