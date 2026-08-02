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
        void Wait() const
        {
            std::atomic_wait(&bCompleted, false);
        }
    };

    using FTaskHandle = TSharedPtr<FTaskCompletion>;

    // Body signature for AsyncTask: a half-open range [Start, End) plus the executing worker slot.
    typedef TMoveOnlyFunction<void(uint32 Start, uint32 End, uint32 Thread)> TaskSetFunction;
}
