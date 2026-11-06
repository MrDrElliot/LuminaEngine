#include <gtest/gtest.h>

#include <algorithm>
#include <list>
#include <memory>
#include <numeric>
#include <span>
#include <string>

#include "Memory/SmartPtr.h"
#include "Containers/Span.h"
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
    EXPECT_TRUE(Values.IsEmpty());
    EXPECT_EQ(Values.Max(), 0);

    for (int Index = 0; Index < 1000; ++Index)
    {
        Values.Add(Index);
    }

    ASSERT_EQ(Values.Num(), 1000);
    EXPECT_GE(Values.Max(), 1000);
    for (int Index = 0; Index < 1000; ++Index)
    {
        EXPECT_EQ(Values[Index], Index);
    }
}

TEST(VectorBasics, GrowthIsGeometric)
{
    TVector<int> Values;

    size_t Reallocations = 0;
    const int* Previous = nullptr;
    for (int Index = 0; Index < 4096; ++Index)
    {
        Values.Add(Index);
        if (Values.GetData() != Previous)
        {
            ++Reallocations;
            Previous = Values.GetData();
        }
    }

    EXPECT_LT(Reallocations, 40u);
}

TEST(VectorBasics, ReserveHonorsTheRequestExactly)
{
    TVector<int> Values;
    Values.Reserve(100);
    EXPECT_EQ(Values.Max(), 100);
    EXPECT_EQ(Values.Num(), 0);

    const int* Base = Values.GetData();
    for (int Index = 0; Index < 100; ++Index)
    {
        Values.Add(Index);
    }
    EXPECT_EQ(Values.GetData(), Base);
}

TEST(VectorBasics, EmplaceReturnsTheNewElement)
{
    TVector<FLifetime> Values;
    FLifetime& Added = Values.EmplaceGetRef(7);
    EXPECT_EQ(Added.Value, 7);
    EXPECT_EQ(&Added, &Values.Last());
}

TEST(VectorBasics, AddDefaultedGetRefPointsIntoTheGrownBuffer)
{
    TVector<int32> Values;
    Values.AddDefaultedGetRef() = 11;
    ASSERT_EQ(Values.Num(), 1);
    EXPECT_EQ(Values[0], 11);

    while (Values.Num() < Values.Max())
    {
        Values.Add(0);
    }
    const int32 FullNum = Values.Num();
    const int32* OldData = Values.GetData();
    int32& Added = Values.AddDefaultedGetRef();
    ASSERT_NE(Values.GetData(), OldData);
    Added = 22;
    EXPECT_EQ(&Added, &Values[FullNum]);
    EXPECT_EQ(Values[FullNum], 22);
    EXPECT_EQ(Values[0], 11);
}

TEST(VectorBasics, ResizeDefaultInitializesAndShrinks)
{
    TVector<int> Values;
    Values.SetNum(8);
    ASSERT_EQ(Values.Num(), 8);
    for (int Value : Values)
    {
        EXPECT_EQ(Value, 0);
    }

    Values.SetNum(16, 5);
    ASSERT_EQ(Values.Num(), 16);
    EXPECT_EQ(Values[8], 5);
    EXPECT_EQ(Values[15], 5);

    Values.SetNum(2);
    EXPECT_EQ(Values.Num(), 2);
}

TEST(VectorBasics, PopBackValueMovesTheElementOut)
{
    TVector<std::string> Values;
    Values.Add("first");
    Values.Add("second");

    const std::string Last = Values.Pop();
    EXPECT_EQ(Last, "second");
    EXPECT_EQ(Values.Num(), 1);
}

TEST(VectorLifetime, EveryConstructionIsMatchedByADestruction)
{
    FLifetime::Reset();
    {
        TVector<FLifetime> Values;
        for (int Index = 0; Index < 200; ++Index)
        {
            Values.Emplace(Index);
        }
        Values.RemoveAt(10, 10);
        Values.SetNum(50);
        Values.Insert(FLifetime(99), 0);
        EXPECT_GT(FLifetime::Live(), 0);
    }
    EXPECT_EQ(FLifetime::Live(), 0);
}

TEST(VectorLifetime, ClearDestroysElementsButKeepsCapacity)
{
    FLifetime::Reset();
    TVector<FLifetime> Values;
    Values.SetNum(32);
    const int32 Capacity = Values.Max();

    Values.Reset();
    EXPECT_EQ(FLifetime::Live(), 0);
    EXPECT_EQ(Values.Max(), Capacity);

    Values.Empty();
    EXPECT_EQ(Values.Max(), 0);
}

