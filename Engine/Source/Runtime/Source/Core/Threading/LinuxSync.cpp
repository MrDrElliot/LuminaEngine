#include "RuntimePCH.h"
#ifndef LE_PLATFORM_WINDOWS

#include "Sync.h"

#include <cerrno>
#include <climits>
#include <ctime>
#include <immintrin.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Lumina::LinuxSyncInternal
{
    constexpr uint32 kSpinCount = 100;

    constexpr uint32 kMutexUnlocked  = 0;
    constexpr uint32 kMutexLocked    = 1;
    constexpr uint32 kMutexContended = 2;

    constexpr uint32 kReadLocked     = 1;
    constexpr uint32 kLockMask       = (1u << 30) - 1;
    constexpr uint32 kWriteLocked    = kLockMask;
    constexpr uint32 kMaxReaders     = kLockMask - 1;
    constexpr uint32 kReadersWaiting = 1u << 30;
    constexpr uint32 kWritersWaiting = 1u << 31;

    FORCEINLINE uint32* WordAddress(TAtomic<uint32>& Word) noexcept
    {
        return reinterpret_cast<uint32*>(&Word);
    }

    inline bool FutexWait(TAtomic<uint32>& Word, uint32 Expected, const timespec* Deadline) noexcept
    {
        while (Word.load(std::memory_order_relaxed) == Expected)
        {
            const long Result = ::syscall(SYS_futex, WordAddress(Word), FUTEX_WAIT_BITSET_PRIVATE, Expected, Deadline, nullptr, FUTEX_BITSET_MATCH_ANY);
            if (Result == 0)
            {
                return true;
            }

            if (errno == ETIMEDOUT)
            {
                return false;
            }

            if (errno != EINTR)
            {
                return true;
            }
        }

        return true;
    }

    inline bool FutexWakeOne(TAtomic<uint32>& Word) noexcept
    {
        return ::syscall(SYS_futex, WordAddress(Word), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0) > 0;
    }

    inline void FutexWakeAll(TAtomic<uint32>& Word) noexcept
    {
        ::syscall(SYS_futex, WordAddress(Word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
    }

    template <typename TPredicate>
    FORCEINLINE uint32 SpinUntil(const TAtomic<uint32>& Word, TPredicate Predicate) noexcept
    {
        for (uint32 Spin = kSpinCount; ; --Spin)
        {
            const uint32 Value = Word.load(std::memory_order_relaxed);
            if (Predicate(Value) || Spin == 0)
            {
                return Value;
            }

            _mm_pause();
        }
    }

    FORCEINLINE bool IsUnlocked(uint32 Value) noexcept { return (Value & kLockMask) == 0; }
    FORCEINLINE bool IsWriteLocked(uint32 Value) noexcept { return (Value & kLockMask) == kWriteLocked; }
    FORCEINLINE bool HasReadersWaiting(uint32 Value) noexcept { return (Value & kReadersWaiting) != 0; }
    FORCEINLINE bool HasWritersWaiting(uint32 Value) noexcept { return (Value & kWritersWaiting) != 0; }

    FORCEINLINE bool IsReadLockable(uint32 Value) noexcept
    {
        return (Value & kLockMask) < kMaxReaders && !HasReadersWaiting(Value) && !HasWritersWaiting(Value);
    }
}

namespace Lumina
{
    void FMutex::Lock() noexcept
    {
        using namespace LinuxSyncInternal;

        uint32 Expected = kMutexUnlocked;
        if (LIKELY(State.compare_exchange_strong(Expected, kMutexLocked, std::memory_order_acquire, std::memory_order_relaxed)))
        {
            return;
        }

        uint32 Value = SpinUntil(State, [](uint32 V) { return V != kMutexLocked; });
        if (Value == kMutexUnlocked)
        {
            Expected = kMutexUnlocked;
            if (State.compare_exchange_strong(Expected, kMutexLocked, std::memory_order_acquire, std::memory_order_relaxed))
            {
                return;
            }
            Value = Expected;
        }

        for (;;)
        {
            if (Value != kMutexContended && State.exchange(kMutexContended, std::memory_order_acquire) == kMutexUnlocked)
            {
                return;
            }

            FutexWait(State, kMutexContended, nullptr);
            Value = SpinUntil(State, [](uint32 V) { return V != kMutexLocked; });
        }
    }

    bool FMutex::TryLock() noexcept
    {
        uint32 Expected = LinuxSyncInternal::kMutexUnlocked;
        return State.compare_exchange_strong(Expected, LinuxSyncInternal::kMutexLocked, std::memory_order_acquire, std::memory_order_relaxed);
    }

    void FMutex::Unlock() noexcept
    {
        if (State.exchange(LinuxSyncInternal::kMutexUnlocked, std::memory_order_release) == LinuxSyncInternal::kMutexContended)
        {
            LinuxSyncInternal::FutexWakeOne(State);
        }
    }

    void FSharedMutex::Lock() noexcept
    {
        using namespace LinuxSyncInternal;

        uint32 Expected = 0;
        if (LIKELY(State.compare_exchange_weak(Expected, kWriteLocked, std::memory_order_acquire, std::memory_order_relaxed)))
        {
            return;
        }

        uint32 Value = SpinUntil(State, [](uint32 V) { return IsUnlocked(V) || HasWritersWaiting(V); });
        uint32 OtherWritersWaiting = 0;

        for (;;)
        {
            if (IsUnlocked(Value))
            {
                if (State.compare_exchange_weak(Value, Value | kWriteLocked | OtherWritersWaiting, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    return;
                }
                continue;
            }

            if (!HasWritersWaiting(Value))
            {
                if (!State.compare_exchange_strong(Value, Value | kWritersWaiting, std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    continue;
                }
            }

            OtherWritersWaiting = kWritersWaiting;

            const uint32 Notify = WriterNotify.load(std::memory_order_acquire);
            Value = State.load(std::memory_order_relaxed);
            if (IsUnlocked(Value) || !HasWritersWaiting(Value))
            {
                continue;
            }

            FutexWait(WriterNotify, Notify, nullptr);
            Value = SpinUntil(State, [](uint32 V) { return IsUnlocked(V) || HasWritersWaiting(V); });
        }
    }

    bool FSharedMutex::TryLock() noexcept
    {
        using namespace LinuxSyncInternal;

        uint32 Value = State.load(std::memory_order_relaxed);
        while (IsUnlocked(Value))
        {
            if (State.compare_exchange_weak(Value, Value + kWriteLocked, std::memory_order_acquire, std::memory_order_relaxed))
            {
                return true;
            }
        }

        return false;
    }

    void FSharedMutex::Unlock() noexcept
    {
        using namespace LinuxSyncInternal;

        const uint32 Value = State.fetch_sub(kWriteLocked, std::memory_order_release) - kWriteLocked;
        if (HasWritersWaiting(Value) || HasReadersWaiting(Value))
        {
            WakeWriterOrReaders(Value);
        }
    }

    void FSharedMutex::LockShared() noexcept
    {
        using namespace LinuxSyncInternal;

        uint32 Value = State.load(std::memory_order_relaxed);
        if (LIKELY(IsReadLockable(Value) && State.compare_exchange_weak(Value, Value + kReadLocked, std::memory_order_acquire, std::memory_order_relaxed)))
        {
            return;
        }

        const auto SpinRead = [this]
        {
            return SpinUntil(State, [](uint32 V) { return !IsWriteLocked(V) || HasReadersWaiting(V) || HasWritersWaiting(V); });
        };

        Value = SpinRead();
        for (;;)
        {
            if (IsReadLockable(Value))
            {
                if (State.compare_exchange_weak(Value, Value + kReadLocked, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    return;
                }
                continue;
            }

            if (!HasReadersWaiting(Value))
            {
                if (!State.compare_exchange_strong(Value, Value | kReadersWaiting, std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    continue;
                }
            }

            FutexWait(State, Value | kReadersWaiting, nullptr);
            Value = SpinRead();
        }
    }

    bool FSharedMutex::TryLockShared() noexcept
    {
        using namespace LinuxSyncInternal;

        uint32 Value = State.load(std::memory_order_relaxed);
        while (IsReadLockable(Value))
        {
            if (State.compare_exchange_weak(Value, Value + kReadLocked, std::memory_order_acquire, std::memory_order_relaxed))
            {
                return true;
            }
        }

        return false;
    }

    void FSharedMutex::UnlockShared() noexcept
    {
        using namespace LinuxSyncInternal;

        const uint32 Value = State.fetch_sub(kReadLocked, std::memory_order_release) - kReadLocked;
        if (IsUnlocked(Value) && HasWritersWaiting(Value))
        {
            WakeWriterOrReaders(Value);
        }
    }

    void FSharedMutex::WakeWriterOrReaders(uint32 Value) noexcept
    {
        using namespace LinuxSyncInternal;

        const auto WakeWriter = [this]
        {
            WriterNotify.fetch_add(1, std::memory_order_release);
            return FutexWakeOne(WriterNotify);
        };

        if (Value == kWritersWaiting)
        {
            if (State.compare_exchange_strong(Value, 0, std::memory_order_relaxed, std::memory_order_relaxed))
            {
                WakeWriter();
                return;
            }
        }

        if (Value == (kReadersWaiting | kWritersWaiting))
        {
            if (!State.compare_exchange_strong(Value, kReadersWaiting, std::memory_order_relaxed, std::memory_order_relaxed))
            {
                return;
            }

            if (WakeWriter())
            {
                return;
            }

            Value = kReadersWaiting;
        }

        if (Value == kReadersWaiting)
        {
            if (State.compare_exchange_strong(Value, 0, std::memory_order_relaxed, std::memory_order_relaxed))
            {
                FutexWakeAll(State);
            }
        }
    }

    void FConditionVariable::Wait(FUniqueLock& Lock) noexcept
    {
        const uint32 Observed = Sequence.load(std::memory_order_relaxed);
        Lock.Unlock();
        LinuxSyncInternal::FutexWait(Sequence, Observed, nullptr);
        Lock.Lock();
    }

    bool FConditionVariable::WaitFor(FUniqueLock& Lock, double Seconds) noexcept
    {
        timespec Deadline{};
        clock_gettime(CLOCK_MONOTONIC, &Deadline);

        const int64 Nanoseconds = Seconds > 0.0 ? static_cast<int64>(Seconds * 1e9) : 0;
        Deadline.tv_sec += static_cast<time_t>(Nanoseconds / 1000000000ll);
        Deadline.tv_nsec += static_cast<long>(Nanoseconds % 1000000000ll);
        if (Deadline.tv_nsec >= 1000000000l)
        {
            Deadline.tv_nsec -= 1000000000l;
            ++Deadline.tv_sec;
        }

        const uint32 Observed = Sequence.load(std::memory_order_relaxed);
        Lock.Unlock();
        const bool bWoken = LinuxSyncInternal::FutexWait(Sequence, Observed, &Deadline);
        Lock.Lock();
        return bWoken;
    }

    void FConditionVariable::NotifyOne() noexcept
    {
        Sequence.fetch_add(1, std::memory_order_relaxed);
        LinuxSyncInternal::FutexWakeOne(Sequence);
    }

    void FConditionVariable::NotifyAll() noexcept
    {
        Sequence.fetch_add(1, std::memory_order_relaxed);
        LinuxSyncInternal::FutexWakeAll(Sequence);
    }
}

#endif
