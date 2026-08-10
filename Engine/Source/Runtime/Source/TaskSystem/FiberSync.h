#pragma once

#include "Core/Threading/Atomic.h"
#include "Platform/GenericPlatform.h"

// Layered on Jobs::ParkFiber/ResumeFiber: a blocked worker fiber parks and the worker runs other jobs.
// Prefer these over std FMutex for any lock held while a job runs, or that a job may contend.
namespace Lumina
{
    namespace FiberSyncDetail { struct FWaiterNode; }

    // Fiber-aware non-recursive mutex. FIFO with direct hand-off (the unlocker passes ownership straight
    // to the next waiter, so no thundering herd). NOT recursive, re-locking on the same fiber deadlocks.
    class FFiberMutex
    {
    public:
        FFiberMutex() = default;
        FFiberMutex(const FFiberMutex&)            = delete;
        FFiberMutex& operator=(const FFiberMutex&) = delete;

        RUNTIME_API void Lock();
        RUNTIME_API bool TryLock();
        RUNTIME_API void Unlock();

    private:
        TAtomic<uint32>               Spin{0};       // guards the fields below
        bool                          bLocked = false;
        FiberSyncDetail::FWaiterNode* Head    = nullptr; // FIFO waiter queue
        FiberSyncDetail::FWaiterNode* Tail    = nullptr;
    };

    // FIFO-fair: a waiting writer blocks newly arriving readers, and readers queued behind a writer are
    // granted as a batch. NOT recursive or upgradable.
    class FFiberSharedMutex
    {
    public:
        FFiberSharedMutex() = default;
        FFiberSharedMutex(const FFiberSharedMutex&)            = delete;
        FFiberSharedMutex& operator=(const FFiberSharedMutex&) = delete;

        RUNTIME_API void Lock();          // exclusive
        RUNTIME_API bool TryLock();
        RUNTIME_API void Unlock();

        RUNTIME_API void LockShared();    // shared
        RUNTIME_API bool TryLockShared();
        RUNTIME_API void UnlockShared();

    private:
        void WakeFrontLocked();           // grant front waiters; called holding Spin

        TAtomic<uint32>               Spin{0};
        int32                         Readers = 0;     // active shared holders
        bool                          bWriter = false; // an exclusive holder is active
        FiberSyncDetail::FWaiterNode* Head    = nullptr;
        FiberSyncDetail::FWaiterNode* Tail    = nullptr;
    };

    // Fiber-aware counting semaphore. Release hands a permit directly to a waiter (no over-count churn).
    class FFiberSemaphore
    {
    public:
        explicit FFiberSemaphore(int32 InitialCount = 0) : Count(InitialCount) {}
        FFiberSemaphore(const FFiberSemaphore&)            = delete;
        FFiberSemaphore& operator=(const FFiberSemaphore&) = delete;

        RUNTIME_API void Acquire();
        RUNTIME_API bool TryAcquire();
        RUNTIME_API void Release(int32 N = 1);

    private:
        TAtomic<uint32>               Spin{0};
        int32                         Count = 0;
        FiberSyncDetail::FWaiterNode* Head  = nullptr;
        FiberSyncDetail::FWaiterNode* Tail  = nullptr;
    };

    // Wait atomically releases the mutex and suspends, re-acquiring before it returns. Spurious wakeups
    // are possible -- always re-check the predicate in a loop, or use the predicate overload.
    class FFiberConditionVariable
    {
    public:
        FFiberConditionVariable() = default;
        FFiberConditionVariable(const FFiberConditionVariable&)            = delete;
        FFiberConditionVariable& operator=(const FFiberConditionVariable&) = delete;

        RUNTIME_API void Wait(FFiberMutex& Mutex);
        RUNTIME_API void NotifyOne();
        RUNTIME_API void NotifyAll();

        template<typename TPredicate>
        void Wait(FFiberMutex& Mutex, TPredicate&& Pred)
        {
            while (!Pred())
            {
                Wait(Mutex);
            }
        }

    private:
        TAtomic<uint32>               Spin{0};
        FiberSyncDetail::FWaiterNode* Head = nullptr;
        FiberSyncDetail::FWaiterNode* Tail = nullptr;
    };

    // RAII guards.
    class FFiberScopeLock
    {
    public:
        explicit FFiberScopeLock(FFiberMutex& InMutex) : Mutex(&InMutex) { Mutex->Lock(); }
        ~FFiberScopeLock() { Mutex->Unlock(); }
        FFiberScopeLock(const FFiberScopeLock&)            = delete;
        FFiberScopeLock& operator=(const FFiberScopeLock&) = delete;
    private:
        FFiberMutex* Mutex;
    };

    class FFiberReadScopeLock
    {
    public:
        explicit FFiberReadScopeLock(FFiberSharedMutex& InMutex) : Mutex(&InMutex) { Mutex->LockShared(); }
        ~FFiberReadScopeLock() { Mutex->UnlockShared(); }
        FFiberReadScopeLock(const FFiberReadScopeLock&)            = delete;
        FFiberReadScopeLock& operator=(const FFiberReadScopeLock&) = delete;
    private:
        FFiberSharedMutex* Mutex;
    };

    class FFiberWriteScopeLock
    {
    public:
        explicit FFiberWriteScopeLock(FFiberSharedMutex& InMutex) : Mutex(&InMutex) { Mutex->Lock(); }
        ~FFiberWriteScopeLock() { Mutex->Unlock(); }
        FFiberWriteScopeLock(const FFiberWriteScopeLock&)            = delete;
        FFiberWriteScopeLock& operator=(const FFiberWriteScopeLock&) = delete;
    private:
        FFiberSharedMutex* Mutex;
    };
}
