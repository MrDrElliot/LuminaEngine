#pragma once
#include <type_traits>
#include <utility>

#include "Containers/Function.h"
#include "Core/Threading/Sync.h"
#include "Platform/GenericPlatform.h"


namespace Lumina
{
    using FScopeLock            = TScopeLock<FMutex>;
    using FWriteScopeLock       = TScopeLock<FSharedMutex>;
    using FRecursiveScopeLock   = TScopeLock<FRecursiveMutex>;

    /** An OS thread that owns its callable; join or detach it before the handle goes away. */
    class RUNTIME_API FThread
    {
    public:

        FThread() noexcept = default;

        template <typename TCallable, typename... TArgs>
        requires (!std::is_same_v<std::decay_t<TCallable>, FThread>)
        explicit FThread(TCallable&& Body, TArgs&&... Args)
        {
            Start(new TMoveOnlyFunction<void()>(
                [Callable = std::decay_t<TCallable>(std::forward<TCallable>(Body)),
                 ...Arguments = std::decay_t<TArgs>(std::forward<TArgs>(Args))]() mutable
                {
                    Invoke(Callable, Arguments...);
                }));
        }

        FThread(FThread&& Other) noexcept : Handle(Other.Handle), Id(Other.Id)
        {
            Other.Handle = nullptr;
            Other.Id = 0;
        }

        FThread& operator=(FThread&& Other) noexcept;

        FThread(const FThread&) = delete;
        FThread& operator=(const FThread&) = delete;

        ~FThread();

        NODISCARD bool Joinable() const noexcept { return Handle != nullptr; }
        NODISCARD uint64 GetId() const noexcept { return Id; }

        void Join() noexcept;
        void Detach() noexcept;

        FORCEINLINE bool joinable() const noexcept { return Joinable(); }
        FORCEINLINE void join() noexcept { Join(); }
        FORCEINLINE void detach() noexcept { Detach(); }

    private:

        void Start(TMoveOnlyFunction<void()>* Body);

        void*  Handle = nullptr;
        uint64 Id = 0;
    };

    namespace Threading
    {
        // Pinned rather than std::hardware_destructive_interference_size, whose value differs between
        // compilers and versions and would silently change the layout of every CACHE_ALIGN type.
        constexpr size_t kCacheLineSize = 64;
        #define CACHE_ALIGN alignas(::Lumina::Threading::kCacheLineSize)

        using ThreadID = uint64;

        // Tracy timeline ordering: lower hint sorts higher (Main pinned to the top), and threads sharing a
        // hint are grouped together. Passed through to tracy::SetThreadNameWithHint / TracyFiberEnterHint.
        enum EThreadGroup : int32
        {
            ThreadGroup_Main    = 0,
            ThreadGroup_Physics = 10,
            ThreadGroup_Audio   = 20,
            ThreadGroup_Worker  = 100,
            ThreadGroup_Fiber   = 200,
            ThreadGroup_Other   = 1000,
        };

        RUNTIME_API void ThreadYield();
        RUNTIME_API uint64 GetThreadID();
        RUNTIME_API bool IsMainThread();

        RUNTIME_API uint32 GetNumThreads();

        RUNTIME_API void Sleep(uint64 Milliseconds);
        
        RUNTIME_API void Initialize(const char* MainThreadName);
        RUNTIME_API void Shutdown();

        RUNTIME_API void InitializeThreadHeap();
        RUNTIME_API void ShutdownThreadHeap();
        RUNTIME_API bool SetThreadName(const char* Name);

        // Names the current thread and assigns its Tracy timeline group (see EThreadGroup).
        RUNTIME_API bool SetThreadName(const char* Name, int32 GroupHint);

        // Opts the current thread out of EcoQoS power throttling.
        RUNTIME_API bool SetThreadPerformanceHint();

        // One entry per logical processor. SMT siblings share PhysicalCore; last-level-cache peers share
        // CacheGroup. Both are dense ids with no meaning beyond equality.
        struct FCpuTopology
        {
            uint16 PhysicalCore = 0;
            uint16 CacheGroup   = 0;
        };

        // Fills up to MaxCount entries and returns how many logical processors were described. Returns 0 when
        // the platform cannot report topology, which callers must treat as "every processor is equidistant".
        RUNTIME_API uint32 GetCpuTopology(FCpuTopology* Out, uint32 MaxCount);

        // Asks the scheduler to prefer LogicalProcessor for the calling thread. A hint, not an affinity mask:
        // the thread still migrates under load, so nothing may depend on it for correctness.
        RUNTIME_API void SetThreadIdealProcessor(uint32 LogicalProcessor);

        // Sleeps until the word at Address stops reading Compare or TimeoutMs elapses, whichever comes first.
        // The timeout is what makes a missed wake cost latency instead of a hang, so callers may treat the
        // wake as advisory and re-check their own condition on every return.
        RUNTIME_API bool WaitOnAddress32(const volatile uint32* Address, uint32 Compare, uint32 TimeoutMs);

        // Wakes everything sleeping in WaitOnAddress32 on this word. Never mix with std::atomic::wait on the
        // same word: the two use different wait queues on Windows.
        RUNTIME_API void WakeAllOnAddress32(const volatile uint32* Address);
    }
    

}
