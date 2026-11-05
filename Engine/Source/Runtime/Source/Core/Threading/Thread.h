#pragma once
#include <type_traits>
#include <utility>

#include <atomic>
#include "Containers/Function.h"
#include "Core/Threading/Sync.h"
#include "Lumina.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"


namespace Lumina
{
    using FScopeLock            = TScopeLock<FMutex>;
    using FWriteScopeLock       = TScopeLock<FSharedMutex>;
    using FRecursiveScopeLock   = TScopeLock<FRecursiveMutex>;

    namespace Threading
    {
        // Tracy sorts lower hints higher with Main pinned on top, and groups the threads that share a hint.
        enum EThreadGroup : int32
        {
            ThreadGroup_Main    = 0,
            ThreadGroup_Physics = 10,
            ThreadGroup_Audio   = 20,
            ThreadGroup_Worker  = 100,
            ThreadGroup_Other   = 1000,
        };
    }

    enum class EThreadPriority : uint8
    {
        Normal,

        // Runs in the time the frame's threads leave, for throughput work nothing waits on.
        Background,

        // Ahead of everything, for a device callback with a hard deadline.
        TimeCritical,
    };

    // How a thread starts, applied on the thread itself before its body runs.
    struct FThreadOptions
    {
        const char*     Name = nullptr;
        int32           Group = Threading::ThreadGroup_Other;
        EThreadPriority Priority = EThreadPriority::Normal;
        uint32          StackSize = 0;
    };

    // Raised by the owner and read by the body, so a thread winds down at a point of its own choosing.
    class RUNTIME_API FThreadStop
    {
    public:

        NODISCARD bool ShouldStop() const noexcept { return Word.load(std::memory_order_acquire) != 0; }

        // Sleeps until a stop is requested or TimeoutMs passes, true when it was the stop.
        bool WaitForStop(uint32 TimeoutMs) const noexcept;

        void Request() noexcept;

    private:

        std::atomic<uint32> Word{ 0 };
    };

    // An OS thread that owns its callable, joined or detached before the handle goes, whose body may take the FThreadStop its owner raises.
    class RUNTIME_API FThread
    {
    public:

        FThread() noexcept = default;

        template <typename TCallable, typename... TArgs>
        requires (!std::is_same_v<std::decay_t<TCallable>, FThread> && !std::is_same_v<std::decay_t<TCallable>, FThreadOptions>)
        explicit FThread(TCallable&& Body, TArgs&&... Args)
            : FThread(FThreadOptions{}, std::forward<TCallable>(Body), std::forward<TArgs>(Args)...)
        {
        }

        template <typename TCallable, typename... TArgs>
        FThread(const FThreadOptions& Options, TCallable&& Body, TArgs&&... Args)
            : Stop(MakeShared<FThreadStop>())
        {
            Start(Options, new TMoveOnlyFunction<void()>(
                [Callable = std::decay_t<TCallable>(std::forward<TCallable>(Body)),
                 ...Arguments = std::decay_t<TArgs>(std::forward<TArgs>(Args)),
                 State = Stop]() mutable
                {
                    if constexpr (std::is_invocable_v<std::decay_t<TCallable>&, const FThreadStop&, std::decay_t<TArgs>&...>)
                    {
                        Invoke(Callable, *State, Arguments...);
                    }
                    else
                    {
                        Invoke(Callable, Arguments...);
                    }
                }));
        }

        FThread(FThread&& Other) noexcept : Handle(Other.Handle), Id(Other.Id), Stop(std::move(Other.Stop))
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

        // Raises the stop the body reads. A body asleep on its own event or socket still needs its owner to wake it.
        void RequestStop() noexcept;

        void Join() noexcept;
        void Detach() noexcept;

        FORCEINLINE bool joinable() const noexcept { return Joinable(); }
        FORCEINLINE void join() noexcept { Join(); }
        FORCEINLINE void detach() noexcept { Detach(); }

    private:

        void Start(const FThreadOptions& Options, TMoveOnlyFunction<void()>* Body);

        void*                   Handle = nullptr;
        uint64                  Id = 0;
        TSharedPtr<FThreadStop> Stop;
    };

    namespace Threading
    {
        #define CACHE_ALIGN alignas(::Lumina::Constants::kCacheLineSize)

        using ThreadID = uint64;

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

        RUNTIME_API bool SetCurrentThreadPriority(EThreadPriority Priority);

        // SMT siblings share PhysicalCore, last-level-cache peers share CacheGroup; both are dense ids with no meaning beyond equality.
        struct FCpuTopology
        {
            uint16 PhysicalCore = 0;
            uint16 CacheGroup   = 0;
        };

        // Returns 0 when the platform cannot report topology; callers must treat that as every processor equidistant.
        RUNTIME_API uint32 GetCpuTopology(FCpuTopology* Out, uint32 MaxCount);

        // A hint, not an affinity mask: the thread still migrates, so nothing may depend on it for correctness.
        RUNTIME_API void SetThreadIdealProcessor(uint32 LogicalProcessor);

        // The timeout makes a missed wake cost latency, not a hang; callers must re-check their own condition on every return.
        RUNTIME_API bool WaitOnAddress32(const volatile uint32* Address, uint32 Compare, uint32 TimeoutMs);

        // Never mix with std::atomic::wait on the same word: they use different wait queues on Windows.
        RUNTIME_API void WakeAllOnAddress32(const volatile uint32* Address);
        RUNTIME_API void WakeOneOnAddress32(const volatile uint32* Address);

        // Orders every running thread's earlier memory operations before this point; false where the OS offers nothing better than a local fence.
        RUNTIME_API bool ProcessWideMemoryBarrier();
    }
    

}
