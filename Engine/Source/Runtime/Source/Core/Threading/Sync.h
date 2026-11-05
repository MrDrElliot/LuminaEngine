#pragma once

#include "Core/Threading/Atomic.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina
{
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
        alignas(void*) TAtomic<uint32> State{ 0 };
    #endif
    };

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
        void WakeWriterOrReaders(uint32 Value) noexcept;

        TAtomic<uint32> State{ 0 };
        TAtomic<uint32> WriterNotify{ 0 };
    #endif
    };

    class RUNTIME_API FRecursiveMutex
    {
    public:

        FRecursiveMutex() noexcept = default;

        ~FRecursiveMutex() = default;
        FRecursiveMutex(const FRecursiveMutex&) = delete;
        FRecursiveMutex& operator=(const FRecursiveMutex&) = delete;
        FRecursiveMutex(FRecursiveMutex&&) = delete;
        FRecursiveMutex& operator=(FRecursiveMutex&&) = delete;

        void Lock() noexcept;
        NODISCARD bool TryLock() noexcept;
        void Unlock() noexcept;

        FORCEINLINE void lock() noexcept { Lock(); }
        FORCEINLINE bool try_lock() noexcept { return TryLock(); }
        FORCEINLINE void unlock() noexcept { Unlock(); }

    private:

        FMutex          Inner;
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

        // False on timeout; a spurious wake returns true, so re-check the predicate.
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
        TAtomic<uint32> Sequence{ 0 };
    #endif
    };

    class RUNTIME_API FSemaphore
    {
    public:

        explicit FSemaphore(uint32 InitialCount = 0) noexcept : Permits(InitialCount) {}

        ~FSemaphore() = default;
        FSemaphore(const FSemaphore&) = delete;
        FSemaphore& operator=(const FSemaphore&) = delete;
        FSemaphore(FSemaphore&&) = delete;
        FSemaphore& operator=(FSemaphore&&) = delete;

        void Acquire() noexcept;
        NODISCARD bool TryAcquire() noexcept;
        void Release(uint32 Count = 1) noexcept;

    private:

        FMutex             Mutex;
        FConditionVariable Available;
        uint32             Permits = 0;
    };

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

        // True for the one caller that runs the initializer, which must then call EndOnce.
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
