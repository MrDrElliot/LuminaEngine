#pragma once

#include "Core/Threading/Atomic.h"
#include "TaskSystem/FiberSync.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

#if !defined(LE_PLATFORM_WINDOWS)
    #include <pthread.h>
#endif

namespace Lumina
{
    /** Non-recursive mutex: one pointer over SRWLOCK on Windows, a pthread mutex elsewhere. */
    class RUNTIME_API FMutex
    {
    public:

        constexpr FMutex() noexcept = default;

        ~FMutex() = default;
        FMutex(const FMutex&) = delete;
        FMutex& operator=(const FMutex&) = delete;
        FMutex(FMutex&&) = delete;
        FMutex& operator=(FMutex&&) = delete;

        void Lock() noexcept;
        NODISCARD bool TryLock() noexcept;
        void Unlock() noexcept;

        FORCEINLINE void lock() noexcept { Lock(); }
        FORCEINLINE bool try_lock() noexcept { return TryLock(); }
        FORCEINLINE void unlock() noexcept { Unlock(); }

    private:

        friend class FConditionVariable;

    #if defined(LE_PLATFORM_WINDOWS)
        void* Handle = nullptr;
    #else
        pthread_mutex_t Handle = PTHREAD_MUTEX_INITIALIZER;
    #endif
    };

    /** Readers share, writers exclude: SRWLOCK on Windows, a pthread rwlock elsewhere. */
    class RUNTIME_API FSharedMutex
    {
    public:

        constexpr FSharedMutex() noexcept = default;

        ~FSharedMutex() = default;
        FSharedMutex(const FSharedMutex&) = delete;
        FSharedMutex& operator=(const FSharedMutex&) = delete;
        FSharedMutex(FSharedMutex&&) = delete;
        FSharedMutex& operator=(FSharedMutex&&) = delete;

        void Lock() noexcept;
        NODISCARD bool TryLock() noexcept;
        void Unlock() noexcept;

        void LockShared() noexcept;
        NODISCARD bool TryLockShared() noexcept;
        void UnlockShared() noexcept;

        FORCEINLINE void lock() noexcept { Lock(); }
        FORCEINLINE void unlock() noexcept { Unlock(); }
        FORCEINLINE void lock_shared() noexcept { LockShared(); }
        FORCEINLINE void unlock_shared() noexcept { UnlockShared(); }

    private:

    #if defined(LE_PLATFORM_WINDOWS)
        void* Handle = nullptr;
    #else
        pthread_rwlock_t Handle = PTHREAD_RWLOCK_INITIALIZER;
    #endif
    };

    /** Re-entrant for the thread that already holds it; counted on top of FMutex rather than the OS type. */
    class RUNTIME_API FRecursiveMutex
    {
    public:

        FRecursiveMutex() noexcept = default;

        ~FRecursiveMutex() = default;
        FRecursiveMutex(const FRecursiveMutex&) = delete;
        FRecursiveMutex& operator=(const FRecursiveMutex&) = delete;
        FRecursiveMutex(FRecursiveMutex&&) = delete;
        FRecursiveMutex& operator=(FRecursiveMutex&&) = delete;

        // Out of line so whole-program optimization cannot reuse a caller's TLS address from before a park.
        FORCENOINLINE void Lock() noexcept;
        FORCENOINLINE NODISCARD bool TryLock() noexcept;
        void Unlock() noexcept;

        FORCEINLINE void lock() noexcept { Lock(); }
        FORCEINLINE bool try_lock() noexcept { return TryLock(); }
        FORCEINLINE void unlock() noexcept { Unlock(); }

    private:

        // Owned by a fiber rather than a thread, and parks rather than blocking, since a holder may park mid-section.
        FFiberMutex     Inner;
        TAtomic<uint64> Owner{ 0 };
        uint32          Depth = 0;
    };

    template <typename TLockable>
    class TScopeLock
    {
    public:

        explicit TScopeLock(TLockable& InLockable) noexcept : Lockable(InLockable) { Lockable.Lock(); }
        ~TScopeLock() { Lockable.Unlock(); }

        TScopeLock(const TScopeLock&) = delete;
        TScopeLock& operator=(const TScopeLock&) = delete;
        TScopeLock(TScopeLock&&) = delete;
        TScopeLock& operator=(TScopeLock&&) = delete;

    private:

        TLockable& Lockable;
    };

    struct FTryLock
    {
        explicit constexpr FTryLock() = default;
    };

    inline constexpr FTryLock TryToLock{};

    class FReadScopeLock
    {
    public:

        explicit FReadScopeLock(FSharedMutex& InMutex) noexcept : Mutex(InMutex) { Mutex.LockShared(); }

