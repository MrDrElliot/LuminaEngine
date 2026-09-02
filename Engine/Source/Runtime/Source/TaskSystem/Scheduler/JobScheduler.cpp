#include "Platform/Time/PlatformTime.h"
#include "Memory/Construct.h"
#include "RuntimePCH.h"
#include "JobScheduler.h"

#include "Core/Threading/Thread.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Fiber.h"
#include "Memory/Memory.h"
#include "Containers/ConcurrentQueue.h"
#include "Platform/Process/PlatformProcess.h"
#include "Containers/Vector.h"
#include "Containers/BoundedQueue.h"
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

// External threads have no fiber and assist-wait instead, running queued jobs inline.
namespace Lumina::Jobs
{
    namespace
    {
        FORCEINLINE void CpuPause() { _mm_pause(); }

        constexpr uint32 kCounterPoolSize   = 8192;
        constexpr uint32 kDefaultWorkFibers = 256;
        constexpr uint32 kDefaultFiberStack = 512 * 1024;

        // Only created fibers cost anything, so this bounds a leak into an error rather than address space.
        constexpr uint32 kDefaultMaxWorkFibers = 4096;

        struct FQueuedJob
        {
            FJobFunction Function = nullptr;
            void*        Argument = nullptr;
            // The counter is 64-byte aligned, so the low bits are free and a separate bool would cost padding.
            uintptr_t    CounterAndPark = 0;
#if USING(WITH_EDITOR)
            const char*  Name     = nullptr; // label for the editor profiler; absent otherwise to shrink the queue element
#endif
            FORCEINLINE void SetCounter(FCounter* Counter, bool bMayPark)
            {
                CounterAndPark = reinterpret_cast<uintptr_t>(Counter) | (bMayPark ? 1ull : 0ull);
            }
            FORCEINLINE FCounter* GetCounter() const
            {
                return reinterpret_cast<FCounter*>(CounterAndPark & ~static_cast<uintptr_t>(1));
            }
            FORCEINLINE bool MayPark() const { return (CounterAndPark & 1) != 0; }
        };

        // Long-lived, looping one bound job then switching back, and migrating when a job parks elsewhere.
        struct FWorkFiber
        {
            Fibers::FFiber Handle = nullptr;
            FQueuedJob     Job{};   // bound by the scheduler immediately before switching in
            // Conceptually per-fiber but read at the park site, so without save and restore it leaks onward.
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

        // Lives on the parking fiber's own stack, which stays alive while parked.
        struct FWaitNode
        {
            FWorkFiber* Fiber  = nullptr;
            int32       Target = 0;
            FWaitNode*  Next   = nullptr;
        };
    }

    // The public opaque type, defined here and referencing the wait node from the unnamed namespace.
    struct alignas(64) FCounter
    {
        TAtomic<int32>  Value{0};
        // Claimed before the value moves, so FreeCounter can wait out a release still walking this.
        TAtomic<int32>  Releasers{0};
        FCompletionFn   Completion    = nullptr; // fired once when Value reaches 0
        void*           CompletionCtx = nullptr;

        TAtomic<uint32> WaitLock{0};         // spinlock guarding Waiters
        FWaitNode*      Waiters = nullptr;   // intrusive list, guarded by WaitLock
        TAtomic<bool>   HasWaiters{false};   // seq_cst gate so the lock-free decrement can skip the lock

        // Monotonic on purpose, since a stale sequence costs an extra wakeup where a reset one misses a sleep.
        TAtomic<uint32> ThreadWaitSeq{0};
        TAtomic<bool>   HasThreadWaiters{false};

        // The futex layer works on a plain word; std::atomic<uint32> is lock-free and layout-compatible.
        const volatile uint32* SeqWord() const
        {
            return reinterpret_cast<const volatile uint32*>(&ThreadWaitSeq);
        }

        uint16          PoolIndex = 0xFFFF;
        bool            bPooled   = false;
    };

    // The park flag lives in bit 0, which only works while a counter never lands on an odd address.
    static_assert(alignof(FCounter) >= 2, "FQueuedJob::CounterAndPark needs bit 0 of an FCounter* free.");

    namespace
    {
        using FJobQueue   = TConcurrentQueue<FQueuedJob>;

        // The job queues stay unbounded, since a submit burst has no such ceiling.
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
            uint32         FruitlessWaits = 0;       // consecutive waits that spun and then parked anyway
            bool           bOwnsExternalSlot = false; // this thread holds an external slot to give back
            bool           bNativeJob        = false; // running a job on the scheduler fiber, with no work fiber

            FWorkFiber*    CachedFiber    = nullptr;

            // A worker index is claimed LAZILY by any thread completing a job inline, so exits would leak one.
            ~FThreadState();
        };
        thread_local FThreadState TLS;

        // Parking strands the pump, and the fiber can resume on a different thread than its state.
        thread_local const char* GNoParkGuardName = nullptr;

        // A fan-out wakes every idle worker in PARALLEL rather than filing them through one mutex.
        struct alignas(64) FWorkerLocal
        {
            FJobQueue                  Queues[kNumJobPriorities];
            TAtomic<uint32>            WakeSignal{0}; // bumped (with notify) to wake this worker from its wait

            TAtomic<uint8>             HasCachedFiber{0};
            // Nonempty-band hint, one bit per priority, set before enqueue so a failed victim probe is one load.
            TAtomic<uint8>             BandMask{0};

