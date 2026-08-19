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
        static_assert(TArray<int, 4>::size() == 4);

        constexpr TArray<int, 3> Values{ 1, 2, 3 };
        static_assert(Values[0] == 1);
        static_assert(Values[2] == 3);
        SUCCEED();
    }

    TEST(StaticArray, AccessorsAndIteration)
    {
        TArray<int, 4> Values{ 10, 20, 30, 40 };
        EXPECT_EQ(Values.front(), 10);
        EXPECT_EQ(Values.back(), 40);
        EXPECT_EQ(Values.at(2), 30);
        EXPECT_EQ(*Values.data(), 10);
        EXPECT_FALSE(Values.empty());

        int Sum = 0;
        for (int Element : Values)
        {
            Sum += Element;
        }
        EXPECT_EQ(Sum, 100);

        Values.fill(7);
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

        Left.swap(Right);
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
        EXPECT_TRUE(Empty.empty());
        EXPECT_EQ(Empty.size(), 0u);
        EXPECT_EQ(Empty.begin(), Empty.end());
    }

    TEST(Deque, PushesAndPopsBothEnds)
    {
        TDeque<int> Values;
        EXPECT_TRUE(Values.empty());

        Values.push_back(2);
        Values.push_back(3);
        Values.push_front(1);
        Values.push_front(0);

        EXPECT_EQ(Values.size(), 4u);
        EXPECT_EQ(Values.front(), 0);
        EXPECT_EQ(Values.back(), 3);
        EXPECT_EQ(Values[1], 1);
        EXPECT_EQ(Values[2], 2);

        Values.pop_front();
        EXPECT_EQ(Values.front(), 1);
        Values.pop_back();
        EXPECT_EQ(Values.back(), 2);
        EXPECT_EQ(Values.size(), 2u);
    }

    // The ring wraps long before it grows, which is the case indexing has to get right.
    TEST(Deque, StaysCorrectAcrossManyWraps)
    {
        TDeque<int> Values;
        for (int Index = 0; Index < 8; ++Index)
        {
            Values.push_back(Index);
        }

        for (int Round = 0; Round < 500; ++Round)
        {
            EXPECT_EQ(Values.front(), Round);
            Values.pop_front();
            Values.push_back(Round + 8);
            EXPECT_EQ(Values.size(), 8u);
            EXPECT_EQ(Values.back(), Round + 8);
        }

        for (size_t Index = 0; Index < Values.size(); ++Index)
        {
            EXPECT_EQ(Values[Index], static_cast<int>(500 + Index));
        }
    }

    TEST(Deque, GrowsFromAWrappedStateInOrder)
    {
        TDeque<int> Values;
        for (int Index = 0; Index < 8; ++Index)
        {
            Values.push_back(Index);
        }
        for (int Index = 0; Index < 5; ++Index)
        {
            Values.pop_front();
            Values.push_back(100 + Index);
        }

        const size_t Before = Values.size();
        for (int Index = 0; Index < 200; ++Index)
        {
            Values.push_back(1000 + Index);
        }

        EXPECT_EQ(Values.size(), Before + 200);
        EXPECT_EQ(Values.front(), 5);
        EXPECT_EQ(Values.back(), 1199);

        int Expected = 5;
        size_t Position = 0;
        for (; Position < 3; ++Position, ++Expected)
        {
            EXPECT_EQ(Values[Position], Expected) << "order lost at " << Position;
        }
    }

    TEST(Deque, IteratesAndComparesAndClears)
    {
        TDeque<int> Values;
        for (int Index = 0; Index < 5; ++Index)
        {
            Values.push_back(Index);
        }

        int Sum = 0;
        for (int Element : Values)
        {
            Sum += Element;
        }
        EXPECT_EQ(Sum, 10);

        TDeque<int> Copy = Values;
        EXPECT_TRUE(Copy == Values);

        Copy.pop_back();
        EXPECT_FALSE(Copy == Values);

        Values.clear();
        EXPECT_TRUE(Values.empty());
        EXPECT_EQ(Values.size(), 0u);
    }

    TEST(Deque, DestroysEveryElementExactlyOnce)
    {
        FCounted::Live = 0;
        {
            TDeque<FCounted> Values;
            for (int Index = 0; Index < 300; ++Index)
            {
                Values.emplace_back(Index);
            }
            EXPECT_EQ(FCounted::Live, 300);

            for (int Index = 0; Index < 150; ++Index)
            {
                Values.pop_front();
            }
            EXPECT_EQ(FCounted::Live, 150);
        }
        EXPECT_EQ(FCounted::Live, 0);
    }

    TEST(Deque, MoveLeavesTheSourceUsable)
    {
        TDeque<int> Source;
        Source.push_back(1);
        Source.push_back(2);

        TDeque<int> Moved = std::move(Source);
        EXPECT_EQ(Moved.size(), 2u);
        EXPECT_TRUE(Source.empty());

        Source.push_back(9);
        EXPECT_EQ(Source.size(), 1u);
        EXPECT_EQ(Source.front(), 9);
    }

    TEST(Queue, IsFirstInFirstOut)
    {
        TQueue<int> Values;
        EXPECT_TRUE(Values.empty());

        Values.push(1);
        Values.push(2);
        Values.emplace(3);

        EXPECT_EQ(Values.size(), 3u);
        EXPECT_EQ(Values.front(), 1);
        EXPECT_EQ(Values.back(), 3);

        Values.pop();
        EXPECT_EQ(Values.front(), 2);
        Values.pop();
        Values.pop();
        EXPECT_TRUE(Values.empty());
    }

    TEST(Queue, SwapsWithAnother)
    {
        TQueue<int> Left;
        TQueue<int> Right;
        Left.push(1);
        Right.push(8);
        Right.push(9);

        Left.swap(Right);
        EXPECT_EQ(Left.size(), 2u);
        EXPECT_EQ(Left.front(), 8);
        EXPECT_EQ(Right.size(), 1u);
        EXPECT_EQ(Right.front(), 1);
    }

    TEST(Stack, IsLastInFirstOut)
    {
        TStack<Lumina::FString> Values;
        Values.push(Lumina::FString("a"));
        Values.emplace("b");

        EXPECT_EQ(Values.size(), 2u);
        EXPECT_EQ(Values.top(), Lumina::FString("b"));

        Values.pop();
        EXPECT_EQ(Values.top(), Lumina::FString("a"));
        Values.pop();
        EXPECT_TRUE(Values.empty());
    }

    TEST(List, PushesBothEndsAndIterates)
    {
        TList<int> Values;
        EXPECT_TRUE(Values.empty());

        Values.push_back(2);
        Values.push_back(3);
        Values.push_front(1);

        EXPECT_EQ(Values.size(), 3u);
        EXPECT_EQ(Values.front(), 1);
        EXPECT_EQ(Values.back(), 3);

        std::vector<int> Seen;
        for (int Element : Values)
        {
            Seen.push_back(Element);
        }
        EXPECT_EQ(Seen, (std::vector<int>{ 1, 2, 3 }));
    }

    // The reason to pick a list over a vector: an element address outlives every later insert.
    TEST(List, ElementAddressesAreStable)
    {
        TList<int> Values;
        std::vector<const int*> Addresses;
        for (int Index = 0; Index < 500; ++Index)
        {
            Addresses.push_back(&Values.emplace_back(Index));
        }

        auto It = Values.begin();
        for (int Index = 0; Index < 500; ++Index, ++It)
        {
            EXPECT_EQ(Addresses[static_cast<size_t>(Index)], &*It) << "moved at " << Index;
            EXPECT_EQ(*It, Index);
        }
    }

    TEST(List, ErasesAndPops)
    {
        TList<int> Values;
        for (int Index = 0; Index < 5; ++Index)
        {
            Values.push_back(Index);
        }

        for (auto It = Values.begin(); It != Values.end();)
        {
            It = (*It % 2 == 0) ? Values.erase(It) : ++It;
        }

        EXPECT_EQ(Values.size(), 2u);
        EXPECT_EQ(Values.front(), 1);
        EXPECT_EQ(Values.back(), 3);

        Values.pop_front();
        EXPECT_EQ(Values.front(), 3);
        Values.pop_back();
        EXPECT_TRUE(Values.empty());
    }

    TEST(List, DestroysEveryNodeExactlyOnce)
    {
        FCounted::Live = 0;
        {
            TList<FCounted> Values;
            for (int Index = 0; Index < 200; ++Index)
            {
                Values.emplace_back(Index);
            }
            EXPECT_EQ(FCounted::Live, 200);

            Values.clear();
            EXPECT_EQ(FCounted::Live, 0);

            Values.emplace_back(1);
            EXPECT_EQ(FCounted::Live, 1);
        }
        EXPECT_EQ(FCounted::Live, 0);
    }

    TEST(List, CopyAndMoveAreIndependent)
    {
        TList<int> Source;
        Source.push_back(1);
        Source.push_back(2);

        TList<int> Copy = Source;
        Copy.push_back(3);
        EXPECT_EQ(Copy.size(), 3u);
        EXPECT_EQ(Source.size(), 2u);

        TList<int> Moved = std::move(Source);
        EXPECT_EQ(Moved.size(), 2u);
        EXPECT_TRUE(Source.empty());

        Source.push_back(9);
        EXPECT_EQ(Source.size(), 1u);
    }

    TEST(BitSet, SetsTestsAndCounts)
    {
        TBitSet<128> Bits;
        EXPECT_EQ(Bits.size(), 128u);
        EXPECT_TRUE(Bits.None());
        EXPECT_EQ(Bits.Count(), 0u);

        Bits.Set(0);
        Bits.Set(64);
        Bits.Set(127);

        EXPECT_TRUE(Bits.Test(0));
        EXPECT_TRUE(Bits.Test(64));
        EXPECT_TRUE(Bits.Test(127));
        EXPECT_FALSE(Bits.Test(1));
        EXPECT_EQ(Bits.Count(), 3u);
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
        EXPECT_EQ(Bits.Count(), 70u);
        EXPECT_TRUE(Bits.All());

        Bits.Flip();
        EXPECT_EQ(Bits.Count(), 0u);
        EXPECT_TRUE(Bits.None());

        TBitSet<3> Small;
        Small.Set();
        EXPECT_EQ(Small.Count(), 3u);
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
        EXPECT_EQ(And.Count(), 1u);
        EXPECT_TRUE(And.Test(2));

        const TBitSet<32> Or = Left | Right;
        EXPECT_EQ(Or.Count(), 3u);

        const TBitSet<32> Xor = Left ^ Right;
        EXPECT_EQ(Xor.Count(), 2u);
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