        FReadScopeLock(FSharedMutex& InMutex, FTryLock) noexcept
            : Mutex(InMutex), bOwns(InMutex.TryLockShared())
        {
        }

        ~FReadScopeLock()
        {
            if (bOwns)
            {
                Mutex.UnlockShared();
            }
        }

        FReadScopeLock(const FReadScopeLock&) = delete;
        FReadScopeLock& operator=(const FReadScopeLock&) = delete;
        FReadScopeLock(FReadScopeLock&&) = delete;
        FReadScopeLock& operator=(FReadScopeLock&&) = delete;

        NODISCARD bool OwnsLock() const noexcept { return bOwns; }

    private:

        FSharedMutex& Mutex;
        bool          bOwns = true;
    };

    struct FDeferLock
    {
        explicit constexpr FDeferLock() = default;
    };

    inline constexpr FDeferLock DeferLock{};

    /** A lock that can be released and retaken, which is what a condition variable wait needs. */
    class FUniqueLock
    {
    public:

        explicit FUniqueLock(FMutex& InMutex) noexcept : Mutex(&InMutex) { Mutex->Lock(); bOwns = true; }
        FUniqueLock(FMutex& InMutex, FDeferLock) noexcept : Mutex(&InMutex) {}

        ~FUniqueLock()
        {
            if (bOwns)
            {
                Mutex->Unlock();
            }
        }

        FUniqueLock(const FUniqueLock&) = delete;
        FUniqueLock& operator=(const FUniqueLock&) = delete;
        FUniqueLock(FUniqueLock&&) = delete;
        FUniqueLock& operator=(FUniqueLock&&) = delete;

        void Lock() noexcept { Mutex->Lock(); bOwns = true; }
        void Unlock() noexcept { Mutex->Unlock(); bOwns = false; }

        NODISCARD bool OwnsLock() const noexcept { return bOwns; }
        NODISCARD FMutex& GetMutex() const noexcept { return *Mutex; }

    private:

        FMutex* Mutex = nullptr;
        bool    bOwns = false;
    };

    class RUNTIME_API FConditionVariable
    {
    public:

        constexpr FConditionVariable() noexcept = default;

        ~FConditionVariable() = default;
        FConditionVariable(const FConditionVariable&) = delete;
        FConditionVariable& operator=(const FConditionVariable&) = delete;
        FConditionVariable(FConditionVariable&&) = delete;
        FConditionVariable& operator=(FConditionVariable&&) = delete;

        void Wait(FUniqueLock& Lock) noexcept;

        /** False when the wait timed out; a spurious wake returns true, so re-check the predicate. */
        NODISCARD bool WaitFor(FUniqueLock& Lock, double Seconds) noexcept;

        void NotifyOne() noexcept;
        void NotifyAll() noexcept;

        template <typename TPredicate>
        void Wait(FUniqueLock& Lock, TPredicate Predicate)
        {
            while (!Predicate())
            {
                Wait(Lock);
            }
        }

        template <typename TPredicate>
        bool WaitFor(FUniqueLock& Lock, double Seconds, TPredicate Predicate)
        {
            while (!Predicate())
            {
                if (!WaitFor(Lock, Seconds))
                {
                    return Predicate();
                }
            }

            return true;
        }

    private:

    #if defined(LE_PLATFORM_WINDOWS)
        void* Handle = nullptr;
    #else
        pthread_cond_t Handle = PTHREAD_COND_INITIALIZER;
    #endif
    };

    /** One-time initialization; the threads that lose the race block until the winner is done. */
    class RUNTIME_API FOnceFlag
    {
    public:

        constexpr FOnceFlag() noexcept = default;

        ~FOnceFlag() = default;
        FOnceFlag(const FOnceFlag&) = delete;
        FOnceFlag& operator=(const FOnceFlag&) = delete;
        FOnceFlag(FOnceFlag&&) = delete;
        FOnceFlag& operator=(FOnceFlag&&) = delete;

        NODISCARD bool IsDone() const noexcept { return State.load(std::memory_order_acquire) == 2; }

        /** True for the single caller that should run the initializer; it must then call EndOnce. */
        NODISCARD bool BeginOnce() noexcept;
        void EndOnce() noexcept;

    private:

        TAtomic<uint8> State{ 0 };
    };

    template <typename TCallable>
    void CallOnce(FOnceFlag& Flag, TCallable&& Body)
    {
        if (Flag.IsDone())
        {
            return;
        }

        if (Flag.BeginOnce())
        {
            Body();
            Flag.EndOnce();
        }
    }
}
