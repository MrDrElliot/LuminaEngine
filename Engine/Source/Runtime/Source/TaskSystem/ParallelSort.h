#pragma once

#include "Containers/Algorithm.h"
#include "Containers/ContainerAllocator.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina::Task
{
    namespace Private
    {
        // Below this the fan-out and the scratch buffer cost more than the parallelism returns.
        inline constexpr size_t kParallelSortThreshold = 4096;

        // Keeps every chunk worth a job; a smaller one is cheaper sorted alongside its neighbor.
        inline constexpr size_t kParallelSortMinChunk = 2048;

        inline constexpr uint32 kParallelSortMaxChunks = 64;
    }

    /** Sorts each chunk on the job system, then merges the chunks back in parallel rounds. */
    template <typename TIt, typename TPredicate = Algo::FLess>
    void ParallelSort(TIt First, TIt Last, TPredicate Pred = {})
    {
        using FValue = typename std::iterator_traits<TIt>::value_type;

        const size_t Count = static_cast<size_t>(Last - First);
        if (Count < 2)
        {
            return;
        }

        uint32 ChunkCount = GTaskSystem != nullptr ? Jobs::GetNumWorkers() : 0u;
        const size_t UsefulChunks = Count / Private::kParallelSortMinChunk;

        if (ChunkCount > Private::kParallelSortMaxChunks)
        {
            ChunkCount = Private::kParallelSortMaxChunks;
        }

        if (static_cast<size_t>(ChunkCount) > UsefulChunks)
        {
            ChunkCount = static_cast<uint32>(UsefulChunks);
        }

        if (Count < Private::kParallelSortThreshold || ChunkCount < 2)
        {
            Algo::Sort(First, Last, Pred);
            return;
        }

        LUMINA_PROFILE_SECTION("Task::ParallelSort");

        const auto ChunkBegin = [Count, ChunkCount](uint32 Index) -> size_t
        {
            const uint32 Clamped = Index < ChunkCount ? Index : ChunkCount;
            return (Count * Clamped) / ChunkCount;
        };

        ParallelFor(ChunkCount, [First, &ChunkBegin, &Pred](uint32 Index)
        {
            TPredicate Local = Pred;
            Algo::Sort(First + ChunkBegin(Index), First + ChunkBegin(Index + 1), Local);
        }, 1);

        FValue* const Buffer = static_cast<FValue*>(
            FHeapAllocator::Allocate(sizeof(FValue) * Count, alignof(FValue)));

        bool bLivesInBuffer = false;
        for (uint32 Width = 1; Width < ChunkCount; Width *= 2)
        {
            const uint32 Tasks = (ChunkCount + 2 * Width - 1) / (2 * Width);
            const bool bFromBuffer = bLivesInBuffer;

            ParallelFor(Tasks, [=, &ChunkBegin, &Pred](uint32 Index)
            {
                const size_t LeftBegin = ChunkBegin(Index * 2 * Width);
                const size_t Middle    = ChunkBegin(Index * 2 * Width + Width);
                const size_t RightEnd  = ChunkBegin((Index + 1) * 2 * Width);

                TPredicate Local = Pred;

                if (bFromBuffer)
                {
                    Algo::Private::MergeRuns<false>(Buffer + LeftBegin, Buffer + Middle, Buffer + RightEnd,
                                                    First + LeftBegin, Local);
                }
                else
                {
                    Algo::Private::MergeRuns<true>(First + LeftBegin, First + Middle, First + RightEnd,
                                                   Buffer + LeftBegin, Local);
                }
            }, 1);

            if (bFromBuffer)
            {
                for (size_t Index = 0; Index < Count; ++Index)
                {
                    Buffer[Index].~FValue();
                }
            }

            bLivesInBuffer = !bLivesInBuffer;
        }

        if (bLivesInBuffer)
        {
            for (size_t Index = 0; Index < Count; ++Index)
            {
                First[Index] = std::move(Buffer[Index]);
                Buffer[Index].~FValue();
            }
        }

        FHeapAllocator::Deallocate(Buffer, sizeof(FValue) * Count, alignof(FValue));
    }
}
