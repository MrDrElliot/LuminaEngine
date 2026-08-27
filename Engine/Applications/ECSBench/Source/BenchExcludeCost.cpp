#include "BenchCommon.h"

namespace ECSBench
{
    namespace
    {
        // A second tag, so the exclude pool is a different type from anything the view includes.
        struct FExcludedTag {};

        double MeasurePlain(size_t EntityCount, size_t Passes)
        {
            ECS::FRegistry Registry;
            for (size_t Index = 0; Index < EntityCount; ++Index)
            {
                Registry.Emplace<FPosition>(Registry.Create());
            }

            float Sink = 0.0f;
            const double Nanos = MeasureNanosPerOp(EntityCount, Passes, [&]
            {
                float Total = 0.0f;
                Registry.View<FPosition>().ForEach([&Total](ECS::FEntity, FPosition& P) { Total += P.X; });
                Sink = Total;
            });
            return Sink == 12345.0f ? Nanos + 1.0 : Nanos;
        }

        // TaggedInN entities carry the excluded tag; 0 means the pool exists but is empty.
        double MeasureExcluded(size_t EntityCount, size_t Passes, uint32 TaggedInN)
        {
            ECS::FRegistry Registry;
            TVector<ECS::FEntity> Entities;
            Entities.reserve(EntityCount);

            for (size_t Index = 0; Index < EntityCount; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FPosition>(Entity);
                Entities.push_back(Entity);
            }

            // Assured either way, so the empty case still pays for the pool lookup and the probe.
            (void)Registry.GetStorage<FExcludedTag>();

            if (TaggedInN > 0)
            {
                for (size_t Index = 0; Index < EntityCount; Index += TaggedInN)
                {
                    Registry.Emplace<FExcludedTag>(Entities[Index]);
                }
            }

            float Sink = 0.0f;
            const double Nanos = MeasureNanosPerOp(EntityCount, Passes, [&]
            {
                float Total = 0.0f;
                Registry.View<FPosition>(ECS::TExclude<FExcludedTag>{})
                    .ForEach([&Total](ECS::FEntity, FPosition& P) { Total += P.X; });
                Sink = Total;
            });
            return Sink == 12345.0f ? Nanos + 1.0 : Nanos;
        }
    }

    void RunExcludeCostCases(size_t EntityCount, size_t Passes)
    {
        std::printf("\nCost of a tag exclude on a single-component view, nanoseconds per entity.\n\n");
        std::printf("%-42s %12s %10s\n", "case", "ns per op", "vs plain");
        std::printf("%s\n", "------------------------------------------------------------------");

        const double Plain = MeasurePlain(EntityCount, Passes);
        std::printf("%-42s %12.3f %10s\n", "no exclude", Plain, "-");

        struct FCase { const char* Name; uint32 TaggedInN; };
        const FCase Cases[] = {
            { "exclude an empty tag pool",        0   },
            { "exclude a tag on 1 in 1000",       1000 },
            { "exclude a tag on 1 in 100",        100  },
            { "exclude a tag on 1 in 10",         10   },
            { "exclude a tag on 1 in 2",          2    },
        };

        for (const FCase& Case : Cases)
        {
            const double Nanos = MeasureExcluded(EntityCount, Passes, Case.TaggedInN);
            std::printf("%-42s %12.3f %9.2fx\n", Case.Name, Nanos, Nanos / Plain);
        }
    }
}
