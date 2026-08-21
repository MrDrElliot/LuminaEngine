#pragma once

#include "TaskTypes.h"
#include "TaskSystem.h"
#include "Future.h"
#include "Scheduler/JobScheduler.h"
#include "Containers/Vector.h"
#include "Core/Assertions/Assert.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Memory/Memory.h"
#include "Platform/GenericPlatform.h"

#include <coroutine>
#include <type_traits>
#include <utility>
#include <new>

// The type is TTask, not Task, because Lumina::Task is already a namespace of free helpers.
namespace Lumina
{
    template<typename T>
    class TTask;

    namespace CoroDetail
    {
        RUNTIME_API void ScheduleResume(std::coroutine_handle<> Handle, ETaskPriority Priority);

        struct FPromiseBase;
        RUNTIME_API void LaunchDetached(std::coroutine_handle<> Handle, FPromiseBase& Promise, ETaskPriority Priority);

        struct FTaskAccess;

        struct FPromiseBase
        {
            std::coroutine_handle<> Continuation{};
            Jobs::FCounter*         CompletionCounter = nullptr;
            ETaskPriority           Priority          = ETaskPriority::Medium;

            std::suspend_always initial_suspend() noexcept { return {}; }

            struct FFinalAwaiter
            {
                bool await_ready() const noexcept { return false; }

                template<typename P>
                std::coroutine_handle<> await_suspend(std::coroutine_handle<P> Handle) noexcept
                {
                    FPromiseBase&           Promise = Handle.promise();
                    std::coroutine_handle<> Next    = Promise.Continuation ? Promise.Continuation
                                                                           : std::noop_coroutine();
                    Jobs::FCounter*         Counter = Promise.CompletionCounter;

                    // Copy out above; once the counter drops another thread may free this frame.
                    if (Counter)
                    {
                        Jobs::DecrementCounter(Counter, 1);
                    }
                    return Next;
                }

                void await_resume() const noexcept {}
            };

            FFinalAwaiter final_suspend() noexcept { return {}; }

            void unhandled_exception() { ASSERT(false); }
        };
    }

    template<typename T>
    struct TTaskPromise : CoroDetail::FPromiseBase
    {
        alignas(T) unsigned char Storage[sizeof(T)];
        bool bHasValue = false;

        TTask<T> get_return_object() noexcept;

        template<typename U = T>
        void return_value(U&& Value)
        {
            ::new (static_cast<void*>(Storage)) T(std::forward<U>(Value));
            bHasValue = true;
        }

        T& Result() { return *reinterpret_cast<T*>(Storage); }

        ~TTaskPromise()
        {
            if (bHasValue)
            {
                Result().~T();
            }
        }
    };

    template<>
    struct TTaskPromise<void> : CoroDetail::FPromiseBase
    {
        TTask<void> get_return_object() noexcept;
        void        return_void() noexcept {}
    };

    template<typename T>
    class TTask
    {
    public:
        using promise_type = TTaskPromise<T>;
        using FHandle      = std::coroutine_handle<promise_type>;

        TTask() noexcept = default;
        explicit TTask(FHandle InHandle) noexcept : Handle(InHandle) {}

        TTask(TTask&& Other) noexcept : Handle(Other.Handle) { Other.Handle = {}; }
        TTask& operator=(TTask&& Other) noexcept
        {
            if (this != &Other)
            {
                if (Handle) { Handle.destroy(); }
                Handle       = Other.Handle;
                Other.Handle = {};
            }
            return *this;
        }

        TTask(const TTask&)            = delete;
        TTask& operator=(const TTask&) = delete;

        ~TTask() { if (Handle) { Handle.destroy(); } }

        bool IsValid() const noexcept { return static_cast<bool>(Handle); }

        auto operator co_await() && noexcept
        {
            struct FAwaiter
            {
                FHandle Coro;

                bool await_ready() const noexcept { return !Coro || Coro.done(); }

                void await_suspend(std::coroutine_handle<> Caller) noexcept
                {
                    Coro.promise().Continuation = Caller;
                    CoroDetail::ScheduleResume(Coro, Coro.promise().Priority);
                }

                T await_resume()
                {
                    if constexpr (!std::is_void_v<T>)
                    {
                        return Move(Coro.promise().Result());
                    }
                }
            };
            return FAwaiter{ Handle };
        }

