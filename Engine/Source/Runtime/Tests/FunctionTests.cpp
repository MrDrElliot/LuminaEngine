#include <gtest/gtest.h>

#include <type_traits>

#include "Containers/Function.h"
#include "Containers/FunctionRef.h"
#include "Containers/Invoke.h"
#include "Containers/String.h"
#include "Containers/Vector.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaFunctionTests
{
    using Lumina::TFunction;
    using Lumina::TCopyableFunction;
    using Lumina::TMoveOnlyFunction;
    using Lumina::TFunctionRef;
    using Lumina::TInPlaceType;
    using Lumina::FString;
    using Lumina::TVector;
    using Lumina::int32;

    int32 FreeAdd(int32 A, int32 B)
    {
        return A + B;
    }

    struct FCounter
    {
        static inline int32 Constructions = 0;
        static inline int32 Destructions = 0;
        static inline int32 Copies = 0;
        static inline int32 Moves = 0;

        static void Reset()
        {
            Constructions = 0;
            Destructions = 0;
            Copies = 0;
            Moves = 0;
        }

        int32 Payload = 0;

        explicit FCounter(int32 InPayload) : Payload(InPayload) { ++Constructions; }
        FCounter(const FCounter& Other) : Payload(Other.Payload) { ++Copies; }
        FCounter(FCounter&& Other) noexcept : Payload(Other.Payload) { ++Moves; }
        ~FCounter() { ++Destructions; }

        int32 operator()() const { return Payload; }
    };

    struct FSubject
    {
        int32 Value = 7;

        int32 Doubled() const { return Value * 2; }
        int32 Scale(int32 By) const { return Value * By; }
    };

    struct FMoveOnlyPayload
    {
        int32 Value = 0;

        explicit FMoveOnlyPayload(int32 InValue) : Value(InValue) {}
        FMoveOnlyPayload(const FMoveOnlyPayload&) = delete;
        FMoveOnlyPayload& operator=(const FMoveOnlyPayload&) = delete;
        FMoveOnlyPayload(FMoveOnlyPayload&&) noexcept = default;
        FMoveOnlyPayload& operator=(FMoveOnlyPayload&&) noexcept = default;
    };

    TEST(Invoke, CallsFunctorsFunctionPointersAndMemberPointers)
    {
        const FSubject Subject;

        EXPECT_EQ(Lumina::Invoke(FreeAdd, 2, 3), 5);
        EXPECT_EQ(Lumina::Invoke([](int32 A) { return A * 3; }, 4), 12);
        EXPECT_EQ(Lumina::Invoke(&FSubject::Doubled, Subject), 14);
        EXPECT_EQ(Lumina::Invoke(&FSubject::Doubled, &Subject), 14);
        EXPECT_EQ(Lumina::Invoke(&FSubject::Scale, Subject, 3), 21);
        EXPECT_EQ(Lumina::Invoke(&FSubject::Value, Subject), 7);
    }

    TEST(Invoke, DiscardsTheResultWhenTheRequestedReturnIsVoid)
    {
        int32 Calls = 0;
        Lumina::InvokeR<void>([&Calls] { ++Calls; return 42; });
        EXPECT_EQ(Calls, 1);
    }

    TEST(Function, DefaultConstructsEmpty)
    {
        TFunction<int32()> Function;

        EXPECT_FALSE(Function.IsSet());
        EXPECT_FALSE(static_cast<bool>(Function));
        EXPECT_TRUE(Function == nullptr);
    }

    TEST(Function, HoldsAndCallsALambda)
    {
        TFunction<int32(int32)> Function = [](int32 Value) { return Value + 1; };

        EXPECT_TRUE(Function.IsSet());
        EXPECT_EQ(Function(41), 42);
    }

    TEST(Function, CallsThroughAConstReference)
    {
        int32 Observed = 0;
        const TFunction<void(int32)> Function = [&Observed](int32 Value) { Observed = Value; };

        Function(9);
        EXPECT_EQ(Observed, 9);
    }

    TEST(Function, HoldsAFunctionPointerAndAMemberPointer)
    {
        const TFunction<int32(int32, int32)> Add = &FreeAdd;
        const TFunction<int32(const FSubject&)> Doubled = &FSubject::Doubled;
        const FSubject Subject;

        EXPECT_EQ(Add(20, 22), 42);
        EXPECT_EQ(Doubled(Subject), 14);
    }

    TEST(Function, ANullFunctionPointerProducesAnEmptyFunction)
    {
        int32 (*NullPointer)(int32, int32) = nullptr;
        const TFunction<int32(int32, int32)> Function = NullPointer;

        EXPECT_FALSE(Function.IsSet());
    }

    TEST(Function, DiscardsAReturnValueTheSignatureDoesNotWant)
    {
        int32 Calls = 0;
        const TFunction<void()> Function = [&Calls] { ++Calls; return 1; };

        Function();
        EXPECT_EQ(Calls, 1);
    }

    TEST(Function, SmallTargetsStayInline)
    {
        void* Pointers[4] = {};
        const TFunction<void()> Small = [Pointers] { (void)Pointers; };
        const TFunction<void()> Large = [Pointers, Extra = int64(0)] { (void)Pointers; (void)Extra; };

        EXPECT_TRUE(Small.IsInline());
        EXPECT_FALSE(Large.IsInline());
    }

    TEST(Function, ATargetTooBigForTheBufferStillWorks)
    {
        int64 Fill[16] = {};
        Fill[15] = 5;

        const TFunction<int64()> Function = [Fill] { return Fill[15]; };

        EXPECT_FALSE(Function.IsInline());
        EXPECT_EQ(Function(), 5);
    }

    TEST(Function, CopyingDuplicatesTheTarget)
    {
        FCounter::Reset();
        {
            const TFunction<int32()> Original = FCounter(3);
            const TFunction<int32()> Copy = Original;

            EXPECT_EQ(Original(), 3);
            EXPECT_EQ(Copy(), 3);
            EXPECT_GE(FCounter::Copies, 1);
        }

        EXPECT_EQ(FCounter::Constructions + FCounter::Copies + FCounter::Moves, FCounter::Destructions);
    }

    TEST(Function, MovingLeavesTheSourceEmpty)
    {
        TFunction<int32()> Original = [] { return 11; };
        const TFunction<int32()> Moved = std::move(Original);

        EXPECT_FALSE(Original.IsSet());
        EXPECT_EQ(Moved(), 11);
    }

    TEST(Function, DestroysItsTargetExactlyOnce)
    {
        FCounter::Reset();
        {
            TFunction<int32()> Function = FCounter(1);
            Function.Reset();
            EXPECT_FALSE(Function.IsSet());
        }

        EXPECT_EQ(FCounter::Constructions + FCounter::Copies + FCounter::Moves, FCounter::Destructions);
    }

    TEST(Function, AssignmentReplacesTheTarget)
    {
        TFunction<int32()> Function = [] { return 1; };
        Function = [] { return 2; };
        EXPECT_EQ(Function(), 2);

        Function = nullptr;
        EXPECT_FALSE(Function.IsSet());
    }

    TEST(Function, SelfAssignmentKeepsTheTarget)
    {
        TFunction<int32()> Function = [] { return 4; };
        const TFunction<int32()>& Alias = Function;

        Function = Alias;
        EXPECT_EQ(Function(), 4);
    }

    TEST(Function, SwapExchangesTargets)
    {
        TFunction<int32()> Left = [] { return 1; };
        TFunction<int32()> Right = [] { return 2; };

        Left.Swap(Right);

        EXPECT_EQ(Left(), 2);
        EXPECT_EQ(Right(), 1);
    }

    TEST(Function, SwapWorksWhenOneSideIsEmpty)
    {
        TFunction<int32()> Held = [] { return 8; };
        TFunction<int32()> Empty;

        Held.Swap(Empty);

        EXPECT_FALSE(Held.IsSet());
        EXPECT_EQ(Empty(), 8);
    }

    TEST(Function, ConstructsATargetInPlace)
    {
        const TFunction<int32()> Function(TInPlaceType<FCounter>{}, 6);
        EXPECT_EQ(Function(), 6);
    }

    TEST(Function, CarriesANonTriviallyRelocatableCapture)
    {
        TFunction<FString()> Function = [Text = FString("captured")] { return Text; };
        const TFunction<FString()> Moved = std::move(Function);

        EXPECT_EQ(Moved(), FString("captured"));
    }

    TEST(Function, SurvivesVectorGrowth)
    {
        TVector<TFunction<int32()>> Functions;

        for (int32 Index = 0; Index < 64; ++Index)
        {
            Functions.push_back([Index, Text = FString("padding")] { return Index + static_cast<int32>(Text.size()); });
        }

        for (int32 Index = 0; Index < 64; ++Index)
        {
            EXPECT_EQ(Functions[Index](), Index + 7);
        }
    }

    TEST(Function, WrapsAnotherFunction)
    {
        const TFunction<int32()> Inner = [] { return 12; };
        const TFunction<int32()> Outer = Inner;

        EXPECT_EQ(Outer(), 12);
    }

    TEST(MoveOnlyFunction, HoldsAMoveOnlyCapture)
    {
        TMoveOnlyFunction<int32()> Function =
            [Payload = FMoveOnlyPayload(21)]() mutable { return Payload.Value * 2; };

        EXPECT_EQ(Function(), 42);

        TMoveOnlyFunction<int32()> Moved = std::move(Function);
        EXPECT_FALSE(Function.IsSet());
        EXPECT_EQ(Moved(), 42);
    }

    TEST(MoveOnlyFunction, IsNotCopyable)
    {
        EXPECT_FALSE(std::is_copy_constructible_v<TMoveOnlyFunction<void()>>);
        EXPECT_FALSE(std::is_copy_assignable_v<TMoveOnlyFunction<void()>>);
        EXPECT_TRUE(std::is_move_constructible_v<TMoveOnlyFunction<void()>>);
        EXPECT_TRUE(std::is_move_assignable_v<TMoveOnlyFunction<void()>>);
    }

    TEST(MoveOnlyFunction, SurvivesVectorGrowth)
    {
        TVector<TMoveOnlyFunction<int32()>> Functions;

        for (int32 Index = 0; Index < 32; ++Index)
        {
            Functions.push_back([Payload = FMoveOnlyPayload(Index)] { return Payload.Value; });
        }

        for (int32 Index = 0; Index < 32; ++Index)
        {
            EXPECT_EQ(Functions[Index](), Index);
        }
    }

    TEST(CopyableFunction, IsTheSameTypeAsTFunction)
    {
        EXPECT_TRUE((std::is_same_v<TFunction<void()>, TCopyableFunction<void()>>));
        EXPECT_TRUE(std::is_copy_constructible_v<TCopyableFunction<void()>>);
    }

    TEST(CopyableFunction, RejectsATargetItCannotCopy)
    {
        const auto MoveOnlyLambda = [](FMoveOnlyPayload Payload) { return Payload.Value; };
        (void)MoveOnlyLambda;

        EXPECT_FALSE((std::is_constructible_v<TCopyableFunction<int32()>,
                                              decltype([Payload = FMoveOnlyPayload(1)] { return Payload.Value; })>));
        EXPECT_TRUE((std::is_constructible_v<TMoveOnlyFunction<int32()>,
                                             decltype([Payload = FMoveOnlyPayload(1)] { return Payload.Value; })>));
    }

    TEST(FunctionRef, ViewsALambdaWithoutOwningIt)
    {
        int32 Observed = 0;
        auto Callable = [&Observed](int32 Value) { Observed = Value; };

        const TFunctionRef<void(int32)> Ref = Callable;
        Ref(5);

        EXPECT_EQ(Observed, 5);
        EXPECT_TRUE(Ref.IsSet());
    }

    TEST(FunctionRef, DefaultsToEmpty)
    {
        const TFunctionRef<void()> Ref;

        EXPECT_FALSE(Ref.IsSet());
        EXPECT_TRUE(Ref == nullptr);
    }

    TEST(FunctionRef, CarriesAMemberPointer)
    {
        const FSubject Subject;
        auto Member = &FSubject::Doubled;
        const TFunctionRef<int32(const FSubject&)> Ref = Member;

        EXPECT_EQ(Ref(Subject), 14);
    }

    TEST(FunctionRef, IsTriviallyCopyableAndPointerSized)
    {
        EXPECT_TRUE(std::is_trivially_copyable_v<TFunctionRef<void()>>);
        EXPECT_EQ(sizeof(TFunctionRef<void()>), 2 * sizeof(void*));
    }

    TEST(FunctionRef, ObservesMutationsThroughTheViewedCallable)
    {
        int32 Calls = 0;
        auto Callable = [&Calls] { ++Calls; };

        const TFunctionRef<void()> Ref = Callable;
        Ref();
        Ref();

        EXPECT_EQ(Calls, 2);
    }

    void CountThroughRef(TFunctionRef<void(int32)> Sink)
    {
        for (int32 Index = 0; Index < 3; ++Index)
        {
            Sink(Index);
        }
    }

    TEST(FunctionRef, BindsToATemporaryLambdaAtTheCallSite)
    {
        int32 Sum = 0;
        CountThroughRef([&Sum](int32 Value) { Sum += Value; });

        EXPECT_EQ(Sum, 3);
    }

    TEST(FunctionLayout, StaysWithinFivePointers)
    {
        EXPECT_LE(sizeof(TFunction<void()>), 5 * sizeof(void*));
        EXPECT_EQ(sizeof(TFunction<void()>), sizeof(TMoveOnlyFunction<void()>));
    }
}
