#pragma once

#include "TaskTypes.h"
#include "Scheduler/JobScheduler.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Core/Assertions/Assert.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

#include <new>
#include <type_traits>
#include <utility>

// Fiber-aware futures and promises. A TPromise<T> produces a single value; a TFuture<T> consumes it.
// Waiting (Get / Wait) is fiber-aware, a worker fiber parks until the value is ready and the worker
// runs other jobs meanwhile; an external thread assist-waits. Continuations (.Then) are scheduled as
// jobs when the value lands, so chains run on the worker pool rather than recursing on the setter.
//
// The shared state is reference counted (TSharedPtr), so the promise and any number of futures /
// continuations can outlive each other in any order.
namespace Lumina
{
    namespace FutureDetail
    {
        // Schedule a type-erased continuation to run on a worker. Defined in Future.cpp.
        RUNTIME_API void DispatchContinuation(TMoveOnlyFunction<void()>&& Fn);

        struct FStateBase
        {
            Jobs::FCounter*                       Counter = nullptr; // 1 -> 0 when the value is set
            TAtomic<bool>                         bReady{false};
            TAtomic<bool>                         bSet{false};       // guards against a double SetValue
            FMutex                                ContLock;
            TVector<TMoveOnlyFunction<void()>>    Continuations;

            FStateBase() { Counter = Jobs::AllocCounter(1); }
            ~FStateBase() { Jobs::FreeCounter(Counter); }

            FStateBase(const FStateBase&)            = delete;
            FStateBase& operator=(const FStateBase&) = delete;

            bool IsReady() const { return bReady.load(std::memory_order_acquire); }
            void Wait() const    { Jobs::WaitForCounter(Counter, 0); }

            // Publish readiness, wake waiters, and fire continuations. Called once, after the value is
            // stored. The value store happens-before the release here; a waiter that observes readiness
            // (via the counter / bReady acquire) sees the stored value.
            void Signal()
            {
                bReady.store(true, std::memory_order_release);

                TVector<TMoveOnlyFunction<void()>> Local;
                {
                    FScopeLock Lock(ContLock);
                    Local.swap(Continuations);
                }
                Jobs::DecrementCounter(Counter, 1);
                for (TMoveOnlyFunction<void()>& Fn : Local)
                {
                    DispatchContinuation(Move(Fn));
                }
            }

            void AddContinuation(TMoveOnlyFunction<void()>&& Fn)
            {
                {
                    FScopeLock Lock(ContLock);
                    if (!bReady.load(std::memory_order_acquire))
                    {
                        Continuations.push_back(Move(Fn));
                        return;
                    }
                }
                DispatchContinuation(Move(Fn)); // already ready, run now
            }
        };

        template<typename T>
        struct TState : FStateBase
        {
            alignas(T) unsigned char Storage[sizeof(T)];

            ~TState()
            {
                if (IsReady())
                {
                    Ptr()->~T();
                }
            }

            T* Ptr() { return reinterpret_cast<T*>(Storage); }

            template<typename U>
            void Construct(U&& Value) { ::new (static_cast<void*>(Storage)) T(std::forward<U>(Value)); }
        };

        template<>
        struct TState<void> : FStateBase {};
    }

    template<typename T>
    class TFuture;

    // ----------------------------------------------------------------------------------------------
    // TPromise<T>
    // ----------------------------------------------------------------------------------------------
    template<typename T>
    class TPromise
    {
    public:
        TPromise() : State(MakeShared<FutureDetail::TState<T>>()) {}

        TPromise(const TPromise&)            = delete;
        TPromise& operator=(const TPromise&) = delete;
        TPromise(TPromise&&)                 = default;
        TPromise& operator=(TPromise&&)      = default;

        TFuture<T> GetFuture() const;

        void SetValue(const T& Value)
        {
            const bool Was = State->bSet.exchange(true, std::memory_order_acq_rel);
            ASSERT(!Was); // a promise may be fulfilled only once
            State->Construct(Value);
            State->Signal();
        }

