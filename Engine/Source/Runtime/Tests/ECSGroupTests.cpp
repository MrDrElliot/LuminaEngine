#include <gtest/gtest.h>

#include "World/ECS/Query.h"

#include <algorithm>
#include <random>

using namespace Lumina;

namespace ECSGroupTestsInternal
{
    struct FGroupA { int32 Value = 0; };
    struct FGroupB { int32 Value = 0; };
    struct FGroupC { int32 Value = 0; };
    struct FGroupTag {};
    struct FGroupOther { int32 Value = 0; };

    using FAll = ECS::TQuery<ECS::Write<FGroupA>, ECS::Read<FGroupB>, ECS::Read<FGroupC>>;
    using FAllNoTag = ECS::TQuery<ECS::Read<FGroupA>, ECS::Read<FGroupB>, ECS::Read<FGroupC>, ECS::Without<FGroupTag>>;

    void ExpectPacked(ECS::FRegistry& Registry)
    {
        const ECS::FOwningGroup* Group = Registry.FindOwningGroup<FGroupA>();
        ASSERT_NE(Group, nullptr);
        const ECS::FSparseSet* A = Registry.FindStorage<FGroupA>().GetSet();
        const ECS::FSparseSet* B = Registry.FindStorage<FGroupB>().GetSet();
        const ECS::FSparseSet* C = Registry.FindStorage<FGroupC>().GetSet();

        for (int32 Index = 0; Index < Group->Num; ++Index)
        {
            const ECS::FEntity Entity = A->GetDenseData()[Index];
            EXPECT_EQ(B->GetDenseData()[Index], Entity);
            EXPECT_EQ(C->GetDenseData()[Index], Entity);
            EXPECT_TRUE((Registry.HasAll<FGroupA, FGroupB, FGroupC>(Entity)));
            EXPECT_EQ(Registry.Get<FGroupA>(Entity).Value, Registry.Get<FGroupB>(Entity).Value);
            EXPECT_EQ(Registry.Get<FGroupA>(Entity).Value, Registry.Get<FGroupC>(Entity).Value);
        }

        for (const ECS::FSparseSet* Set : { A, B, C })
        {
            for (int32 Index = Group->Num; Index < Set->GetDenseSize(); ++Index)
            {
                EXPECT_FALSE((Registry.HasAll<FGroupA, FGroupB, FGroupC>(Set->GetDenseData()[Index])));
            }
        }
    }

    TVector<int64> Collect(ECS::FRegistry& Registry)
    {
        TVector<int64> Out;
        FAllNoTag Query;
        Query.ForEach(Registry, [&](ECS::FEntity Entity, const FGroupA& A, const FGroupB& B, const FGroupC& C)
        {
            Out.Add((static_cast<int64>(Entity.GetPacked()) << 32) | static_cast<uint32>(A.Value + B.Value * 3 + C.Value * 7));
        });
        std::sort(Out.begin(), Out.end());
        return Out;
    }

    void EmplaceOne(ECS::FRegistry& Registry, ECS::FEntity Entity, int32 Which)
    {
        const int32 Value = static_cast<int32>(Entity.GetIndex());
        switch (Which)
        {
            case 0: Registry.Emplace<FGroupA>(Entity).Value = Value; break;
            case 1: Registry.Emplace<FGroupB>(Entity).Value = Value; break;
            default: Registry.Emplace<FGroupC>(Entity).Value = Value; break;
        }
    }

    void RemoveOne(ECS::FRegistry& Registry, ECS::FEntity Entity, int32 Which)
    {
        switch (Which)
        {
            case 0: Registry.Remove<FGroupA>(Entity); break;
            case 1: Registry.Remove<FGroupB>(Entity); break;
            default: Registry.Remove<FGroupC>(Entity); break;
        }
    }
}

using namespace ECSGroupTestsInternal;

