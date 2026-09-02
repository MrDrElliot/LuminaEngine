#pragma once

#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"
#include "Memory/Construct.h"
#include "Invoke.h"
#include "Lumina.h"

namespace Lumina::Algo
{
    struct FLess
    {
        template <typename TLeft, typename TRight>
        NODISCARD constexpr bool operator()(const TLeft& Left, const TRight& Right) const
        {
            return Left < Right;
        }
    };

    /** Projection that leaves the element alone, so a call with no projection costs nothing. */
    struct FIdentity
    {
        template <typename T>
        NODISCARD constexpr T&& operator()(T&& Value) const
        {
            return static_cast<T&&>(Value);
        }
    };

    namespace Private
    {
        /** Excludes iterators, which is what keeps two-argument calls off the iterator-pair overloads. */
        template <typename T>
        concept Range = requires(T& Container)
        {
            std::begin(Container);
            std::end(Container);
        };

        /** Free begin and end so a C array of static table entries is a range like any container. */
        template <typename TRange>
        NODISCARD constexpr decltype(auto) Begin(TRange&& Range)
        {
            return std::begin(Range);
        }

        template <typename TRange>
        NODISCARD constexpr decltype(auto) End(TRange&& Range)
        {
            return std::end(Range);
        }

        /** Routes through Invoke so &FThing::Field and &FThing::Method work wherever a callable does. */
        template <typename TProj, typename TElem>
        NODISCARD constexpr decltype(auto) Project(TProj& Proj, TElem&& Elem)
        {
            return Containers::Invoke(Proj, static_cast<TElem&&>(Elem));
        }

        /** Wraps a projection into the unary predicate the iterator overloads already take. */
        template <typename TProj, typename T>
        NODISCARD constexpr auto EqualsBy(TProj& Proj, const T& Value)
        {
            return [&Proj, &Value](auto&& Elem) -> bool { return Project(Proj, Elem) == Value; };
        }

        /** Wraps a caller's predicate so a member pointer is accepted in its place. */
        template <typename TPred>
        NODISCARD constexpr auto AsPredicate(TPred& Pred)
        {
            return [&Pred](auto&& Elem) -> bool { return static_cast<bool>(Project(Pred, Elem)); };
        }

        template <typename TIt>
        using TValue = typename std::iterator_traits<TIt>::value_type;

        template <typename TIt>
        using TDiff = typename std::iterator_traits<TIt>::difference_type;

        template <typename TIt>
        constexpr void IterSwap(TIt Left, TIt Right)
        {
            TValue<TIt> Temp = std::move(*Left);
            *Left = std::move(*Right);
            *Right = std::move(Temp);
        }

        template <bool bConstruct, typename TDest, typename TSource>
        FORCEINLINE void PlaceMove(TDest Dest, TSource&& Source)
        {
            if constexpr (bConstruct)
            {
                Memory::ConstructAt(&*Dest, std::move(Source));
            }
            else
            {
                *Dest = std::move(Source);
            }
        }
    }

    template <typename TIt, typename T>
    NODISCARD constexpr TIt Find(TIt First, TIt Last, const T& Value)
    {
        for (; First != Last; ++First)
        {
            if (*First == Value)
            {
                break;
            }
        }

        return First;
    }

    template <typename TIt, typename TPred>
    NODISCARD constexpr TIt FindIf(TIt First, TIt Last, TPred Pred)
    {
        for (; First != Last; ++First)
        {
            if (Pred(*First))
            {
                break;
            }
        }

        return First;
    }

    template <typename TIt, typename TPred>
    NODISCARD constexpr TIt FindIfNot(TIt First, TIt Last, TPred Pred)
    {
        for (; First != Last; ++First)
        {
            if (!Pred(*First))
            {
                break;
            }
        }

        return First;
    }

    template <typename TIt, typename T>
    NODISCARD constexpr bool Contains(TIt First, TIt Last, const T& Value)
    {
        return Find(First, Last, Value) != Last;
    }

