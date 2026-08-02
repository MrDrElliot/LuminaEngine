#pragma once
#include <mutex>
#include <shared_mutex>
#include <thread>
#include "Platform/GenericPlatform.h"


namespace Lumina
{
    using FThread               = std::thread;
    using FSharedMutex          = std::shared_mutex;
    using FMutex                = std::mutex;
    using FRecursiveMutex       = std::recursive_mutex;
    using FScopeLock            = std::scoped_lock<FMutex>;
    using FReadScopeLock        = std::shared_lock<FSharedMutex>;
    using FWriteScopeLock       = std::unique_lock<FSharedMutex>;
    using FRecursiveScopeLock   = std::scoped_lock<FRecursiveMutex>;

    namespace Threading
    {
        constexpr auto kCacheLineSize = std::hardware_destructive_interference_size;
        #define CACHE_ALIGN alignas(std::hardware_destructive_interference_size)

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
        RUNTIME_API bool IsRenderThread();
        RUNTIME_API bool IsPhysicsThread();

        RUNTIME_API uint32 GetNumThreads();

        RUNTIME_API void SetRenderThread(std::thread::id ID);
        RUNTIME_API void SetPhysicsThread(std::thread::id ID);
        
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
    }
    

}
