#pragma once

#include <atomic>
#include "Containers/Deque.h"
#include "Containers/Function.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Threading/Sync.h"
#include "Core/Threading/Thread.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina
{
    // Threads of its own below normal priority, so a tile rebake runs in whatever time the frame leaves instead of on a worker the frame waits for.
    class RUNTIME_API FNavBuildPool
    {
    public:

        explicit FNavBuildPool(uint32 NumThreads);
        ~FNavBuildPool();

        FNavBuildPool(const FNavBuildPool&) = delete;
        FNavBuildPool& operator=(const FNavBuildPool&) = delete;
        FNavBuildPool(FNavBuildPool&&) = delete;
        FNavBuildPool& operator=(FNavBuildPool&&) = delete;

        void Enqueue(TMoveOnlyFunction<void()>&& Job);

        uint32 GetNumThreads() const { return (uint32)Threads.Num(); }

        // The caller takes a share and drains the queue while it waits, so a job on the pool can fan out without deadlocking it.
        template <typename TFunc>
        void ParallelFor(uint32 Num, TFunc&& Func)
        {
            if (Num == 0)
            {
                return;
            }

            std::atomic<uint32> Next{ 0 };
            std::atomic<uint32> HelpersDone{ 0 };
            auto Body = [&]
            {
                for (uint32 Index = Next.fetch_add(1, std::memory_order_relaxed); Index < Num; Index = Next.fetch_add(1, std::memory_order_relaxed))
                {
                    Func(Index);
                }
            };

            const uint32 Helpers = Math::Min(Num - 1, GetNumThreads());
            for (uint32 i = 0; i < Helpers; ++i)
            {
                Enqueue([&Body, &HelpersDone]
                {
                    Body();
                    HelpersDone.fetch_add(1, std::memory_order_release);
                });
            }
            Body();

            // A helper still queued holds this frame by reference, so the call waits for every one of them to run.
            while (HelpersDone.load(std::memory_order_acquire) < Helpers)
            {
                if (!RunOne())
                {
                    Threading::ThreadYield();
                }
            }
        }

    private:

        bool RunOne();
        void ThreadLoop();

        FMutex                              QueueLock;
        FConditionVariable                  QueueWake;
        TDeque<TMoveOnlyFunction<void()>>   Queue;
        bool                                bStopping = false;
        TVector<FThread>                    Threads;
    };

    // Fans out on the pool when there is one, or on the frame's workers when there is not.
    template <typename TFunc>
    void NavParallelFor(FNavBuildPool* Pool, uint32 Num, TFunc&& Func, uint32 MinRange, Jobs::EJobPriority Priority)
    {
        if (Pool != nullptr)
        {
            Pool->ParallelFor(Num, std::forward<TFunc>(Func));
        }
        else
        {
            Jobs::ParallelFor(Num, std::forward<TFunc>(Func), MinRange, Priority);
        }
    }
}
