#pragma once

#include "Core/LuminaMacros.h"
#include "Core/Templates/LuminaTemplate.h"
#include "ModuleAPI.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

#include <atomic>
#include <cstddef>

#include <coroutine>
#include <exception>
#include <new>
#include <type_traits>
#include <utility>

namespace Lumina::Jobs
{
    enum class EJobPriority : uint8
    {
        High       = 0,
        Normal     = 1,
        Low        = 2,
        Background = 3,
    };

    inline constexpr uint32 kNumJobPriorities = 4;

    enum class EFrameKind : uint8
    {
        Call,
        Fork,
        Root,
    };

    struct FRootLink
    {
        std::atomic<uint32>* Done      = nullptr;
        void               (*OnDone)(void*) = nullptr;
        void*                OnDoneCtx = nullptr;
    };

    class FStack;

    // The part of every task's promise the scheduler reads.
    struct FFrame
    {
        static constexpr uint32 kJoinInit = 1u << 30;

        std::coroutine_handle<> Handle;
        FFrame*                 Parent = nullptr;
        FStack*                 Stack  = nullptr;
        FFrame*                 NextInjected = nullptr;
        FRootLink               Root;
        std::atomic<uint32>     Joins{kJoinInit};
        uint32                  Steals = 0;
        EFrameKind              Kind   = EFrameKind::Call;
        EJobPriority            Band   = EJobPriority::Normal;
    };

    namespace ForkDetail
    {
        RUNTIME_API void* AllocateFrame(std::size_t Size);
        RUNTIME_API void  FreeFrame(void* Memory, std::size_t Size);

        RUNTIME_API void BindCall(FFrame& Parent, FFrame& Child);
        RUNTIME_API void BindFork(FFrame& Parent, FFrame& Child);
        RUNTIME_API std::coroutine_handle<> Finish(FFrame& Frame);
        RUNTIME_API bool SuspendAtJoin(FFrame& Frame);

        RUNTIME_API FStack* BeginRoot();
        RUNTIME_API void    EndRoot(FFrame& Root, FStack* Stack, EJobPriority Band);
        RUNTIME_API void    RunRootAndWait(FFrame& Root);
        RUNTIME_API void    SpawnRoot(FFrame& Root);

        // Parks Frame's stack with it so another thread can resume it after an event; false if it must resume now.
        RUNTIME_API void ParkForEvent(FFrame& Frame);
        RUNTIME_API void ResumeParked(FFrame& Frame);
        RUNTIME_API void ReclaimParked(FFrame& Frame);

        using FWaitDone = bool (*)(const void* Context);

        // Runs queued work on this thread until IsDone holds, sleeping briefly on SleepWord when nothing is runnable.
        RUNTIME_API void HelpUntil(FWaitDone IsDone, const void* Context, const std::atomic<uint32>& SleepWord, uint32 BandLimit);

        struct FJoinTag {};
    }

    template<typename T = void>
    class TTask;

    namespace ForkDetail
    {
        struct FFinalAwaiter
        {
            bool await_ready() const noexcept { return false; }
            template<typename TPromise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise> Handle) noexcept { return Finish(Handle.promise()); }
            void await_resume() const noexcept {}
        };

        struct FJoinAwaiter
        {
            FFrame& Frame;

            bool await_ready() const noexcept { return Frame.Steals == 0; }
            bool await_suspend(std::coroutine_handle<>) noexcept { return SuspendAtJoin(Frame); }
            void await_resume() noexcept
            {
                Frame.Joins.store(FFrame::kJoinInit, std::memory_order_relaxed);
                Frame.Steals = 0;
            }
        };

        template<typename T>
        struct TPromiseBase : FFrame
        {
            T* Out = nullptr;

            template<typename U>
            void return_value(U&& Value) { *Out = Forward<U>(Value); }
        };