            FWorkerLocal() = default;
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
            // Bumped when InFlight reaches zero, so WaitForAll sleeps instead of spinning a core it needs.
            alignas(64) TAtomic<uint32> DrainSeq{0};
            TAtomic<bool>               HasDrainWaiters{false};

            alignas(64) TAtomic<int64> AvailJobs{0};   // queued, not yet popped (idle-wake hint)

            // Queued Background work would pass the fast-fail and force a full scan every spin.
            alignas(64) TAtomic<int64> AvailAssistJobs{0};
            alignas(64) TAtomic<int64> InFlight{0};    // submitted, not yet completed (WaitForAll)

            // Flat and never reallocated, since wait queues reference parked fibers by ADDRESS.
            FWorkFiber*    WorkFibers = nullptr;
            alignas(64) TAtomic<uint32> FibersCreated{0};
            FFiberQueue    FreeFibers;           // idle, ready to be bound to a job
            FFiberQueue    ReadyFibers;          // parked fibers whose counter is now satisfied
            alignas(64) TAtomic<int64> ReadyCount{0};  // ReadyFibers size hint (idle-wake)

            FCounter*       CounterPool = nullptr;
            FIndexQueue     FreeCounters;

            // A monotonic counter leaks one slot per cycle, after which threads alias each other's arrays.
            TAtomic<uint64> ExternalSlotsFree{0};
            TAtomic<bool>   bShutdown{false};

            // Wall-clock gate for the pool-wedged report, shared so only one worker ever prints it.
            alignas(64) TAtomic<int64> NextWedgeReportMs{0};

            // The same gate for the assist-wait stall report; every waiting thread shares one.
            alignas(64) TAtomic<int64> NextAssistStallMs{0};

            // The tier ends let a probe rotate WITHIN a tier rather than always starting at its head.
            uint16* StealOrder = nullptr;
            uint16* StealTiers = nullptr;
            uint16* WorkerCpu  = nullptr;   // logical processor each worker asks the scheduler to prefer

            // Load-bearing, since WakeWorkers scans it to wake only as many idle workers as there are jobs.
            alignas(64) std::atomic<uint64>* IdleMask = nullptr; // [IdleMaskWords]
            uint32 IdleMaskWords = 0;

            // Its own cache line, since many workers write it and it must not false-share the hot counters.
            alignas(64) TAtomic<int32> PoppersInFlight{0};

#if USING(WITH_EDITOR)
            // Sampled at fiber dispatch, giving the editor which core a worker last ran a job on.
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

        // Pairs with the fence a parking worker runs, so a submit racing a park is never lost.
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

        // The ideal-processor hint is soft, so a wrong guess costs a worse probe order and nothing else.
        void BuildStealOrder()
        {
            const uint32 W = G->NumWorkers;
            const uint32 RowSize = W > 1 ? W - 1 : 1;

            G->StealOrder = static_cast<uint16*>(Memory::Malloc(sizeof(uint16) * W * RowSize, alignof(uint16)));
            G->StealTiers = static_cast<uint16*>(Memory::Malloc(sizeof(uint16) * W * 2, alignof(uint16)));
            G->WorkerCpu  = static_cast<uint16*>(Memory::Malloc(sizeof(uint16) * W, alignof(uint16)));
            Memory::Memzero(G->StealOrder, sizeof(uint16) * W * RowSize);
            Memory::Memzero(G->StealTiers, sizeof(uint16) * W * 2);

            const uint32 NumCpus = Threading::GetNumThreads();
            for (uint32 i = 0; i < W; ++i)
            {
                // Leaves logical processor 0 to the main and render threads, which are not workers.
                G->WorkerCpu[i] = static_cast<uint16>(NumCpus > 1 ? (i + 1) % NumCpus : 0);
            }

            constexpr uint32 kMaxDescribed = 256;
            Threading::FCpuTopology Topology[kMaxDescribed];
            const uint32 Described = Threading::GetCpuTopology(Topology,
                NumCpus < kMaxDescribed ? NumCpus : kMaxDescribed);

            for (uint32 Self = 0; Self < W; ++Self)
            {
                uint16* Row = G->StealOrder + static_cast<size_t>(Self) * RowSize;
                uint32  Fill = 0;

                // Three passes over the same list keeps the rows dense and the tier bounds exact.
                for (uint32 Tier = 0; Tier < 3; ++Tier)
                {
                    for (uint32 Other = 1; Other < W; ++Other)
                    {
                        const uint32 V = (Self + Other) % W;
                        const uint32 CpuA = G->WorkerCpu[Self];
                        const uint32 CpuB = G->WorkerCpu[V];
                        const bool bDescribed = Described > CpuA && Described > CpuB;

                        const bool bSameCore  = bDescribed && Topology[CpuA].PhysicalCore == Topology[CpuB].PhysicalCore;
                        const bool bSameCache = bDescribed && Topology[CpuA].CacheGroup == Topology[CpuB].CacheGroup;

                        const uint32 Belongs = bSameCore ? 0u : (bSameCache ? 1u : 2u);
                        if (Belongs == Tier)
                        {
                            Row[Fill++] = static_cast<uint16>(V);
                        }
                    }
                    if (Tier < 2)
                    {
                        G->StealTiers[Self * 2 + Tier] = static_cast<uint16>(Fill);
                    }
                }
            }

            if (Described == 0)
            {
                LOG_DISPLAY("Job system: {} workers, CPU topology unavailable; every worker probes the rest in "
                            "rotation.", W);
                return;
            }

            uint32 CacheGroups = 0;
            uint32 WithSibling = 0;
            for (uint32 i = 0; i < W; ++i)
            {
                CacheGroups = Topology[G->WorkerCpu[i]].CacheGroup + 1u > CacheGroups
                            ? Topology[G->WorkerCpu[i]].CacheGroup + 1u : CacheGroups;
                WithSibling += G->StealTiers[i * 2] > 0 ? 1u : 0u;
            }
            LOG_DISPLAY("Job system: {} workers across {} cache group(s); {} have an SMT sibling in the pool and "
                        "steal from it first.", W, CacheGroups, WithSibling);
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
        // Latched so increment and decrement stay balanced across a mid-call toggle, and RAII covers returns.
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

        // Bulk enqueue per slice keeps the queue's per-item overhead amortized.
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
                // A thief that probes a set bit and finds it empty clears it, and nothing re-sets it after.
                G->Workers[Wk].Queues[Prio].EnqueueBulk(Jobs + Idx, N);
                G->Workers[Wk].BandMask.fetch_or(static_cast<uint8>(1u << Prio), std::memory_order_release);
                Idx += N;
                ++Recipients;
            }
            return FSubmitSpan{ Start, Recipients };
        }