    template <typename TIt, typename T>
    NODISCARD constexpr Private::TDiff<TIt> Count(TIt First, TIt Last, const T& Value)
    {
        Private::TDiff<TIt> Total = 0;
        for (; First != Last; ++First)
        {
            Total += (*First == Value) ? 1 : 0;
        }

        return Total;
    }

    template <typename TIt, typename TPred>
    NODISCARD constexpr Private::TDiff<TIt> CountIf(TIt First, TIt Last, TPred Pred)
    {
        Private::TDiff<TIt> Total = 0;
        for (; First != Last; ++First)
        {
            Total += Pred(*First) ? 1 : 0;
        }

        return Total;
    }

    template <typename TIt, typename TPred>
    NODISCARD constexpr bool AllOf(TIt First, TIt Last, TPred Pred)
    {
        return FindIfNot(First, Last, Pred) == Last;
    }

    template <typename TIt, typename TPred>
    NODISCARD constexpr bool AnyOf(TIt First, TIt Last, TPred Pred)
    {
        return FindIf(First, Last, Pred) != Last;
    }

    template <typename TIt, typename TPred>
    NODISCARD constexpr bool NoneOf(TIt First, TIt Last, TPred Pred)
    {
        return FindIf(First, Last, Pred) == Last;
    }

    template <typename TIt, typename TFunc>
    constexpr TFunc ForEach(TIt First, TIt Last, TFunc Func)
    {
        for (; First != Last; ++First)
        {
            Func(*First);
        }

        return Func;
    }

    template <typename TIt1, typename TIt2>
    NODISCARD constexpr bool Equal(TIt1 First1, TIt1 Last1, TIt2 First2)
    {
        for (; First1 != Last1; ++First1, ++First2)
        {
            if (!(*First1 == *First2))
            {
                return false;
            }
        }

        return true;
    }

    template <typename TIt, typename T>
    constexpr void Fill(TIt First, TIt Last, const T& Value)
    {
        for (; First != Last; ++First)
        {
            *First = Value;
        }
    }

    template <typename TIt, typename T>
    constexpr void Iota(TIt First, TIt Last, T Value)
    {
        for (; First != Last; ++First, ++Value)
        {
            *First = Value;
        }
    }

    template <typename TIt, typename TOut>
    constexpr TOut Copy(TIt First, TIt Last, TOut Out)
    {
        for (; First != Last; ++First, ++Out)
        {
            *Out = *First;
        }

        return Out;
    }

    template <typename TIt, typename TOut, typename TPred>
    constexpr TOut CopyIf(TIt First, TIt Last, TOut Out, TPred Pred)
    {
        for (; First != Last; ++First)
        {
            if (Pred(*First))
            {
                *Out = *First;
                ++Out;
            }
        }

        return Out;
    }

    template <typename TIt, typename TOut, typename TFunc>
    constexpr TOut Transform(TIt First, TIt Last, TOut Out, TFunc Func)
    {
        for (; First != Last; ++First, ++Out)
        {
            *Out = Func(*First);
        }

        return Out;
    }

    template <typename TIt, typename T>
    constexpr void Replace(TIt First, TIt Last, const T& Old, const T& New)
    {
        for (; First != Last; ++First)
        {
            if (*First == Old)
            {
                *First = New;
            }
        }
    }

    template <typename TIt, typename TPred, typename T>
    constexpr void ReplaceIf(TIt First, TIt Last, TPred Pred, const T& New)
    {
        for (; First != Last; ++First)
        {
            if (Pred(*First))
            {
                *First = New;
            }
        }
    }

    template <typename TIt>
    constexpr void Reverse(TIt First, TIt Last)
    {
        while (First != Last && First != --Last)
        {
            Private::IterSwap(First, Last);
            ++First;
        }
    }

    /** Rotates so Middle becomes the first element; returns where the old first element ended up. */
    template <typename TIt>
    constexpr TIt Rotate(TIt First, TIt Middle, TIt Last)
    {
        if (First == Middle)
        {
            return Last;
        }

        if (Middle == Last)
        {
            return First;
        }

        Reverse(First, Middle);
        Reverse(Middle, Last);
        Reverse(First, Last);
        return First + (Last - Middle);
    }

