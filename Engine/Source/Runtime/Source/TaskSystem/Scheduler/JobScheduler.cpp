#include "RuntimePCH.h"
#include "JobScheduler.h"

#include "Core/Threading/Thread.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Fiber.h"
#include "Memory/Memory.h"
#include "Memory/MemoryConcurrentQueue.h"
#include "Platform/Process/PlatformProcess.h"
#include "Containers/Vector.h"
#include "Containers/BoundedMPMCQueue.h"
#include "Core/Diagnostics/HangWatchdog.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/Math.h"
#include "Core/Profiler/Profile.h"
#include "Log/Log.h"

#if USING(WITH_EDITOR)
#include "JobProfiler.h"
#endif

#if defined(_MSC_VER)
    #include <intrin.h>
#else
    #include <immintrin.h>
#endif

#include <atomic>
#include <bit>
#include <cstdio>

// Jobs run on pooled user-mode fibers: a worker's WaitForCounter parks the fiber and switches to other
// work. External threads have no fiber and assist-wait instead, running queued jobs inline.
namespace Lumina::Jobs
{
    namespace
    {
        FORCEINLINE void CpuPause() { _mm_pause(); }

        constexpr uint32 kCounterPoolSize   = 8192;
        constexpr uint32 kDefaultWorkFibers = 256;
        constexpr uint32 kDefaultFiberStack = 512 * 1024;

        // Ceiling for on-demand growth. Only created fibers cost anything (32KB committed of a 512KB
        // reservation), so this bounds a genuine leak into a diagnosable error instead of address space.
        constexpr uint32 kDefaultMaxWorkFibers = 4096;

        struct FQueuedJob
        {
            FJobFunction Function = nullptr;
            void*        Argument = nullptr;
            FCounter*    Counter  = nullptr; // FCounter is incomplete here; pointer only
#if USING(WITH_EDITOR)
            const char*  Name     = nullptr; // label for the editor profiler; absent otherwise to shrink the queue element
#endif
        };

        // A pooled fiber jobs run on. Long-lived: it loops running one bound job then switching back to
        // the scheduler to be reused. Migrates between workers when a job parks and resumes elsewhere.
        struct FWorkFiber
        {
            Fibers::FFiber Handle = nullptr;
            FQueuedJob     Job{};   // bound by the scheduler immediately before switching in
            // Saved here while parked. The guard is conceptually per-fiber but must live in a thread_local to be
            // readable at the park site; without the save/restore it leaks onto the next fiber on this worker.
            const char*    NoParkGuard = nullptr;
#if USING(WITH_EDITOR)
            // Editor-only live state for the Task System profiler (the fiber grid / by-fiber timeline).
            uint16          Index       = 0;                 // pool index, stable
            TAtomic<uint8>  State{0};                   // EFiberState
            TAtomic<uint16> OwnerWorker{0xFFFF};        // worker last/currently running this fiber
            TAtomic<uint32> WaitCounterId{0};           // counter pool index when Parked
#endif
#if defined(TRACY_ENABLE)
            char            TracyName[24] = {};       // stable per-fiber label for Tracy's fiber zones
#endif
        };

#if defined(TRACY_ENABLE)
        thread_local bool GTracyFiberEntered = false;
        FORCEINLINE void TracyEnterFiber(FWorkFiber* F)
        {
            GTracyFiberEntered = TracyIsConnected;
            if (GTracyFiberEntered)
            {
                // Group hint clusters all work-fiber tracks together below the worker threads.
                TracyFiberEnterHint(F->TracyName, Threading::ThreadGroup_Fiber);
            }
        }
        FORCEINLINE void TracyLeaveFiber()
        {
            if (GTracyFiberEntered)
            {
                TracyFiberLeave;
                GTracyFiberEntered = false;
            }
        }
#else
        FORCEINLINE void TracyEnterFiber(void*) {}
        FORCEINLINE void TracyLeaveFiber()      {}
#endif

        // A fiber parked on a counter. Lives on the parking fiber's own stack (safe: the fiber stays
        // alive while parked), linked into FCounter::Waiters under the counter spinlock.
        struct FWaitNode
        {
            FWorkFiber* Fiber  = nullptr;
            int32       Target = 0;
            FWaitNode*  Next   = nullptr;
        };
    }

    // Public opaque type; the definition lives here. References FWaitNode from the unnamed namespace
    // above (accessible unqualified in the enclosing namespace).
    struct alignas(64) FCounter
    {
        TAtomic<int32>  Value{0};
        // Decrements currently inside ReleaseCounter, claimed before the value moves, so FreeCounter can wait
        // out a release still walking this counter. Shares the line Value is already written on.
        TAtomic<int32>  Releasers{0};
        FCompletionFn   Completion    = nullptr; // fired once when Value reaches 0
        void*           CompletionCtx = nullptr;

        TAtomic<uint32> WaitLock{0};         // spinlock guarding Waiters
        FWaitNode*      Waiters = nullptr;   // intrusive list, guarded by WaitLock
        TAtomic<bool>   HasWaiters{false};   // seq_cst gate so the lock-free decrement can skip the lock

        uint16          PoolIndex = 0xFFFF;
        bool            bPooled   = false;
    };

    namespace
    {
        using FJobQueue   = moodycamel::ConcurrentQueue<FQueuedJob, Memory::FTrackedConcurrentQueueTraits>;

        // These hand out slots from a pool of known size, so a bounded ring is the exact shape and it
        // allocates once at startup. The job queues stay unbounded: a submit burst has no such ceiling.
        using FIndexQueue = TBoundedMPMCQueue<uint16>;
        using FFiberQueue = TBoundedMPMCQueue<FWorkFiber*>;

        // Deferred action the scheduler fiber performs AFTER a work fiber has switched away.
        enum class EPending : uint8 { None, Park, Free, ParkFn };

        struct FPendingSwitch
        {
            EPending    Action  = EPending::None;
            FWorkFiber* Fiber   = nullptr; // the work fiber we just switched away from
            FCounter*   Counter = nullptr; // park target (Park)
            FWaitNode*  Node    = nullptr; // park node, on the parking fiber's stack (Park)
            FParkFn     ParkFn  = nullptr; // publish callback (ParkFn)
            void*       ParkCtx = nullptr; // callback context (ParkFn)
        };

        struct FThreadState
        {
            uint32         WorkerIndex    = ~0u;
            bool           bIsWorker      = false;
            Fibers::FFiber SchedulerFiber = nullptr; // this worker's scheduler fiber
            FWorkFiber*    CurrentFiber   = nullptr; // work fiber currently switched in on this worker
            FPendingSwitch Pending;
            uint32         StealCursor    = 0;       // rotating victim offset for work-stealing
            bool           bOwnsExternalSlot = false; // this thread holds an external slot to give back
            bool           bInlineRecovery   = false; // running a job on the scheduler fiber (pool wedged)

            FWorkFiber*    CachedFiber    = nullptr;

            // Hands the slot back if the thread never did. GetWorkerIndex() claims one LAZILY for any thread that
            // completes a job inline, and short-lived threads just exit -- the contract alone leaks one each.
            ~FThreadState();
        };
        thread_local FThreadState TLS;

        // Set while this thread runs a serial pump that must never yield. Parking under it strands the pump,
        // and the fiber can resume on a DIFFERENT thread, breaking thread_local state the pump relies on.
        thread_local const char* GNoParkGuardName = nullptr;

        // Per-worker queues plus a private wake futex. A burst submit is spread across these at enqueue time,
        // so a fan-out wakes every idle worker in PARALLEL instead of filing them through one CV mutex.
        struct alignas(64) FWorkerLocal
        {
            FJobQueue                  Queues[kNumJobPriorities];
            moodycamel::ConsumerToken* Home[kNumJobPriorities] = {};
            TAtomic<uint32>            WakeSignal{0}; // bumped (with notify) to wake this worker from its wait

            TAtomic<uint8>             HasCachedFiber{0};
            // Nonempty-band hint, one bit per priority, set before enqueue so a failed victim probe is one load.
            TAtomic<uint8>             BandMask{0};

            FWorkerLocal()
            {
                for (uint32 P = 0; P < kNumJobPriorities; ++P)
                {
                    Home[P] = Memory::New<moodycamel::ConsumerToken>(Queues[P]);
                }
            }
            ~FWorkerLocal()
            {
                for (uint32 P = 0; P < kNumJobPriorities; ++P)
                {
                    Memory::Delete(Home[P]);
                }
            }
        };

        struct FScheduler
        {
            uint32 NumWorkers     = 0;
            uint32 NumExternal    = 0;
            uint32 NumThreadSlots = 0;
            uint32 NumWorkFibers  = 0; // created up front
            uint32 MaxWorkFibers  = 0; // ceiling the pool may grow to
            uint32 FiberStackSize = 0;

            TVector<FThread> WorkerThreads;

            FWorkerLocal*   Workers = nullptr;        // [NumWorkers] per-worker queues (the job storage)
            alignas(64) TAtomic<uint32> NextSubmitWorker{0}; // rotating distribution start, anti-bias
            alignas(64) TAtomic<int64> AvailJobs{0};   // queued, not yet popped (idle-wake hint)

