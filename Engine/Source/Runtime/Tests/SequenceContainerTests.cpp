#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <vector>

#include "Containers/BitSet.h"
#include "Containers/Deque.h"
#include "Containers/List.h"
#include "Containers/Queue.h"
#include "Containers/StaticArray.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaSequenceTests
{
    template <typename T, size_t N>
    using TArray = Lumina::Containers::TArray<T, N>;

    template <typename T>
    using TDeque = Lumina::Containers::TDeque<T>;

    template <typename T>
    using TList = Lumina::Containers::TList<T>;

    template <typename T>
    using TQueue = Lumina::Containers::TQueue<T>;

    template <typename T>
    using TStack = Lumina::Containers::TStack<T>;

    template <size_t N>
    using TBitSet = Lumina::Containers::TBitSet<N>;

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

    TEST(StaticArray, IsAnAggregateWithNoOverhead)
    {
        static_assert(sizeof(TArray<int, 4>) == 4 * sizeof(int));
        static_assert(std::is_trivially_copyable_v<TArray<int, 4>>);
        static_assert(TArray<int, 4>::Num() == 4);

        constexpr TArray<int, 3> Values{ 1, 2, 3 };
        static_assert(Values[0] == 1);
        static_assert(Values[2] == 3);
        SUCCEED();
    }

    // The accessors are one deducing-this template each, so constness is deduced rather than declared.
    TEST(StaticArray, ConstAccessorsStayConst)
    {
        using FMutable = TArray<int, 4>&;
        using FConst   = const TArray<int, 4>&;

        static_assert(std::is_same_v<decltype(std::declval<FMutable>()[0]), int&>);
        static_assert(std::is_same_v<decltype(std::declval<FConst>()[0]), const int&>);
        static_assert(std::is_same_v<decltype(std::declval<FMutable>()[0]), int&>);
        static_assert(std::is_same_v<decltype(std::declval<FConst>()[0]), const int&>);
        static_assert(std::is_same_v<decltype(std::declval<FMutable>().Last()), int&>);
        static_assert(std::is_same_v<decltype(std::declval<FConst>().Last()), const int&>);
        static_assert(std::is_same_v<decltype(std::declval<FMutable>().GetData()), int*>);
        static_assert(std::is_same_v<decltype(std::declval<FConst>().GetData()), const int*>);
        static_assert(std::is_same_v<decltype(std::declval<FMutable>().begin()), int*>);
        static_assert(std::is_same_v<decltype(std::declval<FConst>().begin()), const int*>);
        static_assert(std::is_same_v<decltype(std::declval<FMutable>().end()), int*>);
        static_assert(std::is_same_v<decltype(std::declval<FConst>().end()), const int*>);
        SUCCEED();
    }

    TEST(StaticArray, AccessorsAndIteration)
    {
        TArray<int, 4> Values{ 10, 20, 30, 40 };
        EXPECT_EQ(Values[0], 10);
        EXPECT_EQ(Values.Last(), 40);
        EXPECT_EQ(Values[2], 30);
        EXPECT_EQ(*Values.GetData(), 10);
        EXPECT_FALSE(Values.IsEmpty());

        int Sum = 0;
        for (int Element : Values)
        {
            Sum += Element;
        }
        EXPECT_EQ(Sum, 100);

        Values.Fill(7);
        EXPECT_EQ(Values[0], 7);
        EXPECT_EQ(Values[3], 7);
    }

    TEST(StaticArray, ComparesAndSwaps)
    {
        TArray<int, 3> Left{ 1, 2, 3 };
        TArray<int, 3> Right{ 1, 2, 3 };
        EXPECT_TRUE(Left == Right);

        Right[2] = 4;
        EXPECT_FALSE(Left == Right);
        EXPECT_LT(Left, Right);

        swap(Left, Right);
        EXPECT_EQ(Left[2], 4);
        EXPECT_EQ(Right[2], 3);
    }

    TEST(StaticArray, StructuredBindingsAndZeroLength)
    {
        TArray<int, 2> Pair{ 5, 6 };
        auto& [First, Second] = Pair;
        EXPECT_EQ(First, 5);
        EXPECT_EQ(Second, 6);

        TArray<int, 0> Empty;
        EXPECT_TRUE(Empty.IsEmpty());
        EXPECT_EQ(Empty.Num(), 0);
        EXPECT_EQ(Empty.begin(), Empty.end());
    }

    TEST(Deque, PushesAndPopsBothEnds)
    {
        TDeque<int> Values;
        EXPECT_TRUE(Values.IsEmpty());

        Values.PushLast(2);
        Values.PushLast(3);
        Values.PushFirst(1);
        Values.PushFirst(0);

        EXPECT_EQ(Values.Num(), 4);
        EXPECT_EQ(Values[0], 0);
        EXPECT_EQ(Values.Last(), 3);
        EXPECT_EQ(Values[1], 1);
        EXPECT_EQ(Values[2], 2);

        Values.PopFirst();
        EXPECT_EQ(Values[0], 1);
        Values.PopLast();
        EXPECT_EQ(Values.Last(), 2);
        EXPECT_EQ(Values.Num(), 2);
    }

    // The ring wraps long before it grows, which is the case indexing has to get right.
    TEST(Deque, StaysCorrectAcrossManyWraps)
    {
        TDeque<int> Values;
        for (int Index = 0; Index < 8; ++Index)
        {
            Values.PushLast(Index);
        }

        for (int Round = 0; Round < 500; ++Round)
        {
            EXPECT_EQ(Values[0], Round);
            Values.PopFirst();
            Values.PushLast(Round + 8);
            EXPECT_EQ(Values.Num(), 8);
            EXPECT_EQ(Values.Last(), Round + 8);
        }

        for (int32 Index = 0; Index < Values.Num(); ++Index)
        {
            EXPECT_EQ(Values[Index], 500 + Index);
        }
    }

    TEST(Deque, GrowsFromAWrappedStateInOrder)
    {
        TDeque<int> Values;
        for (int Index = 0; Index < 8; ++Index)
        {
            Values.PushLast(Index);
        }
        for (int Index = 0; Index < 5; ++Index)
        {
            Values.PopFirst();
            Values.PushLast(100 + Index);
        }

        const int32 Before = Values.Num();
        for (int Index = 0; Index < 200; ++Index)
        {
            Values.PushLast(1000 + Index);
        }

        EXPECT_EQ(Values.Num(), Before + 200);
        EXPECT_EQ(Values[0], 5);
        EXPECT_EQ(Values.Last(), 1199);

        int Expected = 5;
        size_t Position = 0;
        for (; Position < 3; ++Position, ++Expected)
        {
            EXPECT_EQ(Values[static_cast<int32>(Position)], Expected) << "order lost at " << Position;
        }
    }

    TEST(Deque, IteratesAndComparesAndClears)
    {
        TDeque<int> Values;
        for (int Index = 0; Index < 5; ++Index)
        {
            Values.PushLast(Index);
        }

        int Sum = 0;
        for (int Element : Values)
        {
            Sum += Element;
        }
        EXPECT_EQ(Sum, 10);

        TDeque<int> Copy = Values;
        EXPECT_TRUE(Copy == Values);

        Copy.PopLast();
        EXPECT_FALSE(Copy == Values);

        Values.Reset();
        EXPECT_TRUE(Values.IsEmpty());
        EXPECT_EQ(Values.Num(), 0);
    }

    TEST(Deque, DestroysEveryElementExactlyOnce)
    {
        FCounted::Live = 0;
        {
            TDeque<FCounted> Values;
            for (int Index = 0; Index < 300; ++Index)
            {
                Values.EmplaceLast(Index);
            }
            EXPECT_EQ(FCounted::Live, 300);

            for (int Index = 0; Index < 150; ++Index)
            {
                Values.PopFirst();
            }
            EXPECT_EQ(FCounted::Live, 150);
        }
        EXPECT_EQ(FCounted::Live, 0);
    }

    TEST(Deque, MoveLeavesTheSourceUsable)
    {
        TDeque<int> Source;
        Source.PushLast(1);
        Source.PushLast(2);

        TDeque<int> Moved = std::move(Source);
        EXPECT_EQ(Moved.Num(), 2);
        EXPECT_TRUE(Source.IsEmpty());

        Source.PushLast(9);
        EXPECT_EQ(Source.Num(), 1);
        EXPECT_EQ(Source[0], 9);
    }

    TEST(Queue, IsFirstInFirstOut)
    {
        TQueue<int> Values;
        EXPECT_TRUE(Values.IsEmpty());

        Values.Enqueue(1);
        Values.Enqueue(2);
        Values.Emplace(3);

        EXPECT_EQ(Values.Num(), 3);
        EXPECT_EQ(Values.Peek(), 1);
        EXPECT_EQ(Values.GetContainer().Last(), 3);

        Values.Pop();
        EXPECT_EQ(Values.Peek(), 2);
        Values.Pop();
        Values.Pop();
        EXPECT_TRUE(Values.IsEmpty());
    }

    TEST(Queue, SwapsWithAnother)
    {
        TQueue<int> Left;
        TQueue<int> Right;
        Left.Enqueue(1);
        Right.Enqueue(8);
        Right.Enqueue(9);

        swap(Left, Right);
        EXPECT_EQ(Left.Num(), 2);
        EXPECT_EQ(Left.Peek(), 8);
        EXPECT_EQ(Right.Num(), 1);
        EXPECT_EQ(Right.Peek(), 1);
    }

    TEST(Stack, IsLastInFirstOut)
    {
        TStack<Lumina::FString> Values;
        Values.Push(Lumina::FString("a"));
        Values.Emplace("b");

        EXPECT_EQ(Values.Num(), 2);
        EXPECT_EQ(Values.Top(), Lumina::FString("b"));

        Values.Pop();
        EXPECT_EQ(Values.Top(), Lumina::FString("a"));
        Values.Pop();
        EXPECT_TRUE(Values.IsEmpty());
    }

    TEST(List, PushesBothEndsAndIterates)
    {
        TList<int> Values;
        EXPECT_TRUE(Values.IsEmpty());

        Values.AddTail(2);
        Values.AddTail(3);
        Values.AddHead(1);

        EXPECT_EQ(Values.Num(), 3);
        EXPECT_EQ(Values.Head(), 1);
        EXPECT_EQ(Values.Tail(), 3);

        std::vector<int> Seen;
        for (int Element : Values)
        {
            Seen.push_back(Element);
        }
        EXPECT_EQ(Seen, (std::vector<int>{ 1, 2, 3 }));
    }

    // The reason to pick a list over a vector is that an element address outlives every insert.
    TEST(List, ElementAddressesAreStable)
    {
        TList<int> Values;
        std::vector<const int*> Addresses;
        for (int Index = 0; Index < 500; ++Index)
        {
            Addresses.push_back(&Values.EmplaceTail(Index));
        }

        auto It = Values.begin();
        for (int Index = 0; Index < 500; ++Index, ++It)
        {
            EXPECT_EQ(Addresses[static_cast<size_t>(Index)], &*It) << "moved at " << Index;
            EXPECT_EQ(*It, Index);
        }
    }

    TEST(List, InsertsAtPositionAndReturnsTheNewElement)
    {
        TList<int> Values;
        for (int Index = 0; Index < 3; ++Index)
        {
            Values.AddTail(Index * 10);
        }

        TList<int>::iterator Second = Values.begin();
        ++Second;

        TList<int>::iterator Inserted = Values.Insert(Second, 5);
        EXPECT_EQ(*Inserted, 5);

        TList<int>::iterator Expected = Values.begin();
        ++Expected;
        EXPECT_EQ(&*Inserted, &*Expected);

        TList<int>::iterator Front = Values.Insert(Values.begin(), -1);
        EXPECT_EQ(*Front, -1);
        EXPECT_EQ(&*Front, &Values.Head());

        TList<int>::iterator Back = Values.Emplace(Values.end(), 99);
        EXPECT_EQ(*Back, 99);
        EXPECT_EQ(&*Back, &Values.Tail());

        std::vector<int> Seen;
        for (int Element : Values)
        {
            Seen.push_back(Element);
        }
        EXPECT_EQ(Seen, (std::vector<int>{ -1, 0, 5, 10, 20, 99 }));
        EXPECT_EQ(Values.Num(), 6);
    }

    TEST(List, ErasesAndPops)
    {
        TList<int> Values;
        for (int Index = 0; Index < 5; ++Index)
        {
            Values.AddTail(Index);
        }

        for (auto It = Values.begin(); It != Values.end();)
        {
            It = (*It % 2 == 0) ? Values.Remove(It) : ++It;
        }

        EXPECT_EQ(Values.Num(), 2);
        EXPECT_EQ(Values.Head(), 1);
        EXPECT_EQ(Values.Tail(), 3);

        Values.RemoveHead();
        EXPECT_EQ(Values.Head(), 3);
        Values.RemoveTail();
        EXPECT_TRUE(Values.IsEmpty());
    }

    TEST(List, DestroysEveryNodeExactlyOnce)
    {
        FCounted::Live = 0;
        {
            TList<FCounted> Values;
            for (int Index = 0; Index < 200; ++Index)
            {
                Values.EmplaceTail(Index);
            }
            EXPECT_EQ(FCounted::Live, 200);

            Values.Empty();
            EXPECT_EQ(FCounted::Live, 0);

            Values.EmplaceTail(1);
            EXPECT_EQ(FCounted::Live, 1);
        }
        EXPECT_EQ(FCounted::Live, 0);
    }

    TEST(List, CopyAndMoveAreIndependent)
    {
        TList<int> Source;
        Source.AddTail(1);
        Source.AddTail(2);

        TList<int> Copy = Source;
        Copy.AddTail(3);
        EXPECT_EQ(Copy.Num(), 3);
        EXPECT_EQ(Source.Num(), 2);

        TList<int> Moved = std::move(Source);
        EXPECT_EQ(Moved.Num(), 2);
        EXPECT_TRUE(Source.IsEmpty());

        Source.AddTail(9);
        EXPECT_EQ(Source.Num(), 1);
    }

    TEST(BitSet, SetsTestsAndCounts)
    {
        TBitSet<128> Bits;
        EXPECT_EQ(Bits.Num(), 128);
        EXPECT_TRUE(Bits.None());
        EXPECT_EQ(Bits.Count(), 0);

        Bits.Set(0);
        Bits.Set(64);
        Bits.Set(127);

        EXPECT_TRUE(Bits.Test(0));
        EXPECT_TRUE(Bits.Test(64));
        EXPECT_TRUE(Bits.Test(127));
        EXPECT_FALSE(Bits.Test(1));
        EXPECT_EQ(Bits.Count(), 3);
        EXPECT_TRUE(Bits.Any());
        EXPECT_FALSE(Bits.All());
        EXPECT_EQ(Bits.FindFirst(), 0u);

        Bits.Reset(0);
        EXPECT_EQ(Bits.FindFirst(), 64u);
    }

    // Bits past the width must never be reported, which is what the tail mask is for.
    TEST(BitSet, IgnoresBitsPastTheWidth)
    {
        TBitSet<70> Bits;
        Bits.Set();
        EXPECT_EQ(Bits.Count(), 70);
        EXPECT_TRUE(Bits.All());

        Bits.Flip();
        EXPECT_EQ(Bits.Count(), 0);
        EXPECT_TRUE(Bits.None());

        TBitSet<3> Small;
        Small.Set();
        EXPECT_EQ(Small.Count(), 3);
    }

    TEST(BitSet, ReferenceWritesBack)
    {
        TBitSet<16> Bits;
        Bits[3] = true;
        EXPECT_TRUE(Bits.Test(3));

        Bits[3] = false;
        EXPECT_FALSE(Bits.Test(3));

        const TBitSet<16>& Const = Bits;
        EXPECT_FALSE(Const[3]);
    }

    TEST(BitSet, BitwiseOperators)
    {
        TBitSet<32> Left;
        TBitSet<32> Right;
        Left.Set(1);
        Left.Set(2);
        Right.Set(2);
        Right.Set(3);

        const TBitSet<32> And = Left & Right;
        EXPECT_EQ(And.Count(), 1);
        EXPECT_TRUE(And.Test(2));

        const TBitSet<32> Or = Left | Right;
        EXPECT_EQ(Or.Count(), 3);

        const TBitSet<32> Xor = Left ^ Right;
        EXPECT_EQ(Xor.Count(), 2);
        EXPECT_FALSE(Xor.Test(2));

        EXPECT_TRUE(Left == Left);
        EXPECT_FALSE(Left == Right);
    }

    TEST(BitSet, FindFirstReportsTheWidthWhenEmpty)
    {
        TBitSet<64> Bits;
        EXPECT_EQ(Bits.FindFirst(), 64u);

        Bits.Set(63);
        EXPECT_EQ(Bits.FindFirst(), 63u);
    }
}