    /** Shifts every element that is not Value forward; returns the new logical end. */
    template <typename TIt, typename T>
    constexpr TIt Remove(TIt First, TIt Last, const T& Value)
    {
        TIt Out = Find(First, Last, Value);
        if (Out == Last)
        {
            return Last;
        }

        for (TIt It = Out; ++It != Last; )
        {
            if (!(*It == Value))
            {
                *Out = std::move(*It);
                ++Out;
            }
        }

        return Out;
    }

    template <typename TIt, typename TPred>
    constexpr TIt RemoveIf(TIt First, TIt Last, TPred Pred)
    {
        TIt Out = FindIf(First, Last, Pred);
        if (Out == Last)
        {
            return Last;
        }

        for (TIt It = Out; ++It != Last; )
        {
            if (!Pred(*It))
            {
                *Out = std::move(*It);
                ++Out;
            }
        }

        return Out;
    }

    /** Collapses runs of adjacent equal elements; returns the new logical end. */
    template <typename TIt, typename TEqual>
    constexpr TIt Unique(TIt First, TIt Last, TEqual AreEqual)
    {
        if (First == Last)
        {
            return Last;
        }

        TIt Out = First;
        while (++First != Last)
        {
            if (!AreEqual(*Out, *First) && ++Out != First)
            {
                *Out = std::move(*First);
            }
        }

        return ++Out;
    }

    template <typename TIt>
    constexpr TIt Unique(TIt First, TIt Last)
    {
        return Unique(First, Last, [](const auto& Left, const auto& Right) { return Left == Right; });
    }

    template <typename TIt, typename TPred = FLess>
    NODISCARD constexpr TIt MinElement(TIt First, TIt Last, TPred Pred = {})
    {
        if (First == Last)
        {
            return Last;
        }

        TIt Best = First;
        while (++First != Last)
        {
            if (Pred(*First, *Best))
            {
                Best = First;
            }
        }

        return Best;
    }

    template <typename TIt, typename TPred = FLess>
    NODISCARD constexpr TIt MaxElement(TIt First, TIt Last, TPred Pred = {})
    {
        if (First == Last)
        {
            return Last;
        }

        TIt Best = First;
        while (++First != Last)
        {
            if (Pred(*Best, *First))
            {
                Best = First;
            }
        }

        return Best;
    }

    template <typename TIt, typename T, typename TPred = FLess>
    NODISCARD constexpr TIt LowerBound(TIt First, TIt Last, const T& Value, TPred Pred = {})
    {
        Private::TDiff<TIt> Remaining = Last - First;
        while (Remaining > 0)
        {
            const Private::TDiff<TIt> Half = Remaining / 2;
            TIt Middle = First + Half;
            if (Pred(*Middle, Value))
            {
                First = ++Middle;
                Remaining -= Half + 1;
            }
            else
            {
                Remaining = Half;
            }
        }

        return First;
    }

    template <typename TIt, typename T, typename TPred = FLess>
    NODISCARD constexpr TIt UpperBound(TIt First, TIt Last, const T& Value, TPred Pred = {})
    {
        Private::TDiff<TIt> Remaining = Last - First;
        while (Remaining > 0)
        {
            const Private::TDiff<TIt> Half = Remaining / 2;
            TIt Middle = First + Half;
            if (Pred(Value, *Middle))
            {
                Remaining = Half;
            }
            else
            {
                First = ++Middle;
                Remaining -= Half + 1;
            }
        }

        return First;
    }

    template <typename TIt, typename T, typename TPred = FLess>
    NODISCARD constexpr bool BinarySearch(TIt First, TIt Last, const T& Value, TPred Pred = {})
    {
        TIt Found = LowerBound(First, Last, Value, Pred);
        return Found != Last && !Pred(Value, *Found);
    }

