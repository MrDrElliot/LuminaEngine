#include "RuntimePCH.h"
#include "Sync.h"

#include "Thread.h"
#include "Platform/Time/PlatformTime.h"

namespace Lumina
{
    void FRecursiveMutex::Lock() noexcept
    {
        const uint64 Self = Threading::GetThreadID();

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
        const uint64 Self = Threading::GetThreadID();

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