TEST(VectorLifetime, RelocationDoesNotMoveConstructTrivialTypes)
{
    FLifetime::Reset();
    TVector<FLifetime> Values;
    Values.Reserve(4);
    for (int Index = 0; Index < 4; ++Index)
    {
        Values.Emplace(Index);
    }

    const int MovesBeforeGrowth = FLifetime::Moved;
    Values.Emplace(4);
    EXPECT_GT(FLifetime::Moved, MovesBeforeGrowth);
    EXPECT_EQ(Values[0].Value, 0);
    EXPECT_EQ(Values[4].Value, 4);
}

TEST(VectorAliasing, PushBackOfOwnElementSurvivesReallocation)
{
    TVector<int> Values;
    Values.Reserve(4);
    Values.Add(11);
    Values.Add(22);
    Values.Add(33);
    Values.Add(44);
    ASSERT_EQ(Values.Max(), 4);

    Values.Add(Values[0]);
    EXPECT_EQ(Values.Num(), 5);
    EXPECT_EQ(Values[4], 11);
}

TEST(VectorAliasing, InsertOfOwnElementSurvivesReallocation)
{
    TVector<int> Values;
    Values.Reserve(4);
    for (int Index = 0; Index < 4; ++Index)
    {
        Values.Add(Index);
    }

    Values.Insert(Values[3], 0);
    ASSERT_EQ(Values.Num(), 5);
    EXPECT_EQ(Values[0], 3);
    EXPECT_EQ(Values[1], 0);
    EXPECT_EQ(Values[4], 3);
}

TEST(VectorInsertErase, InsertAtEveryPositionKeepsOrder)
{
    TVector<int> Values{1, 2, 3};

    Values.Insert(0, 0);
    Values.Add(4);
    Values.Insert(99, 2);

    const TVector<int> Expected{0, 1, 99, 2, 3, 4};
    EXPECT_EQ(Values, Expected);
}

TEST(VectorInsertErase, InsertRangeAndInitializerList)
{
    TVector<int> Values{1, 5};
    const TVector<int> Middle{2, 3, 4};

    Values.Insert(Middle, 1);
    const TVector<int> Expected{1, 2, 3, 4, 5};
    EXPECT_EQ(Values, Expected);

    Values.Append({6, 7});
    EXPECT_EQ(Values.Num(), 7);
    EXPECT_EQ(Values.Last(), 7);
}

TEST(VectorInsertErase, InsertRangeTakesTheContiguousFastPathAndTheGenericOne)
{
    // Contiguous, same value type, so this goes through the memcpy path for a trivial element.
    TVector<int> Trivial{1, 6};
    const TVector<int> Source{2, 3, 4, 5};
    Trivial.Insert(Source, 1);
    EXPECT_EQ(Trivial, (TVector<int>{1, 2, 3, 4, 5, 6}));

    // Appending at the end is the Append path, which is where the gap is empty.
    Trivial.Append(Source);
    EXPECT_EQ(Trivial.Num(), 10);
    EXPECT_EQ(Trivial.Last(), 5);

    // A differing value type must fall back to the element loop rather than memcpy.
    TVector<double> Widened{0.0};
    Widened.Append(Source);
    EXPECT_EQ(Widened.Num(), 5);
    EXPECT_DOUBLE_EQ(Widened[4], 5.0);

    // A non-contiguous iterator must also take the element loop.
    const std::list<int> Listed{7, 8, 9};
    TVector<int> FromList;
    FromList.Append(Listed);
    EXPECT_EQ(FromList, (TVector<int>{7, 8, 9}));
}

TEST(VectorInsertErase, InsertRepeatedValue)
{
    TVector<int> Values{1, 4};
    Values.Insert(TVector<int>(3, 9), 1);

    const TVector<int> Expected{1, 9, 9, 9, 4};
    EXPECT_EQ(Values, Expected);
}

TEST(VectorInsertErase, EraseSingleAndRange)
{
    TVector<int> Values{0, 1, 2, 3, 4, 5};

    Values.RemoveAt(2);
    EXPECT_EQ(Values, (TVector<int>{0, 1, 3, 4, 5}));

    Values.RemoveAt(1, 2);
    EXPECT_EQ(Values, (TVector<int>{0, 4, 5}));
}