    template <typename TIt, typename TPred = FLess>
    NODISCARD constexpr bool IsSorted(TIt First, TIt Last, TPred Pred = {})
    {
        if (First == Last)
        {
            return true;
        }

        for (TIt Next = First; ++Next != Last; First = Next)
        {
            if (Pred(*Next, *First))
            {
                return false;
            }
        }

        return true;
    }

    namespace Private
    {
        inline constexpr int64 kInsertionSortThreshold = 24;

        template <typename TIt, typename TPred>
        constexpr void InsertionSort(TIt First, TIt Last, TPred& Pred)
        {
            if (First == Last)
            {
                return;
            }

            for (TIt It = First + 1; It != Last; ++It)
            {
                TValue<TIt> Key = std::move(*It);
                TIt Hole = It;
                while (Hole != First && Pred(Key, *(Hole - 1)))
                {
                    *Hole = std::move(*(Hole - 1));
                    --Hole;
                }

                *Hole = std::move(Key);
            }
        }

        template <typename TIt, typename TPred>
        constexpr void SiftDown(TIt First, TDiff<TIt> Root, TDiff<TIt> Count, TPred& Pred)
        {
            TValue<TIt> Value = std::move(First[Root]);
            for (TDiff<TIt> Child = 2 * Root + 1; Child < Count; Child = 2 * Root + 1)
            {
                if (Child + 1 < Count && Pred(First[Child], First[Child + 1]))
                {
                    ++Child;
                }

                if (!Pred(Value, First[Child]))
                {
                    break;
                }

                First[Root] = std::move(First[Child]);
                Root = Child;
            }

            First[Root] = std::move(Value);
        }

        template <typename TIt, typename TPred>
        constexpr void HeapSort(TIt First, TIt Last, TPred& Pred)
        {
            const TDiff<TIt> Count = Last - First;
            if (Count < 2)
            {
                return;
            }

            for (TDiff<TIt> Root = Count / 2 - 1; Root >= 0; --Root)
            {
                SiftDown(First, Root, Count, Pred);
            }

            for (TDiff<TIt> End = Count - 1; End > 0; --End)
            {
                IterSwap(First, First + End);
                SiftDown(First, 0, End, Pred);
            }
        }

        // Puts the median of the three candidates in Result, which gives the partition its sentinels.
        template <typename TIt, typename TPred>
        constexpr void MoveMedianToFirst(TIt Result, TIt A, TIt B, TIt C, TPred& Pred)
        {
            if (Pred(*A, *B))
            {
                if (Pred(*B, *C))      { IterSwap(Result, B); }
                else if (Pred(*A, *C)) { IterSwap(Result, C); }
                else                   { IterSwap(Result, A); }
            }
            else if (Pred(*A, *C))     { IterSwap(Result, A); }
            else if (Pred(*B, *C))     { IterSwap(Result, C); }
            else                       { IterSwap(Result, B); }
        }

        template <typename TIt, typename TPred>
        constexpr TIt UnguardedPartition(TIt First, TIt Last, TIt Pivot, TPred& Pred)
        {
            while (true)
            {
                while (Pred(*First, *Pivot))
                {
                    ++First;
                }

                --Last;
                while (Pred(*Pivot, *Last))
                {
                    --Last;
                }

                if (!(First < Last))
                {
                    return First;
                }

                IterSwap(First, Last);
                ++First;
            }
        }

        template <typename TIt, typename TPred>
        constexpr TIt PartitionAroundMedian(TIt First, TIt Last, TPred& Pred)
        {
            MoveMedianToFirst(First, First + 1, First + (Last - First) / 2, Last - 1, Pred);
            return UnguardedPartition(First + 1, Last, First, Pred);
        }

        template <typename TIt, typename TPred>
        constexpr void IntroSortLoop(TIt First, TIt Last, int32 DepthLimit, TPred& Pred)
        {
            while (Last - First > kInsertionSortThreshold)
            {
                if (DepthLimit == 0)
                {
                    HeapSort(First, Last, Pred);
                    return;
                }

                --DepthLimit;
                TIt Cut = PartitionAroundMedian(First, Last, Pred);
                IntroSortLoop(Cut, Last, DepthLimit, Pred);
                Last = Cut;
            }
        }

