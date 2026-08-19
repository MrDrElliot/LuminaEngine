#include "RuntimePCH.h"
#ifdef LE_PLATFORM_WINDOWS

#include "Sync.h"

#include <windows.h>

namespace Lumina
{
    namespace
    {
        static_assert(sizeof(void*) == sizeof(SRWLOCK), "FMutex stores an SRWLOCK in its handle slot.");
        static_assert(sizeof(void*) == sizeof(CONDITION_VARIABLE),
                      "FConditionVariable stores a CONDITION_VARIABLE in its handle slot.");

    }

    void FMutex::Lock() noexcept
    {
        AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&Handle));
    }

    bool FMutex::TryLock() noexcept
    {
        return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&Handle)) != FALSE;
    }

    void FMutex::Unlock() noexcept
    {
        ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&Handle));
    }

    void FSharedMutex::Lock() noexcept
    {
        AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&Handle));
    }

    bool FSharedMutex::TryLock() noexcept
    {
        return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&Handle)) != FALSE;
    }

    void FSharedMutex::Unlock() noexcept
    {
        ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&Handle));
    }

    void FSharedMutex::LockShared() noexcept
    {
        AcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(&Handle));
    }

    bool FSharedMutex::TryLockShared() noexcept
    {
        return TryAcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(&Handle)) != FALSE;
    }

    void FSharedMutex::UnlockShared() noexcept
    {
        ReleaseSRWLockShared(reinterpret_cast<PSRWLOCK>(&Handle));
    }

    void FConditionVariable::Wait(FUniqueLock& Lock) noexcept
    {
        SleepConditionVariableSRW(reinterpret_cast<PCONDITION_VARIABLE>(&Handle),
                                  reinterpret_cast<PSRWLOCK>(&Lock.GetMutex().Handle), INFINITE, 0);
    }

    bool FConditionVariable::WaitFor(FUniqueLock& Lock, double Seconds) noexcept
    {
        const double Milliseconds = Seconds * 1000.0;
        const DWORD Timeout = Milliseconds <= 0.0
                            ? 0u
                            : (Milliseconds >= static_cast<double>(INFINITE - 1)
                                ? INFINITE - 1
                                : static_cast<DWORD>(Milliseconds + 0.999));

        const BOOL Result = SleepConditionVariableSRW(reinterpret_cast<PCONDITION_VARIABLE>(&Handle),
                                                     reinterpret_cast<PSRWLOCK>(&Lock.GetMutex().Handle),
                                                     Timeout, 0);
        return Result != FALSE;
    }

    void FConditionVariable::NotifyOne() noexcept
    {
        WakeConditionVariable(reinterpret_cast<PCONDITION_VARIABLE>(&Handle));
    }

    void FConditionVariable::NotifyAll() noexcept
    {
        WakeAllConditionVariable(reinterpret_cast<PCONDITION_VARIABLE>(&Handle));
    }
}

#endif
