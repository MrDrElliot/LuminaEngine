#include "BenchCommon.h"

#include <algorithm>

namespace ECSBench
{
    namespace
    {
        // A second fat component, so a pair walk touches two cache lines per entity rather than one.
        struct FPose { float M[16] = {}; };

        struct FBuild
        {
            // The world as it is, with the two pools filled independently.
            ECS::FRegistry Registry;

            // The same matched set with both pools filled in lockstep, which is the state a group holds.
            ECS::FRegistry Grouped;
            int32         Matched = 0;

            // The absolute floor, two flat arrays with no pool machinery at all.
            TVector<ECS::FEntity> FlatDense;
            TVector<FPosition>    FlatA;
            TVector<FPose>        FlatB;
        };

        // One driver element in MatchInN carries the second component, and bShuffle scatters its fill order.
        void BuildPair(FBuild& Out, int32 EntityCount, uint32 MatchInN, bool bShuffle)
        {
            TVector<ECS::FEntity> Entities;
            Entities.Reserve(EntityCount);

            for (int32 Index = 0; Index < EntityCount; ++Index)
            {
                const ECS::FEntity Entity = Out.Registry.Create();
                Out.Registry.Emplace<FPosition>(Entity);
                Entities.Add(Entity);
            }

            TVector<ECS::FEntity> Carriers;
            Carriers.Reserve(EntityCount / static_cast<int32>(MatchInN) + 1);
            for (int32 Index = 0; Index < EntityCount; Index += MatchInN)
            {
                Carriers.Add(Entities[Index]);
            }

            // A real world fills the second pool at some other time, so its dense order is unrelated.
            if (bShuffle)
            {
                FSplitMix Random(0xC0FFEEull);
                for (int32 Index = Carriers.Num(); Index > 1; --Index)
                {
                    const uint32 Pick = Random.NextBelow(static_cast<uint32>(Index));
                    std::swap(Carriers[Index - 1], Carriers[Pick]);
                }
            }

            for (const ECS::FEntity Entity : Carriers)
            {
                Out.Registry.Emplace<FPose>(Entity);
            }

            Out.Matched = Carriers.Num();

            //~ The grouped copy holds only the matched entities, both pools in the same dense order.

            Out.FlatDense.Reserve(Carriers.Num());
            Out.FlatA.SetNum(Carriers.Num());
            Out.FlatB.SetNum(Carriers.Num());

            for (int32 Index = 0; Index < Carriers.Num(); ++Index)
            {
                const ECS::FEntity Mirror = Out.Grouped.Create();
                Out.Grouped.Emplace<FPosition>(Mirror);
                Out.Grouped.Emplace<FPose>(Mirror);

                Out.FlatDense.Add(Mirror);
            }
        }

        double MeasureView(FBuild& Build, int32 Passes)
        {
            float Sink = 0.0f;
            const double Nanos = MeasureNanosPerOp(Build.Matched, Passes, [&]
            {
                float Total = 0.0f;
                Build.Registry.View<FPosition, FPose>().ForEach(
                    [&Total](ECS::FEntity Entity, FPosition& P, FPose& Q)
                    {
                        Total += P.X + Q.M[0] + float(Entity.GetIndex() & 1u);
                    });
                Sink = Total;
            });
            return Sink == 12345.0f ? Nanos + 1.0 : Nanos;
        }

        // Exactly what an owning group's steady state walk is, two pools indexed by one dense cursor.
        double MeasureGroup(FBuild& Build, int32 Passes)
        {
            const ECS::TComponentStorage<FPosition> A = Build.Grouped.GetStorage<FPosition>();
            const ECS::TComponentStorage<FPose> B = Build.Grouped.GetStorage<FPose>();
            const ECS::FSparseSet* Set = A.GetSet();

            float Sink = 0.0f;
            const double Nanos = MeasureNanosPerOp(Build.Matched, Passes, [&]
            {
                float Total = 0.0f;
                const int32 Count = Set->GetDenseSize();
                const ECS::FEntity* Dense = Set->GetDenseData();

                for (int32 Index = 0; Index < Count; ++Index)
                {
                    const ECS::FEntity Entity = Dense[Index];
                    FPosition& P = A.GetAtDense(static_cast<uint32>(Index));
                    FPose& Q = B.GetAtDense(static_cast<uint32>(Index));
                    Total += P.X + Q.M[0] + float(Entity.GetIndex() & 1u);
                }
                Sink = Total;
            });
            return Sink == 12345.0f ? Nanos + 1.0 : Nanos;
        }

        // No pool at all, which bounds how much of the gap is the paged indirection rather than the probe.
        double MeasureFlat(FBuild& Build, int32 Passes)
        {
            float Sink = 0.0f;
            const double Nanos = MeasureNanosPerOp(Build.Matched, Passes, [&]
            {
                float Total = 0.0f;
                const int32 Count = Build.FlatDense.Num();
                const ECS::FEntity* Dense = Build.FlatDense.GetData();
                const FPosition* A = Build.FlatA.GetData();
                const FPose* B = Build.FlatB.GetData();

                for (int32 Index = 0; Index < Count; ++Index)
                {
                    Total += A[Index].X + B[Index].M[0] + float(Dense[Index].GetIndex() & 1u);
                }
                Sink = Total;
            });
            return Sink == 12345.0f ? Nanos + 1.0 : Nanos;
        }