        template <typename TIt>
        NODISCARD constexpr int32 DepthLimitFor(TDiff<TIt> Count)
        {
            int32 Log2 = 0;
            while (Count > 1)
            {
                Count >>= 1;
                ++Log2;
            }

            return 2 * Log2;
        }

        template <bool bConstruct, typename TSrc, typename TDest, typename TPred>
        void MergeRuns(TSrc Left, TSrc Middle, TSrc Right, TDest Out, TPred& Pred)
        {
            TSrc A = Left;
            TSrc B = Middle;

            while (A != Middle && B != Right)
            {
                // Ties take from the left run, which is what makes the sort stable.
                if (Pred(*B, *A))
                {
                    PlaceMove<bConstruct>(Out, std::move(*B));
                    ++B;
                }
                else
                {
                    PlaceMove<bConstruct>(Out, std::move(*A));
                    ++A;
                }

                ++Out;
            }

            for (; A != Middle; ++A, ++Out)
            {
                PlaceMove<bConstruct>(Out, std::move(*A));
            }

            for (; B != Right; ++B, ++Out)
            {
                PlaceMove<bConstruct>(Out, std::move(*B));
            }
        }
    }

    /** Introsort: quicksort on a median of three, heapsort past the depth limit, insertion sort at the end. */
    template <typename TIt, typename TPred = FLess>
    constexpr void Sort(TIt First, TIt Last, TPred Pred = {})
    {
        if (Last - First < 2)
        {
            return;
        }

        Private::IntroSortLoop(First, Last, Private::DepthLimitFor<TIt>(Last - First), Pred);
        Private::InsertionSort(First, Last, Pred);
    }

    /** Bottom-up merge sort through one heap buffer; keeps equal elements in their original order. */
    template <typename TIt, typename TPred = FLess>
    void StableSort(TIt First, TIt Last, TPred Pred = {})
    {
        using FValue = Private::TValue<TIt>;
        using FDiff = Private::TDiff<TIt>;

        const FDiff Count = Last - First;
        if (Count < 2)
        {
            return;
        }

        constexpr FDiff kRun = 32;
        for (FDiff Begin = 0; Begin < Count; Begin += kRun)
        {
            const FDiff End = (Begin + kRun < Count) ? Begin + kRun : Count;
            Private::InsertionSort(First + Begin, First + End, Pred);
        }

        if (Count <= kRun)
        {
            return;
        }

        FValue* Buffer = static_cast<FValue*>(
            FHeapAllocator::Allocate(sizeof(FValue) * static_cast<size_t>(Count), alignof(FValue)));

        bool bLivesInBuffer = false;
        for (FDiff Width = kRun; Width < Count; Width *= 2)
        {
            for (FDiff Begin = 0; Begin < Count; Begin += 2 * Width)
            {
                const FDiff Middle = (Begin + Width < Count) ? Begin + Width : Count;
                const FDiff End = (Begin + 2 * Width < Count) ? Begin + 2 * Width : Count;

                if (bLivesInBuffer)
                {
                    Private::MergeRuns<false>(Buffer + Begin, Buffer + Middle, Buffer + End, First + Begin, Pred);
                }
                else
                {
                    Private::MergeRuns<true>(First + Begin, First + Middle, First + End, Buffer + Begin, Pred);
                }
            }

            if (bLivesInBuffer)
            {
                for (FDiff Index = 0; Index < Count; ++Index)
                {
                    Buffer[Index].~FValue();
                }
            }

            bLivesInBuffer = !bLivesInBuffer;
        }

        if (bLivesInBuffer)
        {
            for (FDiff Index = 0; Index < Count; ++Index)
            {
                First[Index] = std::move(Buffer[Index]);
                Buffer[Index].~FValue();
            }
        }

        FHeapAllocator::Deallocate(Buffer, sizeof(FValue) * static_cast<size_t>(Count), alignof(FValue));
    }

