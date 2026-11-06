#pragma once

#include "Containers/Algorithm.h"
#include "Containers/ContainerAllocator.h"
#include "Core/Profiler/Profile.h"
#include "TaskSystem/Fork/ForkRuntime.h"

#include <iterator>

namespace Lumina::Task
{
    namespace Private
    {
        inline constexpr size_t kParallelSortThreshold = 4096;
        inline constexpr size_t kParallelSortLeaf      = 2048;

        template<typename TIt, typename TPredicate>
        Jobs::TTask<void> SortRun(TIt First, size_t Count, std::iter_value_t<TIt>* Buffer, const TPredicate* Pred, bool bResultInBuffer)
        {
            using FValue = std::iter_value_t<TIt>;

            TPredicate Local = *Pred;

            if (Count <= kParallelSortLeaf)
            {
                Algo::Sort(First, First + Count, Local);
                if (bResultInBuffer)
                {
                    for (size_t Index = 0; Index < Count; ++Index)
                    {
                        ::new (Buffer + Index) FValue(std::move(First[Index]));
                    }
                }
                co_return;
            }

            const size_t Half = Count / 2;
            co_await Jobs::Fork(SortRun<TIt, TPredicate>(First, Half, Buffer, Pred, !bResultInBuffer));
            co_await Jobs::Call(SortRun<TIt, TPredicate>(First + Half, Count - Half, Buffer + Half, Pred, !bResultInBuffer));
            co_await Jobs::Join();

            if (bResultInBuffer)
            {
                Algo::Private::MergeRuns<true>(First, First + Half, First + Count, Buffer, Local);
                co_return;
            }

            Algo::Private::MergeRuns<false>(Buffer, Buffer + Half, Buffer + Count, First, Local);
            for (size_t Index = 0; Index < Count; ++Index)
            {
                Buffer[Index].~FValue();
            }
        }
    }

    // Splits the range recursively, sorts the leaves in parallel, then merges back up through a scratch buffer.
    template <typename TIt, typename TPredicate = Algo::FLess>
    void ParallelSort(TIt First, TIt Last, TPredicate Pred = {})
    {
        using FValue = std::iter_value_t<TIt>;

        const size_t Count = static_cast<size_t>(Last - First);
        if (Count < 2)
        {
            return;
        }

        if (Count < Private::kParallelSortThreshold || !Jobs::IsRuntimeRunning() || Jobs::GetNumWorkers() == 0)
        {
            Algo::Sort(First, Last, Pred);
            return;
        }

        LUMINA_PROFILE_SECTION("Task::ParallelSort");

        FValue* const Buffer = static_cast<FValue*>(FHeapAllocator::Allocate(sizeof(FValue) * Count, alignof(FValue)));
        Jobs::SyncWait(Jobs::EJobPriority::Normal, &Private::SortRun<TIt, TPredicate>, First, Count, Buffer, &Pred, false);
        FHeapAllocator::Deallocate(Buffer, sizeof(FValue) * Count, alignof(FValue));
    }
}
