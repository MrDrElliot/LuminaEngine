#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

#include "Containers/HashTable.h"
#include "Containers/Pair.h"
#include "Containers/Span.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Memory/Allocators/Allocator.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaHashTableTests
{
    namespace Internal = Lumina::Containers::HashTableInternal;

    template <typename K, typename V>
    using TMap = Lumina::Containers::THashMap<K, V>;

    template <typename T>
    using TSet = Lumina::Containers::THashSet<T>;

    struct FTracked
    {
        static inline int Live = 0;
        static inline int Moves = 0;

        int Value = 0;

        FTracked() { ++Live; }
        explicit FTracked(int InValue) : Value(InValue) { ++Live; }
        FTracked(const FTracked& Other) : Value(Other.Value) { ++Live; }
        FTracked(FTracked&& Other) noexcept : Value(Other.Value) { ++Live; ++Moves; }
        FTracked& operator=(const FTracked& Other) { Value = Other.Value; return *this; }
        FTracked& operator=(FTracked&& Other) noexcept { Value = Other.Value; return *this; }
        ~FTracked() { --Live; }

        bool operator==(const FTracked& Other) const noexcept { return Value == Other.Value; }
    };

    inline Lumina::uint64 GetTypeHash(const FTracked& Tracked) noexcept
    {
        return Lumina::Containers::MixHash64(static_cast<Lumina::uint64>(Tracked.Value));
    }

    TEST(HashTableControl, ByteEncodingSeparatesFullFromSpecial)
    {
        static_assert(Internal::CtrlEmpty == -128);
        static_assert(Internal::CtrlDeleted == -2);
        static_assert(Internal::CtrlSentinel == -1);

        EXPECT_TRUE(Internal::IsEmpty(Internal::CtrlEmpty));
        EXPECT_TRUE(Internal::IsDeleted(Internal::CtrlDeleted));
        EXPECT_TRUE(Internal::IsEmptyOrDeleted(Internal::CtrlEmpty));
        EXPECT_TRUE(Internal::IsEmptyOrDeleted(Internal::CtrlDeleted));
        EXPECT_FALSE(Internal::IsEmptyOrDeleted(Internal::CtrlSentinel));
        EXPECT_FALSE(Internal::IsFull(Internal::CtrlSentinel));

        for (int H = 0; H < 128; ++H)
        {
            const Internal::FCtrl Byte = static_cast<Internal::FCtrl>(H);
            EXPECT_TRUE(Internal::IsFull(Byte));
            EXPECT_FALSE(Internal::IsEmptyOrDeleted(Byte));
        }
    }

    TEST(HashTableControl, HashSplitCoversBothHalves)
    {
        const Lumina::uint64 Hash = 0xDEADBEEFCAFEF00Dull;
        EXPECT_EQ(Internal::H2(Hash), static_cast<Internal::FCtrl>(Hash & 0x7F));
        EXPECT_EQ(Internal::H1(Hash), Hash >> 7);
        EXPECT_TRUE(Internal::IsFull(Internal::H2(Hash)));
    }

    // Both group backends must agree; on x86 only the SSE2 one is selected, so this is the portable path's only cover.
    template <typename TGroup>
    void RunGroupContract()
    {
        alignas(16) Internal::FCtrl Bytes[32];
        std::fill(std::begin(Bytes), std::end(Bytes), Internal::CtrlEmpty);

        Bytes[0] = static_cast<Internal::FCtrl>(0x11);
        Bytes[1] = Internal::CtrlDeleted;
        Bytes[2] = static_cast<Internal::FCtrl>(0x11);
        Bytes[3] = static_cast<Internal::FCtrl>(0x22);

        const TGroup Group(Bytes);

        std::vector<Lumina::uint32> Matches;
        for (Lumina::uint32 Bit : Group.Match(static_cast<Internal::FCtrl>(0x11)))
        {
            Matches.push_back(Bit);
        }
        EXPECT_EQ(Matches, (std::vector<Lumina::uint32>{ 0u, 2u }));

        EXPECT_EQ(*Group.MatchEmpty(), 4u);
        EXPECT_EQ(*Group.MatchEmptyOrDeleted(), 1u);
        EXPECT_EQ(*Group.MatchFull(), 0u);
        EXPECT_EQ(Group.CountLeadingEmptyOrDeleted(), 0u);

        alignas(16) Internal::FCtrl AllEmpty[32];
        std::fill(std::begin(AllEmpty), std::end(AllEmpty), Internal::CtrlEmpty);
        EXPECT_EQ(TGroup(AllEmpty).CountLeadingEmptyOrDeleted(), static_cast<Lumina::uint32>(TGroup::kWidth));

        alignas(16) Internal::FCtrl Converted[32];
        std::fill(std::begin(Converted), std::end(Converted), Internal::CtrlEmpty);
        Group.ConvertSpecialToEmptyAndFullToDeleted(Converted);
        EXPECT_TRUE(Internal::IsDeleted(Converted[0]));
        EXPECT_TRUE(Internal::IsEmpty(Converted[1]));
        EXPECT_TRUE(Internal::IsDeleted(Converted[2]));
        EXPECT_TRUE(Internal::IsDeleted(Converted[3]));
    }

    TEST(HashTableGroup, PortableBackendHonorsTheContract)
    {
        RunGroupContract<Internal::FGroupPortable>();
    }

#if LUMINA_HASHTABLE_SSE2
    TEST(HashTableGroup, Sse2BackendHonorsTheContract)
    {
        RunGroupContract<Internal::FGroupSse2>();
    }
#endif

    TEST(HashTableCapacity, GrowthMathStaysOnPowerOfTwoMinusOne)
    {
        for (size_t Count = 0; Count < 1000; ++Count)
        {
            const size_t Capacity = Internal::NormalizeCapacity(Count);
            EXPECT_TRUE(Internal::IsValidCapacity(Capacity));
            EXPECT_GE(Capacity, Count == 0 ? 1u : Count);
        }

        EXPECT_EQ(Internal::NormalizeCapacity(0), 1u);
        EXPECT_EQ(Internal::NormalizeCapacity(1), 1u);
        EXPECT_EQ(Internal::NormalizeCapacity(2), 3u);
        EXPECT_EQ(Internal::NormalizeCapacity(5), 7u);
        EXPECT_EQ(Internal::CapacityToGrowth(7), 7u - 0u);
        EXPECT_EQ(Internal::CapacityToGrowth(15), 14u);
        EXPECT_EQ(Internal::CapacityToGrowth(1023), 896u);
    }

    TEST(HashSetBasics, EmptySetNeverAllocatesAndNeverMatches)
    {
        const TSet<int> Set;
        EXPECT_TRUE(Set.IsEmpty());
        EXPECT_EQ(Set.Num(), 0);
        EXPECT_EQ(Set.Max(), 0);
        EXPECT_EQ(Set.GetAllocatedSize(), 0u);
        EXPECT_FALSE(Set.Contains(0));
        EXPECT_FALSE(Set.Contains(12345));
        EXPECT_EQ(Set.begin(), Set.end());
    }

    TEST(HashSetBasics, InsertFindEraseRoundTrip)
    {
        TSet<int> Set;
        EXPECT_TRUE(Set.TryAdd(7));
        EXPECT_FALSE(Set.TryAdd(7));
        EXPECT_EQ(Set.Num(), 1);
        EXPECT_TRUE(Set.Contains(7));
        EXPECT_EQ(Set.Contains(7), 1u);
        EXPECT_EQ(Set.Contains(8), 0u);

        EXPECT_EQ(Set.Remove(8), 0);
        EXPECT_EQ(Set.Remove(7), 1);
        EXPECT_TRUE(Set.IsEmpty());
        EXPECT_FALSE(Set.Contains(7));
    }

    TEST(HashSetBasics, InitializerListDeduplicates)
    {
        const TSet<int> Set{ 1, 2, 2, 3, 3, 3 };
        EXPECT_EQ(Set.Num(), 3);
        EXPECT_TRUE(Set.Contains(1));
        EXPECT_TRUE(Set.Contains(2));
        EXPECT_TRUE(Set.Contains(3));
    }

    TEST(HashSetGrowth, HoldsEveryKeyAcrossManyRehashes)
    {
        constexpr int kCount = 20000;
        TSet<int> Set;
        for (int i = 0; i < kCount; ++i)
        {
            EXPECT_TRUE(Set.TryAdd(i));
        }

        EXPECT_EQ(Set.Num(), kCount);
        for (int i = 0; i < kCount; ++i)
        {
            EXPECT_TRUE(Set.Contains(i)) << "missing " << i;
        }
        for (int i = kCount; i < kCount * 2; ++i)
        {
            EXPECT_FALSE(Set.Contains(i)) << "phantom " << i;
        }
    }

    TEST(HashSetGrowth, LoadFactorStaysUnderSevenEighths)
    {
        TSet<int> Set;
        for (int i = 0; i < 5000; ++i)
        {
            Set.Add(i * 7);
            EXPECT_LE(Set.Num(), Internal::CapacityToGrowth(Set.Max()));
        }
    }

    TEST(HashSetGrowth, ReserveAvoidsFurtherRehashing)
    {
        TSet<int> Set;
        Set.Reserve(1000);
        const size_t Reserved = Set.Max();
        EXPECT_GE(Internal::CapacityToGrowth(Reserved), 1000u);

        for (int i = 0; i < 1000; ++i)
        {
            Set.Add(i);
        }
        EXPECT_EQ(Set.Max(), Reserved);
    }

    TEST(HashSetIteration, VisitsEveryElementExactlyOnce)
    {
        TSet<int> Set;
        for (int i = 0; i < 500; ++i)
        {
            Set.Add(i);
        }

        std::set<int> Seen;
        for (int Value : Set)
        {
            EXPECT_TRUE(Seen.insert(Value).second) << "duplicate " << Value;
        }
        EXPECT_EQ(Seen.size(), 500u);
    }

    TEST(HashSetIteration, SurvivesInterleavedEraseAndInsert)
    {
        TSet<int> Set;
        for (int i = 0; i < 2000; ++i)
        {
            Set.Add(i);
        }
        for (int i = 0; i < 2000; i += 2)
        {
            EXPECT_EQ(Set.Remove(i), 1);
        }
        for (int i = 2000; i < 3000; ++i)
        {
            Set.Add(i);
        }

        EXPECT_EQ(Set.Num(), 1000 + 1000);
        for (int i = 1; i < 2000; i += 2)
        {
            EXPECT_TRUE(Set.Contains(i));
        }
        for (int i = 0; i < 2000; i += 2)
        {
            EXPECT_FALSE(Set.Contains(i));
        }
        for (int i = 2000; i < 3000; ++i)
        {
            EXPECT_TRUE(Set.Contains(i));
        }
    }

    // Churn at a steady size must reclaim tombstones in place instead of growing without bound.
    TEST(HashSetGrowth, SteadyStateChurnDoesNotGrowForever)
    {
        TSet<int> Set;
        for (int i = 0; i < 1000; ++i)
        {
            Set.Add(i);
        }
        const size_t SettledCapacity = Set.Max();

        for (int Round = 0; Round < 50; ++Round)
        {
            for (int i = 0; i < 1000; ++i)
            {
                Set.Remove(Round * 1000 + i);
                Set.Add((Round + 1) * 1000 + i);
            }
            EXPECT_EQ(Set.Num(), 1000);
        }

        EXPECT_LE(Set.Max(), SettledCapacity * 4);
        for (int i = 0; i < 1000; ++i)
        {
            EXPECT_TRUE(Set.Contains(50 * 1000 + i));
        }
    }

    TEST(HashSetErase, IteratorEraseReturnsTheNextElement)
    {
        TSet<int> Set;
        for (int i = 0; i < 200; ++i)
        {
            Set.Add(i);
        }

        for (auto It = Set.begin(); It != Set.end();)
        {
            if (*It % 2 == 0)
            {
                It = Set.Remove(It);
            }
            else
            {
                ++It;
            }
        }

        EXPECT_EQ(Set.Num(), 100);
        for (int Value : Set)
        {
            EXPECT_EQ(Value % 2, 1);
        }
    }

    TEST(HashSetLifetime, DestroysEveryElementOnce)
    {
        FTracked::Live = 0;
        {
            TSet<FTracked> Set;
            for (int i = 0; i < 500; ++i)
            {
                Set.TryAdd(i);
            }
            EXPECT_EQ(Set.Num(), 500);
            EXPECT_EQ(FTracked::Live, 500);

            for (int i = 0; i < 250; ++i)
            {
                Set.Remove(FTracked(i));
            }
            EXPECT_EQ(FTracked::Live, 250);
        }
        EXPECT_EQ(FTracked::Live, 0);
    }

    TEST(HashSetLifetime, ClearDestroysWithoutReleasingCapacity)
    {
        FTracked::Live = 0;
        TSet<FTracked> Set;
        for (int i = 0; i < 100; ++i)
        {
            Set.TryAdd(i);
        }
        const size_t Capacity = Set.Max();

        Set.Reset();
        EXPECT_EQ(FTracked::Live, 0);
        EXPECT_EQ(Set.Num(), 0);
        EXPECT_EQ(Set.Max(), Capacity);
        EXPECT_FALSE(Set.Contains(FTracked(5)));
    }

    TEST(HashSetCopyMove, CopyIsIndependent)
    {
        TSet<int> Source{ 1, 2, 3 };
        TSet<int> Copy = Source;

        EXPECT_EQ(Copy.Num(), 3);
        Copy.Add(4);
        EXPECT_TRUE(Copy.Contains(4));
        EXPECT_FALSE(Source.Contains(4));
        EXPECT_EQ(Source.Num(), 3);
    }

    TEST(HashSetCopyMove, MoveLeavesTheSourceEmptyAndUsable)
    {
        TSet<int> Source{ 1, 2, 3 };
        TSet<int> Moved = std::move(Source);

        EXPECT_EQ(Moved.Num(), 3);
        EXPECT_TRUE(Source.IsEmpty());
        EXPECT_EQ(Source.Max(), 0);

        Source.Add(9);
        EXPECT_EQ(Source.Num(), 1);
        EXPECT_TRUE(Source.Contains(9));
    }

    TEST(HashSetCopyMove, SwapExchangesContents)
    {
        TSet<int> Left{ 1, 2 };
        TSet<int> Right{ 8, 9, 10 };
        swap(Left, Right);

        EXPECT_EQ(Left.Num(), 3);
        EXPECT_EQ(Right.Num(), 2);
        EXPECT_TRUE(Left.Contains(9));
        EXPECT_TRUE(Right.Contains(1));
    }

    TEST(HashMapBasics, InsertLookupAndOverwrite)
    {
        TMap<int, int> Map;
        EXPECT_TRUE(Map.TryAdd(1, 10));
        EXPECT_FALSE(Map.TryAdd(1, 99));
        EXPECT_EQ(Map.FindChecked(1), 10);

        Map.Add(1, 99);
        EXPECT_EQ(Map.FindChecked(1), 99);
        EXPECT_EQ(Map.Num(), 1);
    }

    TEST(HashMapBasics, SubscriptDefaultConstructsOnMiss)
    {
        TMap<int, int> Map;
        EXPECT_EQ(Map[5], 0);
        EXPECT_EQ(Map.Num(), 1);

        Map[5] = 42;
        EXPECT_EQ(Map[5], 42);
        EXPECT_EQ(Map.Num(), 1);
    }

    TEST(HashMapBasics, FindExposesAMutableValue)
    {
        TMap<int, int> Map{ { 1, 1 }, { 2, 2 } };
        int32* const Found = Map.Find(2);
        ASSERT_NE(Found, nullptr);
        EXPECT_EQ(*Found, 2);

        *Found = 20;
        EXPECT_EQ(Map.FindChecked(2), 20);
    }

    TEST(HashMapBasics, EraseRemovesOnlyTheNamedKey)
    {
        TMap<int, int> Map;
        for (int i = 0; i < 100; ++i)
        {
            Map[i] = i * 2;
        }

        EXPECT_EQ(Map.Remove(50), 1);
        EXPECT_EQ(Map.Remove(50), 0);
        EXPECT_EQ(Map.Num(), 99);

        for (int i = 0; i < 100; ++i)
        {
            EXPECT_EQ(Map.Contains(i), i != 50);
        }
    }

    TEST(HashMapGrowth, ValuesSurviveRehashing)
    {
        constexpr int kCount = 10000;
        TMap<int, Lumina::FString> Map;
        for (int i = 0; i < kCount; ++i)
        {
            Map[i] = Lumina::Format("value-{}", i);
        }

        EXPECT_EQ(Map.Num(), kCount);
        for (int i = 0; i < kCount; ++i)
        {
            const auto* Found = Map.Find(i);
            ASSERT_NE(Found, nullptr) << "missing " << i;
            EXPECT_EQ(*Found, Lumina::Format("value-{}", i));
        }
    }

    TEST(HashMapIteration, VisitsEveryPairExactlyOnce)
    {
        TMap<int, int> Map;
        for (int i = 0; i < 400; ++i)
        {
            Map[i] = i * 3;
        }

        std::set<int> Seen;
        for (const auto& Pair : Map)
        {
            EXPECT_EQ(Pair.Value, Pair.Key * 3);
            EXPECT_TRUE(Seen.insert(Pair.Key).second);
        }
        EXPECT_EQ(Seen.size(), 400u);
    }

    TEST(HashMapLifetime, MovedOnlyValuesWork)
    {
        TMap<int, std::unique_ptr<int>> Map;
        Map.FindOrAdd(1, std::make_unique<int>(11));
        Map.FindOrAdd(2, std::make_unique<int>(22));

        ASSERT_NE(Map.Find(1), nullptr);
        EXPECT_EQ(*Map.FindChecked(1), 11);
        EXPECT_EQ(*Map.FindChecked(2), 22);

        Map.Remove(1);
        EXPECT_EQ(Map.Num(), 1);
        EXPECT_FALSE(Map.Contains(1));
    }

    TEST(HashMapEquality, ComparesByContentNotOrder)
    {
        TMap<int, int> Left;
        TMap<int, int> Right;
        for (int i = 0; i < 50; ++i)
        {
            Left[i] = i;
        }
        for (int i = 49; i >= 0; --i)
        {
            Right[i] = i;
        }

        EXPECT_TRUE(Left == Right);
        Right[7] = 8;
        EXPECT_FALSE(Left == Right);
    }

    TEST(HashKeys, EngineKeyTypesHashAndCompare)
    {
        TMap<Lumina::FName, int> Names;
        Names[Lumina::FName("Alpha")] = 1;
        Names[Lumina::FName("Beta")] = 2;
        Names[Lumina::FName("Alpha")] = 3;

        EXPECT_EQ(Names.Num(), 2);
        EXPECT_EQ(Names.FindChecked(Lumina::FName("Alpha")), 3);

        int Storage[4] = {};
        TSet<int*> Pointers{ &Storage[0], &Storage[1] };
        EXPECT_TRUE(Pointers.Contains(&Storage[0]));
        EXPECT_FALSE(Pointers.Contains(&Storage[2]));

        enum class EKind : Lumina::uint8 { A, B };
        TSet<EKind> Kinds{ EKind::A };
        EXPECT_TRUE(Kinds.Contains(EKind::A));
        EXPECT_FALSE(Kinds.Contains(EKind::B));
    }

    // Looking a string-keyed map up by view is the whole point of the transparent hasher.
    TEST(HashKeys, StringLookupByViewDoesNotNeedAnOwningKey)
    {
        TMap<Lumina::FString, int> Map;
        Map[Lumina::FString("content/meshes/rock")] = 1;

        const Lumina::FStringView View("content/meshes/rock");
        const auto* Found = Map.Find(View);
        ASSERT_NE(Found, nullptr);
        EXPECT_EQ(*Found, 1);

        EXPECT_TRUE(Map.Contains(Lumina::FCStringView("content/meshes/rock")));
        EXPECT_FALSE(Map.Contains(Lumina::FStringView("content/meshes/tree")));
    }

    TEST(HashKeys, StringHashersAgreeAcrossTheStringTypes)
    {
        const Lumina::FString Owning("a moderately long asset path that leaves the inline buffer");
        const Lumina::FStringView View = Owning;
        const Lumina::FCStringView CView = Owning.CView();

        const Lumina::Containers::FDefaultHash Hash;
        EXPECT_EQ(Hash(Owning), Hash(View));
        EXPECT_EQ(Hash(Owning), Hash(CView));
    }

    TEST(HashTableAllocator, ScratchTableLivesInTheThreadArena)
    {
        Lumina::FMemMark Mark;

        Lumina::Containers::TScratchHashMap<int, int> Map;
        for (int i = 0; i < 500; ++i)
        {
            Map[i] = i;
        }

        EXPECT_EQ(Map.Num(), 500);
        for (int i = 0; i < 500; ++i)
        {
            EXPECT_EQ(Map.FindChecked(i), i);
        }
    }

    // A hash that collapses every key to one bucket must still be correct, only slower.
    struct FTerribleHash
    {
        using is_transparent = void;

        template <typename T>
        Lumina::uint64 operator()(const T&) const noexcept { return 0; }
    };

    TEST(HashTableRobustness, DegenerateHashStillFindsEveryKey)
    {
        Lumina::Containers::THashSet<int, FTerribleHash> Set;
        for (int i = 0; i < 400; ++i)
        {
            EXPECT_TRUE(Set.TryAdd(i));
        }
        EXPECT_EQ(Set.Num(), 400);

        for (int i = 0; i < 400; ++i)
        {
            EXPECT_TRUE(Set.Contains(i));
        }
        EXPECT_FALSE(Set.Contains(400));

        for (int i = 0; i < 400; i += 3)
        {
            EXPECT_EQ(Set.Remove(i), 1);
        }
        for (int i = 0; i < 400; ++i)
        {
            EXPECT_EQ(Set.Contains(i), i % 3 != 0);
        }
    }

    TEST(HashTableRobustness, ShrinkToFitReleasesAnEmptiedTable)
    {
        TSet<int> Set;
        for (int i = 0; i < 1000; ++i)
        {
            Set.Add(i);
        }
        EXPECT_GT(Set.GetAllocatedSize(), 0u);

        Set.Reset();
        Set.Shrink();
        EXPECT_EQ(Set.Max(), 0);
        EXPECT_EQ(Set.GetAllocatedSize(), 0u);
        EXPECT_FALSE(Set.Contains(1));

        Set.Add(1);
        EXPECT_TRUE(Set.Contains(1));
    }
}