    /** Leaves the element that belongs at Nth in place, everything smaller before it and larger after. */
    template <typename TIt, typename TPred = FLess>
    constexpr void NthElement(TIt First, TIt Nth, TIt Last, TPred Pred = {})
    {
        while (Last - First > 3)
        {
            TIt Cut = Private::PartitionAroundMedian(First, Last, Pred);
            if (Cut <= Nth)
            {
                First = Cut;
            }
            else
            {
                Last = Cut;
            }
        }

        Private::InsertionSort(First, Last, Pred);
    }

    /** Moves everything Pred accepts to the front, keeping relative order on both sides. */
    template <typename TIt, typename TPred>
    TIt StablePartition(TIt First, TIt Last, TPred Pred)
    {
        using FValue = Private::TValue<TIt>;
        using FDiff = Private::TDiff<TIt>;

        const FDiff Count = Last - First;
        if (Count == 0)
        {
            return First;
        }

        FValue* Buffer = static_cast<FValue*>(
            FHeapAllocator::Allocate(sizeof(FValue) * static_cast<size_t>(Count), alignof(FValue)));

        FDiff Rejected = 0;
        TIt Out = First;
        for (TIt It = First; It != Last; ++It)
        {
            if (Pred(*It))
            {
                if (Out != It)
                {
                    *Out = std::move(*It);
                }

                ++Out;
            }
            else
            {
                Memory::ConstructAt(Buffer + Rejected, std::move(*It));
                ++Rejected;
            }
        }

        for (FDiff Index = 0; Index < Rejected; ++Index, ++Out)
        {
            *Out = std::move(Buffer[Index]);
            Buffer[Index].~FValue();
        }

        FHeapAllocator::Deallocate(Buffer, sizeof(FValue) * static_cast<size_t>(Count), alignof(FValue));
        return First + (Count - Rejected);
    }

    /** Range overloads, each taking a member pointer wherever it takes a predicate or projection. */

    template <Private::Range TRange, typename T, typename TProj = FIdentity>
    NODISCARD constexpr auto Find(TRange&& Range, const T& Value, TProj Proj = {})
    {
        return FindIf(Private::Begin(Range), Private::End(Range), Private::EqualsBy(Proj, Value));
    }

    template <Private::Range TRange, typename TPred>
    NODISCARD constexpr auto FindIf(TRange&& Range, TPred Pred)
    {
        return FindIf(Private::Begin(Range), Private::End(Range), Private::AsPredicate(Pred));
    }

    template <Private::Range TRange, typename TPred>
    NODISCARD constexpr auto FindIfNot(TRange&& Range, TPred Pred)
    {
        return FindIfNot(Private::Begin(Range), Private::End(Range), Private::AsPredicate(Pred));
    }

    template <Private::Range TRange, typename T, typename TProj = FIdentity>
    NODISCARD constexpr bool Contains(TRange&& Range, const T& Value, TProj Proj = {})
    {
        return Find(Range, Value, Proj) != Private::End(Range);
    }

    template <Private::Range TRange, typename T, typename TProj = FIdentity>
    NODISCARD constexpr auto Count(TRange&& Range, const T& Value, TProj Proj = {})
    {
        return CountIf(Private::Begin(Range), Private::End(Range), Private::EqualsBy(Proj, Value));
    }

    template <Private::Range TRange, typename TPred>
    NODISCARD constexpr auto CountIf(TRange&& Range, TPred Pred)
    {
        return CountIf(Private::Begin(Range), Private::End(Range), Private::AsPredicate(Pred));
    }

    template <Private::Range TRange, typename TPred>
    NODISCARD constexpr bool AllOf(TRange&& Range, TPred Pred)
    {
        return AllOf(Private::Begin(Range), Private::End(Range), Private::AsPredicate(Pred));
    }

    template <Private::Range TRange, typename TPred>
    NODISCARD constexpr bool AnyOf(TRange&& Range, TPred Pred)
    {
        return AnyOf(Private::Begin(Range), Private::End(Range), Private::AsPredicate(Pred));
    }

