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

    // The whole reason the node variant exists: a value address has to outlive every rehash.
    TEST(NodeHashMap, ValueAddressesSurviveGrowth)
    {
        TNodeMap<int, Lumina::FString> Map;

        std::vector<Lumina::FString*> Addresses;
        for (int Index = 0; Index < 2000; ++Index)
        {
            Map[Index] = Lumina::Format("value-{}", Index);
            Addresses.push_back(&Map.at(Index));
        }

        EXPECT_GT(Map.capacity(), 2000u);
        for (int Index = 0; Index < 2000; ++Index)
        {
            EXPECT_EQ(Addresses[static_cast<size_t>(Index)], &Map.at(Index)) << "moved " << Index;
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

        int* const Kept = &Map.at(250);
        for (int Index = 0; Index < 500; ++Index)
        {
            if (Index != 250)
            {
                Map.erase(Index);
            }
        }

        EXPECT_EQ(Map.size(), 1u);
        EXPECT_EQ(Kept, &Map.at(250));
        EXPECT_EQ(*Kept, 250);
    }

    // Contrast case, so the guarantee the flat table does NOT make is written down rather than assumed.
    TEST(NodeHashMap, FlatTableIsExpectedToMoveItsValues)
    {
        TFlatMap<int, int> Map;
        Map.reserve(8);
        Map[0] = 0;

        const int* const Before = &Map.at(0);
        for (int Index = 1; Index < 4000; ++Index)
        {
            Map[Index] = Index;
        }

        EXPECT_NE(Before, &Map.at(0));
        EXPECT_EQ(Map.at(0), 0);
    }

    TEST(NodeHashSet, ElementAddressesSurviveGrowth)
    {
        TNodeSet<int> Set;

        std::vector<const int*> Addresses;
        for (int Index = 0; Index < 1000; ++Index)
        {
            Set.insert(Index);
            Addresses.push_back(&*Set.find(Index));
        }

        for (int Index = 0; Index < 1000; ++Index)
        {
            EXPECT_EQ(Addresses[static_cast<size_t>(Index)], &*Set.find(Index)) << "moved " << Index;
        }
    }

    TEST(NodeHashMap, DestroysEveryNodeExactlyOnce)
    {
        FCounted::Live = 0;
        {
            TNodeMap<int, FCounted> Map;
            for (int Index = 0; Index < 800; ++Index)
            {
                Map.try_emplace(Index, Index);
            }
            EXPECT_EQ(FCounted::Live, 800);

            for (int Index = 0; Index < 400; ++Index)
            {
                Map.erase(Index);
            }
            EXPECT_EQ(FCounted::Live, 400);

            Map.clear();
            EXPECT_EQ(FCounted::Live, 0);

            for (int Index = 0; Index < 100; ++Index)
            {
                Map.try_emplace(Index, Index);
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
                Set.emplace(Index);
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

        EXPECT_EQ(Map.size(), 2u);
        EXPECT_TRUE(Map.contains(Lumina::FStringView("alpha")));
        EXPECT_EQ(Map.find(Lumina::FStringView("beta"))->second, 2);

        Map.insert_or_assign(Lumina::FString("beta"), 20);
        EXPECT_EQ(Map.at(Lumina::FString("beta")), 20);
        EXPECT_EQ(Map.size(), 2u);

        std::set<int> Seen;
        for (const auto& Pair : Map)
        {
            Seen.insert(Pair.second);
        }
        EXPECT_EQ(Seen, (std::set<int>{ 1, 20 }));
    }

    TEST(NodeHashMap, MoveAndCopyKeepTheirOwnNodes)
    {
        FCounted::Live = 0;
        TNodeMap<int, FCounted> Source;
        for (int Index = 0; Index < 50; ++Index)
        {
            Source.try_emplace(Index, Index);
        }

        TNodeMap<int, FCounted> Copy = Source;
        EXPECT_EQ(FCounted::Live, 100);
        EXPECT_NE(&Copy.at(7), &Source.at(7));
        EXPECT_EQ(Copy.at(7).Value, Source.at(7).Value);

        const FCounted* const Address = &Source.at(7);
        TNodeMap<int, FCounted> Moved = std::move(Source);
        EXPECT_EQ(FCounted::Live, 100);
        EXPECT_EQ(&Moved.at(7), Address);
        EXPECT_TRUE(Source.empty());
    }

    TEST(InlineHashSet, StaysOffTheHeapUntilItOutgrowsTheBuffer)
    {
        TInlineSet<int, 32> Set;
        EXPECT_TRUE(Set.IsInline());
        EXPECT_EQ(Set.GetAllocatedBytes(), 0u);
        EXPECT_GE(Set.capacity(), 31u);

        for (int Index = 0; Index < 20; ++Index)
        {
            Set.insert(Index);
        }
        EXPECT_TRUE(Set.IsInline());
        EXPECT_EQ(Set.GetAllocatedBytes(), 0u);

        for (int Index = 20; Index < 500; ++Index)
        {
            Set.insert(Index);
        }
        EXPECT_FALSE(Set.IsInline());
        EXPECT_GT(Set.GetAllocatedBytes(), 0u);

        EXPECT_EQ(Set.size(), 500u);
        for (int Index = 0; Index < 500; ++Index)
        {
            EXPECT_TRUE(Set.contains(Index)) << "lost " << Index;
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
        EXPECT_EQ(Map.GetAllocatedBytes(), 0u);

        for (int Index = 10; Index < 300; ++Index)
        {
            Map[Index] = Index * 2;
        }
        EXPECT_FALSE(Map.IsInline());

        for (int Index = 0; Index < 300; ++Index)
        {
            EXPECT_EQ(Map.at(Index), Index * 2) << "lost " << Index;
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
                Set.emplace(Index);
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
                Set.emplace(Index);
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
            Source.insert(Index);
        }

        TInlineSet<int, 32> Moved = std::move(Source);
        EXPECT_EQ(Moved.size(), 20u);
        EXPECT_TRUE(Moved.IsInline());
        for (int Index = 0; Index < 20; ++Index)
        {
            EXPECT_TRUE(Moved.contains(Index));
        }

        EXPECT_TRUE(Source.empty());
        EXPECT_FALSE(Source.contains(1));

        Source.insert(99);
        EXPECT_EQ(Source.size(), 1u);
        EXPECT_TRUE(Source.contains(99));
    }

    TEST(InlineHashSet, MoveFromASpilledTableStealsTheBlock)
    {
        TInlineSet<int, 8> Source;
        for (int Index = 0; Index < 400; ++Index)
        {
            Source.insert(Index);
        }
        EXPECT_FALSE(Source.IsInline());

        TInlineSet<int, 8> Moved = std::move(Source);
        EXPECT_EQ(Moved.size(), 400u);
        EXPECT_FALSE(Moved.IsInline());
        EXPECT_TRUE(Source.empty());
        EXPECT_TRUE(Source.IsInline());
    }

    TEST(InlineHashSet, SwapHandlesEveryCombinationOfInlineAndSpilled)
    {
        const auto Fill = [](auto& Set, int First, int Count)
        {
            for (int Index = First; Index < First + Count; ++Index)
            {
                Set.insert(Index);
            }
        };

        TInlineSet<int, 16> SmallLeft;
        TInlineSet<int, 16> SmallRight;
        Fill(SmallLeft, 0, 5);
        Fill(SmallRight, 100, 8);
        SmallLeft.swap(SmallRight);
        EXPECT_EQ(SmallLeft.size(), 8u);
        EXPECT_EQ(SmallRight.size(), 5u);
        EXPECT_TRUE(SmallLeft.contains(100));
        EXPECT_TRUE(SmallRight.contains(0));

        TInlineSet<int, 16> Inline;
        TInlineSet<int, 16> Spilled;
        Fill(Inline, 0, 5);
        Fill(Spilled, 1000, 300);
        Inline.swap(Spilled);
        EXPECT_EQ(Inline.size(), 300u);
        EXPECT_EQ(Spilled.size(), 5u);
        EXPECT_TRUE(Inline.contains(1200));
        EXPECT_TRUE(Spilled.contains(4));
        EXPECT_TRUE(Spilled.IsInline());
    }

    TEST(InlineHashSet, ShrinkToFitReturnsToTheInlineBuffer)
    {
        TInlineSet<int, 32> Set;
        for (int Index = 0; Index < 500; ++Index)
        {
            Set.insert(Index);
        }
        EXPECT_FALSE(Set.IsInline());

        Set.clear();
        Set.shrink_to_fit();
        EXPECT_TRUE(Set.IsInline());
        EXPECT_EQ(Set.GetAllocatedBytes(), 0u);

        Set.insert(1);
        EXPECT_TRUE(Set.IsInline());
        EXPECT_TRUE(Set.contains(1));
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
        EXPECT_EQ(Copy.size(), 6u);

        Copy[99] = 99;
        EXPECT_TRUE(Copy.contains(99));
        EXPECT_FALSE(Source.contains(99));
    }

    TEST(InlineHashSet, ErasesAndReinsertsWhileInline)
    {
        TInlineSet<int, 32> Set;
        for (int Round = 0; Round < 200; ++Round)
        {
            for (int Index = 0; Index < 12; ++Index)
            {
                Set.insert(Round * 12 + Index);
            }
            for (int Index = 0; Index < 12; ++Index)
            {
                EXPECT_EQ(Set.erase(Round * 12 + Index), 1u);
            }
        }

        EXPECT_TRUE(Set.empty());
        EXPECT_TRUE(Set.IsInline());
    }
}