        // Slices are one deep whenever the count fits the workers, which every cursor fan-out hits.
        FSubmitSpan DistributeIdenticalJobs(const FQueuedJob& Job, uint32 Count, int Prio)
        {
            const uint32 W     = G->NumWorkers;
            const uint32 Start = G->NextSubmitWorker.fetch_add(1, std::memory_order_relaxed) % W;
            const uint32 Base  = Count / W;
            const uint32 Rem   = Count % W;
            uint32 Recipients  = 0;
            for (uint32 i = 0; i < W; ++i)
            {
                const uint32 N = Base + (i < Rem ? 1u : 0u);
                if (N == 0)
                {
                    continue;
                }
                const uint32 Wk = (Start + i) % W;
                for (uint32 j = 0; j < N; ++j)
                {
                    G->Workers[Wk].Queues[Prio].Enqueue(Job);
                }
                G->Workers[Wk].BandMask.fetch_or(static_cast<uint8>(1u << Prio), std::memory_order_release);
                ++Recipients;
            }
            return FSubmitSpan{ Start, Recipients };
        }

        // Background is deliberately absent from the assist hint, so it must not be decremented there.
        FORCEINLINE void NoteJobDequeued(uint32 Priority)
        {
            G->AvailJobs.fetch_sub(1, std::memory_order_relaxed);
            if (Priority <= kMaxAssistPriority)
            {
                G->AvailAssistJobs.fetch_sub(1, std::memory_order_relaxed);
            }
        }

        // The cursor rotates WITHIN each tier, so peers do not all probe the same victim first.
        FORCEINLINE uint32 StealCandidate(uint32 Slot, uint32 Probe, uint32 Cursor)
        {
            const uint32 W    = G->NumWorkers;
            const uint16* Row = G->StealOrder + static_cast<size_t>(Slot) * (W - 1);
            const uint32 Bounds[3] = { G->StealTiers[Slot * 2], G->StealTiers[Slot * 2 + 1], W - 1 };

            uint32 Begin = 0;
            for (uint32 Tier = 0; Tier < 3; ++Tier)
            {
                const uint32 Size = Bounds[Tier] - Begin;
                if (Probe < Size)
                {
                    return Row[Begin + (Cursor + Probe) % Size];
                }
                Probe -= Size;
                Begin  = Bounds[Tier];
            }
            return Row[0];
        }

        FORCEINLINE bool TryStealFromWorker(FQueuedJob& Out, uint32 V, uint32 MaxPrio)
        {
            const uint8 Filter = static_cast<uint8>((1u << (MaxPrio + 1u)) - 1u);
            uint8 Mask = G->Workers[V].BandMask.load(std::memory_order_acquire) & Filter;
            while (Mask != 0)
            {
                const uint32 P = (uint32)Math::CountTrailingZeros64(Mask);
                Mask &= static_cast<uint8>(Mask - 1);
                if (G->Workers[V].Queues[P].TryDequeue(Out))
                {
                    NoteJobDequeued(P);
                    return true;
                }
                // Empty despite the bit, so clear it, then re-set if an enqueue raced the clear.
                G->Workers[V].BandMask.fetch_and(static_cast<uint8>(~(1u << P)), std::memory_order_relaxed);
                if (G->Workers[V].Queues[P].SizeApprox() != 0)
                {
                    G->Workers[V].BandMask.fetch_or(static_cast<uint8>(1u << P), std::memory_order_release);
                }
            }
            return false;
        }