        void SetPriority(ETaskPriority Priority) noexcept
        {
            if (Handle) { Handle.promise().Priority = Priority; }
        }

    private:
        friend struct CoroDetail::FTaskAccess;

        FHandle Release() noexcept { FHandle H = Handle; Handle = {}; return H; }

        FHandle Handle{};
    };

    namespace CoroDetail
    {
        struct FTaskAccess
        {
            template<typename T> static typename TTask<T>::FHandle Get(const TTask<T>& InTask) noexcept { return InTask.Handle; }
            template<typename T> static typename TTask<T>::FHandle Release(TTask<T>& InTask)     noexcept { return InTask.Release(); }
        };
    }

    template<typename T>
    TTask<T> TTaskPromise<T>::get_return_object() noexcept
    {
        return TTask<T>(TTask<T>::FHandle::from_promise(*this));
    }

    inline TTask<void> TTaskPromise<void>::get_return_object() noexcept
    {
        return TTask<void>(TTask<void>::FHandle::from_promise(*this));
    }

    // Runs a coroutine from non-coroutine code and blocks (fiber-aware) until it finishes.
    template<typename T>
    T SyncWait(TTask<T> InTask)
    {
        ASSERT(InTask.IsValid());
        typename TTask<T>::FHandle Handle = CoroDetail::FTaskAccess::Get(InTask);

        Jobs::FCounter* Done = Jobs::AllocCounter(1);
        Handle.promise().CompletionCounter = Done;
        CoroDetail::ScheduleResume(Handle, Handle.promise().Priority);

        Jobs::WaitForCounter(Done, 0);
        Jobs::FreeCounter(Done);

        if constexpr (!std::is_void_v<T>)
        {
            return Move(Handle.promise().Result());
        }
    }

    template<typename T>
    void Launch(TTask<T> InTask, ETaskPriority Priority = ETaskPriority::Medium)
    {
        ASSERT(InTask.IsValid());
        typename TTask<T>::FHandle Handle = CoroDetail::FTaskAccess::Release(InTask);
        CoroDetail::LaunchDetached(Handle, Handle.promise(), Priority);
    }

    template<typename T>
    auto operator co_await(TFuture<T> Future) noexcept
    {
        struct FAwaiter
        {
            TFuture<T> Future;

            bool await_ready() const noexcept { return Future.IsReady(); }

            void await_suspend(std::coroutine_handle<> Caller) const
            {
                // Continue() already runs on a worker, so resume inline there.
                Future.Continue([Caller]() { Caller.resume(); });
            }

            T await_resume()
            {
                if constexpr (std::is_void_v<T>) { Future.Get(); }
                else                             { return Future.Get(); }
            }
        };
        return FAwaiter{ Move(Future) };
    }

    // Runs all tasks concurrently, resumes once every one completes.
    inline auto WhenAll(TVector<TTask<void>> Tasks) noexcept
    {
        struct FAwaiter
        {
            TVector<TTask<void>>    Tasks;
            Jobs::FCounter*         Counter = nullptr;
            std::coroutine_handle<> Caller{};

            bool await_ready() const noexcept { return Tasks.empty(); }

            void await_suspend(std::coroutine_handle<> InCaller)
            {
                Caller  = InCaller;
                Counter = Jobs::AllocCounter(static_cast<int32>(Tasks.size()));
                Jobs::SetCounterCompletion(Counter, &FAwaiter::OnAll, this);

                for (TTask<void>& T : Tasks)
                {
                    TTask<void>::FHandle H = CoroDetail::FTaskAccess::Get(T);
                    H.promise().CompletionCounter = Counter;
                    CoroDetail::ScheduleResume(H, H.promise().Priority);
                }
            }

            void await_resume() const noexcept {}

            static void OnAll(void* Ctx, uint32 /*Worker*/)
            {
                FAwaiter*               Self = static_cast<FAwaiter*>(Ctx);
                std::coroutine_handle<> C    = Self->Caller;
                Jobs::FreeCounter(Self->Counter);
                C.resume();
            }
        };
        return FAwaiter{ Move(Tasks), nullptr, {} };
    }