TEST(VectorInsertErase, RemoveAtSwapIsUnorderedButComplete)
{
    TVector<int> Values{0, 1, 2, 3, 4};
    Values.RemoveAtSwap(1);

    ASSERT_EQ(Values.Num(), 4);
    EXPECT_FALSE(Values.Contains(1));
    EXPECT_TRUE(Values.Contains(4));

    Values.RemoveAtSwap(Values.Num() - 1);
    EXPECT_EQ(Values.Num(), 3);
}

TEST(VectorSearch, IndexOfContainsAndFind)
{
    TVector<int> Values{10, 20, 30};

    EXPECT_EQ(Values.Find(20), 1);
    EXPECT_EQ(Values.Find(99), Lumina::Constants::kIndexNone);
    EXPECT_TRUE(Values.Contains(30));
    EXPECT_FALSE(Values.Contains(31));

    EXPECT_EQ(*Values.FindByKey(10), 10);
    EXPECT_EQ(Values.FindByKey(99), nullptr);
    EXPECT_EQ(Values.IndexOfByPredicate([](int V) { return V > 15; }), 1);
}

TEST(VectorSearch, AddUniqueDoesNotDuplicate)
{
    TVector<int> Values;
    EXPECT_EQ(Values.AddUnique(5), 0);
    EXPECT_EQ(Values.AddUnique(5), 0);
    EXPECT_EQ(Values.AddUnique(6), 1);
    EXPECT_EQ(Values.Num(), 2);
}

TEST(VectorSearch, RemoveAllByCompactsInOnePass)
{
    TVector<int> Values;
    for (int Index = 0; Index < 20; ++Index)
    {
        Values.Add(Index);
    }

    const size_t Removed = Values.RemoveAll([](int V) { return V % 2 == 0; });
    EXPECT_EQ(Removed, 10u);
    ASSERT_EQ(Values.Num(), 10);
    for (int32 Index = 0; Index < Values.Num(); ++Index)
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
            Values.Emplace(Index);
        }

        const size_t Removed = Values.RemoveAll([](const FLifetime& V) { return V.Value % 3 != 0; });
        EXPECT_EQ(Removed, 64u - 22u);
        EXPECT_EQ(Values.Num(), 22);
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

    EXPECT_TRUE(Values.RemoveSingle(1));
    EXPECT_EQ(Values.Num(), 4);
    EXPECT_FALSE(Values.RemoveSingle(9));
    EXPECT_EQ(Values.Remove(1), 2);
    EXPECT_EQ(Values, (TVector<int>{2, 3}));
}

TEST(VectorCopyMove, CopyIsDeepAndIndependent)
{
    TVector<std::string> Source{"a", "b", "c"};
    TVector<std::string> Copy = Source;

    ASSERT_EQ(Copy.Num(), 3);
    EXPECT_NE(Copy.GetData(), Source.GetData());

    Copy[0] = "changed";
    EXPECT_EQ(Source[0], "a");
}

TEST(VectorCopyMove, MoveStealsTheHeapBlock)
{
    TVector<int> Source;
    Source.Reserve(64);
    Source.Add(1);
    const int* Base = Source.GetData();

    TVector<int> Moved = std::move(Source);
    EXPECT_EQ(Moved.GetData(), Base);
    EXPECT_EQ(Moved.Num(), 1);
    EXPECT_EQ(Source.Num(), 0);
    EXPECT_EQ(Source.Max(), 0);
}