        // Resumes the scan where the last steal landed, and decrements the queued hints on success.
        bool TryGetJobWorker(FQueuedJob& Out, uint32 Slot)
        {
            POPPER_SCOPE();
            FWorkerLocal& Self = G->Workers[Slot];
            // All bands including Background, since a real worker is exactly who should run it.
            for (uint32 P = 0; P < kNumJobPriorities; ++P)
            {
                if (Self.Queues[P].TryDequeue(Out))
                {
                    NoteJobDequeued(P);
                    return true;
                }
            }
            // The hint is bumped BEFORE enqueue, so a non-positive value means there is genuinely nothing.
            if (G->AvailJobs.load(std::memory_order_relaxed) <= 0)
            {
                return false;
            }
            const uint32 W      = G->NumWorkers;
            const uint32 Cursor = TLS.StealCursor;
            for (uint32 i = 0; i + 1 < W; ++i)
            {
                const uint32 V = StealCandidate(Slot, i, Cursor);
                if (TryStealFromWorker(Out, V, kNumJobPriorities - 1))
                {
                    TLS.StealCursor = Cursor + i + 1;
                    return true;
                }
            }
            return false;
        }

        // Untargeted, so without the cap an assisting thread would adopt a long build mid-frame.
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
            const uint32 Slot   = TLS.WorkerIndex;

            if (Slot < W)
            {
                // Its own queue first, since a worker assisting inside a native job is its home consumer.
                if (TryStealFromWorker(Out, Slot, MaxPrio))
                {
                    return true;
                }
                for (uint32 i = 0; i + 1 < W; ++i)
                {
                    const uint32 V = StealCandidate(Slot, i, Cursor);
                    if (TryStealFromWorker(Out, V, MaxPrio))
                    {
                        TLS.StealCursor = Cursor + i + 1;
                        return true;
                    }
                }
                return false;
            }

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
            // A native job is a real worker, so its inner wait may take any band, unlike an external assist.
            return TLS.bNativeJob ? (uint32)EJobPriority::Background : kMaxAssistPriority;
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

        // An external assist never joins the waiter list, so the poller can recycle this mid-walk.
        void ReleaseCounter(FCounter* Counter, int32 NewValue, uint32 WorkerIndex)
        {
            // A sleeping thread may wait on a NON-zero target, so any decrement has to reach it.
            const bool bWakeThreads = Counter->HasThreadWaiters.load(std::memory_order_seq_cst);

            // The fast path, where nothing reached zero and nobody is parked at all.
            if (NewValue > 0 && !bWakeThreads && !Counter->HasWaiters.load(std::memory_order_seq_cst))
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

            if (bWakeThreads)
            {
                Counter->ThreadWaitSeq.fetch_add(1, std::memory_order_release);
                Threading::WakeAllOnAddress32(Counter->SeqWord());
            }

            // Dropped before the completion callback, which owns the lifetime and may free the counter.
            Counter->Releasers.fetch_sub(1, std::memory_order_release);

            if (Completion != nullptr)
            {
                Completion(Ctx, WorkerIndex); // owns the counter's lifetime
            }
        }

        // The guard is claimed BEFORE the value moves, so a released waiter cannot recycle it early.
        FORCEINLINE void DecrementAndRelease(FCounter* Counter, int32 By, uint32 WorkerIndex)
        {
            Counter->Releasers.fetch_add(1, std::memory_order_acquire);
            const int32 NewValue = Counter->Value.fetch_sub(By, std::memory_order_seq_cst) - By;
            ReleaseCounter(Counter, NewValue, WorkerIndex);
        }

        FORCEINLINE void NoteJobRetired()
        {
            const int64 Remaining = G->InFlight.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (Remaining == 0 && G->HasDrainWaiters.load(std::memory_order_seq_cst))
            {
                G->DrainSeq.fetch_add(1, std::memory_order_release);
                Threading::WakeAllOnAddress32(reinterpret_cast<const volatile uint32*>(&G->DrainSeq));
            }
        }

