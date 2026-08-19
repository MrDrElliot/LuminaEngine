#pragma once

#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"

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

    namespace Private
    {
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
                using FTarget = std::remove_reference_t<decltype(*Dest)>;
                ::new (static_cast<void*>(&*Dest)) FTarget(std::move(Source));
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
                ::new (static_cast<void*>(Buffer + Rejected)) FValue(std::move(*It));
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
}