        template<>
        struct TPromiseBase<void> : FFrame
        {
            void return_void() noexcept {}
        };
    }

    // A fork-join task. Create it inline in a co_await of Fork or Call, or as the root of SyncWait or Spawn.
    template<typename T>
    class TTask
    {
    public:

        struct promise_type : ForkDetail::TPromiseBase<T>
        {
            TTask get_return_object() noexcept
            {
                this->Handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return TTask(std::coroutine_handle<promise_type>::from_promise(*this));
            }

            std::suspend_always initial_suspend() const noexcept { return {}; }
            ForkDetail::FFinalAwaiter final_suspend() const noexcept { return {}; }
            void unhandled_exception() const noexcept { std::terminate(); }

            ForkDetail::FJoinAwaiter await_transform(ForkDetail::FJoinTag) noexcept { return { *this }; }

            template<typename TAwaitable>
            TAwaitable&& await_transform(TAwaitable&& Awaitable) const noexcept { return Forward<TAwaitable>(Awaitable); }

            static void* operator new(std::size_t Size) { return ForkDetail::AllocateFrame(Size); }
            static void operator delete(void* Memory, std::size_t Size) noexcept { ForkDetail::FreeFrame(Memory, Size); }
        };

        using FHandle = std::coroutine_handle<promise_type>;

        TTask() noexcept = default;
        explicit TTask(FHandle InHandle) noexcept : Handle(InHandle) {}
        TTask(TTask&& Other) noexcept : Handle(std::exchange(Other.Handle, nullptr)) {}
        TTask(const TTask&) = delete;
        TTask& operator=(const TTask&) = delete;

        TTask& operator=(TTask&& Other) noexcept
        {
            if (this != &Other)
            {
                Reset();
                Handle = std::exchange(Other.Handle, nullptr);
            }
            return *this;
        }

        ~TTask() { Reset(); }

        NODISCARD FHandle Release() noexcept { return std::exchange(Handle, nullptr); }

    private:

        void Reset() noexcept
        {
            if (Handle)
            {
                Handle.destroy();
                Handle = nullptr;
            }
        }

        FHandle Handle;
    };

    namespace ForkDetail
    {
        template<typename T>
        struct TForkAwaiter
        {
            typename TTask<T>::FHandle Child;

            bool await_ready() const noexcept { return false; }

            // The push hands the parent to thieves, which may resume it and end this awaiter, so nothing here is read after it.
            template<typename TPromise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise> Parent) noexcept
            {
                const typename TTask<T>::FHandle Runnable = Child;
                BindFork(Parent.promise(), Runnable.promise());
                return Runnable;
            }

            void await_resume() const noexcept {}
        };

        template<typename T>
        struct TCallAwaiter
        {
            typename TTask<T>::FHandle Child;
            alignas(T) unsigned char Storage[sizeof(T)];

            bool await_ready() const noexcept { return false; }

            template<typename TPromise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise> Parent) noexcept
            {
                Child.promise().Out = reinterpret_cast<T*>(Storage);
                new (Storage) T();
                BindCall(Parent.promise(), Child.promise());
                return Child;
            }

            T await_resume() noexcept
            {
                T* Value = std::launder(reinterpret_cast<T*>(Storage));
                T Result = Move(*Value);
                Value->~T();
                return Result;
            }
        };

        template<>
        struct TCallAwaiter<void>
        {
            typename TTask<void>::FHandle Child;

            bool await_ready() const noexcept { return false; }

            template<typename TPromise>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<TPromise> Parent) noexcept
            {
                BindCall(Parent.promise(), Child.promise());
                return Child;
            }

            void await_resume() const noexcept {}
        };
    }

    // Runs Child now and leaves this task's continuation for another worker to steal; Out is valid after Join.
    template<typename T>
    NODISCARD ForkDetail::TForkAwaiter<T> Fork(T* Out, TTask<T>&& Child) noexcept
    {
        auto Handle = Child.Release();
        Handle.promise().Out = Out;
        return { Handle };
    }

    NODISCARD inline ForkDetail::TForkAwaiter<void> Fork(TTask<void>&& Child) noexcept
    {
        return { Child.Release() };
    }

    // Runs Child to completion as an ordinary call and yields its result.
    template<typename T>
    NODISCARD ForkDetail::TCallAwaiter<T> Call(TTask<T>&& Child) noexcept
    {
        return { Child.Release() };
    }

    NODISCARD inline ForkDetail::FJoinTag Join() noexcept { return {}; }

    namespace ForkDetail
    {
        template<typename T> inline constexpr bool bIsTask            = false;
        template<typename T> inline constexpr bool bIsTask<TTask<T>>  = true;

        template<typename T> struct TTaskValue           { using Type = T; };
        template<typename T> struct TTaskValue<TTask<T>> { using Type = T; };

        template<typename TFunc, typename... TArgs>
        using TResultOf = typename TTaskValue<std::invoke_result_t<TFunc, TArgs...>>::Type;

        // A function returning TTask is started directly; anything else, such as a capturing lambda, is copied into this frame first.
        template<typename TFunc, typename... TArgs>
        inline constexpr bool bDirect = (std::is_pointer_v<std::decay_t<TFunc>> || std::is_function_v<std::remove_reference_t<TFunc>>)
                                        && bIsTask<std::invoke_result_t<TFunc, TArgs...>>;

        template<typename TFunc, typename... TArgs>
        TTask<TResultOf<TFunc, TArgs...>> OwnedCall(TFunc Func, TArgs... Args)
        {
            using TResult = std::invoke_result_t<TFunc, TArgs...>;
            if constexpr (bIsTask<TResult>)
            {
                co_return co_await Call(Move(Func)(Move(Args)...));
            }
            else if constexpr (std::is_void_v<TResult>)
            {
                Move(Func)(Move(Args)...);
                co_return;
            }
            else
            {
                co_return Move(Func)(Move(Args)...);
            }
        }

        template<typename TFunc>
        concept CPlainCallable = !bIsTask<std::decay_t<TFunc>> && std::is_invocable_v<std::decay_t<TFunc>>;
    }

    // Plain callables run in a frame of their own, so co_await Jobs::Fork([&] { ... }) works like a forked task.
    template<ForkDetail::CPlainCallable TFunc>
    NODISCARD auto Fork(TFunc&& Func)
    {
        return Fork(ForkDetail::OwnedCall<std::decay_t<TFunc>>(Forward<TFunc>(Func)));
    }

    template<typename T, ForkDetail::CPlainCallable TFunc>
    NODISCARD auto Fork(T* Out, TFunc&& Func)
    {
        return Fork(Out, ForkDetail::OwnedCall<std::decay_t<TFunc>>(Forward<TFunc>(Func)));
    }

    template<ForkDetail::CPlainCallable TFunc>
    NODISCARD auto Call(TFunc&& Func)
    {
        return Call(ForkDetail::OwnedCall<std::decay_t<TFunc>>(Forward<TFunc>(Func)));
    }

    namespace ForkDetail
    {
        template<typename TFunc, typename... TArgs>
        auto MakeRoot(EJobPriority Band, TFunc&& Func, TArgs&&... Args)
        {
            FStack* Stack = BeginRoot();
            auto Task = [&]
            {
                if constexpr (bDirect<TFunc, TArgs...>)
                {
                    return Forward<TFunc>(Func)(Forward<TArgs>(Args)...);
                }
                else
                {
                    return OwnedCall<std::decay_t<TFunc>, std::decay_t<TArgs>...>(Forward<TFunc>(Func), Forward<TArgs>(Args)...);
                }
            }();
            auto Handle = Task.Release();
            EndRoot(Handle.promise(), Stack, Band);
            return Handle;
        }
    }

    // Runs Func(Args...) as a task on this thread, helping the pool until the whole tree finishes. Func may return TTask or plain work.
    template<typename TFunc, typename... TArgs>
    auto SyncWait(EJobPriority Band, TFunc&& Func, TArgs&&... Args)
    {
        using T = ForkDetail::TResultOf<std::decay_t<TFunc>, std::decay_t<TArgs>...>;
        auto Handle = ForkDetail::MakeRoot(Band, Forward<TFunc>(Func), Forward<TArgs>(Args)...);
        if constexpr (std::is_void_v<T>)
        {
            ForkDetail::RunRootAndWait(Handle.promise());
        }
        else
        {
            T Result{};
            Handle.promise().Out = &Result;
            ForkDetail::RunRootAndWait(Handle.promise());
            return Result;
        }
    }

    template<typename TFunc, typename... TArgs>
    auto SyncWait(TFunc&& Func, TArgs&&... Args)
    {
        return SyncWait(EJobPriority::Normal, Forward<TFunc>(Func), Forward<TArgs>(Args)...);
    }

    // Queues Func(Args...) as a task and returns at once. Func and Args are copied into the task, so a capturing lambda is safe.
    template<typename TFunc, typename... TArgs>
    void Spawn(EJobPriority Band, TFunc&& Func, TArgs&&... Args)
    {
        auto Handle = ForkDetail::MakeRoot(Band, Forward<TFunc>(Func), Forward<TArgs>(Args)...);
        ForkDetail::SpawnRoot(Handle.promise());
    }

    inline constexpr uint32 kMaxThreadSlots = 128;

    struct FRuntimeHooks
    {
        void (*SegmentBegin)(uint32 Slot, bool bInjected) = nullptr;
        void (*SegmentEnd)(uint32 Slot)                   = nullptr;
        void (*IdleBegin)(uint32 Slot)                    = nullptr;
        void (*IdleEnd)(uint32 Slot)                      = nullptr;
    };

    // A one-shot event: co_await suspends a task until Signal, which requeues every waiter; Wait blocks a plain caller while helping.
    class FTaskEvent
    {
    public:

        FTaskEvent() = default;
        FTaskEvent(const FTaskEvent&) = delete;
        FTaskEvent& operator=(const FTaskEvent&) = delete;

        RUNTIME_API void Signal();
        RUNTIME_API void Wait() const;
        RUNTIME_API void Reset();

        NODISCARD bool IsSignaled() const { return Waiters.load(std::memory_order_acquire) == kSignaled; }

        struct FAwaiter
        {
            FTaskEvent& Event;

            bool await_ready() const noexcept { return Event.IsSignaled(); }

            template<typename TPromise>
            bool await_suspend(std::coroutine_handle<TPromise> Handle) noexcept { return Event.Enqueue(Handle.promise()); }

            void await_resume() const noexcept {}
        };

        FAwaiter operator co_await() noexcept { return { *this }; }

    private:

        static inline FFrame* const kSignaled = reinterpret_cast<FFrame*>(uintptr_t(1));

        RUNTIME_API bool Enqueue(FFrame& Frame);

        std::atomic<FFrame*> Waiters{nullptr};
        std::atomic<uint32>  Word{0};
    };

    // Counts down to zero, then releases everyone awaiting or waiting on it.
    class FLatch
    {
    public:

        explicit FLatch(uint32 InCount) : Count(InCount)
        {
            if (InCount == 0)
            {
                Event.Signal();
            }
        }

        void CountDown(uint32 By = 1)
        {
            if (Count.fetch_sub(By, std::memory_order_acq_rel) == By)
            {
                Event.Signal();
            }
        }

        void Add(uint32 By) { Count.fetch_add(By, std::memory_order_relaxed); }

        // Re-arms a finished latch for reuse, such as once per frame; nobody may be waiting on it.
        void Reset(uint32 InCount)
        {
            Event.Reset();
            Count.store(InCount, std::memory_order_relaxed);
            if (InCount == 0)
            {
                Event.Signal();
            }
        }
        void Wait() const { Event.Wait(); }
        NODISCARD bool IsDone() const { return Event.IsSignaled(); }
        FTaskEvent::FAwaiter operator co_await() noexcept { return Event.operator co_await(); }

    private:

        std::atomic<uint32> Count;
        FTaskEvent          Event;
    };

    // Spawn that signals Done once the whole task tree has finished, for code that starts work now and checks on it later.
    template<typename TFunc, typename... TArgs>
    void Spawn(EJobPriority Band, FTaskEvent& Done, TFunc&& Func, TArgs&&... Args)
    {
        auto Handle = ForkDetail::MakeRoot(Band, Forward<TFunc>(Func), Forward<TArgs>(Args)...);
        Handle.promise().Root.OnDone    = [](void* Event) { static_cast<FTaskEvent*>(Event)->Signal(); };
        Handle.promise().Root.OnDoneCtx = &Done;
        ForkDetail::SpawnRoot(Handle.promise());
    }

    // Spawn that holds Done open until the whole task tree has finished, for fanning several roots into one wait.
    template<typename TFunc, typename... TArgs>
    void Spawn(EJobPriority Band, FLatch& Done, TFunc&& Func, TArgs&&... Args)
    {
        Done.Add(1);
        auto Handle = ForkDetail::MakeRoot(Band, Forward<TFunc>(Func), Forward<TArgs>(Args)...);
        Handle.promise().Root.OnDone    = [](void* Latch) { static_cast<FLatch*>(Latch)->CountDown(); };
        Handle.promise().Root.OnDoneCtx = &Done;
        ForkDetail::SpawnRoot(Handle.promise());
    }

    struct FRuntimeConfig
    {
        static constexpr uint32 kAutoWorkers = ~0u;

        uint32        NumWorkers         = kAutoWorkers;
        uint32        NumExternalThreads = 32;
        FRuntimeHooks Hooks;

        // Fills each worker's victim order, nearest first; the default walks the other slots in rotation.
        void (*BuildStealOrder)(uint32 NumWorkers, uint32 NumSlots, uint16 (*Order)[kMaxThreadSlots]) = nullptr;
    };

    struct FRuntimeStats
    {
        uint32 QueuedRoots[kNumJobPriorities] = {};
        uint32 QueuedForks[kNumJobPriorities] = {};
        uint32 LiveRoots = 0;
        uint32 Sleepers  = 0;
    };

    RUNTIME_API void   StartRuntime(const FRuntimeConfig& Config);
    RUNTIME_API void   StopRuntime();
    RUNTIME_API bool   IsRuntimeRunning();
    RUNTIME_API uint32 GetNumWorkers();
    RUNTIME_API uint32 GetNumThreadSlots();
    RUNTIME_API uint32 GetThreadSlot();
    RUNTIME_API bool   IsWorkerThread();
    RUNTIME_API void   ReleaseThreadSlot();
    RUNTIME_API void   WaitForSpawned();
    RUNTIME_API void   GetRuntimeStats(FRuntimeStats& Out);

    // While alive, a blocking wait on this thread runs only its own subtree, never unrelated tasks; hold it around thread-identified state.
    class RUNTIME_API FTaskIsolation
    {
    public:

        FTaskIsolation();
        ~FTaskIsolation();

        FTaskIsolation(const FTaskIsolation&) = delete;
        FTaskIsolation& operator=(const FTaskIsolation&) = delete;
    };
}