        void OnJobComplete(FCounter* Counter, uint32 WorkerIndex)
        {
            if (Counter == nullptr)
            {
                NoteJobRetired();
                return;
            }

            Counter->Releasers.fetch_add(1, std::memory_order_acquire);
            const int32 NewValue = Counter->Value.fetch_sub(1, std::memory_order_seq_cst) - 1;
            ReleaseCounter(Counter, NewValue, WorkerIndex);
            // After the release, so WaitForAll cannot return while this job's completion is still running.
            NoteJobRetired();
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

        // Fiber-state stores are unconditional, while span recording self-gates on the profiler.
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

        // The ONLY place a work fiber becomes resumable, which guarantees its state is fully saved.
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
                    // The Dekker pairing for the lock-free release path, and the other order loses a wakeup forever.
                    C->HasWaiters.store(true, std::memory_order_seq_cst);
                    if (C->Value.load(std::memory_order_seq_cst) <= P.Node->Target)
                    {
                        // A spurious true only costs the next decrement a trip through the locked path.
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
                    // If it declined, the condition was already satisfied and the fiber is runnable again.
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
        
        // Forward declared, since the pool grows at runtime and the growth path needs the entry point.
        void FiberMain(void* Arg);

        // The one state a stackful scheduler cannot resolve, and the claim is CAS'd so it stays unique.
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

            // Logged in powers of two, so a burst does not flood the log with the same message.
            const uint32 Count = Index + 1;
            if ((Count & (Count - 1)) == 0)
            {
                LOG_WARN("Job system: fiber pool grew to {} (of {} max) -- {} jobs are blocked at once.",
                    Count, G->MaxWorkFibers, Count);
            }
            return F;
        }

        constexpr int kHotPauseSpins = 1024; // a tight pause spin, catching the next wave without a syscall
        constexpr int kHotYieldSpins = 128;  // OS-friendly tail; near-free when idle, holds hot when busy

        // A spin that parks anyway was a losing bet, so each consecutive loss halves the next one.
        constexpr uint32 kMaxSpinBackoffShift = 6;

        // Pauses spent waiting for another worker to hand a fiber back, before the pool grows a fresh stack.
        constexpr uint32 kFiberAcquireSpins = 256;
        // Wall-clock gap between pool-wedged reports.
        constexpr int64  kWedgeReportMs     = 2000;

        // Counting per worker had thirty of them logging every few ms, contending with the stall itself.
        bool ShouldReportWedge()
        {
            const int64 NowMs = static_cast<int64>(PlatformTime::Seconds() * 1000.0);

            int64 Next = G->NextWedgeReportMs.load(std::memory_order_relaxed);
            if (NowMs < Next)
            {
                return false;
            }
            return G->NextWedgeReportMs.compare_exchange_strong(Next, NowMs + kWedgeReportMs,
                std::memory_order_acq_rel, std::memory_order_relaxed);
        }

        // Failed steals before a waiter stops burning its core and sleeps on the counter instead.
        constexpr uint32 kAssistSpinsBeforePark = 256;

        // How long WaitForCounterBusy spins before it gives up and parks. Sized well above the tens of
        // microseconds a fork/join task runs for, and well under anything a frame would notice.
        constexpr double kBusyWaitSeconds = 0.002;

        // Every wake is advisory, since the sleeper re-checks its counter and re-tries on each return.
        constexpr uint32 kParkTimeoutMs = 1;

        // An assist wait that goes this long without finding one job is pathological, not slow work.
        constexpr double kAssistStallSeconds  = 0.010;
        constexpr int64  kAssistStallReportMs = 2000;

        bool ShouldReportAssistStall()
        {
            const int64 NowMs = static_cast<int64>(PlatformTime::Seconds() * 1000.0);

            int64 Next = G->NextAssistStallMs.load(std::memory_order_relaxed);
            if (NowMs < Next)
            {
                return false;
            }
            return G->NextAssistStallMs.compare_exchange_strong(Next, NowMs + kAssistStallReportMs,
                std::memory_order_acq_rel, std::memory_order_relaxed);
        }

        // CurrentFiber is null for the duration, which routes any wait inside it to the assist loop.
        void RunJobNative(const FQueuedJob& Job, uint32 Slot)
        {
            LUMINA_PROFILE_SECTION_COLORED("Job", tracy::Color::SteelBlue);
#if USING(WITH_EDITOR)
            if (Job.Name != nullptr)
            {
                LUMINA_PROFILE_TAG(Job.Name);
            }
#endif
            FWorkFiber* SavedFiber  = TLS.CurrentFiber;
            const char* SavedGuard  = GNoParkGuardName;
            const bool  bSavedNative = TLS.bNativeJob;
            TLS.bNativeJob   = true;
            TLS.CurrentFiber = nullptr;
            GNoParkGuardName = nullptr;

            Job.Function(Job.Argument, Slot);
            OnJobComplete(Job.GetCounter(), Slot);

            GNoParkGuardName = SavedGuard;
            TLS.CurrentFiber = SavedFiber;
            TLS.bNativeJob   = bSavedNative;
        }

#if USING(WITH_EDITOR)
        // Tallies parked fibers by job label, so the log says WHAT filled the pool.
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
            const uint32 Shift = TLS.FruitlessWaits < kMaxSpinBackoffShift ? TLS.FruitlessWaits
                                                                           : kMaxSpinBackoffShift;
            const int PauseSpins = kHotPauseSpins >> Shift;
            const int YieldSpins = kHotYieldSpins >> Shift;

            for (int Spin = 0; Spin < PauseSpins; ++Spin)
            {
                if (HasWork())
                {
                    return;
                }
                CpuPause();
            }
            for (int Spin = 0; Spin < YieldSpins; ++Spin)
            {
                if (HasWork())
                {
                    return;
                }
                Threading::ThreadYield();
            }

            ++TLS.FruitlessWaits;

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

        // Prefers resuming parked fibers, which drains in-flight work and frees fibers, over fresh jobs.
        void SchedulerLoop()
        {
            const uint32 Slot = TLS.WorkerIndex;

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
                    TLS.FruitlessWaits = 0; // work is flowing again, so the next spin is worth its full budget
                    CascadeWake(Slot); // before the switch, since a releasing counter can dump many at once
                    TLS.CurrentFiber = Ready;
#if USING(WITH_EDITOR)
                    ProfResume(Ready, Slot);
#endif
                    TracyEnterFiber(Ready);
                    Fibers::Switch(Ready->Handle);
                    TracyLeaveFiber();
                    continue;
                }

                // Only a job that declared it may park costs a fiber, and most never need one.
                FQueuedJob Job;
                if (TryGetJobWorker(Job, Slot))
                {
                    TLS.FruitlessWaits = 0;
                    CascadeWake(Slot); // relay the ramp before running, so the two overlap

                    if (!Job.MayPark())
                    {
                        RunJobNative(Job, Slot);
                        continue;
                    }

                    FWorkFiber* Free = AcquireFiber(Slot);
                    for (uint32 Spin = 0; Free == nullptr && Spin < kFiberAcquireSpins; ++Spin)
                    {
                        // Nearly always a blink, so spin before committing a fresh stack reservation.
                        CpuPause();
                        Free = AcquireFiber(Slot);
                    }
                    if (Free == nullptr)
                    {
#if USING(WITH_EDITOR)
                        ProfStarvation(); // count distinct episodes, not every spin
#endif
                        Free = GrowFiberPool(); // null only at the ceiling
                    }

                    if (Free == nullptr)
                    {
                        if (ShouldReportWedge())
                        {
                            LOG_ERROR("Job system: fiber pool wedged at the {} fiber ceiling with park-capable jobs "
                                      "still queued. Running this one on the worker's stack to recover, so a wait "
                                      "inside it assist-spins instead of parking. Raise FConfig::MaxWorkFibers, or "
                                      "stop blocking inside jobs that are themselves spawned in bulk.",
                                      G->MaxWorkFibers);
#if USING(WITH_EDITOR)
                            ReportWedgeCulprits();
#endif
                        }
                        RunJobNative(Job, Slot);
                        continue;
                    }

                    Free->Job        = Job;
                    TLS.CurrentFiber = Free;
#if USING(WITH_EDITOR)
                    ProfBind(Free, Slot);
#endif
                    TracyEnterFiber(Free);
                    Fibers::Switch(Free->Handle);
                    TracyLeaveFiber();
                    continue;
                }

                WaitForWork();
            }

