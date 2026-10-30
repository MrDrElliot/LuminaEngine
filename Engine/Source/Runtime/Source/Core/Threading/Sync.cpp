#include "RuntimePCH.h"
#include "Sync.h"

#include "Thread.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/Scheduler/JobScheduler.h"

namespace Lumina
{
    namespace
    {
        // A fiber can park mid-section and resume on another worker, so a thread id would let the next fiber on that worker in.
        FORCEINLINE uint64 RecursiveOwnerToken() noexcept
        {
            if (const Jobs::FFiberHandle Fiber = Jobs::GetCurrentFiberHandle())
            {
                return reinterpret_cast<uint64>(Fiber.Fiber);
            }

            // The top bit is never set in a user-space address, so a thread can never pass for a fiber.
            return Threading::GetThreadID() | (1ull << 63);
        }
    }

    void FRecursiveMutex::Lock() noexcept
    {
        const uint64 Self = RecursiveOwnerToken();

        if (Owner.load(std::memory_order_relaxed) == Self)
        {
            ++Depth;
            return;
        }

        Inner.Lock();
        Owner.store(Self, std::memory_order_relaxed);
        Depth = 1;
    }

    bool FRecursiveMutex::TryLock() noexcept
    {
        const uint64 Self = RecursiveOwnerToken();

        if (Owner.load(std::memory_order_relaxed) == Self)
        {
            ++Depth;
            return true;
        }

        if (!Inner.TryLock())
        {
            return false;
        }

        Owner.store(Self, std::memory_order_relaxed);
        Depth = 1;
        return true;
    }

    void FRecursiveMutex::Unlock() noexcept
    {
        if (--Depth == 0)
        {
            Owner.store(0, std::memory_order_relaxed);
            Inner.Unlock();
        }
    }

    bool FOnceFlag::BeginOnce() noexcept
    {
        uint8 Expected = 0;
        if (State.compare_exchange_strong(Expected, 1, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return true;
        }

        while (State.load(std::memory_order_acquire) != 2)
        {
            PlatformTime::YieldThread();
        }

        return false;
    }

    void FOnceFlag::EndOnce() noexcept
    {
        State.store(2, std::memory_order_release);
    }
}