TEST(ECSGroup, AddRemoveDestroyInEveryOrder)
{
    int32 Order[3] = { 0, 1, 2 };
    do
    {
        int32 RemoveOrder[3] = { 0, 1, 2 };
        do
        {
            ECS::FRegistry Registry;
            ASSERT_NE((Registry.DeclareGroup<FGroupA, FGroupB, FGroupC>()), nullptr);

            TVector<ECS::FEntity> Entities;
            for (int32 Index = 0; Index < 16; ++Index)
            {
                Entities.Add(Registry.Create());
            }
            for (int32 Step : Order)
            {
                for (int32 Index = 0; Index < Entities.Num(); Index += (Step + 1))
                {
                    EmplaceOne(Registry, Entities[Index], Step);
                }
                ExpectPacked(Registry);
            }
            EXPECT_GT(Registry.FindOwningGroup<FGroupA>()->Num, 0);

            for (int32 Index = 0; Index < Entities.Num(); Index += 3)
            {
                Registry.Destroy(Entities[Index]);
            }
            ExpectPacked(Registry);

            for (int32 Step : RemoveOrder)
            {
                for (int32 Index = 1; Index < Entities.Num(); Index += 2)
                {
                    if (Registry.IsValid(Entities[Index]))
                    {
                        RemoveOne(Registry, Entities[Index], Step);
                    }
                }
                ExpectPacked(Registry);
            }

            for (int32 Step : Order)
            {
                for (int32 Index = 1; Index < Entities.Num(); Index += 2)
                {
                    if (Registry.IsValid(Entities[Index]))
                    {
                        EmplaceOne(Registry, Entities[Index], Step);
                    }
                }
                ExpectPacked(Registry);
            }
        }
        while (std::next_permutation(std::begin(RemoveOrder), std::end(RemoveOrder)));
    }
    while (std::next_permutation(std::begin(Order), std::end(Order)));
}

TEST(ECSGroup, PartialMatchStaysOutside)
{
    ECS::FRegistry Registry;
    const ECS::FOwningGroup* Group = Registry.DeclareGroup<FGroupA, FGroupB, FGroupC>();
    ASSERT_NE(Group, nullptr);

    const ECS::FEntity Partial = Registry.Create();
    EmplaceOne(Registry, Partial, 0);
    EmplaceOne(Registry, Partial, 1);
    const ECS::FEntity Full = Registry.Create();
    EmplaceOne(Registry, Full, 0);
    EmplaceOne(Registry, Full, 1);
    EmplaceOne(Registry, Full, 2);

    EXPECT_EQ(Group->Num, 1);
    ExpectPacked(Registry);

    int32 Visited = 0;
    FAll Query;
    Query.ForEach(Registry, [&](ECS::FEntity Entity, const FGroupA&, const FGroupB&, const FGroupC&)
    {
        EXPECT_EQ(Entity, Full);
        ++Visited;
    });
    EXPECT_EQ(Visited, 1);

    auto View = Registry.View<FGroupA, FGroupB, FGroupC>();
    EXPECT_TRUE(View.IsGrouped());
    EXPECT_EQ(View.TryGet<FGroupA>(Partial), nullptr);
    EXPECT_NE(View.TryGet<FGroupA>(Full), nullptr);
}

TEST(ECSGroup, DeclaringOverExistingEntitiesPacksThem)
{
    ECS::FRegistry Registry;
    std::mt19937 Rng(7);
    for (int32 Index = 0; Index < 500; ++Index)
    {
        const ECS::FEntity Entity = Registry.Create();
        for (int32 Which = 0; Which < 3; ++Which)
        {
            if (Rng() % 4 != 0)
            {
                EmplaceOne(Registry, Entity, Which);
            }
        }
    }

    const TVector<int64> Before = Collect(Registry);
    ASSERT_NE((Registry.DeclareGroup<FGroupA, FGroupB, FGroupC>()), nullptr);
    ExpectPacked(Registry);
    EXPECT_EQ(Collect(Registry), Before);
}

TEST(ECSGroup, SecondOwnerIsRefused)
{
    ECS::FRegistry Registry;
    ECS::FOwningGroup* First = Registry.DeclareGroup<FGroupA, FGroupB>();
    ASSERT_NE(First, nullptr);
    EXPECT_EQ((Registry.DeclareGroup<FGroupA, FGroupB>()), First);
    EXPECT_EQ((Registry.DeclareGroup<FGroupB, FGroupC>()), nullptr);
    EXPECT_EQ((Registry.DeclareGroup<FGroupA, FGroupB, FGroupC>()), nullptr);
    EXPECT_EQ(Registry.FindOwningGroup<FGroupC>(), nullptr);
    EXPECT_NE((Registry.DeclareGroup<FGroupC, FGroupOther>()), nullptr);
}

