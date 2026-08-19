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
        static_assert(Literal.first == 1);
        static_assert(Literal.second == 2);

        TPair<int, Lumina::FString> Mixed(7, Lumina::FString("seven"));
        EXPECT_EQ(Mixed.first, 7);
        EXPECT_EQ(Mixed.second, Lumina::FString("seven"));

        TPair<int, int> Left(1, 2);
        TPair<int, int> Right(3, 4);
        Left.swap(Right);
        EXPECT_EQ(Left.first, 3);
        EXPECT_EQ(Right.first, 1);

        EXPECT_TRUE((TPair<int, int>(1, 2) == TPair<int, int>(1, 2)));
        EXPECT_LT((TPair<int, int>(1, 2)), (TPair<int, int>(1, 3)));
        EXPECT_LT((TPair<int, int>(1, 9)), (TPair<int, int>(2, 0)));
    }

    TEST(PairBasics, MakePairDeducesDecayedTypes)
    {
        const auto Made = MakePair(1, Lumina::FString("value"));
        static_assert(std::is_same_v<decltype(Made), const TPair<int, Lumina::FString>>);
        EXPECT_EQ(Made.first, 1);
    }

    TEST(PairBasics, ConvertsToAndFromTheStandardPair)
    {
        const std::pair<int, int> Standard(3, 4);
        const TPair<int, int> Ours(Standard);
        EXPECT_EQ(Ours.first, 3);

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
        EXPECT_EQ(Value.first, 6);
    }

    TEST(PairBasics, PiecewiseConstructionBuildsInPlace)
    {
        const TPair<Lumina::FString, Lumina::FString> Built(
            std::piecewise_construct,
            std::forward_as_tuple("abc", 3),
            std::forward_as_tuple(4, 'x'));

        EXPECT_EQ(Built.first, Lumina::FString("abc"));
        EXPECT_EQ(Built.second, Lumina::FString("xxxx"));
    }

    TEST(PairLifetime, DestroysBothMembers)
    {
        FTracked::Live = 0;
        {
            TPair<FTracked, FTracked> Value(FTracked(1), FTracked(2));
            EXPECT_EQ(Value.first.Value, 1);
            EXPECT_EQ(FTracked::Live, 2);
        }
        EXPECT_EQ(FTracked::Live, 0);
    }

    TEST(SpanBasics, EmptyAndFromPointerAndSize)
    {
        constexpr TSpan<const int> Empty;
        static_assert(Empty.empty());
        static_assert(Empty.size() == 0);
        EXPECT_EQ(Empty.data(), nullptr);

        int Values[] = { 1, 2, 3, 4 };
        const TSpan<int> Span(Values, 4);
        EXPECT_EQ(Span.size(), 4u);
        EXPECT_EQ(Span.size_bytes(), 4u * sizeof(int));
        EXPECT_EQ(Span.front(), 1);
        EXPECT_EQ(Span.back(), 4);
        EXPECT_EQ(Span[2], 3);
    }

    TEST(SpanBasics, DeducesFromArraysAndContainers)
    {
        int RawArray[] = { 1, 2, 3 };
        const TSpan<int> FromArray(RawArray);
        EXPECT_EQ(FromArray.size(), 3u);

        std::vector<int> Vector{ 4, 5, 6, 7 };
        const TSpan<int> FromVector(Vector);
        EXPECT_EQ(FromVector.size(), 4u);
        EXPECT_EQ(FromVector[0], 4);

        Lumina::Containers::TVector<int> Ours;
        Ours.push_back(8);
        Ours.push_back(9);
        const TSpan<int> FromOurs(Ours);
        EXPECT_EQ(FromOurs.size(), 2u);
        EXPECT_EQ(FromOurs.back(), 9);
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
        EXPECT_EQ(Constant.size(), 3u);

        const std::span<int> Standard = Mutable;
        EXPECT_EQ(Standard.size(), 3u);

        const TSpan<int> Back(Standard);
        EXPECT_EQ(Back[1], 2);
    }

    TEST(SpanSlicing, FirstLastAndSubspan)
    {
        int Values[] = { 0, 1, 2, 3, 4, 5 };
        const TSpan<int> Span(Values);

        EXPECT_EQ(Span.first(2).size(), 2u);
        EXPECT_EQ(Span.first(2).back(), 1);
        EXPECT_EQ(Span.last(2).front(), 4);
        EXPECT_EQ(Span.subspan(2).size(), 4u);
        EXPECT_EQ(Span.subspan(2, 2).back(), 3);
        EXPECT_EQ(Span.subspan(2, 100).size(), 4u);
        EXPECT_TRUE(Span.subspan(6).empty());
    }

    TEST(SpanSearch, ContainsAndIndexOf)
    {
        int Values[] = { 10, 20, 30 };
        const TSpan<int> Span(Values);

        EXPECT_TRUE(Span.Contains(20));
        EXPECT_FALSE(Span.Contains(40));
        EXPECT_EQ(Span.IndexOf(30), 2u);
        EXPECT_EQ(Span.IndexOf(40), TSpan<int>::npos);
    }

    TEST(OptionalBasics, DisengagedByDefault)
    {
        const TOptional<int> Value;
        EXPECT_FALSE(Value.has_value());
        EXPECT_FALSE(Value.IsSet());
        EXPECT_FALSE(static_cast<bool>(Value));
        EXPECT_EQ(Value.value_or(7), 7);
        EXPECT_TRUE(Value == NullOpt);
    }

    TEST(OptionalBasics, EngagedFromAValue)
    {
        TOptional<int> Value = 5;
        EXPECT_TRUE(Value.has_value());
        EXPECT_EQ(Value.value(), 5);
        EXPECT_EQ(*Value, 5);
        EXPECT_EQ(Value.value_or(7), 5);

        Value = 9;
        EXPECT_EQ(*Value, 9);

        Value = NullOpt;
        EXPECT_FALSE(Value.has_value());
    }

    TEST(OptionalBasics, ArrowReachesTheHeldValue)
    {
        TOptional<Lumina::FString> Text(Lumina::FString("hello"));
        EXPECT_EQ(Text->size(), 5u);
        EXPECT_TRUE(Text->starts_with(Lumina::FStringView("hel")));
    }

    TEST(OptionalBasics, EmplaceAndReset)
    {
        TOptional<Lumina::FString> Text;
        Text.emplace(4, 'x');
        EXPECT_EQ(*Text, Lumina::FString("xxxx"));

        Text.reset();
        EXPECT_FALSE(Text.has_value());

        Text.emplace("rebuilt");
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

            Value.reset();
            EXPECT_EQ(FTracked::Live, 0);

            Value.emplace(2);
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
        EXPECT_TRUE(Copy.has_value());
        EXPECT_EQ(Copy->Value, 1);
        EXPECT_TRUE(Engaged.has_value());

        Copy = Disengaged;
        EXPECT_FALSE(Copy.has_value());

        Copy = Engaged;
        EXPECT_TRUE(Copy.has_value());

        TOptional<FTracked> Moved = std::move(Copy);
        EXPECT_TRUE(Moved.has_value());
        EXPECT_FALSE(Copy.has_value());

        Engaged.reset();
        Disengaged.reset();
        Moved.reset();
        EXPECT_EQ(FTracked::Live, 0);
    }

    TEST(OptionalLifetime, SwapCoversBothEngagements)
    {
        TOptional<int> Left = 1;
        TOptional<int> Right;

        Left.swap(Right);
        EXPECT_FALSE(Left.has_value());
        EXPECT_EQ(*Right, 1);

        TOptional<int> Other = 2;
        Right.swap(Other);
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
        EXPECT_TRUE(Value.Is<int>());
        EXPECT_TRUE(Value.IsValid());
        EXPECT_EQ(Value.Get<int>(), 0);
    }

    TEST(VariantBasics, ConstructAndAssignFromAnAlternative)
    {
        TVariant<int, Lumina::FString> Value(Lumina::FString("text"));
        EXPECT_EQ(Value.GetIndex(), 1u);
        EXPECT_TRUE(Value.Is<Lumina::FString>());
        EXPECT_EQ(Value.Get<Lumina::FString>(), Lumina::FString("text"));

        Value = 42;
        EXPECT_TRUE(Value.Is<int>());
        EXPECT_EQ(Value.Get<int>(), 42);
    }

    TEST(VariantBasics, GetIfReportsTheHeldAlternative)
    {
        TVariant<int, float> Value(1.5f);
        EXPECT_EQ(Value.GetIf<int>(), nullptr);
        ASSERT_NE(Value.GetIf<float>(), nullptr);
        EXPECT_FLOAT_EQ(*Value.GetIf<float>(), 1.5f);

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

        Left.swap(Right);
        EXPECT_TRUE(Left.Is<Lumina::FString>());
        EXPECT_EQ(Left.Get<Lumina::FString>(), Lumina::FString("right"));
        EXPECT_TRUE(Right.Is<int>());
        EXPECT_EQ(Right.Get<int>(), 1);
    }
}