            FlushCachedFiber(Slot);
        }

        // The entry for every pooled work fiber, looping to run its bound job then switch back to be reused.
        void FiberMain(void* /*Arg*/)
        {
            while (true)
            {
                FWorkFiber* Self = TLS.CurrentFiber; // set by the scheduler before switching in
                FQueuedJob  Job  = Self->Job;

                Job.Function(Job.Argument, TLS.WorkerIndex);
                OnJobComplete(Job.GetCounter(), TLS.WorkerIndex);

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
            Threading::SetThreadIdealProcessor(G->WorkerCpu[WorkerIndex]);
            Memory::InitializeThreadHeap();

            TLS.SchedulerFiber = Fibers::ThreadToFiber();

            SchedulerLoop();

            Fibers::FiberToThread();
            Memory::ShutdownThreadHeap();
        }
    }

    namespace
    {
        // A parked fiber's stack lives off-thread, so a stall dump needs this to see what it waits on.
        void JobsHangReporter()
        {
            FJobLiveStats Stats;
            GetLiveStats(Stats);
            // A stall from bulk throughput work shows up in that band and nowhere else.
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

        // These four numbers say which, unassistable work, an unresumed ready fiber, or a missing decrement.
        void ReportAssistStall(int32 CounterValue, int32 Target, double IdleSeconds)
        {
            uint32 ParkedWorkers = 0;
            for (uint32 Word = 0; Word < G->IdleMaskWords; ++Word)
            {
                uint64 Bits = G->IdleMask[Word].load(std::memory_order_relaxed);
                while (Bits != 0)
                {
                    Bits &= Bits - 1;
                    ++ParkedWorkers;
                }
            }

            LOG_ERROR("Job system: a counter wait went {}ms without finding a single job to run. "
                      "Counter value {} (waiting for {}), queued {} (assistable {}), ready fibers {}, "
                      "in-flight {}, {}/{} workers parked.",
                static_cast<int64>(IdleSeconds * 1000.0), CounterValue, Target,
                G->AvailJobs.load(std::memory_order_relaxed),
                G->AvailAssistJobs.load(std::memory_order_relaxed),
                G->ReadyCount.load(std::memory_order_relaxed),
                G->InFlight.load(std::memory_order_relaxed),
                ParkedWorkers, G->NumWorkers);

            JobsHangReporter();
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

        // Capped at 64 by the bitmask, and more non-worker threads than that is not a configuration here.
        ASSERT(G->NumExternal <= 64);
        G->ExternalSlotsFree.store(G->NumExternal == 64 ? ~0ull : ((1ull << G->NumExternal) - 1ull),
            std::memory_order_relaxed);

        // Built BEFORE workers start, with placement-new so each constructs its queues in place.
        G->Workers = static_cast<FWorkerLocal*>(Memory::Malloc(sizeof(FWorkerLocal) * G->NumWorkers, alignof(FWorkerLocal)));
        for (uint32 i = 0; i < G->NumWorkers; ++i)
        {
            Memory::ConstructAt(&G->Workers[i]);
        }

        // One idle-mask word per 64 workers, zeroed since nobody has parked yet.
        G->IdleMaskWords = (G->NumWorkers + 63u) / 64u;
        G->IdleMask = static_cast<std::atomic<uint64>*>(Memory::Malloc(sizeof(std::atomic<uint64>) * G->IdleMaskWords, alignof(std::atomic<uint64>)));
        for (uint32 i = 0; i < G->IdleMaskWords; ++i)
        {
            Memory::ConstructAt(&G->IdleMask[i], 0);
        }

        // A ring sized exactly to its pool rides the wrap boundary and spins through a mid-claim cell.
        G->FreeCounters.Initialize(kCounterPoolSize * 2);
        G->FreeFibers.Initialize(G->MaxWorkFibers * 2);
        G->ReadyFibers.Initialize(G->MaxWorkFibers * 2);

        G->CounterPool = static_cast<FCounter*>(Memory::Malloc(sizeof(FCounter) * kCounterPoolSize, alignof(FCounter)));
        for (uint32 i = 0; i < kCounterPoolSize; ++i)
        {
            FCounter* C = Memory::ConstructAt(&G->CounterPool[i]);
            C->bPooled   = true;
            C->PoolIndex = static_cast<uint16>(i);
            G->FreeCounters.Enqueue(static_cast<uint16>(i));
        }

        // Storage covers the growth ceiling and is never reallocated, since parked fibers are held by address.
        G->WorkFibers = static_cast<FWorkFiber*>(Memory::Malloc(sizeof(FWorkFiber) * G->MaxWorkFibers, alignof(FWorkFiber)));
        for (uint32 i = 0; i < G->MaxWorkFibers; ++i)
        {
            Memory::ConstructAt(&G->WorkFibers[i]);
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
            Memory::ConstructAt(&G->WorkerCores[i]);
        }
#endif

        BuildStealOrder();

        G->WorkerThreads.reserve(G->NumWorkers);
        for (uint32 i = 0; i < G->NumWorkers; ++i)
        {
            G->WorkerThreads.emplace_back(WorkerThreadMain, i);
        }

        LOG_DISPLAY("Job system online: {} workers, {} thread slots, {} fibers ({} max, {}KB stacks).",
            G->NumWorkers, G->NumThreadSlots, G->NumWorkFibers, G->MaxWorkFibers, G->FiberStackSize / 1024);

        // Idempotent across initialize and shutdown cycles, since the reporter list is append-only.
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

        // Every claimed index is fully created by now, and the rest never got a stack.
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

        for (uint16** Table : { &G->StealOrder, &G->StealTiers, &G->WorkerCpu })
        {
            if (*Table != nullptr)
            {
                void* Mem = *Table;
                Memory::Free(Mem);
                *Table = nullptr;
            }
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
    bool   CanParkFiber()      { return TLS.CurrentFiber != nullptr; }

    namespace
    {
        // The fallback is silent corruption, since the index sizes every per-thread array, hence the error.
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
        // A stray thread running a job inline lazily claims an external slot.
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
        // Without this a register and unregister cycle burns one permanently until threads alias.
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
        Counter->HasThreadWaiters.store(false, std::memory_order_relaxed);
        return Counter;
    }

    void FreeCounter(FCounter* Counter)
    {
        if (Counter == nullptr)
        {
            return;
        }
        // Recycling mid-walk splices one graph's waiters into another.
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

    namespace
    {
        FORCEINLINE void NoteJobsSubmitted(uint32 Count, int Prio, FCounter* Counter)
        {
            if (Counter != nullptr)
            {
                Counter->Value.fetch_add(static_cast<int32>(Count), std::memory_order_acq_rel);
            }
            G->InFlight.fetch_add(static_cast<int64>(Count), std::memory_order_acq_rel);
            G->AvailJobs.fetch_add(static_cast<int64>(Count), std::memory_order_relaxed);
            if ((uint32)Prio <= kMaxAssistPriority)
            {
                G->AvailAssistJobs.fetch_add(static_cast<int64>(Count), std::memory_order_relaxed);
            }
        }
    }

    void RunJobs(const FJobDecl& Decl, uint32 Count, EJobPriority Priority, FCounter* Counter)
    {
        if (Count == 0)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

#if USING(WITH_EDITOR)
        ProfSubmit(Count, TLS.bIsWorker);
#endif

        const int Prio = static_cast<int>(Priority);
        NoteJobsSubmitted(Count, Prio, Counter);

        FQueuedJob Job;
        Job.Function = Decl.Function;
        Job.Argument = Decl.Argument;
        Job.SetCounter(Counter, Decl.bMayPark);
#if USING(WITH_EDITOR)
        Job.Name     = Decl.Name;
#endif

        const FSubmitSpan Span = DistributeIdenticalJobs(Job, Count, Prio);

        const uint32 DirectWakes = Count < kDirectWakeMax ? Count : kDirectWakeMax;
        WakeWorkers(DirectWakes, Span.Start, Span.NumRecipients);
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

        const int Prio = static_cast<int>(Priority);

        constexpr uint32 kBatch = 256;
        FQueuedJob Batch[kBatch];
        FSubmitSpan Span;
        for (uint32 Base = 0; Base < Count; Base += kBatch)
        {
            const uint32 N = (Count - Base) < kBatch ? (Count - Base) : kBatch;
            // Per batch, identical for ordinary submits and a smaller stale-hint window for huge ones.
            NoteJobsSubmitted(N, Prio, Counter);
            for (uint32 i = 0; i < N; ++i)
            {
                Batch[i].Function = Jobs[Base + i].Function;
                Batch[i].Argument = Jobs[Base + i].Argument;
                Batch[i].SetCounter(Counter, Jobs[Base + i].bMayPark);
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

    void RunJob(FJobFunction Fn, void* Arg, EJobPriority Priority, FCounter* Counter, const char* Name, bool bMayPark)
    {
        FJobDecl Decl{ Fn, Arg, Name, bMayPark };
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

    void WaitForCounterBusy(FCounter* Counter, int32 Value)
    {
        if (Counter == nullptr)
        {
            return;
        }

        // A fiber parks for free and hands its thread back, so it never wants the spin.
        if (TLS.CurrentFiber != nullptr && GNoParkGuardName == nullptr)
        {
            WaitForCounter(Counter, Value);
            return;
        }

        const uint32 Slot = GetWorkerIndex();
        const double Deadline = PlatformTime::Seconds() + kBusyWaitSeconds;
        uint32 Spins = 0;

        while (Counter->Value.load(std::memory_order_acquire) > Value)
        {
            FQueuedJob Job;
            if (TryStealAny(Job, AssistMaxPriority()))
            {
                RunAdoptedJob(Job, Slot);
                OnJobComplete(Job.GetCounter(), Slot);
                Spins = 0;
                continue;
            }

            if (++Spins >= kAssistSpinsBeforePark)
            {
                Spins = 0;

                // Past the budget this stopped being a short wait, so hand it to the path that can park.
                if (PlatformTime::Seconds() >= Deadline)
                {
                    WaitForCounter(Counter, Value);
                    return;
                }
            }

            CpuPause();
        }
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
            // Once per process, since per-occurrence logging is what made this unreadable.
            static std::atomic<bool> bReported{ false };
            bool Expected = false;
            if (bReported.compare_exchange_strong(Expected, true, std::memory_order_relaxed))
            {
                LOG_WARN("Jobs: a counter wait occurred while this thread runs '{0}', which must never yield. "
                         "Servicing queued jobs inline instead of parking. Further occurrences are not logged.",
                         GNoParkGuardName);
            }
        }

        if (TLS.CurrentFiber != nullptr && !bMustNotYield)
        {
            // The releasing decrement touches nothing once we are spliced out, so the caller may reclaim it.
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

        // Taken by an external thread with no fiber, or by a fiber under a no-yield guard.
        const uint32 Slot = GetWorkerIndex();
        uint32 IdleSpins   = 0;
        bool   bRegistered = false;
        // A wait that keeps adopting jobs is working however long it takes, and never pays for this.
        double IdleSince = 0.0;

        while (Counter->Value.load(std::memory_order_acquire) > Value)
        {
            FQueuedJob Job;
            if (TryStealAny(Job, AssistMaxPriority()))
            {
                RunAdoptedJob(Job, Slot);
                OnJobComplete(Job.GetCounter(), Slot);
                IdleSpins = 0;
                IdleSince = 0.0;
                continue;
            }

            if (++IdleSpins < kAssistSpinsBeforePark)
            {
                CpuPause();
                continue;
            }

            const double Now = PlatformTime::Seconds();
            if (IdleSince == 0.0)
            {
                IdleSince = Now;
            }
            else if (Now - IdleSince >= kAssistStallSeconds && ShouldReportAssistStall())
            {
                ReportAssistStall(Counter->Value.load(std::memory_order_relaxed), Value, Now - IdleSince);
                IdleSince = Now;
            }

            // Never cleared, since another thread may wait on the same counter and AllocCounter resets it.
            if (!bRegistered)
            {
                bRegistered = true;
                Counter->HasThreadWaiters.store(true, std::memory_order_seq_cst);
                continue;
            }

            // Yielding hands this core to a spinning worker, and getting it back costs a scheduler quantum.
            const uint32 Seen = Counter->ThreadWaitSeq.load(std::memory_order_acquire);
            if (Counter->Value.load(std::memory_order_acquire) <= Value)
            {
                break;
            }

            Threading::WaitOnAddress32(Counter->SeqWord(), Seen, kParkTimeoutMs);
            IdleSpins = 0;
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

        // A pure spin waits on a cycle it could break itself, and a hung shutdown says nothing.
        const double Start = PlatformTime::Seconds();
        double NextReport  = Start + 5.0;

        // A quiescence barrier rather than a latency-sensitive wait, so it drains EVERY band.
        const uint32 Slot = GetWorkerIndex();
        uint32 IdleSpins = 0;
        while (G->InFlight.load(std::memory_order_acquire) > 0)
        {
            FQueuedJob Job;
            if (TryStealAny(Job, (uint32)EJobPriority::Background))
            {
                Job.Function(Job.Argument, Slot);
                OnJobComplete(Job.GetCounter(), Slot);
                IdleSpins = 0;
                continue;
            }

            if (++IdleSpins < kAssistSpinsBeforePark)
            {
                CpuPause();
                continue;
            }
            IdleSpins = 0;

            // Published before the sequence is read, so a drain racing this cannot leave us asleep unseen.
            G->HasDrainWaiters.store(true, std::memory_order_seq_cst);
            const uint32 Seen = G->DrainSeq.load(std::memory_order_acquire);
            if (G->InFlight.load(std::memory_order_acquire) > 0)
            {
                Threading::WaitOnAddress32(reinterpret_cast<const volatile uint32*>(&G->DrainSeq),
                    Seen, kParkTimeoutMs);
            }

            const double Now = PlatformTime::Seconds();
            if (Now >= NextReport)
            {
                NextReport = Now + 5.0;
                LOG_WARN("Job system: WaitForAll has been waiting {}s on {} in-flight job(s). "
                         "Something is blocked and not completing.",
                    static_cast<int64>(Now - Start),
                    G->InFlight.load(std::memory_order_relaxed));
            }
        }

        G->HasDrainWaiters.store(false, std::memory_order_release);
    }

    void ParkFiber(FParkFn OnPark, void* Ctx)
    {
        // An external thread must assist-wait, and a native job submits with the park flag if it must park.
        ASSERT(TLS.CurrentFiber != nullptr);

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
        return FFiberHandle{ TLS.CurrentFiber };
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
            OnJobComplete(Job.GetCounter(), Slot);
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
                Depth += G->Workers[w].Queues[P].SizeApprox();
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