TEST(VectorCopyMove, MoveAssignReleasesTheOldBlock)
{
    FLifetime::Reset();
    {
        TVector<FLifetime> Target;
        Target.SetNum(10);

        TVector<FLifetime> Source;
        Source.SetNum(3);

        Target = std::move(Source);
        EXPECT_EQ(Target.Num(), 3);
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
    EXPECT_EQ(Values.Num(), 3);
}

TEST(VectorCopyMove, MoveOnlyElementsWork)
{
    TVector<std::unique_ptr<int>> Values;
    for (int Index = 0; Index < 32; ++Index)
    {
        Values.Add(std::make_unique<int>(Index));
    }

    ASSERT_EQ(Values.Num(), 32);
    EXPECT_EQ(*Values[7], 7);

    TVector<std::unique_ptr<int>> Moved = std::move(Values);
    EXPECT_EQ(*Moved[31], 31);
}

TEST(VectorCopyMove, SwapExchangesContents)
{
    TVector<int> A{1, 2, 3};
    TVector<int> B{9};

    swap(A, B);
    EXPECT_EQ(A, (TVector<int>{9}));
    EXPECT_EQ(B, (TVector<int>{1, 2, 3}));
}

TEST(VectorInline, StaysInlineUntilItOverflows)
{
    TInlineVector<int, 4> Values;
    EXPECT_TRUE(Values.IsInline());
    EXPECT_EQ(Values.Max(), 4);

    for (int Index = 0; Index < 4; ++Index)
    {
        Values.Add(Index);
    }
    EXPECT_TRUE(Values.IsInline());

    Values.Add(4);
    EXPECT_FALSE(Values.IsInline());
    ASSERT_EQ(Values.Num(), 5);
    for (int Index = 0; Index < 5; ++Index)
    {
        EXPECT_EQ(Values[Index], Index);
    }
}

TEST(VectorInline, ShrinkToFitReturnsToTheInlineBuffer)
{
    TInlineVector<int, 4> Values;
    for (int Index = 0; Index < 32; ++Index)
    {
        Values.Add(Index);
    }
    ASSERT_FALSE(Values.IsInline());

    Values.SetNum(3);
    Values.Shrink();
    EXPECT_TRUE(Values.IsInline());
    EXPECT_EQ(Values, (TInlineVector<int, 4>{0, 1, 2}));
}

TEST(VectorInline, MoveFromInlineStorageRelocatesElements)
{
    FLifetime::Reset();
    {
        TInlineVector<FLifetime, 4> Source;
        Source.Emplace(1);
        Source.Emplace(2);
        ASSERT_TRUE(Source.IsInline());

        TInlineVector<FLifetime, 4> Target = std::move(Source);
        EXPECT_EQ(Target.Num(), 2);
        EXPECT_EQ(Target[0].Value, 1);
        EXPECT_EQ(Source.Num(), 0);
    }
    EXPECT_EQ(FLifetime::Live(), 0);
}

TEST(VectorInline, MoveFromSpilledStorageStealsTheBlock)
{
    TInlineVector<int, 2> Source;
    Source.Add(1);
    Source.Add(2);
    Source.Add(3);
    ASSERT_FALSE(Source.IsInline());
    const int* Base = Source.GetData();

    TInlineVector<int, 2> Target = std::move(Source);
    EXPECT_EQ(Target.GetData(), Base);
    EXPECT_TRUE(Source.IsInline());
}

TEST(VectorInline, SwapWorksAcrossInlineAndHeap)
{
    TInlineVector<int, 2> Small{1, 2};
    TInlineVector<int, 2> Large{1, 2, 3, 4, 5};
    ASSERT_TRUE(Small.IsInline());
    ASSERT_FALSE(Large.IsInline());

    swap(Small, Large);
    EXPECT_EQ(Small.Num(), 5);
    EXPECT_EQ(Large.Num(), 2);
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
    EXPECT_EQ(Values[0], 63);

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
    EXPECT_EQ(Copied.Num(), 2);
}

TEST(VectorInterop, AppendAndAssign)
{
    TVector<int> Values{1, 2};
    Values.Append({3, 4});
    EXPECT_EQ(Values, (TVector<int>{1, 2, 3, 4}));

    const TVector<int> Extra{5, 6};
    Values.Append(Extra);
    EXPECT_EQ(Values.Num(), 6);

    Values.Init(7, 3);
    EXPECT_EQ(Values, (TVector<int>{7, 7, 7}));
}

TEST(VectorBulk, AddUninitializedGivesWritableSlots)
{
    TVector<int> Values;
    const int32 First = Values.AddUninitialized(16);
    int* Slots = Values.GetData() + First;
    for (int Index = 0; Index < 16; ++Index)
    {
        Slots[Index] = Index;
    }

    ASSERT_EQ(Values.Num(), 16);
    EXPECT_EQ(Values[15], 15);
}

TEST(VectorAllocator, ScratchVectorAllocatesFromTheThreadArena)
{
    FMemMark Mark;

    TScratchVector<int> Values;
    for (int Index = 0; Index < 1024; ++Index)
    {
        Values.Add(Index);
    }

    ASSERT_EQ(Values.Num(), 1024);
    EXPECT_EQ(Values[1023], 1023);
}

TEST(Vector64, CountsInInt64AndBehavesLikeTheDefault)
{
    using TBytes = Lumina::Containers::TVector64<Lumina::uint8>;
    static_assert(std::is_same_v<TBytes::SizeType, Lumina::int64>);
    static_assert(std::is_same_v<TVector<Lumina::uint8>::SizeType, Lumina::int32>);
    static_assert(sizeof(TBytes) == 24);
    static_assert(TBytes::MaxElements == TNumericLimits<Lumina::int64>::Max());

    TBytes Bytes;
    for (int Value = 0; Value < 100; ++Value)
    {
        Bytes.Add(static_cast<Lumina::uint8>(Value));
    }
    Bytes.Insert(static_cast<Lumina::uint8>(200), 10);
    Bytes.RemoveAt(0, 5);
    ASSERT_EQ(Bytes.Num(), 96);
    EXPECT_EQ(Bytes[5], 200);
    EXPECT_EQ(Bytes[6], 10);

    Bytes.SetNumZeroed(1000);
    ASSERT_EQ(Bytes.Num(), 1000);
    EXPECT_EQ(Bytes[999], 0);

    const TBytes Copy = Bytes;
    EXPECT_TRUE(Copy == Bytes);
    EXPECT_EQ(Bytes.Find(static_cast<Lumina::uint8>(200)), 5);
}

TEST(Vector64, SpansFollowTheCountWidth)
{
    using Lumina::Containers::TSpan;
    using TBytes = Lumina::Containers::TVector64<Lumina::uint8>;

    static_assert(std::is_convertible_v<TVector<int>&, TSpan<int>>);
    static_assert(std::is_convertible_v<TSpan<int>, TSpan<int, Lumina::int64>>);
    static_assert(!std::is_convertible_v<TSpan<int, Lumina::int64>, TSpan<int>>);
    static_assert(std::is_constructible_v<TSpan<int>, TSpan<int, Lumina::int64>>);
    static_assert(std::is_convertible_v<TBytes&, TSpan<Lumina::uint8, Lumina::int64>>);
    static_assert(!std::is_convertible_v<TBytes&, TSpan<Lumina::uint8>>);
    static_assert(std::is_constructible_v<TSpan<Lumina::uint8>, TBytes&>);

    TBytes Bytes;
    Bytes.SetNumZeroed(16);
    Bytes[3] = 7;

    const auto Deduced = TSpan(Bytes);
    static_assert(std::is_same_v<std::remove_const_t<decltype(Deduced)>, TSpan<Lumina::uint8, Lumina::int64>>);
    EXPECT_EQ(Deduced.Num(), 16);

    const TSpan<Lumina::uint8> Narrow(Deduced);
    EXPECT_EQ(Narrow.Num(), 16);
    EXPECT_EQ(Narrow.GetData(), Bytes.GetData());
    EXPECT_EQ(Narrow[3], 7);

    const auto Raw = Lumina::Containers::AsBytes(TSpan<const int>(TVector<int>{ 1, 2, 3 }));
    EXPECT_EQ(Raw.Num(), static_cast<Lumina::int32>(3 * sizeof(int)));
}

TEST(Vector64, HandsItsBlockAcrossWidthsWithoutCopying)
{
    using TWide = Lumina::Containers::TVector64<int>;
    static_assert(std::is_convertible_v<TVector<int>&&, TWide>);
    static_assert(!std::is_convertible_v<TWide&&, TVector<int>>);
    static_assert(std::is_constructible_v<TVector<int>, TWide&&>);

    TVector<int> Narrow = { 1, 2, 3 };
    const int* Block = Narrow.GetData();

    TWide Wide = std::move(Narrow);
    EXPECT_EQ(Wide.GetData(), Block);
    ASSERT_EQ(Wide.Num(), 3);
    EXPECT_EQ(Wide[2], 3);
    EXPECT_TRUE(Narrow.IsEmpty());

    Wide.Add(4);
    TVector<int> Back(std::move(Wide));
    ASSERT_EQ(Back.Num(), 4);
    EXPECT_EQ(Back[3], 4);
    EXPECT_TRUE(Wide.IsEmpty());
}

TEST(IntCastChecked, PassesValuesThatKeepTheirMeaning)
{
    EXPECT_EQ(Lumina::IntCastChecked<Lumina::int32>(static_cast<Lumina::uint64>(2147483647)), 2147483647);
    EXPECT_EQ(Lumina::IntCastChecked<Lumina::int32>(static_cast<Lumina::int64>(-5)), -5);
    EXPECT_EQ(Lumina::IntCastChecked<Lumina::uint8>(255), 255);
    EXPECT_FALSE(Lumina::Cmp::InRange<Lumina::int32>(static_cast<Lumina::uint64>(1) << 32));
    EXPECT_FALSE(Lumina::Cmp::InRange<Lumina::uint32>(-1));
}
}