        // The atom of group maintenance, moving one entity across the owned prefix boundary.
        double MeasureSwapAtom(int32 EntityCount, int32 Passes)
        {
            ECS::FRegistry Registry;
            TVector<ECS::FEntity> Entities;
            Entities.Reserve(EntityCount);

            for (int32 Index = 0; Index < EntityCount; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FPosition>(Entity);
                Registry.Emplace<FPose>(Entity);
                Entities.Add(Entity);
            }

            const ECS::TComponentStorage<FPosition> A = Registry.GetStorage<FPosition>();
            const ECS::TComponentStorage<FPose> B = Registry.GetStorage<FPose>();

            float Sink = 0.0f;
            const double Nanos = MeasureNanosPerOp(EntityCount, Passes, [&]
            {
                float Total = 0.0f;
                const uint32 Count = static_cast<uint32>(Entities.Num());

                for (uint32 Index = 0; Index + 1 < Count; ++Index)
                {
                    // Two owned pools, so one boundary move is two element swaps.
                    std::swap(A.GetAtDense(Index), A.GetAtDense(Index + 1));
                    std::swap(B.GetAtDense(Index), B.GetAtDense(Index + 1));
                    Total += A.GetAtDense(Index).X;
                }
                Sink = Total;
            });
            return Sink == 12345.0f ? Nanos + 1.0 : Nanos;
        }

        // The structural change a group has to react to, measured with no group present.
        double MeasureEmplaceRemove(int32 EntityCount, int32 Passes)
        {
            ECS::FRegistry Registry;
            TVector<ECS::FEntity> Entities;
            Entities.Reserve(EntityCount);

            for (int32 Index = 0; Index < EntityCount; ++Index)
            {
                const ECS::FEntity Entity = Registry.Create();
                Registry.Emplace<FPosition>(Entity);
                Entities.Add(Entity);
            }

            (void)Registry.GetStorage<FPose>();

            return MeasureNanosPerOp(EntityCount, Passes, [&]
            {
                for (const ECS::FEntity Entity : Entities)
                {
                    Registry.Emplace<FPose>(Entity);
                }
                for (const ECS::FEntity Entity : Entities)
                {
                    Registry.Remove<FPose>(Entity);
                }
            });
        }

        // What the driver pays to reach entities that do not match, which no payload layout recovers.
        double MeasureDriverScan(FBuild& Build, int32 Passes)
        {
            float Sink = 0.0f;
            const double Nanos = MeasureNanosPerOp(Build.Matched, Passes, [&]
            {
                float Total = 0.0f;
                Build.Registry.View<FPosition>().ForEach(
                    [&Total](ECS::FEntity, FPosition& P) { Total += P.X; });
                Sink = Total;
            });
            return Sink == 12345.0f ? Nanos + 1.0 : Nanos;
        }
    }

    void RunGroupHeadroomCases(int32 EntityCount, int32 Passes)
    {
        std::printf("\nHeadroom an owning group could recover on a two component walk.\n");
        std::printf("Nanoseconds per matched entity, all three doing the same work per entity.\n");
        std::printf("Group is two real paged pools sharing one dense order. Flat is two bare arrays.\n\n");
        std::printf("%-32s %9s %10s %10s %9s %10s\n",
            "case", "matched", "view", "group", "flat", "view/group");
        std::printf("%s\n", "--------------------------------------------------------------------------------");

        struct FCase { const char* Name; uint32 MatchInN; bool bShuffle; };
        const FCase Cases[] = {
            { "all match, same fill order",   1,   false },
            { "all match, scattered order",   1,   true  },
            { "1 in 2 match, scattered",      2,   true  },
            { "1 in 8 match, scattered",      8,   true  },
            { "1 in 64 match, scattered",     64,  true  },
        };

        for (const FCase& Case : Cases)
        {
            FBuild Build;
            BuildPair(Build, EntityCount, Case.MatchInN, Case.bShuffle);

            const double View = MeasureView(Build, Passes);
            const double Group = MeasureGroup(Build, Passes);
            const double Flat = MeasureFlat(Build, Passes);

            std::printf("%-32s %9d %10.3f %10.3f %9.3f %9.2fx\n",
                Case.Name, Build.Matched, View, Group, Flat, View / Group);
        }

        std::printf("%s\n", "--------------------------------------------------------------------------------");

        // The view already drives off the smaller pool, so this is what a bad driver choice costs.
        FBuild Sparse;
        BuildPair(Sparse, EntityCount, 64, true);
        std::printf("scanning the large pool instead at 1 in 64: %.3f ns per matched entity\n",
            MeasureDriverScan(Sparse, Passes));

        std::printf("\nWhat a group would charge structural change, nanoseconds per operation.\n\n");
        std::printf("%-46s %12.3f\n", "emplace plus remove, one pool, ungrouped",
            MeasureEmplaceRemove(EntityCount, Passes));
        std::printf("%-46s %12.3f\n", "boundary move across two owned 64 byte pools",
            MeasureSwapAtom(EntityCount, Passes));
    }
}
