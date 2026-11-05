#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <vector>

#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Memory/Allocators/Allocator.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaHashVariantTests
{
    namespace Internal = Lumina::Containers::HashTableInternal;

    template <typename K, typename V>
    using TNodeMap = Lumina::Containers::TNodeHashMap<K, V>;

    template <typename T>
    using TNodeSet = Lumina::Containers::TNodeHashSet<T>;

    template <typename K, typename V, size_t N>
    using TInlineMap = Lumina::Containers::TInlineHashMap<K, V, N>;

    template <typename T, size_t N>
    using TInlineSet = Lumina::Containers::TInlineHashSet<T, N>;

    template <typename K, typename V>
    using TFlatMap = Lumina::Containers::THashMap<K, V>;

    struct FCounted
    {
        static inline int Live = 0;

        int Value = 0;

        FCounted() { ++Live; }
        explicit FCounted(int InValue) : Value(InValue) { ++Live; }
        FCounted(const FCounted& Other) : Value(Other.Value) { ++Live; }
        FCounted(FCounted&& Other) noexcept : Value(Other.Value) { ++Live; }
        FCounted& operator=(const FCounted&) = default;
        FCounted& operator=(FCounted&&) noexcept = default;
        ~FCounted() { --Live; }

        bool operator==(const FCounted& Other) const noexcept { return Value == Other.Value; }
    };

    inline Lumina::uint64 GetTypeHash(const FCounted& Counted) noexcept
    {
        return Lumina::Containers::MixHash64(static_cast<Lumina::uint64>(Counted.Value));
    }

    // The whole reason the node variant exists is that a value address outlives every rehash.
    TEST(NodeHashMap, ValueAddressesSurviveGrowth)
    {
        TNodeMap<int, Lumina::FString> Map;

        std::vector<Lumina::FString*> Addresses;
        for (int Index = 0; Index < 2000; ++Index)
        {
            Map[Index] = Lumina::Format("value-{}", Index);
            Addresses.push_back(&Map.FindChecked(Index));
        }

        EXPECT_GT(Map.Max(), 2000);
        for (int Index = 0; Index < 2000; ++Index)
        {
            EXPECT_EQ(Addresses[static_cast<size_t>(Index)], &Map.FindChecked(Index)) << "moved " << Index;
            EXPECT_EQ(*Addresses[static_cast<size_t>(Index)], Lumina::Format("value-{}", Index));
        }
    }

    TEST(NodeHashMap, ValueAddressesSurviveUnrelatedErases)
    {
        TNodeMap<int, int> Map;
        for (int Index = 0; Index < 500; ++Index)
        {
            Map[Index] = Index;
        }

        int* const Kept = &Map.FindChecked(250);
        for (int Index = 0; Index < 500; ++Index)
        {
            if (Index != 250)
            {
                Map.Remove(Index);
            }
        }

        EXPECT_EQ(Map.Num(), 1);
        EXPECT_EQ(Kept, &Map.FindChecked(250));
        EXPECT_EQ(*Kept, 250);
    }

    // Contrast case, so the guarantee the flat table does NOT make is written down rather than assumed.
    TEST(NodeHashMap, FlatTableIsExpectedToMoveItsValues)
    {
        TFlatMap<int, int> Map;
        Map.Reserve(8);
        Map[0] = 0;

        const int* const Before = &Map.FindChecked(0);
        for (int Index = 1; Index < 4000; ++Index)
        {
            Map[Index] = Index;
        }

        EXPECT_NE(Before, &Map.FindChecked(0));
        EXPECT_EQ(Map.FindChecked(0), 0);
    }

    TEST(NodeHashSet, ElementAddressesSurviveGrowth)
    {
        TNodeSet<int> Set;

        std::vector<const int*> Addresses;
        for (int Index = 0; Index < 1000; ++Index)
        {
            Set.Add(Index);
            Addresses.push_back(&*Set.Find(Index));
        }

        for (int Index = 0; Index < 1000; ++Index)
        {
            EXPECT_EQ(Addresses[static_cast<size_t>(Index)], &*Set.Find(Index)) << "moved " << Index;
        }
    }

    TEST(NodeHashMap, DestroysEveryNodeExactlyOnce)
    {
        FCounted::Live = 0;
        {
            TNodeMap<int, FCounted> Map;
            for (int Index = 0; Index < 800; ++Index)
            {
                Map.FindOrAdd(Index, Index);
            }
            EXPECT_EQ(FCounted::Live, 800);

            for (int Index = 0; Index < 400; ++Index)
            {
                Map.Remove(Index);
            }
            EXPECT_EQ(FCounted::Live, 400);

            Map.Reset();
            EXPECT_EQ(FCounted::Live, 0);

            for (int Index = 0; Index < 100; ++Index)
            {
                Map.FindOrAdd(Index, Index);
            }
            EXPECT_EQ(FCounted::Live, 100);
        }
        EXPECT_EQ(FCounted::Live, 0);
    }

    TEST(NodeHashSet, DestroysEveryNodeExactlyOnce)
    {
        FCounted::Live = 0;
        {
            TNodeSet<FCounted> Set;
            for (int Index = 0; Index < 600; ++Index)
            {
                Set.TryAdd(Index);
            }
            EXPECT_EQ(FCounted::Live, 600);
        }
        EXPECT_EQ(FCounted::Live, 0);
    }

    TEST(NodeHashMap, BehavesLikeTheFlatMapOtherwise)
    {
        TNodeMap<Lumina::FString, int> Map;
        Map[Lumina::FString("alpha")] = 1;
        Map[Lumina::FString("beta")] = 2;

        EXPECT_EQ(Map.Num(), 2);
        EXPECT_TRUE(Map.Contains(Lumina::FStringView("alpha")));
        EXPECT_EQ((*Map.Find(Lumina::FStringView("beta"))), 2);

        Map.Add(Lumina::FString("beta"), 20);
        EXPECT_EQ(Map.FindChecked(Lumina::FString("beta")), 20);
        EXPECT_EQ(Map.Num(), 2);

        std::set<int> Seen;
        for (const auto& Pair : Map)
        {
            Seen.insert(Pair.Value);
        }
        EXPECT_EQ(Seen, (std::set<int>{ 1, 20 }));
    }

    TEST(NodeHashMap, MoveAndCopyKeepTheirOwnNodes)
    {
        FCounted::Live = 0;
        TNodeMap<int, FCounted> Source;
        for (int Index = 0; Index < 50; ++Index)
        {
            Source.FindOrAdd(Index, Index);
        }

        TNodeMap<int, FCounted> Copy = Source;
        EXPECT_EQ(FCounted::Live, 100);
        EXPECT_NE(&Copy.FindChecked(7), &Source.FindChecked(7));
        EXPECT_EQ(Copy.FindChecked(7).Value, Source.FindChecked(7).Value);

        const FCounted* const Address = &Source.FindChecked(7);
        TNodeMap<int, FCounted> Moved = std::move(Source);
        EXPECT_EQ(FCounted::Live, 100);
        EXPECT_EQ(&Moved.FindChecked(7), Address);
        EXPECT_TRUE(Source.IsEmpty());
    }

    TEST(InlineHashSet, StaysOffTheHeapUntilItOutgrowsTheBuffer)
    {
        TInlineSet<int, 32> Set;
        EXPECT_TRUE(Set.IsInline());
        EXPECT_EQ(Set.GetAllocatedSize(), 0u);
        EXPECT_GE(Set.Max(), 31);

        for (int Index = 0; Index < 20; ++Index)
        {
            Set.Add(Index);
        }
        EXPECT_TRUE(Set.IsInline());
        EXPECT_EQ(Set.GetAllocatedSize(), 0u);

        for (int Index = 20; Index < 500; ++Index)
        {
            Set.Add(Index);
        }
        EXPECT_FALSE(Set.IsInline());
        EXPECT_GT(Set.GetAllocatedSize(), 0u);

        EXPECT_EQ(Set.Num(), 500);
        for (int Index = 0; Index < 500; ++Index)
        {
            EXPECT_TRUE(Set.Contains(Index)) << "lost " << Index;
        }
    }

    TEST(InlineHashMap, StaysOffTheHeapUntilItOutgrowsTheBuffer)
    {
        TInlineMap<int, int, 16> Map;
        EXPECT_TRUE(Map.IsInline());

        for (int Index = 0; Index < 10; ++Index)
        {
            Map[Index] = Index * 2;
        }
        EXPECT_TRUE(Map.IsInline());
        EXPECT_EQ(Map.GetAllocatedSize(), 0u);

        for (int Index = 10; Index < 300; ++Index)
        {
            Map[Index] = Index * 2;
        }
        EXPECT_FALSE(Map.IsInline());

        for (int Index = 0; Index < 300; ++Index)
        {
            EXPECT_EQ(Map.FindChecked(Index), Index * 2) << "lost " << Index;
        }
    }

    // N rounds UP to the next power of two minus one, so asking for 32 reserves 63 and asking for 31 reserves 31.
    TEST(InlineHashSet, CarriesItsBufferInTheObject)
    {
        EXPECT_GT((sizeof(TInlineSet<int, 32>)), (sizeof(Lumina::Containers::THashSet<int>)));

        EXPECT_EQ((TInlineSet<int, 31>::InlineCapacityV), 31u);
        EXPECT_EQ((TInlineSet<int, 32>::InlineCapacityV), 63u);
        EXPECT_EQ((TInlineSet<int, 16>::InlineCapacityV), 31u);
        EXPECT_EQ((TInlineMap<int, int, 100>::InlineCapacityV), 127u);

        // A power-of-two request costs nearly double what the next value down costs.
        EXPECT_LT((sizeof(TInlineSet<int, 31>)), (sizeof(TInlineSet<int, 32>)));
    }

    TEST(InlineHashSet, DestroysEveryElementWhileStillInline)
    {
        FCounted::Live = 0;
        {
            TInlineSet<FCounted, 32> Set;
            for (int Index = 0; Index < 10; ++Index)
            {
                Set.TryAdd(Index);
            }
            EXPECT_TRUE(Set.IsInline());
            EXPECT_EQ(FCounted::Live, 10);
        }
        EXPECT_EQ(FCounted::Live, 0);
    }

    TEST(InlineHashSet, DestroysEveryElementAfterSpilling)
    {
        FCounted::Live = 0;
        {
            TInlineSet<FCounted, 8> Set;
            for (int Index = 0; Index < 400; ++Index)
            {
                Set.TryAdd(Index);
            }
            EXPECT_FALSE(Set.IsInline());
            EXPECT_EQ(FCounted::Live, 400);
        }
        EXPECT_EQ(FCounted::Live, 0);
    }

    // An inline table cannot hand its buffer over, so moving one has to relocate the elements instead.
    TEST(InlineHashSet, MoveFromAnInlineTableRelocatesElements)
    {
        TInlineSet<int, 32> Source;
        for (int Index = 0; Index < 20; ++Index)
        {
            Source.Add(Index);
        }

        TInlineSet<int, 32> Moved = std::move(Source);
        EXPECT_EQ(Moved.Num(), 20);
        EXPECT_TRUE(Moved.IsInline());
        for (int Index = 0; Index < 20; ++Index)
        {
            EXPECT_TRUE(Moved.Contains(Index));
        }

        EXPECT_TRUE(Source.IsEmpty());
        EXPECT_FALSE(Source.Contains(1));

        Source.Add(99);
        EXPECT_EQ(Source.Num(), 1);
        EXPECT_TRUE(Source.Contains(99));
    }

    TEST(InlineHashSet, MoveFromASpilledTableStealsTheBlock)
    {
        TInlineSet<int, 8> Source;
        for (int Index = 0; Index < 400; ++Index)
        {
            Source.Add(Index);
        }
        EXPECT_FALSE(Source.IsInline());

        TInlineSet<int, 8> Moved = std::move(Source);
        EXPECT_EQ(Moved.Num(), 400);
        EXPECT_FALSE(Moved.IsInline());
        EXPECT_TRUE(Source.IsEmpty());
        EXPECT_TRUE(Source.IsInline());
    }

    TEST(InlineHashSet, SwapHandlesEveryCombinationOfInlineAndSpilled)
    {
        const auto Fill = [](auto& Set, int First, int Count)
        {
            for (int Index = First; Index < First + Count; ++Index)
            {
                Set.Add(Index);
            }
        };

        TInlineSet<int, 16> SmallLeft;
        TInlineSet<int, 16> SmallRight;
        Fill(SmallLeft, 0, 5);
        Fill(SmallRight, 100, 8);
        swap(SmallLeft, SmallRight);
        EXPECT_EQ(SmallLeft.Num(), 8);
        EXPECT_EQ(SmallRight.Num(), 5);
        EXPECT_TRUE(SmallLeft.Contains(100));
        EXPECT_TRUE(SmallRight.Contains(0));

        TInlineSet<int, 16> Inline;
        TInlineSet<int, 16> Spilled;
        Fill(Inline, 0, 5);
        Fill(Spilled, 1000, 300);
        swap(Inline, Spilled);
        EXPECT_EQ(Inline.Num(), 300);
        EXPECT_EQ(Spilled.Num(), 5);
        EXPECT_TRUE(Inline.Contains(1200));
        EXPECT_TRUE(Spilled.Contains(4));
        EXPECT_TRUE(Spilled.IsInline());
    }

    TEST(InlineHashSet, ShrinkToFitReturnsToTheInlineBuffer)
    {
        TInlineSet<int, 32> Set;
        for (int Index = 0; Index < 500; ++Index)
        {
            Set.Add(Index);
        }
        EXPECT_FALSE(Set.IsInline());

        Set.Reset();
        Set.Shrink();
        EXPECT_TRUE(Set.IsInline());
        EXPECT_EQ(Set.GetAllocatedSize(), 0u);

        Set.Add(1);
        EXPECT_TRUE(Set.IsInline());
        EXPECT_TRUE(Set.Contains(1));
    }

    TEST(InlineHashMap, CopyIsIndependentAndStartsInline)
    {
        TInlineMap<int, int, 16> Source;
        for (int Index = 0; Index < 6; ++Index)
        {
            Source[Index] = Index;
        }

        TInlineMap<int, int, 16> Copy = Source;
        EXPECT_TRUE(Copy.IsInline());
        EXPECT_EQ(Copy.Num(), 6);

        Copy[99] = 99;
        EXPECT_TRUE(Copy.Contains(99));
        EXPECT_FALSE(Source.Contains(99));
    }

    TEST(InlineHashSet, ErasesAndReinsertsWhileInline)
    {
        TInlineSet<int, 32> Set;
        for (int Round = 0; Round < 200; ++Round)
        {
            for (int Index = 0; Index < 12; ++Index)
            {
                Set.Add(Round * 12 + Index);
            }
            for (int Index = 0; Index < 12; ++Index)
            {
                EXPECT_EQ(Set.Remove(Round * 12 + Index), 1);
            }
        }

        EXPECT_TRUE(Set.IsEmpty());
        EXPECT_TRUE(Set.IsInline());
    }
}
