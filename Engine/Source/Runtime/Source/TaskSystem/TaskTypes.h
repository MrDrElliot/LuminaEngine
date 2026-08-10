#pragma once

#include "Containers/Function.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Threading/Atomic.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"
#include "Scheduler/JobScheduler.h"

namespace Lumina
{
    enum class ETaskPriority : uint8
    {
        High   = 0,
        Medium = 1,
        Low    = 2,

        /** Long-running throughput work, never inlined into another thread's wait. An assist-waiting thread
         *  will pull High/Medium/Low onto itself to avoid idling; this band is excluded from that path. */
        Background = 3,
    };

    FORCEINLINE Jobs::EJobPriority ToJobPriority(ETaskPriority Priority)
    {
        return static_cast<Jobs::EJobPriority>(Priority);
    }

    // Async completion handle: a shared flag the caller can poll or block on.
    struct FTaskCompletion
    {
        FTaskCompletion() = default;

        TAtomic<bool> bCompleted{false};

        bool IsCompleted() const { return bCompleted.load(std::memory_order_acquire); }

        /** Block until the task completes. A plain kernel wait -- it does NOT run queued jobs, so calling it
         *  from a worker fiber blocks the worker and holds the fiber. In a job, wait on a counter or TFuture. */
        void Wait() const
        {
            // atomic_wait may return spuriously, so it is a loop, not a call. Without the re-check a
            // spurious wakeup reports a task complete that is still running.
            while (!IsCompleted())
            {
                std::atomic_wait(&bCompleted, false);
            }
        }
    };

    using FTaskHandle = TSharedPtr<FTaskCompletion>;

    // Body signature for AsyncTask: a half-open range [Start, End) plus the executing worker slot.
    typedef TMoveOnlyFunction<void(uint32 Start, uint32 End, uint32 Thread)> TaskSetFunction;
}
