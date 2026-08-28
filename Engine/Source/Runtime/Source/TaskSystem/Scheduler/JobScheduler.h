#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/Vector.h"

// Fiber scheduler with counter-based dependencies. A job that waits does NOT block its worker: the
// fiber parks and resumes later, possibly on another worker. External threads assist-wait instead.
namespace Lumina::Jobs
{
    enum class EJobPriority : uint8
    {
        High   = 0,
        Normal = 1,
        Low    = 2,

        /** Throughput work that must never be charged to somebody else's latency: an assist-wait never
         *  dequeues from this band. Do NOT use it for a fan-out the submitting thread is about to wait on. */
        Background = 3,
    };

    // Band count, for anything sized per priority. Bands are dense and ordered most-urgent-first.
    static constexpr uint32 kNumJobPriorities = 4;

    // Highest-numbered band an assist-wait may dequeue from. See EJobPriority::Background.
    static constexpr uint32 kMaxAssistPriority = (uint32)EJobPriority::Low;

    // Opaque, pooled. A counter is "done" when its value reaches the waited-for target (default 0).
    struct FCounter;

    using FJobFunction  = void (*)(void* Arg, uint32 WorkerIndex);
    using FCompletionFn = void (*)(void* Ctx, uint32 WorkerIndex);

    struct FJobDecl
    {
        FJobFunction Function = nullptr;
        void*        Argument = nullptr;
        const char*  Name     = nullptr; // optional label (string literal) for the editor profiler
        // Opt in ONLY for a job that must suspend a fiber, i.e. one that calls ParkFiber (directly or through
        // FiberSync) and needs its worker released while it blocks. Everything else runs on the worker's own
        // stack with no fiber and no context switch; a WaitForCounter inside one assist-waits instead.
        bool         bMayPark = false;
    };

    struct FConfig
    {
        uint32 NumWorkerThreads   = 0; // 0 => hardware_concurrency() - 1
        uint32 NumExternalThreads = 8; // reserved thread slots for non-worker threads (main/render/physics/...)
        uint32 NumWorkFibers      = 0; // fibers created up front; 0 => default (kDefaultWorkFibers)
        // Hard ceiling on the pool, which grows on demand. A fiber is pinned while its job is BLOCKED, so a
        // fixed pool one short does not degrade -- it deadlocks. The ceiling only bounds a runaway.
        uint32 MaxWorkFibers      = 0;
        uint32 FiberStackSize     = 0; // per-fiber reserved stack in bytes; 0 => default (kDefaultFiberStack)
    };

    RUNTIME_API void Initialize(const FConfig& Config);
    RUNTIME_API void Shutdown();
    RUNTIME_API bool IsInitialized();

    // Background worker thread count.
    RUNTIME_API uint32 GetNumWorkers();
    // Total addressable thread slots (workers + external). Array-sizing bound for per-thread data.
    RUNTIME_API uint32 GetNumThreadSlots();
    // Dense slot in [0, GetNumThreadSlots()). Stable per OS thread, but a job's slot is only valid until
    // its first WaitForCounter -- a parked fiber may resume elsewhere, so re-read it after any wait.
    RUNTIME_API uint32 GetWorkerIndex();
    // True when the caller is a scheduler worker thread (so WaitForCounter yields instead of blocking).
    RUNTIME_API bool   IsWorkerThread();
    // True when the caller runs on a work fiber it may suspend. False on an external thread AND inside a
    // native (non-fiber) job, which is why it, not IsWorkerThread, gates every park.
    RUNTIME_API bool   CanParkFiber();

    // Claim/release a thread slot for a non-worker thread (render, physics, ...).
    RUNTIME_API uint32 RegisterExternalThread();
    RUNTIME_API void   UnregisterExternalThread();

    // Counters. AllocCounter hands back a fresh counter set to InitialValue.
    RUNTIME_API FCounter* AllocCounter(int32 InitialValue = 0);
    RUNTIME_API void      FreeCounter(FCounter* Counter);
    RUNTIME_API int32     GetCounterValue(const FCounter* Counter);
    // One-shot callback fired (on a worker) the moment the counter reaches 0. May free the counter.
    RUNTIME_API void      SetCounterCompletion(FCounter* Counter, FCompletionFn Fn, void* Ctx);

    // Submit jobs. The counter is incremented by Count up-front; each job decrements it on completion.
    RUNTIME_API void RunJobs(const FJobDecl* Jobs, uint32 Count, EJobPriority Priority, FCounter* Counter);
    // The same, Count times over one declaration. For a fan-out whose workers are interchangeable (every
    // cursor ParallelFor), so no caller builds an array of identical declarations to be copied again.
    RUNTIME_API void RunJobs(const FJobDecl& Decl, uint32 Count, EJobPriority Priority, FCounter* Counter);
    RUNTIME_API void RunJob(FJobFunction Fn, void* Arg, EJobPriority Priority, FCounter* Counter, const char* Name = nullptr, bool bMayPark = false);

