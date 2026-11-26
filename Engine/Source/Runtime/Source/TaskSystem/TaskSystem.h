#pragma once

#include "TaskSystem/Fork/ForkRuntime.h"

#include <iterator>
#include <type_traits>

namespace Lumina::Jobs
{
    // Starts the workers (LUMINA_JOB_WORKERS overrides the count) and gives the calling thread a slot.
    RUNTIME_API void Initialize();
    RUNTIME_API void Shutdown();

    struct FRange
    {
        uint32 Start;
        uint32 End;
        uint32 Thread;
    };

    namespace ParallelDetail
    {
        using FRangeThunk = void (*)(void* Context, uint32 Start, uint32 End, uint32 Thread);

        RUNTIME_API uint32      ComputeGrain(uint32 Num, uint32 MinRange);
        RUNTIME_API const char* CallSiteName(const char* Signature);
        RUNTIME_API void        Run(uint32 Num, uint32 MinRange, EJobPriority Band, FRangeThunk Thunk, void* Context, const char* CallSite);
        RUNTIME_API TTask<void> Split(FRangeThunk Thunk, void* Context, uint32 Begin, uint32 End, uint32 Grain, const char* CallSite);

        template<typename TFunc>
        FORCEINLINE void InvokeRange(TFunc& Func, uint32 Start, uint32 End, uint32 Thread)
        {
            if constexpr (std::is_invocable_v<TFunc&, const FRange&>)
            {
                Func(FRange{ Start, End, Thread });
            }
            else
            {
                for (uint32 Index = Start; Index < End; ++Index)
                {
                    if constexpr (std::is_invocable_v<TFunc&, uint32, uint32>)
                    {
                        Func(Index, Thread);
                    }
                    else
                    {
                        Func(Index);
                    }
                }
            }
        }

        template<typename TFunc>
        FRangeThunk MakeThunk()
        {
            return +[](void* Context, uint32 Start, uint32 End, uint32 Thread)
            {
                InvokeRange(*static_cast<TFunc*>(Context), Start, End, Thread);
            };
        }

        template<typename TFunc>
        const char* CallSite()
        {
#if defined(TRACY_ENABLE)
    #if defined(_MSC_VER)
            static const char* const Name = CallSiteName(__FUNCSIG__);
    #else
            static const char* const Name = CallSiteName(__PRETTY_FUNCTION__);
    #endif
            return Name;
#else
            return nullptr;
#endif
        }
    }

    // Blocks until Func has run over [0, Num). Func takes (Index), (Index, Thread) or (const FRange&).
    template<typename TFunc>
    void ParallelFor(uint32 Num, TFunc&& Func, uint32 MinRange = 0, EJobPriority Band = EJobPriority::Normal)
    {
        if (Num == 0)
        {
            return;
        }
        using TDecayed = std::decay_t<TFunc>;
        TDecayed Stored = Forward<TFunc>(Func);
        ParallelDetail::Run(Num, MinRange, Band, ParallelDetail::MakeThunk<TDecayed>(), &Stored, ParallelDetail::CallSite<TDecayed>());
    }

    template<typename TFunc>
    void ParallelForOrSerial(uint32 Num, uint32 MinNumToSplit, TFunc&& Func, uint32 MinRange = 0, EJobPriority Band = EJobPriority::Normal)
    {
        if (Num >= MinNumToSplit)
        {
            ParallelFor(Num, Forward<TFunc>(Func), MinRange, Band);
            return;
        }
        ParallelDetail::InvokeRange(Func, 0, Num, GetThreadSlot());
    }

    // Copies the items Keep accepts into Out in their original order, deciding in parallel so Keep may only read, with Mask as scratch the caller keeps.
    template<typename TItems, typename TOut, typename TMask, typename TPred>
    void ParallelFilter(const TItems& Items, TOut& Out, TMask& Mask, TPred&& Keep, uint32 MinNumToSplit = 4096)
    {
        Out.Reset();
        const uint32 Num = static_cast<uint32>(Items.Num());
        if (Num < MinNumToSplit)
        {
            for (uint32 Index = 0; Index < Num; ++Index)
            {
                if (Keep(Items[Index]))
                {
                    Out.Add(Items[Index]);
                }
            }
            return;
        }
        Mask.SetNum(Num);
        ParallelFor(Num, [&](uint32 Index) { Mask[Index] = Keep(Items[Index]) ? 1 : 0; }, 1024);
        for (uint32 Index = 0; Index < Num; ++Index)
        {
            if (Mask[Index] != 0)
            {
                Out.Add(Items[Index]);
            }
        }
    }

    template<typename TIterator, typename TFunc>
        requires std::random_access_iterator<TIterator>
    void ParallelForEach(TIterator Begin, TIterator End, TFunc&& Func, EJobPriority Band = EJobPriority::Normal)
    {
        ParallelFor(static_cast<uint32>(End - Begin), [&](uint32 Index) { Func(*(Begin + Index)); }, 0, Band);
    }

    // The same loop as a task, for code already running inside one: co_await Jobs::Call(Jobs::ForEach(Num, Func)).
    template<typename TFunc>
    TTask<void> ForEach(uint32 Num, TFunc Func, uint32 MinRange = 0)
    {
        if (Num == 0)
        {
            co_return;
        }
        co_await Call(ParallelDetail::Split(ParallelDetail::MakeThunk<TFunc>(), &Func, 0, Num,
                                            ParallelDetail::ComputeGrain(Num, MinRange), ParallelDetail::CallSite<TFunc>()));
    }
}