    namespace Task
    {
        // Fans out data-parallel work from inside a coroutine without pinning a fiber.
        template<typename F>
        auto ParallelForAsync(uint32 Num, uint32 MinRange, F&& Func, ETaskPriority Priority = ETaskPriority::Medium)
        {
            using TFn = std::decay_t<F>;

            struct FAwaiter
            {
                uint32        Num;
                uint32        MinRange;
                ETaskPriority Priority;
                TFn           Func;

                Jobs::FCounter*         Counter = nullptr;
                std::coroutine_handle<> Caller{};
                uint32                  Grain   = 1;
                TAtomic<uint32>         Cursor{0};

                // A range that fits in one grain runs right here, on the awaiting coroutine: suspending to
                // dispatch a single job for it costs a counter, a submit and a wake to save nothing. Mirrors
                // the same early-out in the blocking FTaskSystem::ParallelForImpl.
                bool await_ready() noexcept
                {
                    if (Num == 0)
                    {
                        return true;
                    }
                    Grain = ::Lumina::Task::ComputeCursorGrain(Num, MinRange);
                    if (Num > Grain)
                    {
                        return false;
                    }
                    RunRange(0, Num);
                    return true;
                }

                void await_suspend(std::coroutine_handle<> InCaller)
                {
                    Caller  = InCaller;
                    Counter = Jobs::AllocCounter(0);
                    Jobs::SetCounterCompletion(Counter, &FAwaiter::OnComplete, this);

                    // Cursor fan-out: at most one puller job per worker instead of one job per chunk.
                    const uint32 Grabs = (Num + Grain - 1) / Grain;
                    uint32 K = Grabs < Jobs::GetNumWorkers() ? Grabs : Jobs::GetNumWorkers();
                    K = K < ::Lumina::Task::kMaxChunks ? K : ::Lumina::Task::kMaxChunks;

                    const Jobs::FJobDecl Decl{ &FAwaiter::RunChunk, this, "Coro::ParallelFor" };
                    Jobs::RunJobs(Decl, K, ToJobPriority(Priority), Counter);
                }

                void await_resume() const noexcept {}

                void RunRange(uint32 Start, uint32 End)
                {
                    // Re-read the slot per range: the body may wait, and a resumed fiber can migrate.
                    const uint32 Worker = Jobs::GetWorkerIndex();
                    if constexpr (std::is_invocable_v<TFn, const ::Lumina::Task::FParallelRange&>)
                    {
                        Func(::Lumina::Task::FParallelRange{ Start, End, Worker });
                    }
                    else
                    {
                        for (uint32 i = Start; i < End; ++i)
                        {
                            if constexpr (std::is_invocable_v<TFn, uint32, uint32>) { Func(i, Worker); }
                            else                                                     { Func(i); }
                        }
                    }
                }

                static void RunChunk(void* Arg, uint32 /*Worker*/)
                {
                    FAwaiter* Self = static_cast<FAwaiter*>(Arg);
                    for (;;)
                    {
                        const uint32 Start = Self->Cursor.fetch_add(Self->Grain, std::memory_order_relaxed);
                        if (Start >= Self->Num)
                        {
                            return;
                        }
                        const uint32 End = Self->Num - Start < Self->Grain ? Self->Num : Start + Self->Grain;
                        Self->RunRange(Start, End);
                    }
                }

                static void OnComplete(void* Ctx, uint32 /*Worker*/)
                {
                    FAwaiter*               Self = static_cast<FAwaiter*>(Ctx);
                    std::coroutine_handle<> C    = Self->Caller;
                    Jobs::FreeCounter(Self->Counter);
                    C.resume();
                }
            };

            return FAwaiter{ Num, MinRange, Priority, TFn(std::forward<F>(Func)) };
        }
    }
}