    // Manually decrement a counter (not tied to a job). Fires waiters/completion at zero. Used for
    // graph fan-in where a node's completion signals a shared counter.
    RUNTIME_API void DecrementCounter(FCounter* Counter, int32 By = 1);

    // Wait until Counter <= Value. On a worker the calling fiber parks and the worker runs other work;
    // on an external thread it assist-waits (runs queued jobs inline) until satisfied.
    RUNTIME_API void WaitForCounter(FCounter* Counter, int32 Value = 0);

    // The same wait for a job already running and expected back in microseconds. An external thread
    // assists and spins rather than parking, because the syscall pair costs more than the wait.
    RUNTIME_API void WaitForCounterBusy(FCounter* Counter, int32 Value = 0);

    // Block the calling thread until every job submitted so far has completed.
    RUNTIME_API void WaitForAll();

    // ---- Generic fiber suspension (the foundation for fiber-aware mutexes / condition variables /
    // semaphores / futures, layered on top in FiberSync.h and Future.h) ----

    // Opaque token for a suspended worker fiber. A wait queue holds one of these for each parked
    // fiber and hands it back to ResumeFiber to make it runnable again.
    struct FFiberHandle
    {
        void* Fiber = nullptr;
        explicit operator bool() const { return Fiber != nullptr; }
    };

    // Runs on the scheduler fiber AFTER the parking fiber's context is saved. Link the handle into your
    // queue under your own lock. Return false to abort the park when the condition is already satisfied.
    using FParkFn = bool (*)(void* Ctx, FFiberHandle Handle);

    // Suspend the CURRENT worker fiber; returns only once ResumeFiber is called for it, possibly on a
    // different worker. Requires CanParkFiber(): the job must have been submitted with bMayPark.
    RUNTIME_API void ParkFiber(FParkFn OnPark, void* Ctx);

    // Make a previously parked fiber runnable again. Callable from any thread.
    RUNTIME_API void ResumeFiber(FFiberHandle Handle);

    // Use this rather than a thread_local for any "am I still the same logical execution?" flag: a fiber
    // can park and resume on a different worker, so thread identity does not survive a yield.
    RUNTIME_API FFiberHandle GetCurrentFiberHandle();

    // Marks a THREAD as a serial pump that must never yield: WaitForCounter assist-waits instead of
    // parking, which would strand the pump and could resume it on another thread. Pass nullptr to clear.
    RUNTIME_API void SetThreadNoParkGuard(const char* GuardName);

    // Run one queued job inline. The assist primitive for external wait loops -- running queued work
    // while spinning is what keeps things deadlock-free when the awaited signal depends on other jobs.
    RUNTIME_API bool AssistOneJob();

    // ---- Introspection (for the editor Task System profiler) ----

    // Cheap on-demand snapshot of pool occupancy. Always compiled (no standing cost).
    struct FJobLiveStats
    {
        uint32 NumWorkers    = 0;
        uint32 NumWorkFibers = 0;       // fibers created so far (the pool grows on demand)
        uint32 MaxWorkFibers = 0;       // hard ceiling it may grow to
        uint32 FibersFree    = 0;
        uint32 FibersReady   = 0;
        uint32 FibersInUse   = 0;       // NumWorkFibers - Free - Ready (clamped)
        uint32 QueueDepth[kNumJobPriorities] = {}; // per priority (approx), High..Background
        int64  InFlight      = 0;
    };
    RUNTIME_API void GetLiveStats(FJobLiveStats& Out);

    enum class EFiberState : uint8 { Free, Running, Parked, Ready };

    struct FFiberState
    {
        uint16      Index         = 0;
        EFiberState State         = EFiberState::Free;
        uint16      OwnerWorker   = 0xFFFF;  // valid when Running
        uint32      WaitCounterId = 0;       // valid when Parked
        const char* Name          = nullptr; // current/last job label
    };
    // Live per-fiber state, the task system "as it sits". Editor builds only (empty otherwise).
    RUNTIME_API void SnapshotFiberStates(TVector<FFiberState>& Out);

    struct FCounterState
    {
        uint32 Id            = 0;
        int32  Value         = 0;
        uint32 ParkedWaiters = 0;
    };
    // Counters that currently have parked waiters, the live dependency state. Editor builds only.
    RUNTIME_API void SnapshotActiveCounters(TVector<FCounterState>& Out);

    struct FWorkerCoreState
    {
        uint32 Worker = 0;     // worker index in [0, NumWorkers)
        uint32 Core   = 0;     // OS logical processor it last ran a job on
        bool   bBusy  = false; // currently executing a job fiber
    };
    // Per-worker core occupancy, for the editor's CPU-core view. Editor builds only (empty otherwise).
    RUNTIME_API void SnapshotWorkerCores(TVector<FWorkerCoreState>& Out);
}
