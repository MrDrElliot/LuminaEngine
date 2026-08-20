#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <numeric>
#include <span>
#include <string>

#include "Memory/SmartPtr.h"
#include "Containers/Vector.h"
#include "Memory/Allocators/Allocator.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaVectorTests
{
    using Lumina::FMemMark;

    template <typename T, size_t InlineCapacity = 0>
    using TVector = Lumina::Containers::TVector<T, InlineCapacity>;

    template <typename T, size_t InlineCapacity>
    using TInlineVector = Lumina::Containers::TVector<T, InlineCapacity>;

    template <typename T>
    using TScratchVector = Lumina::Containers::TScratchVector<T>;

namespace
{
    struct FLifetime
    {
        static int Constructed;
        static int Destructed;
        static int Copied;
        static int Moved;

        static void Reset()
        {
            Constructed = 0;
            Destructed  = 0;
            Copied      = 0;
            Moved       = 0;
        }

        static int Live() { return Constructed - Destructed; }

        FLifetime() : Value(0) { ++Constructed; }
        explicit FLifetime(int InValue) : Value(InValue) { ++Constructed; }
        FLifetime(const FLifetime& Other) : Value(Other.Value) { ++Constructed; ++Copied; }
        FLifetime(FLifetime&& Other) noexcept : Value(Other.Value) { Other.Value = -1; ++Constructed; ++Moved; }

        FLifetime& operator=(const FLifetime& Other)
        {
            Value = Other.Value;
            ++Copied;
            return *this;
        }

        FLifetime& operator=(FLifetime&& Other) noexcept
        {
            Value = Other.Value;
            Other.Value = -1;
            ++Moved;
            return *this;
        }

        ~FLifetime() { ++Destructed; }

        bool operator==(const FLifetime& Other) const { return Value == Other.Value; }

        int Value;
    };

    int FLifetime::Constructed = 0;
    int FLifetime::Destructed  = 0;
    int FLifetime::Copied      = 0;
    int FLifetime::Moved       = 0;

    struct FSelfReferencing
    {
        FSelfReferencing() : Self(this) {}
        FSelfReferencing(const FSelfReferencing&) : Self(this) {}
        FSelfReferencing(FSelfReferencing&&) noexcept : Self(this) {}
        FSelfReferencing& operator=(const FSelfReferencing&) { return *this; }
        FSelfReferencing& operator=(FSelfReferencing&&) noexcept { return *this; }

        bool IsIntact() const { return Self == this; }

        FSelfReferencing* Self;
    };
}

TEST(VectorLayout, DefaultVectorIsSixteenBytes)
{
    static_assert(sizeof(TVector<int>) == 16);
    static_assert(sizeof(TVector<FLifetime>) == 16);
    static_assert(alignof(TVector<int>) == alignof(void*));
    SUCCEED();
}

TEST(VectorLayout, InlineVectorEmbedsItsBuffer)
{
    static_assert(sizeof(TInlineVector<int, 4>) == 16 + 4 * sizeof(int));
    static_assert(sizeof(TInlineVector<int, 0>) == 16);
    SUCCEED();
}

TEST(VectorLayout, RelocatabilityIsAdvertisedCorrectly)
{
    static_assert(Lumina::TIsTriviallyRelocatable_V<TVector<int>>);
    static_assert(Lumina::TIsTriviallyRelocatable_V<TVector<std::string>>);
    static_assert(!Lumina::TIsTriviallyRelocatable_V<TInlineVector<int, 4>>);
    static_assert(Lumina::TIsTriviallyRelocatable_V<Lumina::TUniquePtr<int>>);
    static_assert(Lumina::TIsTriviallyRelocatable_V<Lumina::TSharedPtr<int>>);
    static_assert(!Lumina::TIsTriviallyRelocatable_V<FSelfReferencing>);
    SUCCEED();
}

TEST(VectorBasics, PushBackGrowsAndPreservesOrder)
{
    TVector<int> Values;
    EXPECT_TRUE(Values.empty());
    EXPECT_EQ(Values.capacity(), 0u);

    for (int Index = 0; Index < 1000; ++Index)
    {
        Values.push_back(Index);
    }

    ASSERT_EQ(Values.size(), 1000u);
    EXPECT_GE(Values.capacity(), 1000u);
    for (int Index = 0; Index < 1000; ++Index)
    {
        EXPECT_EQ(Values[static_cast<size_t>(Index)], Index);
    }
}

TEST(VectorBasics, GrowthIsGeometric)
{
    TVector<int> Values;

    size_t Reallocations = 0;
    const int* Previous = nullptr;
    for (int Index = 0; Index < 4096; ++Index)
    {
        Values.push_back(Index);
        if (Values.data() != Previous)
        {
            ++Reallocations;
            Previous = Values.data();
        }
    }

    EXPECT_LT(Reallocations, 40u);
}

TEST(VectorBasics, ReserveHonorsTheRequestExactly)
{
    TVector<int> Values;
    Values.Reserve(100);
    EXPECT_EQ(Values.capacity(), 100u);
    EXPECT_EQ(Values.size(), 0u);

    const int* Base = Values.data();
    for (int Index = 0; Index < 100; ++Index)
    {
        Values.push_back(Index);
    }
    EXPECT_EQ(Values.data(), Base);
}

TEST(VectorBasics, EmplaceBackReturnsTheNewElement)
{
    TVector<FLifetime> Values;
    FLifetime& Added = Values.emplace_back(7);
    EXPECT_EQ(Added.Value, 7);
    EXPECT_EQ(&Added, &Values.back());
}

TEST(VectorBasics, ResizeDefaultInitializesAndShrinks)
{
    TVector<int> Values;
    Values.resize(8);
    ASSERT_EQ(Values.size(), 8u);
    for (int Value : Values)
    {
        EXPECT_EQ(Value, 0);
    }

    Values.resize(16, 5);
    ASSERT_EQ(Values.size(), 16u);
    EXPECT_EQ(Values[8], 5);
    EXPECT_EQ(Values[15], 5);

    Values.resize(2);
    EXPECT_EQ(Values.size(), 2u);
}

TEST(VectorBasics, PopBackValueMovesTheElementOut)
{
    TVector<std::string> Values;
    Values.push_back("first");
    Values.push_back("second");

    const std::string Last = Values.PopBackValue();
    EXPECT_EQ(Last, "second");
    EXPECT_EQ(Values.size(), 1u);
}

TEST(VectorLifetime, EveryConstructionIsMatchedByADestruction)
{
    FLifetime::Reset();
    {
        TVector<FLifetime> Values;
        for (int Index = 0; Index < 200; ++Index)
        {
            Values.emplace_back(Index);
        }
        Values.erase(Values.begin() + 10, Values.begin() + 20);
        Values.resize(50);
        Values.insert(Values.begin(), FLifetime(99));
        EXPECT_GT(FLifetime::Live(), 0);
    }
    EXPECT_EQ(FLifetime::Live(), 0);
}

TEST(VectorLifetime, ClearDestroysElementsButKeepsCapacity)
{
    FLifetime::Reset();
    TVector<FLifetime> Values;
    Values.resize(32);
    const size_t Capacity = Values.capacity();

    Values.clear();
    EXPECT_EQ(FLifetime::Live(), 0);
    EXPECT_EQ(Values.capacity(), Capacity);

    Values.Reset();
    EXPECT_EQ(Values.capacity(), 0u);
}

TEST(VectorLifetime, RelocationDoesNotMoveConstructTrivialTypes)
{
    FLifetime::Reset();
    TVector<FLifetime> Values;
    Values.Reserve(4);
    for (int Index = 0; Index < 4; ++Index)
    {
        Values.emplace_back(Index);
    }

    const int MovesBeforeGrowth = FLifetime::Moved;
    Values.emplace_back(4);
    EXPECT_GT(FLifetime::Moved, MovesBeforeGrowth);
    EXPECT_EQ(Values[0].Value, 0);
    EXPECT_EQ(Values[4].Value, 4);
}

TEST(VectorAliasing, PushBackOfOwnElementSurvivesReallocation)
{
    TVector<int> Values;
    Values.Reserve(4);
    Values.push_back(11);
    Values.push_back(22);
    Values.push_back(33);
    Values.push_back(44);
    ASSERT_EQ(Values.capacity(), 4u);

    Values.push_back(Values[0]);
    EXPECT_EQ(Values.size(), 5u);
    EXPECT_EQ(Values[4], 11);
}

TEST(VectorAliasing, InsertOfOwnElementSurvivesReallocation)
{
    TVector<int> Values;
    Values.Reserve(4);
    for (int Index = 0; Index < 4; ++Index)
    {
        Values.push_back(Index);
    }

    Values.insert(Values.begin(), Values[3]);
    ASSERT_EQ(Values.size(), 5u);
    EXPECT_EQ(Values[0], 3);
    EXPECT_EQ(Values[1], 0);
    EXPECT_EQ(Values[4], 3);
}

TEST(VectorInsertErase, InsertAtEveryPositionKeepsOrder)
{
    TVector<int> Values{1, 2, 3};

    Values.insert(Values.begin(), 0);
    Values.insert(Values.end(), 4);
    Values.insert(Values.begin() + 2, 99);

    const TVector<int> Expected{0, 1, 99, 2, 3, 4};
    EXPECT_EQ(Values, Expected);
}

TEST(VectorInsertErase, InsertRangeAndInitializerList)
{
    TVector<int> Values{1, 5};
    const TVector<int> Middle{2, 3, 4};

    Values.insert(Values.begin() + 1, Middle.begin(), Middle.end());
    const TVector<int> Expected{1, 2, 3, 4, 5};
    EXPECT_EQ(Values, Expected);

    Values.insert(Values.end(), {6, 7});
    EXPECT_EQ(Values.size(), 7u);
    EXPECT_EQ(Values.back(), 7);
}

TEST(VectorInsertErase, InsertRepeatedValue)
{
    TVector<int> Values{1, 4};
    Values.insert(Values.begin() + 1, 3, 9);

    const TVector<int> Expected{1, 9, 9, 9, 4};
    EXPECT_EQ(Values, Expected);
}

TEST(VectorInsertErase, EraseSingleAndRange)
{
    TVector<int> Values{0, 1, 2, 3, 4, 5};

    Values.erase(Values.begin() + 2);
    EXPECT_EQ(Values, (TVector<int>{0, 1, 3, 4, 5}));

    Values.erase(Values.begin() + 1, Values.begin() + 3);
    EXPECT_EQ(Values, (TVector<int>{0, 4, 5}));
}

TEST(VectorInsertErase, RemoveAtSwapIsUnorderedButComplete)
{
    TVector<int> Values{0, 1, 2, 3, 4};
    Values.RemoveAtSwap(1);

    ASSERT_EQ(Values.size(), 4u);
    EXPECT_FALSE(Values.Contains(1));
    EXPECT_TRUE(Values.Contains(4));

    Values.RemoveAtSwap(Values.size() - 1);
    EXPECT_EQ(Values.size(), 3u);
}

TEST(VectorSearch, IndexOfContainsAndFind)
{
    TVector<int> Values{10, 20, 30};

    EXPECT_EQ(Values.IndexOf(20), 1u);
    EXPECT_EQ(Values.IndexOf(99), TVector<int>::npos);
    EXPECT_TRUE(Values.Contains(30));
    EXPECT_FALSE(Values.Contains(31));

    EXPECT_EQ(*Values.Find(10), 10);
    EXPECT_EQ(Values.Find(99), Values.end());
    EXPECT_EQ(Values.IndexOfBy([](int V) { return V > 15; }), 1u);
}

TEST(VectorSearch, AddUniqueDoesNotDuplicate)
{
    TVector<int> Values;
    EXPECT_EQ(Values.AddUnique(5), 0u);
    EXPECT_EQ(Values.AddUnique(5), 0u);
    EXPECT_EQ(Values.AddUnique(6), 1u);
    EXPECT_EQ(Values.size(), 2u);
}

TEST(VectorSearch, RemoveAllByCompactsInOnePass)
{
    TVector<int> Values;
    for (int Index = 0; Index < 20; ++Index)
    {
        Values.push_back(Index);
    }

    const size_t Removed = Values.RemoveAllBy([](int V) { return V % 2 == 0; });
    EXPECT_EQ(Removed, 10u);
    ASSERT_EQ(Values.size(), 10u);
    for (size_t Index = 0; Index < Values.size(); ++Index)
    {
        EXPECT_EQ(Values[Index], static_cast<int>(Index) * 2 + 1);
    }
}

TEST(VectorSearch, RemoveAllByRunsDestructorsExactlyOnce)
{
    FLifetime::Reset();
    {
        TVector<FLifetime> Values;
        for (int Index = 0; Index < 64; ++Index)
        {
            Values.emplace_back(Index);
        }

        const size_t Removed = Values.RemoveAllBy([](const FLifetime& V) { return V.Value % 3 != 0; });
        EXPECT_EQ(Removed, 64u - 22u);
        EXPECT_EQ(Values.size(), 22u);
        for (const FLifetime& Element : Values)
        {
            EXPECT_EQ(Element.Value % 3, 0);
        }
    }
    EXPECT_EQ(FLifetime::Live(), 0);
}

TEST(VectorSearch, RemoveFirstAndRemoveAll)
{
    TVector<int> Values{1, 2, 1, 3, 1};

    EXPECT_TRUE(Values.RemoveFirst(1));
    EXPECT_EQ(Values.size(), 4u);
    EXPECT_FALSE(Values.RemoveFirst(9));
    EXPECT_EQ(Values.RemoveAll(1), 2u);
    EXPECT_EQ(Values, (TVector<int>{2, 3}));
}

TEST(VectorCopyMove, CopyIsDeepAndIndependent)
{
    TVector<std::string> Source{"a", "b", "c"};
    TVector<std::string> Copy = Source;

    ASSERT_EQ(Copy.size(), 3u);
    EXPECT_NE(Copy.data(), Source.data());

    Copy[0] = "changed";
    EXPECT_EQ(Source[0], "a");
}

TEST(VectorCopyMove, MoveStealsTheHeapBlock)
{
    TVector<int> Source;
    Source.Reserve(64);
    Source.push_back(1);
    const int* Base = Source.data();

    TVector<int> Moved = std::move(Source);
    EXPECT_EQ(Moved.data(), Base);
    EXPECT_EQ(Moved.size(), 1u);
    EXPECT_EQ(Source.size(), 0u);
    EXPECT_EQ(Source.capacity(), 0u);
}

TEST(VectorCopyMove, MoveAssignReleasesTheOldBlock)
{
    FLifetime::Reset();
    {
        TVector<FLifetime> Target;
        Target.resize(10);

        TVector<FLifetime> Source;
        Source.resize(3);

        Target = std::move(Source);
        EXPECT_EQ(Target.size(), 3u);
        EXPECT_EQ(FLifetime::Live(), 3);
    }
    EXPECT_EQ(FLifetime::Live(), 0);
}

TEST(VectorCopyMove, SelfAssignmentIsSafe)
{
    TVector<int> Values{1, 2, 3};

    TVector<int>& Alias = Values;
    Values = Alias;
    EXPECT_EQ(Values, (TVector<int>{1, 2, 3}));

    Values = std::move(Alias);
    EXPECT_EQ(Values.size(), 3u);
}

TEST(VectorCopyMove, MoveOnlyElementsWork)
{
    TVector<std::unique_ptr<int>> Values;
    for (int Index = 0; Index < 32; ++Index)
    {
        Values.push_back(std::make_unique<int>(Index));
    }

    ASSERT_EQ(Values.size(), 32u);
    EXPECT_EQ(*Values[7], 7);

    TVector<std::unique_ptr<int>> Moved = std::move(Values);
    EXPECT_EQ(*Moved[31], 31);
}

TEST(VectorCopyMove, SwapExchangesContents)
{
    TVector<int> A{1, 2, 3};
    TVector<int> B{9};

    A.swap(B);
    EXPECT_EQ(A, (TVector<int>{9}));
    EXPECT_EQ(B, (TVector<int>{1, 2, 3}));
}

TEST(VectorInline, StaysInlineUntilItOverflows)
{
    TInlineVector<int, 4> Values;
    EXPECT_TRUE(Values.IsInline());
    EXPECT_EQ(Values.capacity(), 4u);

    for (int Index = 0; Index < 4; ++Index)
    {
        Values.push_back(Index);
    }
    EXPECT_TRUE(Values.IsInline());

    Values.push_back(4);
    EXPECT_FALSE(Values.IsInline());
    ASSERT_EQ(Values.size(), 5u);
    for (int Index = 0; Index < 5; ++Index)
    {
        EXPECT_EQ(Values[static_cast<size_t>(Index)], Index);
    }
}

TEST(VectorInline, ShrinkToFitReturnsToTheInlineBuffer)
{
    TInlineVector<int, 4> Values;
    for (int Index = 0; Index < 32; ++Index)
    {
        Values.push_back(Index);
    }
    ASSERT_FALSE(Values.IsInline());

    Values.resize(3);
    Values.shrink_to_fit();
    EXPECT_TRUE(Values.IsInline());
    EXPECT_EQ(Values, (TInlineVector<int, 4>{0, 1, 2}));
}

TEST(VectorInline, MoveFromInlineStorageRelocatesElements)
{
    FLifetime::Reset();
    {
        TInlineVector<FLifetime, 4> Source;
        Source.emplace_back(1);
        Source.emplace_back(2);
        ASSERT_TRUE(Source.IsInline());

        TInlineVector<FLifetime, 4> Target = std::move(Source);
        EXPECT_EQ(Target.size(), 2u);
        EXPECT_EQ(Target[0].Value, 1);
        EXPECT_EQ(Source.size(), 0u);
    }
    EXPECT_EQ(FLifetime::Live(), 0);
}

TEST(VectorInline, MoveFromSpilledStorageStealsTheBlock)
{
    TInlineVector<int, 2> Source;
    Source.push_back(1);
    Source.push_back(2);
    Source.push_back(3);
    ASSERT_FALSE(Source.IsInline());
    const int* Base = Source.data();

    TInlineVector<int, 2> Target = std::move(Source);
    EXPECT_EQ(Target.data(), Base);
    EXPECT_TRUE(Source.IsInline());
}

TEST(VectorInline, SwapWorksAcrossInlineAndHeap)
{
    TInlineVector<int, 2> Small{1, 2};
    TInlineVector<int, 2> Large{1, 2, 3, 4, 5};
    ASSERT_TRUE(Small.IsInline());
    ASSERT_FALSE(Large.IsInline());

    Small.swap(Large);
    EXPECT_EQ(Small.size(), 5u);
    EXPECT_EQ(Large.size(), 2u);
    EXPECT_EQ(Small[4], 5);
    EXPECT_EQ(Large[1], 2);
}

TEST(VectorInterop, WorksWithStdAlgorithmsAndSpan)
{
    TVector<int> Values(64u);
    std::iota(Values.begin(), Values.end(), 0);

    EXPECT_EQ(std::accumulate(Values.begin(), Values.end(), 0), 63 * 64 / 2);
    EXPECT_TRUE(std::is_sorted(Values.begin(), Values.end()));

    std::reverse(Values.begin(), Values.end());
    EXPECT_EQ(Values.front(), 63);

    const std::span<const int> View{Values};
    EXPECT_EQ(View.size(), 64u);
    EXPECT_EQ(View[0], 63);
}

TEST(VectorInterop, RangeForAndReverseIteration)
{
    TVector<int> Values{1, 2, 3};

    int Sum = 0;
    for (int Value : Values)
    {
        Sum += Value;
    }
    EXPECT_EQ(Sum, 6);

    int First = *Values.rbegin();
    EXPECT_EQ(First, 3);
}

TEST(VectorInterop, ComparisonOperators)
{
    const TVector<int> A{1, 2, 3};
    const TVector<int> B{1, 2, 3};
    const TVector<int> C{1, 2, 4};
    const TVector<int> D{1, 2};

    EXPECT_EQ(A, B);
    EXPECT_NE(A, C);
    EXPECT_LT(A, C);
    EXPECT_LT(D, A);
    EXPECT_GT(C, A);
}

TEST(VectorInterop, ConstructFromIteratorPair)
{
    const int Raw[] = {1, 2, 3, 4};
    const TVector<int> FromRaw(std::begin(Raw), std::end(Raw));
    EXPECT_EQ(FromRaw, (TVector<int>{1, 2, 3, 4}));

    const TVector<std::string> FromStrings{"a", "b"};
    const TVector<std::string> Copied(FromStrings.begin(), FromStrings.end());
    EXPECT_EQ(Copied.size(), 2u);
}

TEST(VectorInterop, AppendAndAssign)
{
    TVector<int> Values{1, 2};
    Values.Append({3, 4});
    EXPECT_EQ(Values, (TVector<int>{1, 2, 3, 4}));

    const TVector<int> Extra{5, 6};
    Values.Append(Extra);
    EXPECT_EQ(Values.size(), 6u);

    Values.assign(3, 7);
    EXPECT_EQ(Values, (TVector<int>{7, 7, 7}));
}

TEST(VectorBulk, AddUninitializedGivesWritableSlots)
{
    TVector<int> Values;
    int* Slots = Values.AddUninitialized(16);
    for (int Index = 0; Index < 16; ++Index)
    {
        Slots[Index] = Index;
    }

    ASSERT_EQ(Values.size(), 16u);
    EXPECT_EQ(Values[15], 15);
}

TEST(VectorAllocator, ScratchVectorAllocatesFromTheThreadArena)
{
    FMemMark Mark;

    TScratchVector<int> Values;
    for (int Index = 0; Index < 1024; ++Index)
    {
        Values.push_back(Index);
    }

    ASSERT_EQ(Values.size(), 1024u);
    EXPECT_EQ(Values[1023], 1023);
}
}