        void SetValue(T&& Value)
        {
            const bool Was = State->bSet.exchange(true, std::memory_order_acq_rel);
            ASSERT(!Was);
            State->Construct(Move(Value));
            State->Signal();
        }

    private:
        TSharedPtr<FutureDetail::TState<T>> State;
    };

    template<>
    class TPromise<void>
    {
    public:
        TPromise() : State(MakeShared<FutureDetail::TState<void>>()) {}

        TPromise(const TPromise&)            = delete;
        TPromise& operator=(const TPromise&) = delete;
        TPromise(TPromise&&)                 = default;
        TPromise& operator=(TPromise&&)      = default;

        TFuture<void> GetFuture() const;

        void SetValue()
        {
            const bool Was = State->bSet.exchange(true, std::memory_order_acq_rel);
            ASSERT(!Was);
            State->Signal();
        }

    private:
        TSharedPtr<FutureDetail::TState<void>> State;
        friend class TFuture<void>;
    };

    // ----------------------------------------------------------------------------------------------
    // TFuture<T>
    // ----------------------------------------------------------------------------------------------
    template<typename T>
    class TFuture
    {
    public:
        TFuture() = default;

        bool IsValid() const { return static_cast<bool>(State); }
        bool IsReady() const { return State && State->IsReady(); }

        // Fiber-aware: parks the calling worker fiber (or assist-waits on an external thread) until ready.
        void Wait() const { ASSERT(State); State->Wait(); }

        // Wait, then move the value out. Single-shot, call once.
        T Get()
        {
            ASSERT(State);
            State->Wait();
            return Move(*State->Ptr());
        }

        // Wait, then return a reference to the value (safe to call from multiple futures sharing state).
        const T& GetRef() const
        {
            ASSERT(State);
            State->Wait();
            return *State->Ptr();
        }

        // Chain a continuation that runs (on a worker) once this future is ready, receiving the value by
        // reference. Returns a future for the continuation's result.
        template<typename F>
        auto Then(F&& Func, ETaskPriority Priority = ETaskPriority::Medium)
            -> TFuture<std::decay_t<std::invoke_result_t<F, T&>>>;

        // Low-level: register a callback to run on a worker once ready (no value passed). Used by WhenAll.
        void Continue(TMoveOnlyFunction<void()>&& Fn) const
        {
            ASSERT(State);
            State->AddContinuation(Move(Fn));
        }

    private:
        explicit TFuture(TSharedPtr<FutureDetail::TState<T>> InState) : State(Move(InState)) {}
        friend class TPromise<T>;

        TSharedPtr<FutureDetail::TState<T>> State;
    };

    template<>
    class TFuture<void>
    {
    public:
        TFuture() = default;

        bool IsValid() const { return static_cast<bool>(State); }
        bool IsReady() const { return State && State->IsReady(); }

        void Wait() const { ASSERT(State); State->Wait(); }
        void Get() const  { ASSERT(State); State->Wait(); }

        template<typename F>
        auto Then(F&& Func, ETaskPriority Priority = ETaskPriority::Medium)
            -> TFuture<std::decay_t<std::invoke_result_t<F>>>;

        void Continue(TMoveOnlyFunction<void()>&& Fn) const
        {
            ASSERT(State);
            State->AddContinuation(Move(Fn));
        }

    private:
        explicit TFuture(TSharedPtr<FutureDetail::TState<void>> InState) : State(Move(InState)) {}
        friend class TPromise<void>;

        TSharedPtr<FutureDetail::TState<void>> State;
    };

    template<typename T>
    TFuture<T> TPromise<T>::GetFuture() const { return TFuture<T>(State); }

    inline TFuture<void> TPromise<void>::GetFuture() const { return TFuture<void>(State); }