    template <Private::Range TRange, typename TPred>
    NODISCARD constexpr bool NoneOf(TRange&& Range, TPred Pred)
    {
        return NoneOf(Private::Begin(Range), Private::End(Range), Private::AsPredicate(Pred));
    }

    template <Private::Range TRange, typename TFunc>
    constexpr TFunc ForEach(TRange&& Range, TFunc Func)
    {
        for (auto&& Element : Range)
        {
            Private::Project(Func, Element);
        }

        return Func;
    }

    template <Private::Range TRange, typename T>
    constexpr void Fill(TRange&& Range, const T& Value)
    {
        Fill(Private::Begin(Range), Private::End(Range), Value);
    }

    template <Private::Range TRange, typename T>
    constexpr void Iota(TRange&& Range, T Value)
    {
        Iota(Private::Begin(Range), Private::End(Range), Value);
    }

    template <Private::Range TRange, typename TOut>
    constexpr TOut Copy(TRange&& Range, TOut Out)
    {
        return Copy(Private::Begin(Range), Private::End(Range), Out);
    }

    template <Private::Range TRange, typename TOut, typename TPred>
    constexpr TOut CopyIf(TRange&& Range, TOut Out, TPred Pred)
    {
        return CopyIf(Private::Begin(Range), Private::End(Range), Out, Private::AsPredicate(Pred));
    }

    template <Private::Range TRange, typename TOut, typename TFunc>
    constexpr TOut Transform(TRange&& Range, TOut Out, TFunc Func)
    {
        for (auto&& Element : Range)
        {
            *Out = Private::Project(Func, Element);
            ++Out;
        }

        return Out;
    }

    template <Private::Range TRange, typename T>
    constexpr void Replace(TRange&& Range, const T& Old, const T& New)
    {
        Replace(Private::Begin(Range), Private::End(Range), Old, New);
    }

    template <Private::Range TRange, typename TPred, typename T>
    constexpr void ReplaceIf(TRange&& Range, TPred Pred, const T& New)
    {
        ReplaceIf(Private::Begin(Range), Private::End(Range), Private::AsPredicate(Pred), New);
    }

    template <Private::Range TRange>
    constexpr void Reverse(TRange&& Range)
    {
        Reverse(Private::Begin(Range), Private::End(Range));
    }

    template <Private::Range TRange, typename T>
    constexpr auto Remove(TRange&& Range, const T& Value)
    {
        return Remove(Private::Begin(Range), Private::End(Range), Value);
    }

    template <Private::Range TRange, typename TPred>
    constexpr auto RemoveIf(TRange&& Range, TPred Pred)
    {
        return RemoveIf(Private::Begin(Range), Private::End(Range), Private::AsPredicate(Pred));
    }

    template <Private::Range TRange>
    constexpr auto Unique(TRange&& Range)
    {
        return Unique(Private::Begin(Range), Private::End(Range));
    }

    template <Private::Range TRange, typename TEqual>
    constexpr auto Unique(TRange&& Range, TEqual AreEqual)
    {
        return Unique(Private::Begin(Range), Private::End(Range), AreEqual);
    }

    template <Private::Range TRange, typename TPred = FLess>
    NODISCARD constexpr auto MinElement(TRange&& Range, TPred Pred = {})
    {
        return MinElement(Private::Begin(Range), Private::End(Range), Pred);
    }

    template <Private::Range TRange, typename TPred = FLess>
    NODISCARD constexpr auto MaxElement(TRange&& Range, TPred Pred = {})
    {
        return MaxElement(Private::Begin(Range), Private::End(Range), Pred);
    }

    template <Private::Range TRange, typename T, typename TPred = FLess>
    NODISCARD constexpr auto LowerBound(TRange&& Range, const T& Value, TPred Pred = {})
    {
        return LowerBound(Private::Begin(Range), Private::End(Range), Value, Pred);
    }

