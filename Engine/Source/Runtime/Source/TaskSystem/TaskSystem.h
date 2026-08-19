#pragma once

#include "TaskTypes.h"
#include "Scheduler/JobScheduler.h"
#include "Core/Profiler/Profile.h"
#include "Core/Threading/Thread.h"
#include "Memory/Memory.h"
#include "Platform/GenericPlatform.h"
#include <type_traits>
#include <utility>

namespace Lumina
{
    RUNTIME_API extern class FTaskSystem* GTaskSystem;

    namespace Task
    {
        RUNTIME_API void Initialize();
        RUNTIME_API void Shutdown();

        struct FParallelRange
        {
            uint32 Start;
            uint32 End;
            uint32 Thread;
        };

        // Worker-balanced chunk count for splitting [0, Num) with the given grain.
        RUNTIME_API uint32 ComputeChunkCount(uint32 Num, uint32 MinRange);

        inline constexpr uint32 kMaxChunks = 256;

        // Cursor-grab size for shared-cursor fan-outs: never finer than the pre-split balance floor.
        inline uint32 ComputeCursorGrain(uint32 Num, uint32 MinRange)
        {
            uint32 MaxChunks = Jobs::GetNumWorkers() * 4u;
            if (MaxChunks == 0)
            {
                MaxChunks = 1;
            }
            MaxChunks = MaxChunks < kMaxChunks ? MaxChunks : kMaxChunks;
            const uint32 Balanced = (Num + MaxChunks - 1) / MaxChunks;
            const uint32 Grain    = MinRange == 0 ? 1u : MinRange;
            return Grain < Balanced ? Balanced : Grain;
        }
    }

    class FTaskSystem
    {
    public:

        // Background worker threads.
        uint32 GetNumWorkers() const { return Jobs::GetNumWorkers(); }
        // Total addressable thread slots (workers + external). Use this to size per-thread arrays.
        uint32 GetNumTaskThreads() const { return Jobs::GetNumThreadSlots(); }

        uint32 RegisterExternalThread() { return Jobs::RegisterExternalThread(); }
        void   UnregisterExternalThread() { Jobs::UnregisterExternalThread(); }

        /** Num iterations split across worker threads; ranges are unordered. Returned handle is auto-cleaned. */
        RUNTIME_API FTaskHandle ScheduleLambda(uint32 Num, uint32 MinRange, TaskSetFunction&& Function, ETaskPriority Priority = ETaskPriority::Medium);

        template<typename TFunc>
        void ParallelFor(uint32 Num, TFunc&& Func, uint32 MinRange = 0, ETaskPriority Priority = ETaskPriority::Medium)
        {
            if (Num == 0)
            {
                return;
            }

            LUMINA_PROFILE_SECTION("Tasks::ParallelFor");

            using TDecayed = std::decay_t<TFunc>;
            TDecayed Stored = std::forward<TFunc>(Func);

            auto Thunk = +[](void* Ctx, uint32 Start, uint32 End, uint32 Thread)
            {
                TDecayed& F = *static_cast<TDecayed*>(Ctx);
                if constexpr (std::is_invocable_v<TDecayed, const Task::FParallelRange&>)
                {
                    F(Task::FParallelRange{ Start, End, Thread });
                }
                else
                {
                    for (uint32 i = Start; i < End; ++i)
                    {
                        if constexpr (std::is_invocable_v<TDecayed, uint32, uint32>)
                        {
                            F(i, Thread);
                        }
                        else if constexpr (std::is_invocable_v<TDecayed, uint32>)
                        {
                            F(i);
                        }
                    }
                }
            };

            ParallelForImpl(Num, MinRange, Priority, Thunk, &Stored);
        }

        template<typename TIterator, typename TFunc>
        void ParallelForEach(TIterator Begin, TIterator End, TFunc&& Func, ETaskPriority Priority = ETaskPriority::Medium)
        {
            const uint32 Num = static_cast<uint32>(End - Begin);
            if (Num == 0)
            {
                return;
            }

            LUMINA_PROFILE_SECTION("Tasks::ParallelForEach");

            using TDecayed = std::decay_t<TFunc>;
            struct FCtx
            {
                TDecayed  Func;
                TIterator Begin;
            };
            FCtx Ctx{ std::forward<TFunc>(Func), Begin };

            auto Thunk = +[](void* Raw, uint32 Start, uint32 End_, uint32 Thread)
            {
                FCtx& X = *static_cast<FCtx*>(Raw);
                if constexpr (std::is_invocable_v<TDecayed, TIterator, TIterator, uint32>)
                {
                    X.Func(X.Begin + Start, X.Begin + End_, Thread);
                    return;
                }
                else if constexpr (std::is_invocable_v<TDecayed, TIterator, TIterator>)
                {
                    X.Func(X.Begin + Start, X.Begin + End_);
                    return;
                }
                else
                {
                    for (uint32 i = Start; i < End_; ++i)
                    {
                        TIterator It = X.Begin + i;
                        if constexpr (std::is_invocable_v<TDecayed, decltype(*It)&, uint32>)
                        {
                            X.Func(*It, Thread);
                        }
                        else if constexpr (std::is_invocable_v<TDecayed, decltype(*It)&>)
                        {
                            X.Func(*It);
                        }
                        else if constexpr (std::is_invocable_v<TDecayed, TIterator, uint32>)
                        {
                            X.Func(It, Thread);
                        }
                        else if constexpr (std::is_invocable_v<TDecayed, TIterator>)
                        {
                            X.Func(It);
                        }
                    }
                }
            };

            ParallelForImpl(Num, 0, Priority, Thunk, &Ctx);
        }

        /** Block until every job submitted so far has completed. */
        RUNTIME_API void WaitForAll();

    private:

        using FParallelThunk = void (*)(void* Ctx, uint32 Start, uint32 End, uint32 Thread);

        // Splits [0, Num) into worker-balanced chunks, runs them, and waits. Single source of the
        // chunking policy; the templated entry points type-erase their callable into Thunk + Ctx.
        RUNTIME_API void ParallelForImpl(uint32 Num, uint32 MinRange, ETaskPriority Priority, FParallelThunk Thunk, void* Ctx);
    };

    namespace Task
    {
        RUNTIME_API FTaskHandle AsyncTask(uint32 Num, uint32 MinRange, TaskSetFunction&& Function, ETaskPriority Priority = ETaskPriority::Medium);

        template<typename TFunc>
        void ParallelFor(uint32 Num, TFunc&& Func, uint32 MinRange = 0, ETaskPriority Priority = ETaskPriority::Medium)
        {
            GTaskSystem->ParallelFor(Num, std::forward<TFunc>(Func), MinRange, Priority);
        }

        template<typename TIterator, typename TFunc>
        requires(std::is_same_v<typename std::iterator_traits<TIterator>::iterator_category, std::random_access_iterator_tag> ||
            std::is_same_v<typename std::iterator_traits<TIterator>::iterator_category, std::random_access_iterator_tag>)
        void ParallelForEach(TIterator Begin, TIterator End, TFunc&& Func, ETaskPriority Priority = ETaskPriority::Medium)
        {
            GTaskSystem->ParallelForEach(Begin, End, std::forward<TFunc>(Func), Priority);
        }
    }
}
