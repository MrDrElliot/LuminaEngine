#include <gtest/gtest.h>

#include "Containers/String.h"
#include "Containers/Tuple.h"
#include "Containers/Vector.h"

using namespace Lumina;

namespace
{
    struct FTupleMoveOnly
    {
        FTupleMoveOnly() = default;
        explicit FTupleMoveOnly(int InValue) : Value(InValue) {}

        FTupleMoveOnly(const FTupleMoveOnly&) = delete;
        FTupleMoveOnly& operator=(const FTupleMoveOnly&) = delete;

        FTupleMoveOnly(FTupleMoveOnly&& Other) noexcept : Value(Other.Value) { Other.Value = 0; }

        FTupleMoveOnly& operator=(FTupleMoveOnly&& Other) noexcept
        {
            Value = Other.Value;
            Other.Value = 0;
            return *this;
        }

        int Value = 0;
    };
}

TEST(Tuple, ReportsSizeAndElementTypes)
{
    using FThree = TTuple<int, float, FString>;

    static_assert(TTupleSizeV<FThree> == 3);
    static_assert(std::is_same_v<TTupleElementT<0, FThree>, int>);
    static_assert(std::is_same_v<TTupleElementT<1, FThree>, float>);
    static_assert(std::is_same_v<TTupleElementT<2, FThree>, FString>);

    static_assert(TTupleSizeV<TTuple<>> == 0);
    SUCCEED();
}

TEST(Tuple, StoresAndReadsByIndex)
{
    TTuple<int, float, FString> Value(7, 2.5f, FString("hello"));

    EXPECT_EQ(Value.Get<0>(), 7);
    EXPECT_FLOAT_EQ(Value.Get<1>(), 2.5f);
    EXPECT_EQ(Value.Get<2>(), FString("hello"));

    Value.Get<0>() = 9;
    EXPECT_EQ(Get<0>(Value), 9);
}

TEST(Tuple, ReadsByType)
{
    TTuple<int, float> Value(3, 1.5f);

    EXPECT_EQ(Value.Get<int>(), 3);
    EXPECT_FLOAT_EQ(Value.Get<float>(), 1.5f);

    Get<int>(Value) = 11;
    EXPECT_EQ(Value.Get<int>(), 11);
}

TEST(Tuple, HoldsReferencesAndWritesThrough)
{
    int Number = 1;
    FString Text("before");

    TTuple<int&, FString&> Refs(Number, Text);

    Refs.Get<0>() = 42;
    Refs.Get<1>() = FString("after");

    EXPECT_EQ(Number, 42);
    EXPECT_EQ(Text, FString("after"));
}

TEST(Tuple, TieWritesBackToTheOriginals)
{
    int A = 0;
    float B = 0.0f;

    Tie(A, B) = TTuple<int, float>(5, 0.5f);

    EXPECT_EQ(A, 5);
    EXPECT_FLOAT_EQ(B, 0.5f);
}

TEST(Tuple, MakeTupleDecaysItsArguments)
{
    const int Number = 4;
    auto Value = MakeTuple(Number, FString("text"), 2.0f);

    static_assert(std::is_same_v<decltype(Value), TTuple<int, FString, float>>);
    EXPECT_EQ(Value.Get<0>(), 4);
    EXPECT_EQ(Value.Get<2>(), 2.0f);
}

TEST(Tuple, SupportsStructuredBindings)
{
    TTuple<int, float, FString> Value(1, 2.0f, FString("three"));

    auto [Number, Ratio, Text] = Value;

    EXPECT_EQ(Number, 1);
    EXPECT_FLOAT_EQ(Ratio, 2.0f);
    EXPECT_EQ(Text, FString("three"));
}

TEST(Tuple, StructuredBindingsOverReferencesWriteThrough)
{
    int Number = 3;
    FString Text("old");

    TTuple<int&, FString&> Refs(Number, Text);

    // A reference element binds to the referent, so the write lands on the original.
    auto [BoundNumber, BoundText] = Refs;
    BoundNumber = 8;
    BoundText = FString("new");

    EXPECT_EQ(Number, 8);
    EXPECT_EQ(Text, FString("new"));
}

TEST(Tuple, ConcatenatesTuples)
{
    auto Joined = TupleCat(TTuple<int>(1), TTuple<float, FString>(2.0f, FString("x")));

    static_assert(TTupleSizeV<decltype(Joined)> == 3);
    EXPECT_EQ(Joined.Get<0>(), 1);
    EXPECT_FLOAT_EQ(Joined.Get<1>(), 2.0f);
    EXPECT_EQ(Joined.Get<2>(), FString("x"));
}

TEST(Tuple, ConcatenatesManyIncludingEmpties)
{
    auto Joined = TupleCat(TTuple<int>(1), TTuple<>{}, TTuple<float>(2.0f), TTuple<>{}, TTuple<int>(3));

    static_assert(TTupleSizeV<decltype(Joined)> == 3);
    EXPECT_EQ(Joined.Get<0>(), 1);
    EXPECT_FLOAT_EQ(Joined.Get<1>(), 2.0f);
    EXPECT_EQ(Joined.Get<2>(), 3);
}

TEST(Tuple, ConcatenationKeepsReferenceElements)
{
    int Number = 5;
    auto Joined = TupleCat(TTuple<int>(1), TTuple<int&>(Number));

    static_assert(std::is_same_v<TTupleElementT<1, decltype(Joined)>, int&>);

    Joined.Get<1>() = 77;
    EXPECT_EQ(Number, 77);
}

TEST(Tuple, AppliesAFunctionOverTheElements)
{
    TTuple<int, int, int> Value(1, 2, 3);

    const int Sum = Apply([](int A, int B, int C) { return A + B + C; }, Value);
    EXPECT_EQ(Sum, 6);

    int Seen = 0;
    Apply([&Seen](auto... Args) { ((Seen += Args), ...); }, Value);
    EXPECT_EQ(Seen, 6);
}

TEST(Tuple, ComparesElementwise)
{
    // Aliased because the assertion macros would read the template comma as an argument separator.
    using FPair = TTuple<int, float>;
    using FEmpty = TTuple<>;

    EXPECT_TRUE(FPair(1, 2.0f) == FPair(1, 2.0f));
    EXPECT_FALSE(FPair(1, 2.0f) == FPair(1, 3.0f));
    EXPECT_TRUE(FEmpty{} == FEmpty{});
}

TEST(Tuple, CarriesMoveOnlyElements)
{
    TTuple<FTupleMoveOnly> Value(FTupleMoveOnly(9));
    EXPECT_EQ(Value.Get<0>().Value, 9);

    TTuple<FTupleMoveOnly> Moved = Move(Value);
    EXPECT_EQ(Moved.Get<0>().Value, 9);
    EXPECT_EQ(Value.Get<0>().Value, 0);
}

TEST(Tuple, MovesOutOfAnRvalue)
{
    TTuple<FString> Value(FString("payload"));

    FString Taken = Move(Value).Get<0>();
    EXPECT_EQ(Taken, FString("payload"));
}

TEST(Tuple, IsNoLargerThanItsElements)
{
    // Flat storage, so a tuple should not pay a recursive layout tax over the members it holds.
    using FTwoInts = TTuple<int, int>;
    using FThreeWords = TTuple<uint64, uint64, uint64>;

    EXPECT_EQ(sizeof(FTwoInts), sizeof(int) * 2);
    EXPECT_EQ(sizeof(FThreeWords), sizeof(uint64) * 3);
}