TEST(ECSGroup, MatchesUngroupedUnderRandomChurn)
{
    ECS::FRegistry Grouped;
    ECS::FRegistry Plain;
    ASSERT_NE((Grouped.DeclareGroup<FGroupA, FGroupB, FGroupC>()), nullptr);

    TVector<ECS::FEntity> GroupedEntities;
    TVector<ECS::FEntity> PlainEntities;
    std::mt19937 Rng(1234);
    for (int32 Step = 0; Step < 6000; ++Step)
    {
        const uint32 Op = Rng() % 10;
        if (Op < 2 || GroupedEntities.IsEmpty())
        {
            GroupedEntities.Add(Grouped.Create());
            PlainEntities.Add(Plain.Create());
            continue;
        }

        const int32 Pick = static_cast<int32>(Rng() % static_cast<uint32>(GroupedEntities.Num()));
        const ECS::FEntity G = GroupedEntities[Pick];
        const ECS::FEntity P = PlainEntities[Pick];
        const int32 Which = static_cast<int32>(Rng() % 4);
        if (Op < 6)
        {
            if (Which == 3)
            {
                Grouped.EmplaceOrReplace<FGroupTag>(G);
                Plain.EmplaceOrReplace<FGroupTag>(P);
            }
            else
            {
                const int32 Value = static_cast<int32>(Rng() % 1000);
                auto Set = [&](ECS::FRegistry& Registry, ECS::FEntity Entity)
                {
                    switch (Which)
                    {
                        case 0: Registry.EmplaceOrReplace<FGroupA>(Entity).Value = Value; break;
                        case 1: Registry.EmplaceOrReplace<FGroupB>(Entity).Value = Value; break;
                        default: Registry.EmplaceOrReplace<FGroupC>(Entity).Value = Value; break;
                    }
                };
                Set(Grouped, G);
                Set(Plain, P);
            }
        }
        else if (Op < 9)
        {
            if (Which == 3)
            {
                Grouped.Remove<FGroupTag>(G);
                Plain.Remove<FGroupTag>(P);
            }
            else
            {
                RemoveOne(Grouped, G, Which);
                RemoveOne(Plain, P, Which);
            }
        }
        else
        {
            Grouped.Destroy(G);
            Plain.Destroy(P);
            GroupedEntities.RemoveAtSwap(Pick);
            PlainEntities.RemoveAtSwap(Pick);
        }
    }

    ASSERT_EQ(GroupedEntities, PlainEntities);
    const TVector<int64> Expected = Collect(Plain);
    EXPECT_EQ(Collect(Grouped), Expected);
    EXPECT_FALSE(Expected.IsEmpty());

    auto View = Grouped.View<ECS::Read<FGroupA>, ECS::Read<FGroupB>, ECS::Read<FGroupC>, ECS::Without<FGroupTag>>();
    EXPECT_TRUE(View.IsGrouped());

    TVector<int64> FromRanges;
    const int32 Slots = View.NumDenseSlots();
    for (int32 Begin = 0; Begin < Slots; Begin += 37)
    {
        View.ForEachInRange(Begin, Begin + 37, [&](ECS::FEntity Entity, const FGroupA& A, const FGroupB& B, const FGroupC& C)
        {
            FromRanges.Add((static_cast<int64>(Entity.GetPacked()) << 32) | static_cast<uint32>(A.Value + B.Value * 3 + C.Value * 7));
        });
    }
    std::sort(FromRanges.begin(), FromRanges.end());
    EXPECT_EQ(FromRanges, Expected);

    TVector<int64> FromEach;
    for (auto [Entity, A, B, C] : View.Each())
    {
        FromEach.Add((static_cast<int64>(Entity.GetPacked()) << 32) | static_cast<uint32>(A.Value + B.Value * 3 + C.Value * 7));
    }
    std::sort(FromEach.begin(), FromEach.end());
    EXPECT_EQ(FromEach, Expected);

    int32 EntityCount = 0;
    View.ForEachEntity([&](ECS::FEntity) { ++EntityCount; });
    EXPECT_EQ(EntityCount, Expected.Num());
}

