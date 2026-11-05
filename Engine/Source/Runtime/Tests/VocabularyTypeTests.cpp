#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "Containers/Optional.h"
#include "Containers/Pair.h"
#include "Containers/Span.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "Containers/Variant.h"
#include "Containers/Vector.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaVocabularyTests
{
    using Lumina::Containers::TPair;
    using Lumina::Containers::MakePair;
    using Lumina::Containers::TSpan;
    using Lumina::Containers::TOptional;
    using Lumina::Containers::NullOpt;
    using Lumina::Containers::InPlace;
    using Lumina::Containers::TVariant;

    struct FTracked
    {
        static inline int Live = 0;

        int Value = 0;

        FTracked() { ++Live; }
        explicit FTracked(int InValue) : Value(InValue) { ++Live; }
        FTracked(const FTracked& Other) : Value(Other.Value) { ++Live; }
        FTracked(FTracked&& Other) noexcept : Value(Other.Value) { Other.Value = -1; ++Live; }
        FTracked& operator=(const FTracked& Other) { Value = Other.Value; return *this; }
        FTracked& operator=(FTracked&& Other) noexcept { Value = Other.Value; Other.Value = -1; return *this; }
        ~FTracked() { --Live; }

        bool operator==(const FTracked& Other) const noexcept { return Value == Other.Value; }
    };

    TEST(PairBasics, ConstructCompareAndSwap)
    {
        constexpr TPair<int, int> Literal(1, 2);
        static_assert(Literal.Key == 1);
        static_assert(Literal.Value == 2);

        TPair<int, Lumina::FString> Mixed(7, Lumina::FString("seven"));
        EXPECT_EQ(Mixed.Key, 7);
        EXPECT_EQ(Mixed.Value, Lumina::FString("seven"));

        TPair<int, int> Left(1, 2);
        TPair<int, int> Right(3, 4);
        swap(Left, Right);
        EXPECT_EQ(Left.Key, 3);
        EXPECT_EQ(Right.Key, 1);

        EXPECT_TRUE((TPair<int, int>(1, 2) == TPair<int, int>(1, 2)));
        EXPECT_LT((TPair<int, int>(1, 2)), (TPair<int, int>(1, 3)));
        EXPECT_LT((TPair<int, int>(1, 9)), (TPair<int, int>(2, 0)));
    }

    TEST(PairBasics, MakePairDeducesDecayedTypes)
    {
        const auto Made = MakePair(1, Lumina::FString("value"));
        static_assert(std::is_same_v<decltype(Made), const TPair<int, Lumina::FString>>);
        EXPECT_EQ(Made.Key, 1);
    }

    TEST(PairBasics, ConvertsToAndFromTheStandardPair)
    {
        const std::pair<int, int> Standard(3, 4);
        const TPair<int, int> Ours(Standard);
        EXPECT_EQ(Ours.Key, 3);

        const std::pair<int, int> Back = Ours;
        EXPECT_EQ(Back.second, 4);
    }

    TEST(PairBasics, StructuredBindingsWork)
    {
        TPair<int, Lumina::FString> Value(5, Lumina::FString("five"));
        auto& [Number, Text] = Value;
        EXPECT_EQ(Number, 5);
        EXPECT_EQ(Text, Lumina::FString("five"));

        Number = 6;
        EXPECT_EQ(Value.Key, 6);
    }

    TEST(PairBasics, PiecewiseConstructionBuildsInPlace)
    {
        const TPair<Lumina::FString, Lumina::FString> Built(
            std::piecewise_construct,
            Lumina::ForwardAsTuple("abc", 3),
            Lumina::ForwardAsTuple(4, 'x'));

        EXPECT_EQ(Built.Key, Lumina::FString("abc"));
        EXPECT_EQ(Built.Value, Lumina::FString("xxxx"));
    }

    TEST(PairLifetime, DestroysBothMembers)
    {
        FTracked::Live = 0;
        {
            TPair<FTracked, FTracked> Value(FTracked(1), FTracked(2));
            EXPECT_EQ(Value.Key.Value, 1);
            EXPECT_EQ(FTracked::Live, 2);
        }
        EXPECT_EQ(FTracked::Live, 0);
    }

    TEST(SpanBasics, EmptyAndFromPointerAndSize)
    {
        constexpr TSpan<const int> Empty;
        static_assert(Empty.IsEmpty());
        static_assert(Empty.Num() == 0);
        EXPECT_EQ(Empty.GetData(), nullptr);

        int Values[] = { 1, 2, 3, 4 };
        const TSpan<int> Span(Values, 4);
        EXPECT_EQ(Span.Num(), 4);
        EXPECT_EQ(Span.NumBytes(), 4u * sizeof(int));
        EXPECT_EQ(Span[0], 1);
        EXPECT_EQ(Span.Last(), 4);
        EXPECT_EQ(Span[2], 3);
    }

    TEST(SpanBasics, DeducesFromArraysAndContainers)
    {
        int RawArray[] = { 1, 2, 3 };
        const TSpan<int> FromArray(RawArray);
        EXPECT_EQ(FromArray.Num(), 3);

        std::vector<int> Vector{ 4, 5, 6, 7 };
        const TSpan<int> FromVector(Vector);
        EXPECT_EQ(FromVector.Num(), 4);
        EXPECT_EQ(FromVector[0], 4);

        Lumina::Containers::TVector<int> Ours;
        Ours.Add(8);
        Ours.Add(9);
        const TSpan<int> FromOurs(Ours);
        EXPECT_EQ(FromOurs.Num(), 2);
        EXPECT_EQ(FromOurs.Last(), 9);
    }

    TEST(SpanBasics, MutableSpanWritesThrough)
    {
        std::vector<int> Values{ 1, 2, 3 };
        const TSpan<int> Span(Values);
        Span[1] = 20;
        EXPECT_EQ(Values[1], 20);

        for (int& Element : Span)
        {
            Element *= 2;
        }
        EXPECT_EQ(Values[0], 2);
        EXPECT_EQ(Values[2], 6);
    }

    TEST(SpanBasics, ConvertsToConstAndToStdSpan)
    {
        std::vector<int> Values{ 1, 2, 3 };
        const TSpan<int> Mutable(Values);

        const TSpan<const int> Constant = Mutable;
        EXPECT_EQ(Constant.Num(), 3);

        const std::span<int> Standard = Mutable;
        EXPECT_EQ(Standard.size(), 3u);

        const TSpan<int> Back(Standard);
        EXPECT_EQ(Back[1], 2);
    }

    TEST(SpanSlicing, FirstLastAndSubspan)
    {
        int Values[] = { 0, 1, 2, 3, 4, 5 };
        const TSpan<int> Span(Values);

        EXPECT_EQ(Span.Left(2).Num(), 2);
        EXPECT_EQ(Span.Left(2).Last(), 1);
        EXPECT_EQ(Span.Right(2)[0], 4);
        EXPECT_EQ(Span.Mid(2).Num(), 4);
        EXPECT_EQ(Span.Mid(2, 2).Last(), 3);
        EXPECT_EQ(Span.Mid(2, 100).Num(), 4);
        EXPECT_TRUE(Span.Mid(6).IsEmpty());
    }

    TEST(SpanSearch, ContainsAndIndexOf)
    {
        int Values[] = { 10, 20, 30 };
        const TSpan<int> Span(Values);

        EXPECT_TRUE(Span.Contains(20));
        EXPECT_FALSE(Span.Contains(40));
        EXPECT_EQ(Span.Find(30), 2);
        EXPECT_EQ(Span.Find(40), Lumina::Constants::kIndexNone);
    }

    TEST(OptionalBasics, DisengagedByDefault)
    {
        const TOptional<int> Value;
        EXPECT_FALSE(Value.IsSet());
        EXPECT_FALSE(Value.IsSet());
        EXPECT_FALSE(static_cast<bool>(Value));
        EXPECT_EQ(Value.Get(7), 7);
        EXPECT_TRUE(Value == NullOpt);
    }

    TEST(OptionalBasics, EngagedFromAValue)
    {
        TOptional<int> Value = 5;
        EXPECT_TRUE(Value.IsSet());
        EXPECT_EQ(Value.GetValue(), 5);
        EXPECT_EQ(*Value, 5);
        EXPECT_EQ(Value.Get(7), 5);

        Value = 9;
        EXPECT_EQ(*Value, 9);

        Value = NullOpt;
        EXPECT_FALSE(Value.IsSet());
    }

    TEST(OptionalBasics, ArrowReachesTheHeldValue)
    {
        TOptional<Lumina::FString> Text(Lumina::FString("hello"));
        EXPECT_EQ(Text->Len(), 5);
        EXPECT_TRUE(Text->StartsWith(Lumina::FStringView("hel")));
    }

    TEST(OptionalBasics, EmplaceAndReset)
    {
        TOptional<Lumina::FString> Text;
        Text.Emplace(4, 'x');
        EXPECT_EQ(*Text, Lumina::FString("xxxx"));

        Text.Reset();
        EXPECT_FALSE(Text.IsSet());

        Text.Emplace("rebuilt");
        EXPECT_EQ(*Text, Lumina::FString("rebuilt"));
    }

    TEST(OptionalBasics, InPlaceConstruction)
    {
        const TOptional<Lumina::FString> Text(InPlace, 3, 'z');
        EXPECT_EQ(*Text, Lumina::FString("zzz"));
    }

    TEST(OptionalLifetime, DestroysExactlyOnce)
    {
        FTracked::Live = 0;
        {
            TOptional<FTracked> Value(FTracked(1));
            EXPECT_EQ(FTracked::Live, 1);

            Value.Reset();
            EXPECT_EQ(FTracked::Live, 0);

            Value.Emplace(2);
            EXPECT_EQ(FTracked::Live, 1);
        }
        EXPECT_EQ(FTracked::Live, 0);
    }

    TEST(OptionalLifetime, CopyAndMoveAcrossEveryEngagementCombination)
    {
        FTracked::Live = 0;

        TOptional<FTracked> Engaged(FTracked(1));
        TOptional<FTracked> Disengaged;

        TOptional<FTracked> Copy = Engaged;
        EXPECT_TRUE(Copy.IsSet());
        EXPECT_EQ(Copy->Value, 1);
        EXPECT_TRUE(Engaged.IsSet());

        Copy = Disengaged;
        EXPECT_FALSE(Copy.IsSet());

        Copy = Engaged;
        EXPECT_TRUE(Copy.IsSet());

        TOptional<FTracked> Moved = std::move(Copy);
        EXPECT_TRUE(Moved.IsSet());
        EXPECT_FALSE(Copy.IsSet());

        Engaged.Reset();
        Disengaged.Reset();
        Moved.Reset();
        EXPECT_EQ(FTracked::Live, 0);
    }

    TEST(OptionalLifetime, SwapCoversBothEngagements)
    {
        TOptional<int> Left = 1;
        TOptional<int> Right;

        swap(Left, Right);
        EXPECT_FALSE(Left.IsSet());
        EXPECT_EQ(*Right, 1);

        TOptional<int> Other = 2;
        swap(Right, Other);
        EXPECT_EQ(*Right, 2);
        EXPECT_EQ(*Other, 1);
    }

    TEST(OptionalLayout, StaysTriviallyDestructibleForTrivialTypes)
    {
        static_assert(std::is_trivially_destructible_v<TOptional<int>>);
        static_assert(!std::is_trivially_destructible_v<TOptional<Lumina::FString>>);
        SUCCEED();
    }

    TEST(OptionalCompare, EqualityConsidersEngagement)
    {
        EXPECT_TRUE((TOptional<int>() == TOptional<int>()));
        EXPECT_FALSE((TOptional<int>(1) == TOptional<int>()));
        EXPECT_TRUE((TOptional<int>(1) == TOptional<int>(1)));
        EXPECT_FALSE((TOptional<int>(1) == TOptional<int>(2)));
        EXPECT_TRUE((TOptional<int>(1) == 1));
    }

    TEST(VariantBasics, DefaultsToTheFirstAlternative)
    {
        const TVariant<int, float, Lumina::FString> Value;
        EXPECT_EQ(Value.GetIndex(), 0u);
        EXPECT_TRUE(Value.IsType<int>());
        EXPECT_TRUE(Value.IsValid());
        EXPECT_EQ(Value.Get<int>(), 0);
    }

    TEST(VariantBasics, ConstructAndAssignFromAnAlternative)
    {
        TVariant<int, Lumina::FString> Value(Lumina::FString("text"));
        EXPECT_EQ(Value.GetIndex(), 1u);
        EXPECT_TRUE(Value.IsType<Lumina::FString>());
        EXPECT_EQ(Value.Get<Lumina::FString>(), Lumina::FString("text"));

        Value = 42;
        EXPECT_TRUE(Value.IsType<int>());
        EXPECT_EQ(Value.Get<int>(), 42);
    }

    TEST(VariantBasics, GetIfReportsTheHeldAlternative)
    {
        TVariant<int, float> Value(1.5f);
        EXPECT_EQ(Value.TryGet<int>(), nullptr);
        ASSERT_NE(Value.TryGet<float>(), nullptr);
        EXPECT_FLOAT_EQ(*Value.TryGet<float>(), 1.5f);

        EXPECT_TRUE(Lumina::Containers::HoldsAlternative<float>(Value));
        EXPECT_FALSE(Lumina::Containers::HoldsAlternative<int>(Value));
    }

    TEST(VariantBasics, EmplaceByTypeAndByIndex)
    {
        TVariant<int, Lumina::FString> Value;
        Value.Emplace<Lumina::FString>(3, 'q');
        EXPECT_EQ(Value.Get<Lumina::FString>(), Lumina::FString("qqq"));

        Value.Emplace<0>(11);
        EXPECT_EQ(Value.Get<int>(), 11);
    }

    TEST(VariantVisit, DispatchesOnTheHeldAlternative)
    {
        const auto Describe = [](const auto& Held) -> Lumina::FString
        {
            using FHeld = std::remove_cvref_t<decltype(Held)>;
            if constexpr (std::is_same_v<FHeld, int>)
            {
                return Lumina::Format("int:{}", Held);
            }
            else
            {
                return Lumina::Format("text:{}", Held);
            }
        };

        TVariant<int, Lumina::FString> Number(7);
        TVariant<int, Lumina::FString> Text(Lumina::FString("abc"));

        EXPECT_EQ(Lumina::Containers::Visit(Describe, Number), Lumina::FString("int:7"));
        EXPECT_EQ(Lumina::Containers::Visit(Describe, Text), Lumina::FString("text:abc"));
    }

    TEST(VariantVisit, VisitorCanMutateThroughAnLvalue)
    {
        TVariant<int, float> Value(3);
        Lumina::Containers::Visit([](auto& Held) { Held += 1; }, Value);
        EXPECT_EQ(Value.Get<int>(), 4);
    }

    TEST(VariantLifetime, DestroysTheHeldAlternativeExactlyOnce)
    {
        FTracked::Live = 0;
        {
            TVariant<int, FTracked> Value;
            EXPECT_EQ(FTracked::Live, 0);

            Value.Emplace<FTracked>(1);
            EXPECT_EQ(FTracked::Live, 1);

            Value.Emplace<FTracked>(2);
            EXPECT_EQ(FTracked::Live, 1);

            Value = 5;
            EXPECT_EQ(FTracked::Live, 0);

            Value.Emplace<FTracked>(3);
            EXPECT_EQ(FTracked::Live, 1);
        }
        EXPECT_EQ(FTracked::Live, 0);
    }

    TEST(VariantLifetime, CopyAndMoveCarryTheAlternative)
    {
        FTracked::Live = 0;

        TVariant<int, FTracked> Source;
        Source.Emplace<FTracked>(9);

        TVariant<int, FTracked> Copy = Source;
        EXPECT_EQ(FTracked::Live, 2);
        EXPECT_EQ(Copy.Get<FTracked>().Value, 9);
        EXPECT_EQ(Source.Get<FTracked>().Value, 9);

        TVariant<int, FTracked> Moved = std::move(Copy);
        EXPECT_EQ(Moved.Get<FTracked>().Value, 9);
        EXPECT_FALSE(Copy.IsValid());

        Source.Emplace<int>(0);
        Moved.Emplace<int>(0);
        EXPECT_EQ(FTracked::Live, 0);
    }

    TEST(VariantCompare, EqualityMatchesIndexThenValue)
    {
        const TVariant<int, float> A(1);
        const TVariant<int, float> B(1);
        const TVariant<int, float> C(2);
        const TVariant<int, float> D(1.0f);

        EXPECT_TRUE(A == B);
        EXPECT_FALSE(A == C);
        EXPECT_FALSE(A == D);
    }

    TEST(VariantLayout, StoresTheLargestAlternativeInline)
    {
        using FSmall = TVariant<char, short>;
        using FLarge = TVariant<char, Lumina::FString>;

        static_assert(sizeof(FSmall) < sizeof(FLarge));
        EXPECT_GE(sizeof(FLarge), sizeof(Lumina::FString));
    }

    TEST(VariantSwap, ExchangesAlternatives)
    {
        TVariant<int, Lumina::FString> Left(1);
        TVariant<int, Lumina::FString> Right(Lumina::FString("right"));

        swap(Left, Right);
        EXPECT_TRUE(Left.IsType<Lumina::FString>());
        EXPECT_EQ(Left.Get<Lumina::FString>(), Lumina::FString("right"));
        EXPECT_TRUE(Right.IsType<int>());
        EXPECT_EQ(Right.Get<int>(), 1);
    }
}
