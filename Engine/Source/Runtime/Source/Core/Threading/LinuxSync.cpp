#include "RuntimePCH.h"
#ifndef LE_PLATFORM_WINDOWS

#include "Sync.h"

#include <ctime>
#include <errno.h>

namespace Lumina
{
    void FMutex::Lock() noexcept
    {
        pthread_mutex_lock(&Handle);
    }

    bool FMutex::TryLock() noexcept
    {
        return pthread_mutex_trylock(&Handle) == 0;
    }

    void FMutex::Unlock() noexcept
    {
        pthread_mutex_unlock(&Handle);
    }

    void FSharedMutex::Lock() noexcept
    {
        pthread_rwlock_wrlock(&Handle);
    }

    bool FSharedMutex::TryLock() noexcept
    {
        return pthread_rwlock_trywrlock(&Handle) == 0;
    }

    void FSharedMutex::Unlock() noexcept
    {
        pthread_rwlock_unlock(&Handle);
    }

    void FSharedMutex::LockShared() noexcept
    {
        pthread_rwlock_rdlock(&Handle);
    }

    bool FSharedMutex::TryLockShared() noexcept
    {
        return pthread_rwlock_tryrdlock(&Handle) == 0;
    }

    void FSharedMutex::UnlockShared() noexcept
    {
        pthread_rwlock_unlock(&Handle);
    }

    void FConditionVariable::Wait(FUniqueLock& Lock) noexcept
    {
        pthread_cond_wait(&Handle, &Lock.GetMutex().Handle);
    }

    bool FConditionVariable::WaitFor(FUniqueLock& Lock, double Seconds) noexcept
    {
        timespec Deadline{};
        clock_gettime(CLOCK_REALTIME, &Deadline);

        const int64 Nanoseconds = static_cast<int64>(Seconds * 1e9);
        Deadline.tv_sec += static_cast<time_t>(Nanoseconds / 1000000000ll);
        Deadline.tv_nsec += static_cast<long>(Nanoseconds % 1000000000ll);
        if (Deadline.tv_nsec >= 1000000000l)
        {
            Deadline.tv_nsec -= 1000000000l;
            ++Deadline.tv_sec;
        }

        const int Result = pthread_cond_timedwait(&Handle, &Lock.GetMutex().Handle, &Deadline);
        return Result != ETIMEDOUT;
    }

    void FConditionVariable::NotifyOne() noexcept
    {
        pthread_cond_signal(&Handle);
    }

    void FConditionVariable::NotifyAll() noexcept
    {
        pthread_cond_broadcast(&Handle);
    }
}

#endif
