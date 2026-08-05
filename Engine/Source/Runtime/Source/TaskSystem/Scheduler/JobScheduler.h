#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/Array.h"

// Lumina job system, a fiber scheduler with counter-based dependencies.
//
// One worker thread per core drains lock-free MPMC job queues (one per priority). Each job runs on a
// pooled user-mode fiber. When a job waits on a counter it does NOT block the worker: the fiber is
// parked and the worker switches to other runnable work; the fiber is resumed later, possibly on a
// different worker (fibers migrate freely). That keeps every core productive, makes nested parallelism
// deadlock-free, and unlike a blocking wait the waiting stack is suspended rather than held.
//
// Non-worker ("external") threads, main/render/physics, are not on fibers. A WaitForCounter from an
// external thread keeps the old assist-wait behavior: it runs queued jobs inline until satisfied.
//
// This is the low-level core. The task-facing API (Task::ParallelFor / FTaskGraph / etc.)
// is layered on top in TaskSystem / TaskGraph.
namespace Lumina::Jobs
{
    enum class EJobPriority : uint8
    {
        High   = 0,
        Normal = 1,
        Low    = 2,

        /**
         * Throughput work that must never be charged to somebody else's latency.
         *
         * The three bands above are all urgency hints WITHIN the worker pool -- a worker with nothing
         * else to do will happily run a Low job, and a thread assist-waiting on an unrelated counter
         * will happily inline one. Background is the band that changes the second part: an assist-wait
         * never dequeues from it, so a long build cannot end up executing inside a frame's wait.
         *
         * Use it for work whose completion nothing in the current frame depends on and whose duration
         * dwarfs a frame -- terrain chunk builds, asset cooks, bake passes. Do NOT use it for a fan-out
         * the submitting thread is about to wait on: that thread would spin instead of helping, since
         * refusing to inline Background is exactly the point.
         */
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
    };

    struct FConfig
    {
        uint32 NumWorkerThreads   = 0; // 0 => hardware_concurrency() - 1
        uint32 NumExternalThreads = 8; // reserved thread slots for non-worker threads (main/render/physics/...)
        uint32 NumWorkFibers      = 0; // pooled fibers jobs run on; 0 => default (kDefaultWorkFibers)
        uint32 FiberStackSize     = 0; // per-fiber reserved stack in bytes; 0 => default (kDefaultFiberStack)
    };

    RUNTIME_API void Initialize(const FConfig& Config);
    RUNTIME_API void Shutdown();
    RUNTIME_API bool IsInitialized();

    // Background worker thread count.
    RUNTIME_API uint32 GetNumWorkers();
    // Total addressable thread slots (workers + external). Array-sizing bound for per-thread data.
    RUNTIME_API uint32 GetNumThreadSlots();
    // Dense slot in [0, GetNumThreadSlots()) for the OS thread currently running the caller. Stable per
    // OS thread, but a job's slot is only valid until its first WaitForCounter: a parked fiber may resume
    // on a different worker, so re-read this after any wait instead of caching the value across it.
    RUNTIME_API uint32 GetWorkerIndex();
    // True when the caller is a scheduler worker thread (so WaitForCounter yields instead of blocking).
    RUNTIME_API bool   IsWorkerThread();

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
    RUNTIME_API void RunJob(FJobFunction Fn, void* Arg, EJobPriority Priority, FCounter* Counter, const char* Name = nullptr);

    // Manually decrement a counter (not tied to a job). Fires waiters/completion at zero. Used for
    // graph fan-in where a node's completion signals a shared counter.
    RUNTIME_API void DecrementCounter(FCounter* Counter, int32 By = 1);

    // Wait until Counter <= Value. On a worker the calling fiber parks and the worker runs other work;
    // on an external thread it assist-waits (runs queued jobs inline) until satisfied.
    RUNTIME_API void WaitForCounter(FCounter* Counter, int32 Value = 0);

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

    // Called on the scheduler fiber, AFTER the parking fiber's context is fully saved, to publish the
    // fiber into a wait queue. Link the supplied handle into your queue under your own lock here.
    // Return true to actually park; return false to abort the park and resume immediately (used to
    // close the race where the wait condition was satisfied between the caller's check and the park).
    using FParkFn = bool (*)(void* Ctx, FFiberHandle Handle);

    // Suspend the CURRENT worker fiber. The worker is freed to run other jobs; this call returns only
    // once ResumeFiber(handle) is invoked for this fiber (possibly on a different worker). OnPark runs
    // the publish step described above. WORKER FIBERS ONLY, external threads must assist-wait instead
    // (see AssistOneJob); fiber-aware primitives branch on IsWorkerThread().
    RUNTIME_API void ParkFiber(FParkFn OnPark, void* Ctx);

    // Make a previously parked fiber runnable again. Callable from any thread.
    RUNTIME_API void ResumeFiber(FFiberHandle Handle);

    // Identity of the fiber currently executing, or a null handle on an external (non-worker) thread.
    // Use this instead of a thread_local for any "am I still the same logical execution?" flag: a
    // fiber can park and resume on a different worker, so thread identity does not survive a yield.
    RUNTIME_API FFiberHandle GetCurrentFiberHandle();

    // Mark the calling THREAD as running a serial pump that must never yield to the scheduler (the
    // render drain). While set, WaitForCounter on this thread does NOT park: it assist-waits instead,
    // servicing queued jobs inline until the counter clears. Parking would strand the pump until the
    // wait resolved, and the fiber could resume on a different thread, breaking the thread_local state
    // the pump depends on (for the render drain, the RHI recording keyed to that thread).
    //
    // So a fan-out inside a guarded region is safe and still parallel -- the guarded thread just helps
    // rather than yielding. It is reported once per process as a warning, since a blocking fan-out in
    // a serial pump is a design smell worth seeing, not because it is unsafe. Pass nullptr to clear.
    RUNTIME_API void SetThreadNoParkGuard(const char* GuardName);

    // Run one queued job inline if one is available; returns true if it ran one. The assist primitive
    // for external-thread (non-fiber) wait loops in fiber-aware sync objects, running queued work
    // while spinning keeps the system deadlock-free when the awaited signal depends on other jobs.
    RUNTIME_API bool AssistOneJob();

    // ---- Introspection (for the editor Task System profiler) ----

    // Cheap on-demand snapshot of pool occupancy. Always compiled (no standing cost).
    struct FJobLiveStats
    {
        uint32 NumWorkers    = 0;
        uint32 NumWorkFibers = 0;
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
