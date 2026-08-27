#include "BenchCommon.h"

#include "Containers/HashTable.h"

namespace ECSBench
{
    namespace
    {
        // Keyed by the entity's dense index, since the handle already is one.
        class FEntityIndexMap
        {
        public:

            void Reserve(size_t IndexCount) { Slots.resize(IndexCount, ECS::NullEntity); }

            void Set(ECS::FEntity From, ECS::FEntity To)
            {
                const uint32 Index = From.GetIndex();
                if (Index >= Slots.size())
                {
                    Slots.resize(Index + 1u, ECS::NullEntity);
                }
                Slots[Index] = To;
            }

            NODISCARD ECS::FEntity Find(ECS::FEntity From) const
            {
                const uint32 Index = From.GetIndex();
                return Index < Slots.size() ? Slots[Index] : ECS::NullEntity;
            }

            NODISCARD size_t Num() const { return Slots.size(); }

        private:

            TVector<ECS::FEntity> Slots;
        };
    }

    void RunEntityMapCases(size_t EntityCount, size_t Passes)
    {
        std::printf("\nEntity-keyed remap table, nanoseconds per operation.\n\n");
        std::printf("%-46s %12s %10s\n", "case", "ns per op", "vs hash");
        std::printf("%s\n", "----------------------------------------------------------------------");

        ECS::FRegistry Registry;
        TVector<ECS::FEntity> Source;
        TVector<ECS::FEntity> Dest;
        Source.reserve(EntityCount);
        Dest.reserve(EntityCount);

        for (size_t Index = 0; Index < EntityCount; ++Index)
        {
            Source.push_back(Registry.Create());
        }
        for (size_t Index = 0; Index < EntityCount; ++Index)
        {
            Dest.push_back(Registry.Create());
        }

        uint64 Sink = 0;

        const double HashBuild = MeasureNanosPerOp(EntityCount, Passes, [&]
        {
            THashMap<ECS::FEntity, ECS::FEntity> Map;
            for (size_t Index = 0; Index < EntityCount; ++Index)
            {
                Map[Source[Index]] = Dest[Index];
            }
            Sink += Map.size();
        });

        const double FlatBuild = MeasureNanosPerOp(EntityCount, Passes, [&]
        {
            FEntityIndexMap Map;
            Map.Reserve(EntityCount * 2);
            for (size_t Index = 0; Index < EntityCount; ++Index)
            {
                Map.Set(Source[Index], Dest[Index]);
            }
            Sink += Map.Num();
        });

        THashMap<ECS::FEntity, ECS::FEntity> HashMap;
        FEntityIndexMap FlatMap;
        FlatMap.Reserve(EntityCount * 2);
        for (size_t Index = 0; Index < EntityCount; ++Index)
        {
            HashMap[Source[Index]] = Dest[Index];
            FlatMap.Set(Source[Index], Dest[Index]);
        }

        const double HashLookup = MeasureNanosPerOp(EntityCount, Passes, [&]
        {
            uint64 Total = 0;
            for (size_t Index = 0; Index < EntityCount; ++Index)
            {
                const auto It = HashMap.find(Source[Index]);
                Total += (It != HashMap.end()) ? It->second.Value : 0u;
            }
            Sink += Total;
        });

        const double FlatLookup = MeasureNanosPerOp(EntityCount, Passes, [&]
        {
            uint64 Total = 0;
            for (size_t Index = 0; Index < EntityCount; ++Index)
            {
                Total += FlatMap.Find(Source[Index]).Value;
            }
            Sink += Total;
        });

        std::printf("%-46s %12.3f %10s\n", "build, hash map", HashBuild, "-");
        std::printf("%-46s %12.3f %9.2fx\n", "build, dense index array", FlatBuild, HashBuild / FlatBuild);
        std::printf("%-46s %12.3f %10s\n", "lookup, hash map", HashLookup, "-");
        std::printf("%-46s %12.3f %9.2fx\n", "lookup, dense index array", FlatLookup, HashLookup / FlatLookup);

        if (Sink == 0xFFFFFFFFFFull)
        {
            std::printf("unreachable\n");
        }
    }
}
