#include <gtest/gtest.h>
#include "World/ECS/Registry.h"
#include "Platform/Time/PlatformTime.h"
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
    double TimeIteration(int32 Count, bool bGrouped)
    {
        ECS::FRegistry Registry;
        if (bGrouped)
        {
            Registry.DeclareGroup<Ts...>();
        }
        Populate<Ts...>(Registry, Count);
        auto View = Registry.View<Ts...>();

        constexpr int32 Passes = 50;
        float Sink = 0.f;
        int64 Visits = 0;
        const uint64 Start = PlatformTime::Cycles();
        for (int32 Pass = 0; Pass < Passes; ++Pass)
        {
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
    void ReportIteration(const char* Label)
    {
        for (int32 Count : { 10000, 100000 })
        {
            const double Plain = TimeIteration<Ts...>(Count, false);
            const double Grouped = TimeIteration<Ts...>(Count, true);
            std::printf("[ECSGroupBench] %s %7d entities: ungrouped %.2f ns/entity, grouped %.2f ns/entity (%.2fx)\n",
                Label, Count, Plain, Grouped, Grouped > 0.0 ? Plain / Grouped : 0.0);
        }
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

TEST(ECSGroupBench, DISABLED_AddRemoveChurn)
{
    for (int32 Count : { 10000, 100000 })
    {
        const double Plain = TimeChurn(Count, false);
        const double Grouped = TimeChurn(Count, true);
        std::printf("[ECSGroupBench] churn %7d entities: ungrouped %.2f ns/op, grouped %.2f ns/op\n", Count, Plain, Grouped);
    }
}