    template<typename T>
    template<typename F>
    auto TFuture<T>::Then(F&& Func, ETaskPriority Priority)
        -> TFuture<std::decay_t<std::invoke_result_t<F, T&>>>
    {
        using U = std::decay_t<std::invoke_result_t<F, T&>>;
        ASSERT(State);

        TPromise<U> Promise;
        TFuture<U>  Result = Promise.GetFuture();

        auto Cont = [St = State, Fn = std::forward<F>(Func), Pr = Move(Promise)]() mutable
        {
            if constexpr (std::is_void_v<U>)
            {
                Fn(*St->Ptr());
                Pr.SetValue();
            }
            else
            {
                Pr.SetValue(Fn(*St->Ptr()));
            }
        };

        State->AddContinuation(TMoveOnlyFunction<void()>(Move(Cont)));
        (void)Priority;
        return Result;
    }

    template<typename F>
    auto TFuture<void>::Then(F&& Func, ETaskPriority Priority) -> TFuture<std::decay_t<std::invoke_result_t<F>>>
    {
        using U = std::decay_t<std::invoke_result_t<F>>;
        ASSERT(State);

        TPromise<U> Promise;
        TFuture<U>  Result = Promise.GetFuture();

        auto Cont = [Fn = std::forward<F>(Func), Pr = Move(Promise)]() mutable
        {
            if constexpr (std::is_void_v<U>)
            {
                Fn();
                Pr.SetValue();
            }
            else
            {
                Pr.SetValue(Fn());
            }
        };

        State->AddContinuation(TMoveOnlyFunction<void()>(Move(Cont)));
        (void)Priority;
        return Result;
    }

    // ----------------------------------------------------------------------------------------------
    // Free helpers
    // ----------------------------------------------------------------------------------------------

    template<typename T>
    TFuture<std::decay_t<T>> MakeReadyFuture(T&& Value)
    {
        TPromise<std::decay_t<T>> Promise;
        TFuture<std::decay_t<T>>  Future = Promise.GetFuture();
        Promise.SetValue(std::forward<T>(Value));
        return Future;
    }

    inline TFuture<void> MakeReadyFuture()
    {
        TPromise<void> Promise;
        TFuture<void>  Future = Promise.GetFuture();
        Promise.SetValue();
        return Future;
    }

    namespace Task
    {
        // Run Func() on the worker pool and return a future for its result.
        template<typename F>
        auto Async(F&& Func, ETaskPriority Priority = ETaskPriority::Medium) -> TFuture<std::decay_t<std::invoke_result_t<F>>>
        {
            using U = std::decay_t<std::invoke_result_t<F>>;

            TPromise<U> Promise;
            TFuture<U>  Result = Promise.GetFuture();

            auto Job = [Fn = std::forward<F>(Func), Pr = Move(Promise)]() mutable
            {
                if constexpr (std::is_void_v<U>)
                {
                    Fn();
                    Pr.SetValue();
                }
                else
                {
                    Pr.SetValue(Fn());
                }
            };

            FutureDetail::DispatchContinuation(TMoveOnlyFunction<void()>(Move(Job)));
            (void)Priority;
            return Result;
        }
    }

    // Returns a future that is ready once every input future is ready. Captures the inputs so they stay
    // alive until then. Empty input -> an already-ready future.
    template<typename T>
    TFuture<void> WhenAll(const TVector<TFuture<T>>& Futures)
    {
        if (Futures.empty())
        {
            return MakeReadyFuture();
        }

        struct FJoin
        {
            TPromise<void>          Promise;
            TAtomic<int32>          Remaining;
            TVector<TFuture<T>>     Keep; // keep inputs alive
        };
        auto Shared = MakeShared<FJoin>();
        Shared->Remaining.store(static_cast<int32>(Futures.size()), std::memory_order_relaxed);
        Shared->Keep = Futures;

        TFuture<void> Result = Shared->Promise.GetFuture();
        for (const TFuture<T>& F : Futures)
        {
            F.Continue([Shared]() mutable
            {
                if (Shared->Remaining.fetch_sub(1, std::memory_order_acq_rel) - 1 == 0)
                {
                    Shared->Promise.SetValue();
                }
            });
        }
        return Result;
    }
}