            // Counts only bands an assist-wait may take. Separate from AvailJobs because they diverge exactly
            // when it matters: queued Background work would pass the fast-fail and force a full scan every spin.
            alignas(64) TAtomic<int64> AvailAssistJobs{0};
            alignas(64) TAtomic<int64> InFlight{0};    // submitted, not yet completed (WaitForAll)

            // Constructed up front for MaxWorkFibers; only the first FibersCreated hold an actual stack. Flat and
            // never reallocated -- wait queues reference parked fibers by ADDRESS across any growth step.
            FWorkFiber*    WorkFibers = nullptr;
            alignas(64) TAtomic<uint32> FibersCreated{0};
            FFiberQueue    FreeFibers;           // idle, ready to be bound to a job
            FFiberQueue    ReadyFibers;          // parked fibers whose counter is now satisfied
            alignas(64) TAtomic<int64> ReadyCount{0};  // ReadyFibers size hint (idle-wake)

            FCounter*       CounterPool = nullptr;
            FIndexQueue     FreeCounters;

            // One bit per external slot. A slot MUST return to the pool on unregister: a monotonic counter leaks
            // one per cycle, and once the supply is gone threads alias each other's per-thread arrays.
            TAtomic<uint64> ExternalSlotsFree{0};
            TAtomic<bool>   bShutdown{false};

            // Wall-clock gate for the pool-wedged report, shared so only one worker ever prints it.
            alignas(64) TAtomic<int64> NextWedgeReportMs{0};

            // Bit per futex-parked worker. Load-bearing: WakeWorkers scans it to wake only idle workers, and only
            // as many as there are jobs. Own cache line.
            alignas(64) std::atomic<uint64>* IdleMask = nullptr; // [IdleMaskWords]
            uint32 IdleMaskWords = 0;

            // Threads currently inside a job dequeue, sampled by the editor advisor while profiling. Own cache
            // line: the diagnostic writes it from many workers and must not false-share the hot counters.
            alignas(64) TAtomic<int32> PoppersInFlight{0};

#if USING(WITH_EDITOR)
            // Per-worker OS-core occupancy for the editor's CPU view: which logical core a worker last
            // dispatched a job on, and whether it is running one right now. Sampled at fiber dispatch.
            struct FWorkerCoreSample { TAtomic<uint32> Core{0}; TAtomic<uint32> Gen{0}; TAtomic<uint8> Busy{0}; };
            FWorkerCoreSample* WorkerCores = nullptr; // [NumWorkers]
            // Bumped per SnapshotWorkerCores call so workers sample their core once per snapshot, not per job.
            alignas(64) TAtomic<uint32> CoreSampleGen{0};
#endif
        };

        FScheduler* G = nullptr;

        // Give a slot back. Safe to call for a thread that never held one, and after Shutdown.
        FORCEINLINE void ReleaseExternalSlot(uint32 ThreadSlot)
        {
            if (G != nullptr && ThreadSlot != ~0u && ThreadSlot >= G->NumWorkers)
            {
                const uint32 Bit = ThreadSlot - G->NumWorkers;
                if (Bit < G->NumExternal)
                {
                    G->ExternalSlotsFree.fetch_or(1ull << Bit, std::memory_order_release);
                }
            }
        }

        FThreadState::~FThreadState()
        {
            if (bOwnsExternalSlot)
            {
                ReleaseExternalSlot(WorkerIndex);
            }
        }

        bool HasWork()
        {
            return G->AvailJobs.load(std::memory_order_relaxed) > 0
                || G->ReadyCount.load(std::memory_order_relaxed) > 0
                || G->bShutdown.load(std::memory_order_acquire);
        }

        FORCEINLINE void SetWorkerIdle(uint32 W)
        {
            G->IdleMask[W >> 6].fetch_or(1ull << (W & 63), std::memory_order_relaxed);
        }
        FORCEINLINE void ClearWorkerIdle(uint32 W)
        {
            G->IdleMask[W >> 6].fetch_and(~(1ull << (W & 63)), std::memory_order_relaxed);
        }

        FORCEINLINE bool ClaimIdleWorker(uint32 W)
        {
            const uint64 Bit  = 1ull << (W & 63);
            const uint64 Prev = G->IdleMask[W >> 6].fetch_and(~Bit, std::memory_order_relaxed);
            return (Prev & Bit) != 0;
        }

        FORCEINLINE void SignalWorker(uint32 W)
        {
            G->Workers[W].WakeSignal.fetch_add(1, std::memory_order_release);
            G->Workers[W].WakeSignal.notify_one();
        }