    template <Private::Range TRange, typename T, typename TPred = FLess>
    NODISCARD constexpr auto UpperBound(TRange&& Range, const T& Value, TPred Pred = {})
    {
        return UpperBound(Private::Begin(Range), Private::End(Range), Value, Pred);
    }

    template <Private::Range TRange, typename T, typename TPred = FLess>
    NODISCARD constexpr bool BinarySearch(TRange&& Range, const T& Value, TPred Pred = {})
    {
        return BinarySearch(Private::Begin(Range), Private::End(Range), Value, Pred);
    }

    template <Private::Range TRange, typename TPred = FLess>
    NODISCARD constexpr bool IsSorted(TRange&& Range, TPred Pred = {})
    {
        return IsSorted(Private::Begin(Range), Private::End(Range), Pred);
    }

    template <Private::Range TRange, typename TPred = FLess>
    constexpr void Sort(TRange&& Range, TPred Pred = {})
    {
        Sort(Private::Begin(Range), Private::End(Range), Pred);
    }

    template <Private::Range TRange, typename TPred = FLess>
    void StableSort(TRange&& Range, TPred Pred = {})
    {
        StableSort(Private::Begin(Range), Private::End(Range), Pred);
    }

    template <Private::Range TRange, typename TPred>
    auto StablePartition(TRange&& Range, TPred Pred)
    {
        return StablePartition(Private::Begin(Range), Private::End(Range), Private::AsPredicate(Pred));
    }

    template <Private::Range TLeft, Private::Range TRight>
    NODISCARD constexpr bool Equal(TLeft&& Left, TRight&& Right)
    {
        auto First1 = Private::Begin(Left);
        auto First2 = Private::Begin(Right);
        const auto Last1 = Private::End(Left);
        const auto Last2 = Private::End(Right);

        for (; First1 != Last1 && First2 != Last2; ++First1, ++First2)
        {
            if (!(*First1 == *First2))
            {
                return false;
            }
        }

        return First1 == Last1 && First2 == Last2;
    }

    /** Position of the first element whose projection equals Value, or INDEX_NONE. */
    template <Private::Range TRange, typename T, typename TProj = FIdentity>
    NODISCARD constexpr int32 IndexOf(TRange&& Range, const T& Value, TProj Proj = {})
    {
        int32 Index = 0;
        for (auto&& Element : Range)
        {
            if (Private::Project(Proj, Element) == Value)
            {
                return Index;
            }

            ++Index;
        }

        return INDEX_NONE;
    }

    /** Position of the first element satisfying Pred, or INDEX_NONE. */
    template <Private::Range TRange, typename TPred>
    NODISCARD constexpr int32 IndexOfIf(TRange&& Range, TPred Pred)
    {
        int32 Index = 0;
        for (auto&& Element : Range)
        {
            if (Private::Project(Pred, Element))
            {
                return Index;
            }

            ++Index;
        }

        return INDEX_NONE;
    }

    /** Left fold over the projected elements, seeded with Init. */
    template <typename TIt, typename T, typename TProj = FIdentity>
    NODISCARD constexpr T Accumulate(TIt First, TIt Last, T Init, TProj Proj = {})
    {
        for (; First != Last; ++First)
        {
            Init = std::move(Init) + Private::Project(Proj, *First);
        }

        return Init;
    }

    template <Private::Range TRange, typename T, typename TProj = FIdentity>
    NODISCARD constexpr T Accumulate(TRange&& Range, T Init, TProj Proj = {})
    {
        return Accumulate(Private::Begin(Range), Private::End(Range), std::move(Init), Proj);
    }

    /** Sum of the projected elements, from a value-initialized accumulator. */
    template <Private::Range TRange, typename TProj = FIdentity>
    NODISCARD constexpr auto Sum(TRange&& Range, TProj Proj = {})
    {
        using FAccumulator = std::decay_t<decltype(Private::Project(Proj, *Private::Begin(Range)))>;
        return Accumulate(Range, FAccumulator{}, Proj);
    }
}
