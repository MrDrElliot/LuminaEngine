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

        /**
         * Long-running throughput work, never inlined into another thread's wait.
         *
         * High/Medium/Low only order work within the pool; a thread that is assist-waiting on some
         * unrelated counter will still pull one of them onto itself to avoid idling, which is how a
         * multi-second background build ends up executing inside a frame. Background is excluded from
         * that path -- only real workers ever run it.
         *
         * The rule of thumb: if nothing in the current frame is waiting on the result, and the work
         * takes longer than a frame, it belongs here. Terrain/voxel chunk builds, cooks, bakes.
         */
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

        /**
         * Block until the task completes.
         *
         * NOTE: this is a plain kernel wait -- it does NOT run queued jobs the way a counter wait does.
         * Calling it from a worker fiber blocks the whole worker (and holds the fiber) instead of
         * yielding, so use it only from a thread that owns its own execution: the main thread, a
         * dedicated tool thread. Inside a job, wait on a counter or a TFuture instead.
         */
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