        // The seq_cst fence pairs with the one a parking worker runs between publishing its idle bit and
        // re-checking HasWork, so a submit racing a park is never lost. Spurious bumps are harmless.
        void WakeWorkers(uint32 Count, uint32 PreferStart = 0, uint32 NumRecipients = 0)
        {
            if (Count == 0)
            {
                return;
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
            uint32 Woken = 0;

            const uint32 W = G->NumWorkers;
            for (uint32 i = 0; i < NumRecipients && Woken < Count; ++i)
            {
                uint32 Wk = PreferStart + i;
                if (Wk >= W)
                {
                    Wk -= W;
                }
                if (ClaimIdleWorker(Wk))
                {
                    SignalWorker(Wk);
                    ++Woken;
                }
            }

            for (uint32 Wd = 0; Wd < G->IdleMaskWords && Woken < Count; ++Wd)
            {
                uint64 Bits = G->IdleMask[Wd].load(std::memory_order_relaxed);
                while (Bits != 0 && Woken < Count)
                {
                    const uint32 B = (uint32)Math::CountTrailingZeros64(Bits);
                    Bits &= (Bits - 1);
                    const uint32 Wk = (Wd << 6) + B;
                    if (ClaimIdleWorker(Wk))
                    {
                        SignalWorker(Wk);
                        ++Woken;
                    }
                }
            }
        }

        constexpr uint32 kDirectWakeMax     = 8; // wakes the submitting thread issues itself
        constexpr uint32 kCascadeWakeFanout = 2; // wakes each worker relays onward

        FORCEINLINE void CascadeWake(uint32 Slot)
        {
            constexpr int64 kRelayThreshold = (int64)kDirectWakeMax;
            if (G->AvailJobs.load(std::memory_order_relaxed) <= kRelayThreshold
                && G->ReadyCount.load(std::memory_order_relaxed) <= kRelayThreshold)
            {
                return;
            }
            const uint32 Next = Slot + 1 < G->NumWorkers ? Slot + 1 : 0;
            WakeWorkers(kCascadeWakeFanout, Next, kCascadeWakeFanout);
        }

#if USING(WITH_EDITOR)
        // Concurrency-of-poppers sample (contention proxy for the advisor). Latched so inc/dec stay
        // balanced across a mid-call toggle. RAII so every early return is covered.
        struct FPopperScope
        {
            bool Diag;
            FPopperScope()
            {
                Diag = FJobProfiler::Get().IsEnabled();
                if (Diag)
                {
                    const uint32 Conc = static_cast<uint32>(G->PoppersInFlight.fetch_add(1, std::memory_order_relaxed)) + 1u;
                    FJobProfiler::Get().NotePop(Conc);
                }
            }
            ~FPopperScope()
            {
                if (Diag)
                {
                    G->PoppersInFlight.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        };
        #define POPPER_SCOPE() FPopperScope LE_PopperScope_
#else
        #define POPPER_SCOPE() ((void)0)
#endif

        struct FSubmitSpan
        {
            uint32 Start         = 0;
            uint32 NumRecipients = 0;
        };

        // Spread a batch across worker queues as contiguous slices from a rotating start, so each worker
        // gets a local run to begin on. Bulk-enqueue per slice keeps moodycamel's per-item overhead amortized.
        FSubmitSpan DistributeJobs(const FQueuedJob* Jobs, uint32 Count, int Prio)
        {
            const uint32 W     = G->NumWorkers;
            const uint32 Start = G->NextSubmitWorker.fetch_add(1, std::memory_order_relaxed) % W;
            const uint32 Base  = Count / W;
            const uint32 Rem   = Count % W;
            uint32 Idx = 0;
            uint32 Recipients = 0;
            for (uint32 i = 0; i < W; ++i)
            {
                const uint32 N = Base + (i < Rem ? 1u : 0u);
                if (N == 0)
                {
                    continue;
                }
                const uint32 Wk = (Start + i) % W;
                G->Workers[Wk].BandMask.fetch_or(static_cast<uint8>(1u << Prio), std::memory_order_relaxed);
                G->Workers[Wk].Queues[Prio].enqueue_bulk(Jobs + Idx, N);
                Idx += N;
                ++Recipients;
            }
            return FSubmitSpan{ Start, Recipients };
        }

        // Retires one job from the queued hints. Both counters move together for the assistable bands;
        // Background is deliberately absent from the assist hint, so it must not be decremented there.
        FORCEINLINE void NoteJobDequeued(uint32 Priority)
        {
            G->AvailJobs.fetch_sub(1, std::memory_order_relaxed);
            if (Priority <= kMaxAssistPriority)
            {
                G->AvailAssistJobs.fetch_sub(1, std::memory_order_relaxed);
            }
        }

        // Probe one victim's bands up to MaxPrio, set bits only, most-urgent first.
        FORCEINLINE bool TryStealFromWorker(FQueuedJob& Out, uint32 V, uint32 MaxPrio)
        {
            const uint8 Filter = static_cast<uint8>((1u << (MaxPrio + 1u)) - 1u);
            uint8 Mask = G->Workers[V].BandMask.load(std::memory_order_relaxed) & Filter;
            while (Mask != 0)
            {
                const uint32 P = (uint32)Math::CountTrailingZeros64(Mask);
                Mask &= static_cast<uint8>(Mask - 1);
                if (G->Workers[V].Queues[P].try_dequeue(Out))
                {
                    NoteJobDequeued(P);
                    return true;
                }
                // Empty despite the bit: clear it, then re-set if an enqueue raced the clear.
                G->Workers[V].BandMask.fetch_and(static_cast<uint8>(~(1u << P)), std::memory_order_relaxed);
                if (G->Workers[V].Queues[P].size_approx() != 0)
                {
                    G->Workers[V].BandMask.fetch_or(static_cast<uint8>(1u << P), std::memory_order_relaxed);
                }
            }
            return false;
        }

        // Worker fast path: drain own queues (priority order) first, then steal from other workers,
        // resuming the scan where the last steal landed. Decrements the queued hints on success.
        bool TryGetJobWorker(FQueuedJob& Out, uint32 Slot)
        {
            POPPER_SCOPE();
            FWorkerLocal& Self = G->Workers[Slot];
            // All bands, Background included: a real worker is exactly who should run it.
            for (uint32 P = 0; P < kNumJobPriorities; ++P)
            {
                if (Self.Queues[P].try_dequeue(*Self.Home[P], Out))
                {
                    NoteJobDequeued(P);
                    return true;
                }
            }
            // Only pay the cross-worker scan if the hint says work exists. AvailJobs is bumped BEFORE enqueue, so
            // <= 0 means genuinely nothing to steal and the O(workers*prio) probe can be skipped.
            if (G->AvailJobs.load(std::memory_order_relaxed) <= 0)
            {
                return false;
            }
            const uint32 W      = G->NumWorkers;
            const uint32 Cursor = TLS.StealCursor;
            for (uint32 i = 1; i < W; ++i)
            {
                const uint32 V = (Slot + Cursor + i) % W;
                if (TryStealFromWorker(Out, V, kNumJobPriorities - 1))
                {
                    TLS.StealCursor = Cursor + i;
                    return true;
                }
            }
            return false;
        }

        // Stops at kMaxAssistPriority: this steal is untargeted, so an assisting thread would otherwise adopt
        // an unrelated multi-hundred-ms build into the middle of a frame wait. That is why Background exists.
        bool TryStealAny(FQueuedJob& Out, uint32 MaxPrio = kMaxAssistPriority)
        {
            // Gate on the assist count when Background is excluded; both counts are bumped before any enqueue.
            const int64 Avail = MaxPrio > kMaxAssistPriority
                ? G->AvailJobs.load(std::memory_order_relaxed)
                : G->AvailAssistJobs.load(std::memory_order_relaxed);
            if (Avail <= 0)
            {
                return false;
            }
            POPPER_SCOPE();
            const uint32 W      = G->NumWorkers;
            const uint32 Cursor = TLS.StealCursor;
            for (uint32 i = 0; i < W; ++i)
            {
                const uint32 V = (Cursor + i) % W;
                if (TryStealFromWorker(Out, V, MaxPrio))
                {
                    TLS.StealCursor = Cursor + i + 1;
                    return true;
                }
            }
            return false;
        }

        // A worker thread in wedge recovery may take Background; main/render assist loops never do.
        FORCEINLINE uint32 AssistMaxPriority()
        {
            return TLS.bInlineRecovery ? (uint32)EJobPriority::Background : kMaxAssistPriority;
        }

        // Adopted work runs on the waiting thread, so its cost lands inside the caller's wait zone.
        FORCEINLINE void RunAdoptedJob(const FQueuedJob& Job, uint32 Slot)
        {
            LUMINA_PROFILE_SECTION_COLORED("Assist: Adopted Job", tracy::Color::Orange);
#if USING(WITH_EDITOR)
            if (Job.Name != nullptr)
            {
                LUMINA_PROFILE_TAG(Job.Name);
            }
#endif
            Job.Function(Job.Argument, Slot);
        }

        void PushReady(FWorkFiber* Fiber)
        {
#if USING(WITH_EDITOR)
            Fiber->State.store(static_cast<uint8>(EFiberState::Ready), std::memory_order_relaxed);
            // Affinity opportunity.
            if (FJobProfiler::Get().IsEnabled())
            {
                const uint16 Owner = Fiber->OwnerWorker.load(std::memory_order_relaxed);
                if (Owner < G->NumWorkers && (G->IdleMask[Owner >> 6].load(std::memory_order_relaxed) & (1ull << (Owner & 63))) != 0)
                {
                    FJobProfiler::Get().NoteAffinityOpportunity();
                }
            }
#endif
            G->ReadyFibers.Enqueue(Fiber);
            G->ReadyCount.fetch_add(1, std::memory_order_relaxed);
            WakeWorkers(1);
        }

        void LockCounter(FCounter* C)
        {
            uint32 Expected = 0;
            while (!C->WaitLock.compare_exchange_weak(Expected, 1u, std::memory_order_acquire, std::memory_order_relaxed))
            {
                Expected = 0;
                CpuPause();
            }
        }

        void UnlockCounter(FCounter* C)
        {
            C->WaitLock.store(0u, std::memory_order_release);
        }

        // MUST be entered holding a release guard claimed BEFORE the decrement. An external assist-wait never
        // joins the waiter list, so the poller can recycle this counter while we are still walking it.
        void ReleaseCounter(FCounter* Counter, int32 NewValue, uint32 WorkerIndex)
        {
            // Fast path: nothing reached zero and no fiber is parked.
            if (NewValue > 0 && !Counter->HasWaiters.load(std::memory_order_seq_cst))
            {
                Counter->Releasers.fetch_sub(1, std::memory_order_release);
                return;
            }

            FCompletionFn Completion = nullptr;
            void*         Ctx        = nullptr;
            FWaitNode*    Woken      = nullptr;

            LockCounter(Counter);
            {
                FWaitNode** Link = &Counter->Waiters;
                while (*Link != nullptr)
                {
                    FWaitNode* N = *Link;
                    if (Counter->Value.load(std::memory_order_seq_cst) <= N->Target)
                    {
                        *Link   = N->Next; // unlink
                        N->Next = Woken;
                        Woken   = N;
                    }
                    else
                    {
                        Link = &N->Next;
                    }
                }
                if (Counter->Waiters == nullptr)
                {
                    Counter->HasWaiters.store(false, std::memory_order_seq_cst);
                }
            }
            if (NewValue <= 0 && Counter->Completion != nullptr)
            {
                Completion          = Counter->Completion;
                Ctx                 = Counter->CompletionCtx;
                Counter->Completion = nullptr; // one-shot
            }
            UnlockCounter(Counter);

            for (FWaitNode* N = Woken; N != nullptr; )
            {
                FWaitNode* Next = N->Next;
                PushReady(N->Fiber);
                N = Next;
            }

            // Last touch of the counter, so the guard drops here -- before the completion callback, which owns the
            // counter lifetime and may free it. Holding it across would deadlock against our own FreeCounter.
            Counter->Releasers.fetch_sub(1, std::memory_order_release);

            if (Completion != nullptr)
            {
                Completion(Ctx, WorkerIndex); // owns the counter's lifetime
            }
        }

        // Drop a counter by By and run the release. The guard is claimed BEFORE the value moves, so a
        // waiter released by this decrement cannot recycle the counter out from under the release.
        FORCEINLINE void DecrementAndRelease(FCounter* Counter, int32 By, uint32 WorkerIndex)
        {
            Counter->Releasers.fetch_add(1, std::memory_order_acquire);
            const int32 NewValue = Counter->Value.fetch_sub(By, std::memory_order_seq_cst) - By;
            ReleaseCounter(Counter, NewValue, WorkerIndex);
        }

        void OnJobComplete(FCounter* Counter, uint32 WorkerIndex)
        {
            if (Counter == nullptr)
            {
                G->InFlight.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }

            Counter->Releasers.fetch_add(1, std::memory_order_acquire);
            const int32 NewValue = Counter->Value.fetch_sub(1, std::memory_order_seq_cst) - 1;
            ReleaseCounter(Counter, NewValue, WorkerIndex);
            // After the release, so WaitForAll cannot return while this job's completion is still running.
            G->InFlight.fetch_sub(1, std::memory_order_acq_rel);
        }

#if USING(WITH_EDITOR)
        // Stamp the OS core this worker is dispatching on / mark it (un)busy for the editor CPU view.
        void NoteWorkerCore(uint32 Worker, bool Busy)
        {
            if (G->WorkerCores == nullptr || Worker >= G->NumWorkers)
            {
                return;
            }
            if (Busy)
            {
                // Gen 0 means nothing has ever snapshotted; skip the core query on every dispatch then.
                const uint32 Gen = G->CoreSampleGen.load(std::memory_order_relaxed);
                FScheduler::FWorkerCoreSample& Sample = G->WorkerCores[Worker];
                if (Gen != 0 && Sample.Gen.load(std::memory_order_relaxed) != Gen)
                {
                    Sample.Core.store(Platform::GetCurrentCoreNumber(), std::memory_order_relaxed);
                    Sample.Gen.store(Gen, std::memory_order_relaxed);
                }
            }
            G->WorkerCores[Worker].Busy.store(Busy ? 1u : 0u, std::memory_order_relaxed);
        }

        // Editor profiler glue. Fiber-state stores are unconditional (so the live grid works even when
        // span recording is off); span/event recording self-gates on FJobProfiler::IsEnabled().
        void ProfBind(FWorkFiber* F, uint32 Worker)   // fresh job bound â†’ Running
        {
            F->State.store(static_cast<uint8>(EFiberState::Running), std::memory_order_relaxed);
            F->OwnerWorker.store(static_cast<uint16>(Worker), std::memory_order_relaxed);
            NoteWorkerCore(Worker, true);
            FJobProfiler& P = FJobProfiler::Get();
            if (P.IsEnabled())
            {
                P.SliceBegin(Worker, F->Index, F->Job.Name, FJobProfiler::NowMs());
            }
        }
        void ProfResume(FWorkFiber* F, uint32 Worker) // parked fiber resumed â†’ Running (counts migration)
        {
            const bool Migrated = F->OwnerWorker.load(std::memory_order_relaxed) != static_cast<uint16>(Worker);
            FJobProfiler& P = FJobProfiler::Get();
            if (P.IsEnabled())
            {
                P.NoteResume(Migrated);
            }
            F->State.store(static_cast<uint8>(EFiberState::Running), std::memory_order_relaxed);
            F->OwnerWorker.store(static_cast<uint16>(Worker), std::memory_order_relaxed);
            NoteWorkerCore(Worker, true);
            if (P.IsEnabled())
            {
                P.SliceBegin(Worker, F->Index, F->Job.Name, FJobProfiler::NowMs());
            }
        }
        void ProfEnd(uint32 Worker, bool Parked)      // the fiber that just switched back stopped running
        {
            NoteWorkerCore(Worker, false);
            FJobProfiler& P = FJobProfiler::Get();
            if (P.IsEnabled())
            {
                P.SliceEnd(Worker, Parked, FJobProfiler::NowMs());
            }
        }
        void ProfSubmit(uint32 Count, bool ByWorker)  // jobs entering the queue, tagged by origin thread
        {
            FJobProfiler& P = FJobProfiler::Get();
            if (P.IsEnabled())
            {
                P.NoteSubmit(Count, ByWorker);
            }
        }
        void ProfStarvation()                         // a fresh fiber-pool starvation episode
        {
            FJobProfiler& P = FJobProfiler::Get();
            if (P.IsEnabled())
            {
                P.NoteStarvation();
            }
        }
#endif

        FORCEINLINE FWorkFiber* AcquireFiber(uint32 Slot)
        {
            if (FWorkFiber* Cached = TLS.CachedFiber)
            {
                TLS.CachedFiber = nullptr;
                G->Workers[Slot].HasCachedFiber.store(0, std::memory_order_relaxed);
                return Cached;
            }
            FWorkFiber* F = nullptr;
            return G->FreeFibers.TryDequeue(F) ? F : nullptr;
        }

        FORCEINLINE void ReleaseFiber(uint32 Slot, FWorkFiber* F)
        {
            if (TLS.CachedFiber == nullptr)
            {
                TLS.CachedFiber = F;
                G->Workers[Slot].HasCachedFiber.store(1, std::memory_order_relaxed);
                return;
            }
            G->FreeFibers.Enqueue(F);
        }

        FORCEINLINE void FlushCachedFiber(uint32 Slot)
        {
            if (FWorkFiber* Cached = TLS.CachedFiber)
            {
                TLS.CachedFiber = nullptr;
                G->Workers[Slot].HasCachedFiber.store(0, std::memory_order_relaxed);
                G->FreeFibers.Enqueue(Cached);
            }
        }

        // Runs on the scheduler fiber after a work fiber switched back. The ONLY place a work fiber becomes
        // resumable, which is what guarantees its register/stack state is fully saved first.
        void ProcessPending()
        {
            FPendingSwitch P = TLS.Pending;
            TLS.Pending = FPendingSwitch{};

            switch (P.Action)
            {
            case EPending::None:
                return;

            case EPending::Free:
#if USING(WITH_EDITOR)
                ProfEnd(TLS.WorkerIndex, false);
                P.Fiber->State.store(static_cast<uint8>(EFiberState::Free), std::memory_order_relaxed);
#endif
                ReleaseFiber(TLS.WorkerIndex, P.Fiber);
                return;

            case EPending::Park:
                {
#if USING(WITH_EDITOR)
                    ProfEnd(TLS.WorkerIndex, true);
#endif
                    FCounter* C = P.Counter;
                    LockCounter(C);
                    // Announce the waiter BEFORE re-reading the value: this is the Dekker pairing for ReleaseCounter's
                    // lock-free fast path. The other order loses a wakeup forever when Target is non-zero.
                    C->HasWaiters.store(true, std::memory_order_seq_cst);
                    if (C->Value.load(std::memory_order_seq_cst) <= P.Node->Target)
                    {
                        // Satisfied between the fiber's fast-path check and now. Leave HasWaiters set if others are queued;
                        // a spurious true only costs the next decrement a trip through the locked path.
                        C->HasWaiters.store(C->Waiters != nullptr, std::memory_order_seq_cst);
                        UnlockCounter(C);
                        PushReady(P.Fiber);
                    }
                    else
                    {
                        P.Node->Next = C->Waiters;
                        C->Waiters   = P.Node;
                        UnlockCounter(C);
#if USING(WITH_EDITOR)
                        P.Fiber->State.store(static_cast<uint8>(EFiberState::Parked), std::memory_order_relaxed);
                        P.Fiber->WaitCounterId.store(C->PoolIndex, std::memory_order_relaxed);
#endif
                    }
                    return;
                }

            case EPending::ParkFn:
                {
#if USING(WITH_EDITOR)
                    ProfEnd(TLS.WorkerIndex, true);
#endif
                    // The callback links the fiber into its own wait queue under that queue's lock and returns whether it
                    // parked. If it declined, the condition was already satisfied and the fiber is runnable again.
                    const bool Parked = P.ParkFn(P.ParkCtx, FFiberHandle{ P.Fiber });
                    if (!Parked)
                    {
                        PushReady(P.Fiber);
                    }
#if USING(WITH_EDITOR)
                    else
                    {
                        P.Fiber->State.store(static_cast<uint8>(EFiberState::Parked), std::memory_order_relaxed);
                        P.Fiber->WaitCounterId.store(0, std::memory_order_relaxed);
                    }
#endif
                    return;
                }
            }
        }
        
        // Forward declaration: the pool grows at runtime, so the growth path needs the fiber entry point.
        void FiberMain(void* Arg);

        // Called only from the starvation path, the one state a stackful scheduler cannot resolve: the work
        // that would unblock the fibers needs the resource those fibers hold. CAS'd, so claims stay unique.
        FWorkFiber* GrowFiberPool()
        {
            uint32 Index = G->FibersCreated.load(std::memory_order_acquire);
            for (;;)
            {
                if (Index >= G->MaxWorkFibers)
                {
                    return nullptr;
                }
                if (G->FibersCreated.compare_exchange_weak(Index, Index + 1,
                        std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    break;
                }
            }

            FWorkFiber* F = &G->WorkFibers[Index];
#if USING(WITH_EDITOR)
            F->Index = static_cast<uint16>(Index);
#endif
#if defined(TRACY_ENABLE)
            (void)snprintf(F->TracyName, sizeof(F->TracyName), "Job Fiber %u", Index);
#endif
            F->Handle = Fibers::Create(G->FiberStackSize, &FiberMain, F);

            // Worth seeing: the pool outgrowing its configured size means real jobs are blocking deeper
            // than it was sized for. Logged in powers of two so a burst does not flood the log.
            const uint32 Count = Index + 1;
            if ((Count & (Count - 1)) == 0)
            {
                LOG_WARN("Job system: fiber pool grew to {} (of {} max) -- {} jobs are blocked at once.",
                    Count, G->MaxWorkFibers, Count);
            }
            return F;
        }

        constexpr int kHotPauseSpins = 1024; // tight _mm_pause: catch the next wave without a syscall
        constexpr int kHotYieldSpins = 128;  // OS-friendly tail; near-free when idle, holds hot when busy

        // Pauses a starving worker burns before concluding the pool is the constraint. Covers the ordinary
        // transient -- another worker holding the last free fiber across a queue scan.
        constexpr uint32 kStarveSpinBudget = 4096;
        // Yields a wedged worker takes before dropping to a sleep. A pool at its ceiling is not a
        // latency-sensitive state; what matters is getting off the core.
        constexpr uint32 kWedgeYieldSpins  = 64;
        constexpr int64  kWedgeReportMs    = 2000;

        // Rate limited by wall clock and shared by every worker. Counting spins per worker instead had thirty
        // workers logging every few ms, contending with the very threads that had to clear the stall.
        bool ShouldReportWedge()
        {
            const int64 NowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            int64 Next = G->NextWedgeReportMs.load(std::memory_order_relaxed);
            if (NowMs < Next)
            {
                return false;
            }
            return G->NextWedgeReportMs.compare_exchange_strong(Next, NowMs + kWedgeReportMs,
                std::memory_order_acq_rel, std::memory_order_relaxed);
        }

        // Wedge recovery: run a queued job on the scheduler fiber posing as external, so any wait inside it assist-spins instead of parking a fiber that does not exist.
        bool RunOneJobInline(uint32 Slot)
        {
            FQueuedJob Job;
            if (!TryGetJobWorker(Job, Slot))
            {
                return false;
            }

            FWorkFiber* SavedFiber = TLS.CurrentFiber;
            const char* SavedGuard = GNoParkGuardName;
            TLS.bIsWorker       = false;
            TLS.bInlineRecovery = true;
            TLS.CurrentFiber    = nullptr;
            GNoParkGuardName    = nullptr;

            RunAdoptedJob(Job, Slot);
            OnJobComplete(Job.Counter, Slot);

            GNoParkGuardName    = SavedGuard;
            TLS.CurrentFiber    = SavedFiber;
            TLS.bInlineRecovery = false;
            TLS.bIsWorker       = true;
            return true;
        }

#if USING(WITH_EDITOR)
        // Names the wedge: tallies parked fibers by job label so the log says WHAT filled the pool.
        void ReportWedgeCulprits()
        {
            struct FTally { const char* Name = nullptr; uint32 Count = 0; };
            constexpr uint32 kMaxTallies = 12;
            FTally Tallies[kMaxTallies];
            uint32 NumTallies = 0;
            uint32 Untallied  = 0;

            const uint32 Created = G->FibersCreated.load(std::memory_order_acquire);
            for (uint32 i = 0; i < Created; ++i)
            {
                FWorkFiber& F = G->WorkFibers[i];
                if (F.State.load(std::memory_order_relaxed) != static_cast<uint8>(EFiberState::Parked))
                {
                    continue;
                }
                const char* Name = F.Job.Name ? F.Job.Name : "<unnamed>";
                bool bFound = false;
                for (uint32 t = 0; t < NumTallies; ++t)
                {
                    if (Tallies[t].Name == Name)
                    {
                        ++Tallies[t].Count;
                        bFound = true;
                        break;
                    }
                }
                if (!bFound)
                {
                    if (NumTallies < kMaxTallies)
                    {
                        Tallies[NumTallies++] = FTally{ Name, 1 };
                    }
                    else
                    {
                        ++Untallied;
                    }
                }
            }

            for (uint32 t = 0; t < NumTallies; ++t)
            {
                LOG_ERROR("  wedge: {} parked fiber(s) running '{}'", Tallies[t].Count, Tallies[t].Name);
            }
            if (Untallied > 0)
            {
                LOG_ERROR("  wedge: {} parked fiber(s) across further distinct job names", Untallied);
            }
        }
#endif

        void WaitForWork()
        {
            for (int Spin = 0; Spin < kHotPauseSpins; ++Spin)
            {
                if (HasWork())
                {
                    return;
                }
                CpuPause();
            }
            for (int Spin = 0; Spin < kHotYieldSpins; ++Spin)
            {
                if (HasWork())
                {
                    return;
                }
                Threading::ThreadYield();
            }
            
            const uint32 W   = TLS.WorkerIndex;
            const uint32 Sig = G->Workers[W].WakeSignal.load(std::memory_order_acquire);
            SetWorkerIdle(W);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (HasWork())
            {
                ClearWorkerIdle(W);
                return;
            }
            FlushCachedFiber(W);
#if USING(WITH_EDITOR)
            FJobProfiler::Get().IdleBegin(W, FJobProfiler::NowMs());
#endif
            G->Workers[W].WakeSignal.wait(Sig, std::memory_order_acquire);
            ClearWorkerIdle(W);
#if USING(WITH_EDITOR)
            FJobProfiler::Get().IdleEnd(W, FJobProfiler::NowMs());
#endif
        }

        // The scheduler fiber's loop. Prefers resuming parked fibers (drains in-flight work and frees
        // fibers) over binding fresh jobs.
        void SchedulerLoop()
        {
            const uint32 Slot = TLS.WorkerIndex;
            uint32 StarveSpins = 0;

            while (true)
            {
                ProcessPending();

                if (G->bShutdown.load(std::memory_order_acquire))
                {
                    break;
                }

                FWorkFiber* Ready = nullptr;
                if (G->ReadyFibers.TryDequeue(Ready))
                {
                    G->ReadyCount.fetch_sub(1, std::memory_order_relaxed);
                    CascadeWake(Slot); // before the switch -- a releasing counter can dump many at once
                    TLS.CurrentFiber = Ready;
#if USING(WITH_EDITOR)
                    ProfResume(Ready, Slot);
#endif
                    TracyEnterFiber(Ready);
                    Fibers::Switch(Ready->Handle);
                    TracyLeaveFiber();
                    StarveSpins = 0;
                    continue;
                }

                // Claim a free fiber first, then a job, so a job is never popped without somewhere to run
                FWorkFiber* Free = AcquireFiber(Slot);
                if (Free != nullptr)
                {
                    StarveSpins = 0;
                }
                else if (G->AvailJobs.load(std::memory_order_relaxed) > 0)
                {
                    // Every fiber is blocked on work that cannot start until a fiber frees up -- a cycle no scheduling
                    // breaks. Nearly always a blink, so spin a bounded budget before blaming the pool size.
                    if (++StarveSpins < kStarveSpinBudget)
                    {
                        CpuPause();
                        continue;
                    }
#if USING(WITH_EDITOR)
                    if (StarveSpins == kStarveSpinBudget)
                    {
                        ProfStarvation(); // count distinct episodes, not every spin
                    }
#endif
                    Free = GrowFiberPool(); // null only at the ceiling
                    if (Free == nullptr)
                    {
                        if (ShouldReportWedge())
                        {
                            LOG_ERROR("Job system: fiber pool wedged at the {} fiber ceiling with jobs still queued. "
                                      "Every fiber is blocked on work that cannot start; running queued jobs inline "
                                      "to recover. Raise FConfig::MaxWorkFibers, or stop blocking inside jobs that "
                                      "are themselves spawned in bulk.",
                                      G->MaxWorkFibers);
#if USING(WITH_EDITOR)
                            ReportWedgeCulprits();
#endif
                        }
                        if (RunOneJobInline(Slot))
                        {
                            StarveSpins = kStarveSpinBudget; // stay armed, skip the pause ramp between jobs
                            continue;
                        }
                        // Get off the core. Whatever is still running needs it far more, and every idle worker spinning flat
                        // out is how a stall that would have cleared becomes the permanent one this branch reports.
                        if (StarveSpins < kStarveSpinBudget + kWedgeYieldSpins)
                        {
                            Threading::ThreadYield();
                        }
                        else
                        {
                            Threading::Sleep(1);
                        }
                        continue;
                    }
                }

                if (Free != nullptr)
                {
                    FQueuedJob Job;
                    if (TryGetJobWorker(Job, Slot))
                    {
                        CascadeWake(Slot); // relay the ramp before running, so the two overlap
                        Free->Job        = Job;
                        TLS.CurrentFiber = Free;
#if USING(WITH_EDITOR)
                        ProfBind(Free, Slot);
#endif
                        TracyEnterFiber(Free);
                        Fibers::Switch(Free->Handle);
                        TracyLeaveFiber();
                        StarveSpins = 0;
                        continue;
                    }
                    ReleaseFiber(Slot, Free);
                }

                WaitForWork();
            }

            FlushCachedFiber(Slot);
        }

        // Entry for every pooled work fiber. Loops forever: run the bound job, switch back to be reused.
        void FiberMain(void* /*Arg*/)
        {
            while (true)
            {
                FWorkFiber* Self = TLS.CurrentFiber; // set by the scheduler before switching in
                FQueuedJob  Job  = Self->Job;

                Job.Function(Job.Argument, TLS.WorkerIndex);
                OnJobComplete(Job.Counter, TLS.WorkerIndex);

                // A job that returns without clearing its guard must not leak it onto the next fiber.
                GNoParkGuardName  = nullptr;
                Self->NoParkGuard = nullptr;

                TLS.Pending = FPendingSwitch{ EPending::Free, Self, nullptr, nullptr };
                Fibers::Switch(TLS.SchedulerFiber);
                // Resumes here when the scheduler binds a new job to this fiber and switches back in.
            }
        }

        void WorkerThreadMain(uint32 WorkerIndex)
        {
            TLS.WorkerIndex = WorkerIndex;
            TLS.bIsWorker   = true;

            char Name[32];
            (void)snprintf(Name, sizeof(Name), "Lumina Worker %u", WorkerIndex);
            Threading::SetThreadName(Name, Threading::ThreadGroup_Worker);
            Threading::SetThreadPerformanceHint();
            Memory::InitializeThreadHeap();

            TLS.SchedulerFiber = Fibers::ThreadToFiber();

            SchedulerLoop();

            Fibers::FiberToThread();
            Memory::ShutdownThreadHeap();
        }
    }

    namespace
    {
        // Thread stacks cannot show parked fibers -- their stacks live off-thread -- so a stall dump needs
        // this to see which fiber is parked on which counter and what job it was running.
        void JobsHangReporter()
        {
            FJobLiveStats Stats;
            GetLiveStats(Stats);
            // Background depth belongs here: a stall caused by bulk-submitted throughput work shows up in
            // that band and nowhere else, so leaving it out hides the most likely cause.
            LOG_ERROR("Job system: {} workers, fibers {}/{} (free={} ready={} in-use={}), "
                      "queued jobs {}/{}/{}/{} (H/N/L/Bg), in-flight {}",
                Stats.NumWorkers, Stats.NumWorkFibers, Stats.MaxWorkFibers,
                Stats.FibersFree, Stats.FibersReady, Stats.FibersInUse,
                Stats.QueueDepth[0], Stats.QueueDepth[1], Stats.QueueDepth[2], Stats.QueueDepth[3],
                Stats.InFlight);

            TVector<FFiberState> Fibers;
            SnapshotFiberStates(Fibers);   // editor builds only; empty otherwise
            for (const FFiberState& F : Fibers)
            {
                switch (F.State)
                {
                case EFiberState::Running:
                    LOG_ERROR("  fiber {}: RUNNING on worker {} job '{}'", F.Index, F.OwnerWorker, F.Name ? F.Name : "<unnamed>");
                    break;
                case EFiberState::Parked:
                    LOG_ERROR("  fiber {}: PARKED on counter {} job '{}'", F.Index, F.WaitCounterId, F.Name ? F.Name : "<unnamed>");
                    break;
                case EFiberState::Ready:
                    LOG_ERROR("  fiber {}: READY (awaiting a worker) job '{}'", F.Index, F.Name ? F.Name : "<unnamed>");
                    break;
                default:
                    break;
                }
            }

            TVector<FCounterState> Counters;
            SnapshotActiveCounters(Counters);   // editor builds only; empty otherwise
            for (const FCounterState& C : Counters)
            {
                LOG_ERROR("  counter {}: value {} with {} parked waiter(s)", C.Id, C.Value, C.ParkedWaiters);
            }
        }
    }

    void Initialize(const FConfig& Config)
    {
        ASSERT(G == nullptr);

        G = Memory::New<FScheduler>();

        const uint32 Hardware = Threading::GetNumThreads();
        G->NumWorkers     = Config.NumWorkerThreads != 0 ? Config.NumWorkerThreads : (Hardware > 2 ? Hardware - 1 : 1);
        G->NumExternal    = Config.NumExternalThreads != 0 ? Config.NumExternalThreads : 8;
        G->NumThreadSlots = G->NumWorkers + G->NumExternal;
        G->NumWorkFibers  = Config.NumWorkFibers != 0 ? Config.NumWorkFibers : kDefaultWorkFibers;
        G->MaxWorkFibers  = Config.MaxWorkFibers != 0 ? Config.MaxWorkFibers : kDefaultMaxWorkFibers;
        G->MaxWorkFibers  = G->MaxWorkFibers < G->NumWorkFibers ? G->NumWorkFibers : G->MaxWorkFibers;
        G->FiberStackSize = Config.FiberStackSize != 0 ? Config.FiberStackSize : kDefaultFiberStack;

        // External thread slots start out all free (bit set == available). Capped at 64 by the bitmask;
        // more than that many non-worker threads is not a configuration this engine has.
        ASSERT(G->NumExternal <= 64);
        G->ExternalSlotsFree.store(G->NumExternal == 64 ? ~0ull : ((1ull << G->NumExternal) - 1ull),
            std::memory_order_relaxed);

        // Built BEFORE workers start so the home consumer tokens exist when they spin up. Placement-new so
        // each FWorkerLocal constructs its queues and tokens in place.
        G->Workers = static_cast<FWorkerLocal*>(Memory::Malloc(sizeof(FWorkerLocal) * G->NumWorkers, alignof(FWorkerLocal)));
        for (uint32 i = 0; i < G->NumWorkers; ++i)
        {
            new (&G->Workers[i]) FWorkerLocal();
        }

        // One idle-mask word per 64 workers (the wake-targeting bitset). Zeroed: nobody parked yet.
        G->IdleMaskWords = (G->NumWorkers + 63u) / 64u;
        G->IdleMask = static_cast<std::atomic<uint64>*>(Memory::Malloc(sizeof(std::atomic<uint64>) * G->IdleMaskWords, alignof(std::atomic<uint64>)));
        for (uint32 i = 0; i < G->IdleMaskWords; ++i)
        {
            new (&G->IdleMask[i]) std::atomic<uint64>(0);
        }

        // Sized for the CEILING and 2x the pool: an item only goes back in because it came out, but a ring
        // sized exactly to its pool rides the wrap boundary and every put-back spins through a mid-claim cell.
        G->FreeCounters.Initialize(kCounterPoolSize * 2);
        G->FreeFibers.Initialize(G->MaxWorkFibers * 2);
        G->ReadyFibers.Initialize(G->MaxWorkFibers * 2);

        G->CounterPool = static_cast<FCounter*>(Memory::Malloc(sizeof(FCounter) * kCounterPoolSize, alignof(FCounter)));
        for (uint32 i = 0; i < kCounterPoolSize; ++i)
        {
            FCounter* C = new (&G->CounterPool[i]) FCounter();
            C->bPooled   = true;
            C->PoolIndex = static_cast<uint16>(i);
            G->FreeCounters.Enqueue(static_cast<uint16>(i));
        }

        // Built BEFORE workers start. Storage covers the growth ceiling and is never reallocated -- wait
        // queues hold raw FWorkFiber* while a fiber is parked. GrowFiberPool adds stacks on demand.
        G->WorkFibers = static_cast<FWorkFiber*>(Memory::Malloc(sizeof(FWorkFiber) * G->MaxWorkFibers, alignof(FWorkFiber)));
        for (uint32 i = 0; i < G->MaxWorkFibers; ++i)
        {
            new (&G->WorkFibers[i]) FWorkFiber();
        }
        for (uint32 i = 0; i < G->NumWorkFibers; ++i)
        {
            FWorkFiber* F = &G->WorkFibers[i];
#if USING(WITH_EDITOR)
            F->Index = static_cast<uint16>(i);
#endif
#if defined(TRACY_ENABLE)
            (void)snprintf(F->TracyName, sizeof(F->TracyName), "Job Fiber %u", i);
#endif
            F->Handle = Fibers::Create(G->FiberStackSize, &FiberMain, F);
            G->FreeFibers.Enqueue(F);
        }
        G->FibersCreated.store(G->NumWorkFibers, std::memory_order_release);

#if USING(WITH_EDITOR)
        G->WorkerCores = static_cast<FScheduler::FWorkerCoreSample*>(
            Memory::Malloc(sizeof(FScheduler::FWorkerCoreSample) * G->NumWorkers, alignof(FScheduler::FWorkerCoreSample)));
        for (uint32 i = 0; i < G->NumWorkers; ++i)
        {
            new (&G->WorkerCores[i]) FScheduler::FWorkerCoreSample();
        }
#endif

        G->WorkerThreads.reserve(G->NumWorkers);
        for (uint32 i = 0; i < G->NumWorkers; ++i)
        {
            G->WorkerThreads.emplace_back(WorkerThreadMain, i);
        }

        LOG_DISPLAY("Job system online: {} workers, {} thread slots, {} fibers ({} max, {}KB stacks).",
            G->NumWorkers, G->NumThreadSlots, G->NumWorkFibers, G->MaxWorkFibers, G->FiberStackSize / 1024);

        // Idempotent under Initialize/Shutdown cycles: the watchdog reporter list is append-only.
        static bool bReporterRegistered = false;
        if (!bReporterRegistered)
        {
            bReporterRegistered = true;
            HangWatchdog::RegisterReporter(&JobsHangReporter);
        }
    }

    void Shutdown()
    {
        if (G == nullptr)
        {
            return;
        }

        G->bShutdown.store(true, std::memory_order_release);
        // Wake every worker (not just the idle-masked ones) so all observe shutdown promptly.
        for (uint32 i = 0; i < G->NumWorkers; ++i)
        {
            G->Workers[i].WakeSignal.fetch_add(1, std::memory_order_release);
            G->Workers[i].WakeSignal.notify_one();
        }

        for (FThread& Thread : G->WorkerThreads)
        {
            if (Thread.joinable())
            {
                Thread.join();
            }
        }

        // Workers have joined, so no thread has any work fiber switched in and no growth can be in
        // flight. Every claimed index is fully created by now; the rest never got a stack.
        const uint32 Created = G->FibersCreated.load(std::memory_order_acquire);
        for (uint32 i = 0; i < Created; ++i)
        {
            Fibers::Destroy(G->WorkFibers[i].Handle);
        }
        for (uint32 i = 0; i < G->MaxWorkFibers; ++i)
        {
            G->WorkFibers[i].~FWorkFiber();
        }
        void* FiberMem = G->WorkFibers;
        Memory::Free(FiberMem);

        // Workers have joined, so the per-worker queues are quiescent. Tear down after the fibers.
        if (G->Workers != nullptr)
        {
            for (uint32 i = 0; i < G->NumWorkers; ++i)
            {
                G->Workers[i].~FWorkerLocal();
            }
            void* WorkersMem = G->Workers;
            Memory::Free(WorkersMem);
            G->Workers = nullptr;
        }

        if (G->IdleMask != nullptr)
        {
            // std::atomic<uint64> is trivially destructible, just free the storage.
            void* MaskMem = G->IdleMask;
            Memory::Free(MaskMem);
            G->IdleMask = nullptr;
        }

#if USING(WITH_EDITOR)
        if (G->WorkerCores != nullptr)
        {
            for (uint32 i = 0; i < G->NumWorkers; ++i)
            {
                G->WorkerCores[i].~FWorkerCoreSample();
            }
            Memory::Free(G->WorkerCores);
        }
#endif

        void* PoolMem = G->CounterPool;
        Memory::Free(PoolMem);

        Memory::Delete(G);
        G = nullptr;
    }

    bool IsInitialized() { return G != nullptr; }

    uint32 GetNumWorkers()     { return G ? G->NumWorkers     : 0; }
    uint32 GetNumThreadSlots() { return G ? G->NumThreadSlots : 1; }
    bool   IsWorkerThread()    { return TLS.bIsWorker; }

    namespace
    {
        // Claims the lowest free slot, or falls back to the last one when the supply is gone. That fallback
        // is silent corruption -- the index sizes every per-thread array in the engine -- hence the error.
        uint32 ClaimExternalSlot()
        {
            uint64 Free = G->ExternalSlotsFree.load(std::memory_order_relaxed);
            while (Free != 0)
            {
                const uint32 Bit = static_cast<uint32>(Math::CountTrailingZeros64(Free));
                if (G->ExternalSlotsFree.compare_exchange_weak(Free, Free & ~(1ull << Bit),
                        std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    TLS.bOwnsExternalSlot = true;
                    return Bit;
                }
            }

            static std::atomic<bool> bReported{ false };
            bool Expected = false;
            if (bReported.compare_exchange_strong(Expected, true, std::memory_order_relaxed))
            {
                LOG_ERROR("Job system: all {} external thread slots are claimed; this thread has to share "
                          "slot {} with a live one. Per-thread arrays indexed by GetWorkerIndex() are no "
                          "longer race-free. Raise FConfig::NumExternalThreads or unregister retired threads.",
                          G->NumExternal, G->NumExternal - 1);
            }
            TLS.bOwnsExternalSlot = false;
            return G->NumExternal - 1;
        }
    }

    uint32 GetWorkerIndex()
    {
        if (TLS.WorkerIndex != ~0u)
        {
            return TLS.WorkerIndex;
        }
        // Stray thread running a job inline: lazily claim an external slot.
        TLS.WorkerIndex = G->NumWorkers + ClaimExternalSlot();
        return TLS.WorkerIndex;
    }

    uint32 RegisterExternalThread()
    {
        TLS.WorkerIndex = G->NumWorkers + ClaimExternalSlot();
        TLS.bIsWorker   = false;
        return TLS.WorkerIndex;
    }

    void UnregisterExternalThread()
    {
        // Return the slot. Without this a register/unregister cycle (PIE start/stop, plugin load, script
        // thread) burns one permanently, and once the supply runs out every later thread aliases a live one.
        if (TLS.bOwnsExternalSlot)
        {
            ReleaseExternalSlot(TLS.WorkerIndex);
        }
        TLS.bOwnsExternalSlot = false;
        TLS.WorkerIndex       = ~0u;
    }

    FCounter* AllocCounter(int32 InitialValue)
    {
        FCounter* Counter;
        uint16 Index;
        if (G->FreeCounters.TryDequeue(Index))
        {
            Counter = &G->CounterPool[Index];
        }
        else
        {
            Counter = Memory::New<FCounter>();
            Counter->bPooled   = false;
            Counter->PoolIndex = 0xFFFF;
        }

        Counter->Value.store(InitialValue, std::memory_order_relaxed);
        Counter->Completion    = nullptr;
        Counter->CompletionCtx = nullptr;
        Counter->Waiters = nullptr;
        Counter->WaitLock.store(0u, std::memory_order_relaxed);
        Counter->HasWaiters.store(false, std::memory_order_relaxed);
        return Counter;
    }

    void FreeCounter(FCounter* Counter)
    {
        if (Counter == nullptr)
        {
            return;
        }
        // Wait out any decrement still inside its release. The decrement that ended the caller's wait may
        // still be walking this counter list; recycling mid-walk splices one graph's waiters into another.
        while (Counter->Releasers.load(std::memory_order_acquire) != 0)
        {
            CpuPause();
        }
        if (Counter->bPooled)
        {
            G->FreeCounters.Enqueue(Counter->PoolIndex);
        }
        else
        {
            Memory::Delete(Counter);
        }
    }

    int32 GetCounterValue(const FCounter* Counter)
    {
        return Counter ? Counter->Value.load(std::memory_order_acquire) : 0;
    }

    void SetCounterCompletion(FCounter* Counter, FCompletionFn Fn, void* Ctx)
    {
        Counter->Completion    = Fn;
        Counter->CompletionCtx = Ctx;
    }

    void RunJobs(const FJobDecl* Jobs, uint32 Count, EJobPriority Priority, FCounter* Counter)
    {
        if (Count == 0)
        {
            return;
        }
        
        LUMINA_PROFILE_SCOPE();

#if USING(WITH_EDITOR)
        ProfSubmit(Count, TLS.bIsWorker); // tag fork-join (worker) vs externally-fed work for the advisor
#endif

        if (Counter != nullptr)
        {
            Counter->Value.fetch_add(static_cast<int32>(Count), std::memory_order_acq_rel);
        }
        G->InFlight.fetch_add(static_cast<int64>(Count), std::memory_order_acq_rel);

        const int Prio = static_cast<int>(Priority);

        constexpr uint32 kBatch = 256;
        FQueuedJob Batch[kBatch];
        FSubmitSpan Span;
        for (uint32 Base = 0; Base < Count; Base += kBatch)
        {
            const uint32 N = (Count - Base) < kBatch ? (Count - Base) : kBatch;
            // Per batch, not per submit: identical cost for ordinary submits, smaller stale-hint window for huge ones.
            G->AvailJobs.fetch_add(static_cast<int64>(N), std::memory_order_relaxed);
            if ((uint32)Prio <= kMaxAssistPriority)
            {
                G->AvailAssistJobs.fetch_add(static_cast<int64>(N), std::memory_order_relaxed);
            }
            for (uint32 i = 0; i < N; ++i)
            {
                Batch[i].Function = Jobs[Base + i].Function;
                Batch[i].Argument = Jobs[Base + i].Argument;
                Batch[i].Counter  = Counter;
#if USING(WITH_EDITOR)
                Batch[i].Name     = Jobs[Base + i].Name;
#endif
            }
            const FSubmitSpan BatchSpan = DistributeJobs(Batch, N, Prio);
            if (Base == 0)
            {
                Span = BatchSpan;
            }
        }

        const uint32 DirectWakes = Count < kDirectWakeMax ? Count : kDirectWakeMax;
        WakeWorkers(DirectWakes, Span.Start, Span.NumRecipients);
    }

    void RunJob(FJobFunction Fn, void* Arg, EJobPriority Priority, FCounter* Counter, const char* Name)
    {
        FJobDecl Decl{ Fn, Arg, Name };
        RunJobs(&Decl, 1, Priority, Counter);
    }

    void DecrementCounter(FCounter* Counter, int32 By)
    {
        if (Counter == nullptr)
        {
            return;
        }
        DecrementAndRelease(Counter, By, GetWorkerIndex());
    }

    void WaitForCounter(FCounter* Counter, int32 Value)
    {
        if (Counter == nullptr)
        {
            return;
        }

        if (Counter->Value.load(std::memory_order_acquire) <= Value)
        {
            return; // fast path
        }

        // A fiber whose thread runs a no-yield pump.
        const bool bMustNotYield = GNoParkGuardName != nullptr;

        if (bMustNotYield)
        {
            // Once per process: the assist is correct, but a blocking fan-out inside a serial pump is
            // still worth knowing about. Per-occurrence logging is what made this unreadable.
            static std::atomic<bool> bReported{ false };
            bool Expected = false;
            if (bReported.compare_exchange_strong(Expected, true, std::memory_order_relaxed))
            {
                LOG_WARN("Jobs: a counter wait occurred while this thread runs '{0}', which must never yield. "
                         "Servicing queued jobs inline instead of parking. Further occurrences are not logged.",
                         GNoParkGuardName);
            }
        }

        if (TLS.bIsWorker && !bMustNotYield)
        {
            // Park and yield to the scheduler, which links us into the counter. The releasing decrement touches
            // nothing of a waited no-completion counter once we are spliced out, so the caller may reclaim it.
            FWaitNode Node;
            Node.Fiber  = TLS.CurrentFiber;
            Node.Target = Value;
            Node.Next   = nullptr;

#if USING(WITH_EDITOR)
            { FJobProfiler& Prof = FJobProfiler::Get(); if (Prof.IsEnabled())
                {
                    Prof.NotePark();
                }
            }
#endif
            TLS.Pending = FPendingSwitch{ EPending::Park, TLS.CurrentFiber, Counter, &Node };
            FWorkFiber* Self = TLS.CurrentFiber;
            Self->NoParkGuard = GNoParkGuardName;
            GNoParkGuardName  = nullptr;
            Fibers::Switch(TLS.SchedulerFiber);
            // Resumed here once satisfied, possibly on a different worker thread.
            GNoParkGuardName = TLS.CurrentFiber->NoParkGuard;
            return;
        }

        // Assist-wait: run queued jobs inline until satisfied, never yielding to the scheduler. Taken by
        // an external thread (no fiber to park) and by a fiber under a no-yield guard (see above).
        const uint32 Slot = GetWorkerIndex();
        uint32 IdleSpins = 0;
        while (Counter->Value.load(std::memory_order_acquire) > Value)
        {
            FQueuedJob Job;
            if (TryStealAny(Job, AssistMaxPriority()))
            {
                RunAdoptedJob(Job, Slot);
                OnJobComplete(Job.Counter, Slot);
                IdleSpins = 0;
            }
            else if (++IdleSpins < 256)
            {
                CpuPause();
            }
            else
            {
                Threading::ThreadYield();
                IdleSpins = 0;
            }
        }
    }

    void WaitForAll()
    {
        if (G == nullptr)
        {
            return;
        }
        
        LUMINA_PROFILE_SCOPE();
        ASSERT(!TLS.bIsWorker);

        // Assist rather than idle: the queued work drained here is exactly what the parked fibers wait on, so
        // a pure spin waits on a cycle it could break itself. Shutdown calls this, and a hung one says nothing.
        const auto Start = std::chrono::steady_clock::now();
        auto NextReport  = Start + std::chrono::seconds(5);

        // This is a quiescence barrier, not a latency-sensitive frame wait, so it drains EVERY band:
        const uint32 Slot = GetWorkerIndex();
        while (G->InFlight.load(std::memory_order_acquire) > 0)
        {
            FQueuedJob Job;
            if (TryStealAny(Job, (uint32)EJobPriority::Background))
            {
                Job.Function(Job.Argument, Slot);
                OnJobComplete(Job.Counter, Slot);
                continue;
            }
            Threading::ThreadYield();

            const auto Now = std::chrono::steady_clock::now();
            if (Now >= NextReport)
            {
                NextReport = Now + std::chrono::seconds(5);
                LOG_WARN("Job system: WaitForAll has been waiting {}s on {} in-flight job(s). "
                         "Something is blocked and not completing.",
                    std::chrono::duration_cast<std::chrono::seconds>(Now - Start).count(),
                    G->InFlight.load(std::memory_order_relaxed));
            }
        }
    }

    void ParkFiber(FParkFn OnPark, void* Ctx)
    {
        // Worker fibers only. An external thread has no fiber to suspend and must assist-wait instead.
        ASSERT(TLS.bIsWorker && TLS.CurrentFiber != nullptr);

        if (GNoParkGuardName != nullptr)
        {
#if USING(WITH_EDITOR)
            LOG_ERROR("Jobs: fiber '{0}' is parking (ParkFiber) while its thread runs '{1}', which must never yield. "
                      "The pump is stalled until this wait resolves, and the fiber may resume on another thread.",
                      TLS.CurrentFiber->Job.Name ? TLS.CurrentFiber->Job.Name : "<unnamed>", GNoParkGuardName);
#else
            LOG_ERROR("Jobs: a fiber is parking (ParkFiber) while its thread runs '{0}', which must never yield.",
                      GNoParkGuardName);
#endif
        }

#if USING(WITH_EDITOR)
        { 
            FJobProfiler& Prof = FJobProfiler::Get(); 
            if (Prof.IsEnabled())
            {
                Prof.NotePark();
            }
        }
#endif
        TLS.Pending         = FPendingSwitch{};
        TLS.Pending.Action  = EPending::ParkFn;
        TLS.Pending.Fiber   = TLS.CurrentFiber;
        TLS.Pending.ParkFn  = OnPark;
        TLS.Pending.ParkCtx = Ctx;

        FWorkFiber* Self = TLS.CurrentFiber;
        Self->NoParkGuard = GNoParkGuardName;
        GNoParkGuardName  = nullptr;
        Fibers::Switch(TLS.SchedulerFiber);
        // Resumed here once ResumeFiber was called for us, possibly on a different worker thread.
        GNoParkGuardName = TLS.CurrentFiber->NoParkGuard;
    }

    void ResumeFiber(FFiberHandle Handle)
    {
        if (Handle.Fiber == nullptr)
        {
            return;
        }
        PushReady(static_cast<FWorkFiber*>(Handle.Fiber));
    }

    FFiberHandle GetCurrentFiberHandle()
    {
        return TLS.bIsWorker ? FFiberHandle{ TLS.CurrentFiber } : FFiberHandle{};
    }

    void SetThreadNoParkGuard(const char* GuardName)
    {
        GNoParkGuardName = GuardName;
    }

    bool AssistOneJob()
    {
        if (G == nullptr)
        {
            return false;
        }
        const uint32 Slot = GetWorkerIndex();
        FQueuedJob Job;
        if (TryStealAny(Job, AssistMaxPriority()))
        {
            RunAdoptedJob(Job, Slot);
            OnJobComplete(Job.Counter, Slot);
            return true;
        }
        return false;
    }

    void GetLiveStats(FJobLiveStats& Out)
    {
        Out = FJobLiveStats{};
        if (G == nullptr)
        {
            return;
        }
        Out.NumWorkers    = G->NumWorkers;
        Out.NumWorkFibers = G->FibersCreated.load(std::memory_order_acquire);
        Out.MaxWorkFibers = G->MaxWorkFibers;
        uint32 Cached = 0;
        for (uint32 w = 0; w < G->NumWorkers; ++w)
        {
            Cached += G->Workers[w].HasCachedFiber.load(std::memory_order_relaxed);
        }
        Out.FibersFree    = static_cast<uint32>(G->FreeFibers.SizeApprox()) + Cached;
        const int64 Ready = G->ReadyCount.load(std::memory_order_relaxed);
        Out.FibersReady   = Ready > 0 ? static_cast<uint32>(Ready) : 0;
        const uint32 NonRunning = Out.FibersFree + Out.FibersReady;
        Out.FibersInUse   = NonRunning < Out.NumWorkFibers ? Out.NumWorkFibers - NonRunning : 0;
        for (uint32 P = 0; P < kNumJobPriorities; ++P)
        {
            size_t Depth = 0;
            for (uint32 w = 0; w < G->NumWorkers; ++w)
            {
                Depth += G->Workers[w].Queues[P].size_approx();
            }
            Out.QueueDepth[P] = static_cast<uint32>(Depth);
        }
        Out.InFlight = G->InFlight.load(std::memory_order_relaxed);
    }

    void SnapshotFiberStates(TVector<FFiberState>& Out)
    {
        Out.clear();
#if USING(WITH_EDITOR)
        if (G == nullptr || G->WorkFibers == nullptr)
        {
            return;
        }
        const uint32 Created = G->FibersCreated.load(std::memory_order_acquire);
        Out.reserve(Created);
        for (uint32 i = 0; i < Created; ++i)
        {
            FWorkFiber& F = G->WorkFibers[i];
            FFiberState S;
            S.Index         = F.Index;
            S.State         = static_cast<EFiberState>(F.State.load(std::memory_order_relaxed));
            S.OwnerWorker   = F.OwnerWorker.load(std::memory_order_relaxed);
            S.WaitCounterId = F.WaitCounterId.load(std::memory_order_relaxed);
            S.Name          = F.Job.Name;
            Out.push_back(S);
        }
#endif
    }

    void SnapshotActiveCounters(TVector<FCounterState>& Out)
    {
        Out.clear();
#if USING(WITH_EDITOR)
        if (G == nullptr || G->CounterPool == nullptr)
        {
            return;
        }
        for (uint32 i = 0; i < kCounterPoolSize; ++i)
        {
            FCounter& C = G->CounterPool[i];
            if (!C.HasWaiters.load(std::memory_order_acquire))
            {
                continue;
            }
            uint32 Waiters = 0;
            LockCounter(&C);
            for (FWaitNode* N = C.Waiters; N != nullptr; N = N->Next)
            {
                ++Waiters;
            }
            UnlockCounter(&C);
            if (Waiters > 0)
            {
                FCounterState S;
                S.Id            = i;
                S.Value         = C.Value.load(std::memory_order_relaxed);
                S.ParkedWaiters = Waiters;
                Out.push_back(S);
            }
        }
#endif
    }

    void SnapshotWorkerCores(TVector<FWorkerCoreState>& Out)
    {
        Out.clear();
#if USING(WITH_EDITOR)
        if (G == nullptr || G->WorkerCores == nullptr)
        {
            return;
        }
        G->CoreSampleGen.fetch_add(1, std::memory_order_relaxed);
        Out.reserve(G->NumWorkers);
        for (uint32 i = 0; i < G->NumWorkers; ++i)
        {
            FWorkerCoreState S;
            S.Worker = i;
            S.Core   = G->WorkerCores[i].Core.load(std::memory_order_relaxed);
            S.bBusy  = G->WorkerCores[i].Busy.load(std::memory_order_relaxed) != 0;
            Out.push_back(S);
        }
#endif
    }
}
