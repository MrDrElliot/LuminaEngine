#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Containers/Algorithm.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "Containers/Vector.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaAlgorithmTests
{
    namespace Algo = Lumina::Algo;
    using Lumina::FString;
    using Lumina::TVector;
    using Lumina::int32;
    using Lumina::uint32;
    using Lumina::uint64;

    uint32 NextRandom(uint64& State)
    {
        State = State * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32>(State >> 33);
    }

    std::vector<int32> MakeRandom(size_t Count, uint32 Modulus, uint64 Seed)
    {
        uint64 State = Seed;
        std::vector<int32> Values;
        Values.reserve(Count);
        for (size_t Index = 0; Index < Count; ++Index)
        {
            Values.push_back(static_cast<int32>(NextRandom(State) % Modulus));
        }

        return Values;
    }

    struct FKeyed
    {
        int32 Key = 0;
        int32 Order = 0;
    };

    TEST(AlgoSort, MatchesTheReferenceAcrossSizes)
    {
        for (size_t Count = 0; Count <= 300; ++Count)
        {
            std::vector<int32> Ours = MakeRandom(Count, 1000, Count + 1);
            std::vector<int32> Reference = Ours;

            Algo::Sort(Ours.begin(), Ours.end());
            std::sort(Reference.begin(), Reference.end());

            ASSERT_EQ(Ours, Reference) << "count " << Count;
        }
    }

    TEST(AlgoSort, HandlesAdversarialInputs)
    {
        constexpr size_t kCount = 5000;

        std::vector<int32> Ascending(kCount);
        for (size_t Index = 0; Index < kCount; ++Index)
        {
            Ascending[Index] = static_cast<int32>(Index);
        }

        std::vector<int32> Descending(Ascending.rbegin(), Ascending.rend());
        std::vector<int32> AllEqual(kCount, 7);
        std::vector<int32> FewDistinct = MakeRandom(kCount, 3, 99);
        std::vector<int32> Organ(kCount);
        for (size_t Index = 0; Index < kCount; ++Index)
        {
            Organ[Index] = static_cast<int32>(Index < kCount / 2 ? Index : kCount - Index);
        }

        for (std::vector<int32>* Case : { &Ascending, &Descending, &AllEqual, &FewDistinct, &Organ })
        {
            std::vector<int32> Reference = *Case;
            Algo::Sort(Case->begin(), Case->end());
            std::sort(Reference.begin(), Reference.end());
            EXPECT_EQ(*Case, Reference);
        }
    }

    TEST(AlgoSort, TakesACustomComparator)
    {
        std::vector<int32> Values = MakeRandom(500, 200, 4);
        Algo::Sort(Values.begin(), Values.end(), [](int32 Left, int32 Right) { return Left > Right; });

        EXPECT_TRUE(std::is_sorted(Values.begin(), Values.end(), std::greater<int32>{}));
    }

    TEST(AlgoSort, MovesRatherThanCopiesNonTrivialElements)
    {
        TVector<FString> Values;
        for (int32 Index = 0; Index < 200; ++Index)
        {
            Values.push_back(Lumina::Format("item-{:04}", (Index * 37) % 200));
        }

        Algo::Sort(Values.begin(), Values.end());

        for (size_t Index = 1; Index < Values.size(); ++Index)
        {
            EXPECT_LE(Values[Index - 1], Values[Index]);
        }
        EXPECT_EQ(Values.size(), 200u);
    }

    TEST(AlgoSort, DrivenIntoItsHeapsortFallbackStillSorts)
    {
        // A median-of-three killer, so the depth limit has to catch the quicksort degradation.
        constexpr size_t kCount = 4096;
        std::vector<int32> Values(kCount);
        for (size_t Index = 0; Index < kCount; ++Index)
        {
            Values[Index] = static_cast<int32>((Index % 2 == 0) ? Index / 2 : kCount - Index / 2);
        }

        std::vector<int32> Reference = Values;
        Algo::Sort(Values.begin(), Values.end());
        std::sort(Reference.begin(), Reference.end());

        EXPECT_EQ(Values, Reference);
    }

    TEST(AlgoStableSort, KeepsEqualElementsInOrder)
    {
        for (size_t Count : { size_t(0), size_t(1), size_t(31), size_t(32), size_t(33), size_t(64), size_t(1000) })
        {
            uint64 State = Count + 17;
            std::vector<FKeyed> Values;
            for (size_t Index = 0; Index < Count; ++Index)
            {
                Values.push_back(FKeyed{ static_cast<int32>(NextRandom(State) % 8), static_cast<int32>(Index) });
            }

            Algo::StableSort(Values.begin(), Values.end(),
                             [](const FKeyed& Left, const FKeyed& Right) { return Left.Key < Right.Key; });

            for (size_t Index = 1; Index < Values.size(); ++Index)
            {
                ASSERT_LE(Values[Index - 1].Key, Values[Index].Key) << "count " << Count;
                if (Values[Index - 1].Key == Values[Index].Key)
                {
                    ASSERT_LT(Values[Index - 1].Order, Values[Index].Order) << "count " << Count;
                }
            }
        }
    }

    TEST(AlgoStableSort, DestroysEveryElementItMovedThroughTheBuffer)
    {
        TVector<FString> Values;
        for (int32 Index = 0; Index < 500; ++Index)
        {
            Values.push_back(Lumina::Format("a-fairly-long-string-that-heap-allocates-{}", (Index * 31) % 500));
        }

        Algo::StableSort(Values.begin(), Values.end());

        EXPECT_EQ(Values.size(), 500u);
        for (size_t Index = 1; Index < Values.size(); ++Index)
        {
            ASSERT_LE(Values[Index - 1], Values[Index]);
        }
    }

    TEST(AlgoNthElement, PutsTheRequestedRankInPlace)
    {
        std::vector<int32> Values = MakeRandom(777, 5000, 11);
        std::vector<int32> Reference = Values;
        std::sort(Reference.begin(), Reference.end());

        const size_t Rank = 300;
        Algo::NthElement(Values.begin(), Values.begin() + Rank, Values.end());

        EXPECT_EQ(Values[Rank], Reference[Rank]);
        for (size_t Index = 0; Index < Rank; ++Index)
        {
            ASSERT_LE(Values[Index], Values[Rank]);
        }
        for (size_t Index = Rank + 1; Index < Values.size(); ++Index)
        {
            ASSERT_GE(Values[Index], Values[Rank]);
        }
    }

    TEST(AlgoStablePartition, KeepsRelativeOrderOnBothSides)
    {
        std::vector<int32> Values;
        for (int32 Index = 0; Index < 100; ++Index)
        {
            Values.push_back(Index);
        }

        auto IsEven = [](int32 Value) { return Value % 2 == 0; };
        auto Split = Algo::StablePartition(Values.begin(), Values.end(), IsEven);

        EXPECT_EQ(Split - Values.begin(), 50);
        for (int32 Index = 0; Index < 50; ++Index)
        {
            EXPECT_EQ(Values[Index], Index * 2);
            EXPECT_EQ(Values[50 + Index], Index * 2 + 1);
        }
    }

    TEST(AlgoSearch, FindsAndCounts)
    {
        const std::vector<int32> Values = { 4, 8, 15, 16, 23, 42, 8 };

        EXPECT_EQ(Algo::Find(Values.begin(), Values.end(), 15) - Values.begin(), 2);
        EXPECT_EQ(Algo::Find(Values.begin(), Values.end(), 99), Values.end());
        EXPECT_TRUE(Algo::Contains(Values.begin(), Values.end(), 42));
        EXPECT_FALSE(Algo::Contains(Values.begin(), Values.end(), 43));
        EXPECT_EQ(Algo::Count(Values.begin(), Values.end(), 8), 2);
        EXPECT_EQ(Algo::CountIf(Values.begin(), Values.end(), [](int32 V) { return V % 2 == 0; }), 5);

        auto IsOdd = [](int32 V) { return V % 2 != 0; };
        EXPECT_EQ(Algo::FindIf(Values.begin(), Values.end(), IsOdd) - Values.begin(), 2);
        EXPECT_EQ(Algo::FindIfNot(Values.begin(), Values.end(), IsOdd), Values.begin());
    }

    TEST(AlgoSearch, QuantifiersOnAnEmptyRangeMatchTheStandard)
    {
        const std::vector<int32> Empty;
        auto Always = [](int32) { return true; };

        EXPECT_TRUE(Algo::AllOf(Empty.begin(), Empty.end(), Always));
        EXPECT_FALSE(Algo::AnyOf(Empty.begin(), Empty.end(), Always));
        EXPECT_TRUE(Algo::NoneOf(Empty.begin(), Empty.end(), Always));
    }

    TEST(AlgoMutate, RemovesAndCompacts)
    {
        std::vector<int32> Values = { 1, 2, 3, 2, 4, 2, 5 };
        Values.erase(Algo::Remove(Values.begin(), Values.end(), 2), Values.end());
        EXPECT_EQ(Values, (std::vector<int32>{ 1, 3, 4, 5 }));

        std::vector<int32> Others = { 1, 2, 3, 4, 5, 6 };
        Others.erase(Algo::RemoveIf(Others.begin(), Others.end(), [](int32 V) { return V % 2 == 0; }), Others.end());
        EXPECT_EQ(Others, (std::vector<int32>{ 1, 3, 5 }));

        std::vector<int32> Nothing = { 1, 3, 5 };
        EXPECT_EQ(Algo::Remove(Nothing.begin(), Nothing.end(), 2), Nothing.end());
    }

    TEST(AlgoMutate, CollapsesAdjacentDuplicates)
    {
        std::vector<int32> Values = { 1, 1, 2, 2, 2, 3, 1 };
        Values.erase(Algo::Unique(Values.begin(), Values.end()), Values.end());
        EXPECT_EQ(Values, (std::vector<int32>{ 1, 2, 3, 1 }));

        std::vector<int32> Empty;
        EXPECT_EQ(Algo::Unique(Empty.begin(), Empty.end()), Empty.end());
    }

    TEST(AlgoMutate, ReversesAndRotates)
    {
        std::vector<int32> Values = { 1, 2, 3, 4, 5 };
        Algo::Reverse(Values.begin(), Values.end());
        EXPECT_EQ(Values, (std::vector<int32>{ 5, 4, 3, 2, 1 }));

        std::vector<int32> Ordered = { 1, 2, 3, 4, 5 };
        auto Where = Algo::Rotate(Ordered.begin(), Ordered.begin() + 2, Ordered.end());
        EXPECT_EQ(Ordered, (std::vector<int32>{ 3, 4, 5, 1, 2 }));
        EXPECT_EQ(Where - Ordered.begin(), 3);
    }

    TEST(AlgoMutate, ReplacesFillsAndCopies)
    {
        std::vector<int32> Values = { 1, 2, 1, 2 };
        Algo::Replace(Values.begin(), Values.end(), 1, 9);
        EXPECT_EQ(Values, (std::vector<int32>{ 9, 2, 9, 2 }));

        Algo::ReplaceIf(Values.begin(), Values.end(), [](int32 V) { return V == 2; }, 0);
        EXPECT_EQ(Values, (std::vector<int32>{ 9, 0, 9, 0 }));

        std::vector<int32> Filled(3);
        Algo::Fill(Filled.begin(), Filled.end(), 5);
        EXPECT_EQ(Filled, (std::vector<int32>{ 5, 5, 5 }));

        std::vector<int32> Counted(4);
        Algo::Iota(Counted.begin(), Counted.end(), 10);
        EXPECT_EQ(Counted, (std::vector<int32>{ 10, 11, 12, 13 }));

        std::vector<int32> Destination(4);
        Algo::Copy(Counted.begin(), Counted.end(), Destination.begin());
        EXPECT_EQ(Destination, Counted);

        std::vector<int32> Odds(2);
        Algo::CopyIf(Counted.begin(), Counted.end(), Odds.begin(), [](int32 V) { return V % 2 != 0; });
        EXPECT_EQ(Odds, (std::vector<int32>{ 11, 13 }));

        std::vector<int32> Doubled(4);
        Algo::Transform(Counted.begin(), Counted.end(), Doubled.begin(), [](int32 V) { return V * 2; });
        EXPECT_EQ(Doubled, (std::vector<int32>{ 20, 22, 24, 26 }));
    }

    TEST(AlgoOrdered, BoundsAndBinarySearch)
    {
        const std::vector<int32> Values = { 1, 3, 3, 3, 5, 7 };

        EXPECT_EQ(Algo::LowerBound(Values.begin(), Values.end(), 3) - Values.begin(), 1);
        EXPECT_EQ(Algo::UpperBound(Values.begin(), Values.end(), 3) - Values.begin(), 4);
        EXPECT_EQ(Algo::LowerBound(Values.begin(), Values.end(), 4) - Values.begin(), 4);
        EXPECT_EQ(Algo::LowerBound(Values.begin(), Values.end(), 0), Values.begin());
        EXPECT_EQ(Algo::UpperBound(Values.begin(), Values.end(), 9), Values.end());

        EXPECT_TRUE(Algo::BinarySearch(Values.begin(), Values.end(), 5));
        EXPECT_FALSE(Algo::BinarySearch(Values.begin(), Values.end(), 4));
        EXPECT_FALSE(Algo::BinarySearch(Values.begin(), Values.end(), 8));
    }

    TEST(AlgoOrdered, ExtremesAndSortedCheck)
    {
        const std::vector<int32> Values = { 4, 1, 9, 1, 9, 2 };

        EXPECT_EQ(Algo::MinElement(Values.begin(), Values.end()) - Values.begin(), 1);
        EXPECT_EQ(Algo::MaxElement(Values.begin(), Values.end()) - Values.begin(), 2);

        const std::vector<int32> Empty;
        EXPECT_EQ(Algo::MinElement(Empty.begin(), Empty.end()), Empty.end());

        EXPECT_FALSE(Algo::IsSorted(Values.begin(), Values.end()));
        EXPECT_TRUE(Algo::IsSorted(Empty.begin(), Empty.end()));
        EXPECT_TRUE(Algo::IsSorted(Values.begin(), Values.begin() + 1));
    }

    TEST(AlgoOrdered, EqualComparesElementwise)
    {
        const std::vector<int32> Left = { 1, 2, 3 };
        const std::vector<int32> Same = { 1, 2, 3 };
        const std::vector<int32> Different = { 1, 2, 4 };

        EXPECT_TRUE(Algo::Equal(Left.begin(), Left.end(), Same.begin()));
        EXPECT_FALSE(Algo::Equal(Left.begin(), Left.end(), Different.begin()));
    }

    TEST(AlgoForEach, VisitsEveryElement)
    {
        const std::vector<int32> Values = { 1, 2, 3, 4 };
        int32 Sum = 0;
        Algo::ForEach(Values.begin(), Values.end(), [&Sum](int32 V) { Sum += V; });
        EXPECT_EQ(Sum, 10);
    }

    struct FNamed
    {
        FString Name;
        int32   Cost = 0;

        bool IsExpensive() const { return Cost > 10; }
    };

    TVector<FNamed> MakeNamed()
    {
        TVector<FNamed> Items;
        Items.push_back(FNamed{ "alpha", 5 });
        Items.push_back(FNamed{ "beta", 20 });
        Items.push_back(FNamed{ "gamma", 15 });
        return Items;
    }

    TEST(AlgoRange, MatchesTheIteratorOverloads)
    {
        const std::vector<int32> Values = { 4, 1, 3, 1, 5 };

        EXPECT_EQ(Algo::Find(Values, 3), Algo::Find(Values.begin(), Values.end(), 3));
        EXPECT_EQ(Algo::Count(Values, 1), Algo::Count(Values.begin(), Values.end(), 1));
        EXPECT_TRUE(Algo::Contains(Values, 5));
        EXPECT_FALSE(Algo::Contains(Values, 99));

        auto IsOdd = [](int32 V) { return V % 2 != 0; };
        EXPECT_EQ(Algo::CountIf(Values, IsOdd), 4);
        EXPECT_TRUE(Algo::AnyOf(Values, IsOdd));
        EXPECT_FALSE(Algo::AllOf(Values, IsOdd));
        EXPECT_FALSE(Algo::NoneOf(Values, IsOdd));
        EXPECT_EQ(*Algo::MinElement(Values), 1);
        EXPECT_EQ(*Algo::MaxElement(Values), 5);
    }

    TEST(AlgoRange, SortsAndSearchesThroughTheRangeForm)
    {
        std::vector<int32> Values = MakeRandom(64, 100, 7);
        std::vector<int32> Reference = Values;

        Algo::Sort(Values);
        std::sort(Reference.begin(), Reference.end());

        EXPECT_EQ(Values, Reference);
        EXPECT_TRUE(Algo::IsSorted(Values));
        EXPECT_TRUE(Algo::BinarySearch(Values, Values[10]));

        std::vector<int32> Reversed = Values;
        Algo::Reverse(Reversed);
        std::reverse(Reference.begin(), Reference.end());
        EXPECT_EQ(Reversed, Reference);
    }

    TEST(AlgoRange, EqualComparesLengthAsWellAsElements)
    {
        const std::vector<int32> Left = { 1, 2, 3 };
        const std::vector<int32> Same = { 1, 2, 3 };
        const std::vector<int32> Shorter = { 1, 2 };

        EXPECT_TRUE(Algo::Equal(Left, Same));
        EXPECT_FALSE(Algo::Equal(Left, Shorter));
    }

    TEST(AlgoProjection, AcceptsMemberPointersInPlaceOfCallables)
    {
        const TVector<FNamed> Items = MakeNamed();

        EXPECT_TRUE(Algo::AnyOf(Items, &FNamed::IsExpensive));
        EXPECT_FALSE(Algo::AllOf(Items, &FNamed::IsExpensive));
        EXPECT_EQ(Algo::CountIf(Items, &FNamed::IsExpensive), 2);

        EXPECT_TRUE(Algo::Contains(Items, FString("beta"), &FNamed::Name));
        EXPECT_FALSE(Algo::Contains(Items, FString("delta"), &FNamed::Name));
        EXPECT_EQ(Algo::Count(Items, 15, &FNamed::Cost), 1);
    }

    TEST(AlgoProjection, ReachesThroughPointerElements)
    {
        TVector<FNamed> Storage = MakeNamed();

        TVector<FNamed*> Pointers;
        for (FNamed& Item : Storage)
        {
            Pointers.push_back(&Item);
        }

        EXPECT_TRUE(Algo::Contains(Pointers, FString("beta"), &FNamed::Name));
        EXPECT_EQ(Algo::IndexOf(Pointers, 15, &FNamed::Cost), 2);
        EXPECT_EQ(Algo::CountIf(Pointers, &FNamed::IsExpensive), 2);
        EXPECT_EQ(Algo::Sum(Pointers, &FNamed::Cost), 40);
    }

    TEST(AlgoRange, AcceptsCArrays)
    {
        static const FNamed Table[] = { { "alpha", 5 }, { "beta", 20 }, { "gamma", 15 } };
        const int32 Values[] = { 3, 1, 2 };

        EXPECT_EQ(Algo::IndexOf(Table, FString("beta"), &FNamed::Name), 1);
        EXPECT_TRUE(Algo::Contains(Values, 2));
        EXPECT_EQ(Algo::Sum(Values), 6);
        EXPECT_EQ(*Algo::MinElement(Values), 1);
        EXPECT_EQ(Algo::CountIf(Table, &FNamed::IsExpensive), 2);
    }

    TEST(AlgoIndexOf, ReturnsThePositionOrIndexNone)
    {
        const TVector<FNamed> Items = MakeNamed();

        EXPECT_EQ(Algo::IndexOf(Items, FString("gamma"), &FNamed::Name), 2);
        EXPECT_EQ(Algo::IndexOf(Items, FString("missing"), &FNamed::Name), INDEX_NONE);
        EXPECT_EQ(Algo::IndexOfIf(Items, &FNamed::IsExpensive), 1);

        const std::vector<int32> Values = { 7, 8, 9 };
        EXPECT_EQ(Algo::IndexOf(Values, 9), 2);
        EXPECT_EQ(Algo::IndexOf(Values, 0), INDEX_NONE);

        const std::vector<int32> Empty;
        EXPECT_EQ(Algo::IndexOf(Empty, 1), INDEX_NONE);
        EXPECT_EQ(Algo::IndexOfIf(Empty, [](int32 V) { return V > 0; }), INDEX_NONE);
    }

    TEST(AlgoAccumulate, FoldsWithAndWithoutAProjection)
    {
        const std::vector<int32> Values = { 1, 2, 3, 4 };
        const TVector<FNamed> Items = MakeNamed();
        const std::vector<int32> Empty;

        EXPECT_EQ(Algo::Accumulate(Values, 0), 10);
        EXPECT_EQ(Algo::Accumulate(Values, 100), 110);
        EXPECT_EQ(Algo::Sum(Values), 10);
        EXPECT_EQ(Algo::Sum(Empty), 0);
        EXPECT_EQ(Algo::Sum(Items, &FNamed::Cost), 40);
        EXPECT_EQ(Algo::Accumulate(Values.begin(), Values.end(), 0), 10);
    }

    TEST(AlgoRange, MutatesThroughTheRangeForm)
    {
        std::vector<int32> Values = { 1, 2, 3, 2, 1 };

        Algo::Replace(Values, 2, 9);
        EXPECT_EQ(Values, (std::vector<int32>{ 1, 9, 3, 9, 1 }));

        Algo::ReplaceIf(Values, [](int32 V) { return V == 9; }, 0);
        EXPECT_EQ(Values, (std::vector<int32>{ 1, 0, 3, 0, 1 }));

        std::vector<int32> Filled(4);
        Algo::Fill(Filled, 7);
        EXPECT_EQ(Filled, (std::vector<int32>{ 7, 7, 7, 7 }));

        std::vector<int32> Counted(4);
        Algo::Iota(Counted, 1);
        EXPECT_EQ(Counted, (std::vector<int32>{ 1, 2, 3, 4 }));

        std::vector<int32> Source = { 1, 2, 3, 4 };
        std::vector<int32> Doubled;
        Algo::Transform(Source, std::back_inserter(Doubled), [](int32 V) { return V * 2; });
        EXPECT_EQ(Doubled, (std::vector<int32>{ 2, 4, 6, 8 }));

        std::vector<int32> Odds;
        Algo::CopyIf(Source, std::back_inserter(Odds), [](int32 V) { return V % 2 != 0; });
        EXPECT_EQ(Odds, (std::vector<int32>{ 1, 3 }));

        TVector<FNamed> Items = MakeNamed();
        std::vector<FString> Names;
        Algo::Transform(Items, std::back_inserter(Names), &FNamed::Name);
        EXPECT_EQ(Names.size(), 3u);
        EXPECT_EQ(Names[1], FString("beta"));
    }

    TEST(AlgoRange, RemovesAndDedupesThroughTheRangeForm)
    {
        std::vector<int32> Values = { 1, 2, 2, 3, 3, 3 };

        const auto UniqueEnd = Algo::Unique(Values);
        Values.erase(UniqueEnd, Values.end());
        EXPECT_EQ(Values, (std::vector<int32>{ 1, 2, 3 }));

        std::vector<int32> Mixed = { 1, 2, 3, 4, 5 };
        const auto RemovedEnd = Algo::RemoveIf(Mixed, [](int32 V) { return V % 2 == 0; });
        Mixed.erase(RemovedEnd, Mixed.end());
        EXPECT_EQ(Mixed, (std::vector<int32>{ 1, 3, 5 }));
    }
}