TEST(ECSGroup, SortingAnOwnedPoolKeepsTheGroup)
{
    ECS::FRegistry Registry;
    ASSERT_NE((Registry.DeclareGroup<FGroupA, FGroupB, FGroupC>()), nullptr);

    std::mt19937 Rng(99);
    for (int32 Index = 0; Index < 300; ++Index)
    {
        const ECS::FEntity Entity = Registry.Create();
        const int32 Value = static_cast<int32>(Rng() % 10000);
        Registry.Emplace<FGroupA>(Entity).Value = Value;
        if (Index % 3 != 0)
        {
            Registry.Emplace<FGroupB>(Entity).Value = Value;
            Registry.Emplace<FGroupC>(Entity).Value = Value;
        }
    }

    const TVector<int64> Before = Collect(Registry);
    Registry.Sort<FGroupA>([](const FGroupA& L, const FGroupA& R) { return L.Value < R.Value; });
    ExpectPacked(Registry);
    EXPECT_EQ(Collect(Registry), Before);

    const ECS::FOwningGroup* Group = Registry.FindOwningGroup<FGroupA>();
    const auto Storage = Registry.FindStorage<FGroupA>();
    for (int32 Index = 1; Index < Group->Num; ++Index)
    {
        EXPECT_LE(Storage.GetAtDense(Index - 1).Value, Storage.GetAtDense(Index).Value);
    }
    for (int32 Index = Group->Num + 1; Index < Storage.GetDenseSize(); ++Index)
    {
        EXPECT_LE(Storage.GetAtDense(Index - 1).Value, Storage.GetAtDense(Index).Value);
    }

    Registry.Sort<FGroupB>([](const FGroupB& L, const FGroupB& R) { return L.Value > R.Value; });
    ExpectPacked(Registry);
    EXPECT_EQ(Collect(Registry), Before);
}

TEST(ECSGroup, ClearKeepsTheGroupConsistent)
{
    ECS::FRegistry Registry;
    const ECS::FOwningGroup* Group = Registry.DeclareGroup<FGroupA, FGroupB, FGroupC>();
    ASSERT_NE(Group, nullptr);
    for (int32 Index = 0; Index < 20; ++Index)
    {
        const ECS::FEntity Entity = Registry.Create();
        EmplaceOne(Registry, Entity, 0);
        EmplaceOne(Registry, Entity, 1);
        EmplaceOne(Registry, Entity, 2);
    }
    EXPECT_EQ(Group->Num, 20);

    Registry.ClearComponent<FGroupB>();
    EXPECT_EQ(Group->Num, 0);
    ExpectPacked(Registry);

    Registry.ForEachEntity([&](ECS::FEntity Entity) { EmplaceOne(Registry, Entity, 1); });
    EXPECT_EQ(Group->Num, 20);
    ExpectPacked(Registry);

    Registry.Clear();
    EXPECT_EQ(Group->Num, 0);
    EXPECT_TRUE(Collect(Registry).IsEmpty());
}

TEST(ECSGroup, RemovingTheCurrentEntityDuringAGroupedWalkIsSafe)
{
    ECS::FRegistry Registry;
    ASSERT_NE((Registry.DeclareGroup<FGroupA, FGroupB>()), nullptr);
    for (int32 Index = 0; Index < 64; ++Index)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<FGroupA>(Entity).Value = Index;
        Registry.Emplace<FGroupB>(Entity).Value = Index;
    }

    int32 Visited = 0;
    auto View = Registry.View<FGroupA, FGroupB>();
    ASSERT_TRUE(View.IsGrouped());
    View.ForEach([&](ECS::FEntity Entity, FGroupA& A, FGroupB& B)
    {
        EXPECT_EQ(A.Value, B.Value);
        ++Visited;
        if (A.Value % 2 == 0)
        {
            Registry.Remove<FGroupB>(Entity);
        }
    });
    EXPECT_EQ(Visited, 64);
    EXPECT_EQ(Registry.FindOwningGroup<FGroupA>()->Num, 32);
}
