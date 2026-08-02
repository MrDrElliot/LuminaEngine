#pragma once
#include "Containers/Function.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "ModuleAPI.h"


namespace Lumina::Jobs { struct FCounter; }

namespace Lumina
{
    // Schedules physics commands onto the fiber job pool. Kept as a façade (same Enqueue/Flush API) so
    // callers, KickPhysics/WaitForPhysics in WorldManager, the engine lifecycle, are unchanged. The
    // physics step is no longer a dedicated OS thread: each Enqueue submits a pool job, Flush waits the
    // job counter (assist-waits on the calling thread). One-frame-behind kick/join is preserved by the
    // caller (kick at FrameEnd, Flush at the next FrameStart).
    class RUNTIME_API FPhysicsThread
    {
    public:

        using FCommand = TMoveOnlyFunction<void()>;

        FPhysicsThread();
        ~FPhysicsThread();
        LE_NO_COPYMOVE(FPhysicsThread);

        void Start();
        void Stop();

        bool IsRunning() const { return bRunning.load(Atomic::MemoryOrderAcquire); }

        // DebugName must outlive the call (string literal). Submitted as a pool job; runs inline if the
        // system is not started.
        void Enqueue(const char* DebugName, FCommand&& Cmd);
        void EnqueueAndWait(const char* DebugName, FCommand&& Cmd);
        void Flush();

        uint64 EnqueuedCount() const { return CommandsEnqueued.load(Atomic::MemoryOrderAcquire); }
        uint64 CompletedCount() const { return CommandsCompleted.load(Atomic::MemoryOrderAcquire); }

        void WaitForCounter(uint64 Target);

        static FPhysicsThread& Get();

    private:

        struct FJobCtx
        {
            FPhysicsThread* Owner;
            FCommand        Cmd;
        };
        static void RunJobEntry(void* Arg, uint32 WorkerIndex);

        TAtomic<bool>       bRunning = false;
        Jobs::FCounter*     JobCounter = nullptr; // outstanding physics jobs; Flush waits this to zero

        TAtomic<uint64>     CommandsEnqueued = 0;
        TAtomic<uint64>     CommandsCompleted = 0;
    };

    RUNTIME_API extern FPhysicsThread* GPhysicsThread;


    class RUNTIME_API FPhysicsCommandFence
    {
    public:

        FPhysicsCommandFence() = default;
        LE_NO_COPYMOVE(FPhysicsCommandFence);

        void BeginFence();
        void Wait();
        bool IsComplete() const;

    private:

        uint64 TargetCounter = 0;
    };
}
