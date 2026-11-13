#pragma once

#include <type_traits>
#include <utility>

#include "Core/Templates/Optional.h"
#include "Core/Threading/Sync.h"

// TMutex<FState> State(Args...) builds an FState in place, and State.Lock() returns a guard that reaches it until the guard leaves scope.

// TryLock() returns an unset optional while another thread holds the lock, and WithLock(Func) runs Func on the value under the lock.

// TRwLock<FState> gives Read() for shared const access and Write() for exclusive mutable access.
namespace Lumina
{
    // TOwner is const for a guard over const data, and bShared picks UnlockShared over Unlock.
    template <typename TOwner, bool bShared>
    class TLockGuard
    {
    public:

        using FValue = std::conditional_t<std::is_const_v<TOwner>, const typename TOwner::FValue, typename TOwner::FValue>;

        TLockGuard(TLockGuard&& Other) noexcept : Owner(std::exchange(Other.Owner, nullptr)) {}

        TLockGuard& operator=(TLockGuard&& Other) noexcept
        {
            if (this != &Other)
            {
                Release();
                Owner = std::exchange(Other.Owner, nullptr);
            }
            return *this;
        }

        ~TLockGuard() { Release(); }

        TLockGuard(const TLockGuard&) = delete;
        TLockGuard& operator=(const TLockGuard&) = delete;

        NODISCARD FValue& Get() noexcept { return Owner->Value; }
        NODISCARD const FValue& Get() const noexcept { return Owner->Value; }

        NODISCARD FValue* operator->() noexcept { return &Owner->Value; }
        NODISCARD const FValue* operator->() const noexcept { return &Owner->Value; }

        NODISCARD FValue& operator*() noexcept { return Owner->Value; }
        NODISCARD const FValue& operator*() const noexcept { return Owner->Value; }

    private:

        friend std::remove_const_t<TOwner>;

        // The owner has already taken the lock this guard releases.
        explicit TLockGuard(TOwner* InOwner) noexcept : Owner(InOwner) {}

        void Release() noexcept
        {
            if (Owner == nullptr)
            {
                return;
            }

            if constexpr (bShared)
            {
                Owner->Mutex.UnlockShared();
            }
            else
            {
                Owner->Mutex.Unlock();
            }
        }

        TOwner* Owner;
    };

    template <typename T>
    class TMutex final
    {
    public:

        using FValue = T;

        template <typename... TArgs>
        requires std::is_constructible_v<T, TArgs...>
        explicit TMutex(TArgs&&... Args) : Value(std::forward<TArgs>(Args)...) {}

        TMutex(const TMutex&) = delete;
        TMutex& operator=(const TMutex&) = delete;
        TMutex(TMutex&&) = delete;
        TMutex& operator=(TMutex&&) = delete;

        template <typename TSelf>
        NODISCARD TLockGuard<TSelf, false> Lock(this TSelf& Self) noexcept
        {
            Self.Mutex.Lock();
            return TLockGuard<TSelf, false>(&Self);
        }

        template <typename TSelf>
        NODISCARD TOptional<TLockGuard<TSelf, false>> TryLock(this TSelf& Self) noexcept
        {
            if (!Self.Mutex.TryLock())
            {
                return NullOpt;
            }
            return TLockGuard<TSelf, false>(&Self);
        }

        template <typename TSelf, typename TFunc>
        decltype(auto) WithLock(this TSelf& Self, TFunc&& Func)
        {
            TScopeLock Lock(Self.Mutex);
            return std::forward<TFunc>(Func)(Self.Value);
        }

    private:

        template <typename, bool> friend class TLockGuard;

        mutable FMutex Mutex;
        T              Value;
    };

    template <typename T>
    using TMutexGuard = TLockGuard<std::conditional_t<std::is_const_v<T>, const TMutex<std::remove_const_t<T>>, TMutex<T>>, false>;

    template <typename T>
    class TRwLock final
    {
    public:

        using FValue = T;

        template <typename... TArgs>
        requires std::is_constructible_v<T, TArgs...>
        explicit TRwLock(TArgs&&... Args) : Value(std::forward<TArgs>(Args)...) {}

        TRwLock(const TRwLock&) = delete;
        TRwLock& operator=(const TRwLock&) = delete;
        TRwLock(TRwLock&&) = delete;
        TRwLock& operator=(TRwLock&&) = delete;

        NODISCARD TLockGuard<const TRwLock, true> Read() const noexcept
        {
            Mutex.LockShared();
            return TLockGuard<const TRwLock, true>(this);
        }

        NODISCARD TLockGuard<TRwLock, false> Write() noexcept
        {
            Mutex.Lock();
            return TLockGuard<TRwLock, false>(this);
        }

    private:

        template <typename, bool> friend class TLockGuard;

        mutable FSharedMutex Mutex;
        T                    Value;
    };

    template <typename T>
    using TRwLockReadGuard = TLockGuard<const TRwLock<T>, true>;

    template <typename T>
    using TRwLockWriteGuard = TLockGuard<TRwLock<T>, false>;
}
