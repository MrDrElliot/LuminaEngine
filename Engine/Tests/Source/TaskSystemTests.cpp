#include <gtest/gtest.h>

#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/TaskGraph.h"
#include "TaskSystem/Task.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
#include "TaskSystem/FiberSync.h"
#include "TaskSystem/Future.h"
#include "Containers/Array.h"
#include "Log/Log.h"

#include <atomic>
#include <chrono>

using namespace Lumina;

// ----------------------------------------------------------------------------
// ParallelFor
// ----------------------------------------------------------------------------

TEST(TaskSystem, ParallelFor_RunsEachIndexExactlyOnce)
{
    constexpr uint32 N = 100000;
    TVector<int> Visited;
    Visited.resize(N, 0);

    Task::ParallelFor(N, [&](uint32 Index)
    {
        Visited[Index] += 1;
    }, 256);

    for (uint32 i = 0; i < N; ++i)
    {
        ASSERT_EQ(Visited[i], 1) << "index " << i << " visited " << Visited[i] << " times";
    }
}

TEST(TaskSystem, ParallelFor_RangeOverloadCoversContiguously)
{
    constexpr uint32 N = 65536;
    TVector<int> Visited;
    Visited.resize(N, 0);

    std::atomic<uint32> RangeCount{0};

    Task::ParallelFor(N, [&](const Task::FParallelRange& Range)
    {
        RangeCount.fetch_add(1, std::memory_order_relaxed);
        ASSERT_LE(Range.Start, Range.End);
        for (uint32 i = Range.Start; i < Range.End; ++i)
        {
            Visited[i] += 1;
        }
    }, 1024);

    for (uint32 i = 0; i < N; ++i)
    {
        ASSERT_EQ(Visited[i], 1);
    }
    EXPECT_GE(RangeCount.load(), 1u);
}

TEST(TaskSystem, ParallelFor_ZeroIsNoOp)
{
    std::atomic<int> Calls{0};
    Task::ParallelFor(0u, [&](uint32) { Calls.fetch_add(1); });
    EXPECT_EQ(Calls.load(), 0);
}

TEST(TaskSystem, ParallelFor_SingleRunsOnce)
{
    std::atomic<int> Calls{0};
    Task::ParallelFor(1u, [&](uint32 Index)
    {
        EXPECT_EQ(Index, 0u);
        Calls.fetch_add(1);
    });
    EXPECT_EQ(Calls.load(), 1);
}

TEST(TaskSystem, ParallelFor_WorkerIndexInRange)
{
    const uint32 Slots = GTaskSystem->GetNumTaskThreads();
    std::atomic<int> Bad{0};
    Task::ParallelFor(50000u, [&](uint32, uint32 Thread)
    {
        if (Thread >= Slots)
        {
            Bad.fetch_add(1, std::memory_order_relaxed);
        }
    }, 64);
    EXPECT_EQ(Bad.load(), 0) << "a chunk reported a worker index >= GetNumTaskThreads()";
}

TEST(TaskSystem, ParallelForEach_VisitsAndMutates)
{
    TVector<int> Data;
    Data.resize(20000);
    for (int i = 0; i < (int)Data.size(); ++i) Data[i] = i;

    Task::ParallelForEach(Data.begin(), Data.end(), [](int& Value)
    {
        Value *= 2;
    });

    for (int i = 0; i < (int)Data.size(); ++i)
    {
        ASSERT_EQ(Data[i], i * 2);
    }
}

// ----------------------------------------------------------------------------
// AsyncTask
// ----------------------------------------------------------------------------

TEST(TaskSystem, AsyncTask_CompletesAndRunsBody)
{
    std::atomic<uint32> Sum{0};
    constexpr uint32 N = 1000;

    FTaskHandle Handle = Task::AsyncTask(N, 64, [&](uint32 Start, uint32 End, uint32)
    {
        uint32 Local = 0;
        for (uint32 i = Start; i < End; ++i)
        {
            Local += 1;
        }
        Sum.fetch_add(Local, std::memory_order_relaxed);
    });

    ASSERT_NE(Handle, nullptr);
    Handle->Wait();
    EXPECT_TRUE(Handle->IsCompleted());
    EXPECT_EQ(Sum.load(), N);
}

// ----------------------------------------------------------------------------
// TaskGraph dependency ordering
// ----------------------------------------------------------------------------

TEST(TaskSystem, TaskGraph_LinearChainRunsInOrder)
{
    std::atomic<int> Seq{0};
    int OrderA = -1, OrderB = -1, OrderC = -1;

    FTaskGraph Graph;
    auto A = Graph.Add([&] { OrderA = Seq.fetch_add(1); });
    auto B = Graph.Add([&] { OrderB = Seq.fetch_add(1); });
    auto C = Graph.Add([&] { OrderC = Seq.fetch_add(1); });
    Graph.AddDependency(B, A);
    Graph.AddDependency(C, B);
    Graph.Dispatch();
    Graph.Wait();

    EXPECT_LT(OrderA, OrderB);
    EXPECT_LT(OrderB, OrderC);
}

TEST(TaskSystem, TaskGraph_DiamondFanInWaitsForBothParents)
{
    std::atomic<int> Seq{0};
    std::atomic<int> OrderB{-1}, OrderC{-1}, OrderD{-1};

    FTaskGraph Graph;
    auto A = Graph.Add([&] { Seq.fetch_add(1); });
    auto B = Graph.Add([&] { OrderB.store(Seq.fetch_add(1)); });
    auto C = Graph.Add([&] { OrderC.store(Seq.fetch_add(1)); });
    auto D = Graph.Add([&] { OrderD.store(Seq.fetch_add(1)); });
    Graph.AddDependency(B, A);
    Graph.AddDependency(C, A);
    Graph.AddDependency(D, B);
    Graph.AddDependency(D, C);
    Graph.Dispatch();
    Graph.Wait();

    // D must come after both B and C.
    EXPECT_GT(OrderD.load(), OrderB.load());
    EXPECT_GT(OrderD.load(), OrderC.load());
}

// Mirrors the render-extract topology (parallel-for producers + a merge node that
// depends on them) that exposed the root double-scheduling race. Repeated many times
// to stress the dispatch/worker scheduling boundary.
TEST(TaskSystem, TaskGraph_FanOutMerge_Stress)
{
    constexpr int Iterations = 5000;
    FTaskGraph Graph; // reused via Reset() each iteration

    for (int Iter = 0; Iter < Iterations; ++Iter)
    {
        Graph.Reset();

        std::atomic<uint32> ProducedA{0};
        std::atomic<uint32> ProducedB{0};
        std::atomic<int>    MergeRuns{0};
        std::atomic<int>    IndependentRuns{0};
        uint32 SeenA = 0, SeenB = 0;

        // Vary sizes (including empty, like an empty entity view) to exercise edge cases.
        const uint32 CountA = (Iter % 4 == 0) ? 0u : (uint32)(200 + Iter % 500);
        const uint32 CountB = (Iter % 7 == 0) ? 0u : (uint32)(100 + Iter % 300);

        auto NodeA = Graph.AddParallelFor(CountA, 32, [&](const Task::FParallelRange& R)
        {
            ProducedA.fetch_add(R.End - R.Start, std::memory_order_relaxed);
        });
        auto NodeB = Graph.AddParallelFor(CountB, 16, [&](const Task::FParallelRange& R)
        {
            ProducedB.fetch_add(R.End - R.Start, std::memory_order_relaxed);
        });
        auto Independent = Graph.Add([&] { IndependentRuns.fetch_add(1); });
        auto Merge = Graph.Add([&]
        {
            MergeRuns.fetch_add(1);
            SeenA = ProducedA.load(std::memory_order_relaxed);
            SeenB = ProducedB.load(std::memory_order_relaxed);
        });

        Graph.AddDependency(Merge, NodeA);
        Graph.AddDependency(Merge, NodeB);

        Graph.Dispatch();
        Graph.Wait();

        ASSERT_EQ(MergeRuns.load(), 1)        << "merge ran " << MergeRuns.load() << " times at iter " << Iter;
        ASSERT_EQ(IndependentRuns.load(), 1)  << "independent node ran wrong count at iter " << Iter;
        ASSERT_EQ(ProducedA.load(), CountA)   << "producer A miscount at iter " << Iter;
        ASSERT_EQ(ProducedB.load(), CountB)   << "producer B miscount at iter " << Iter;
        // Merge observed fully-produced inputs (dependency honored).
        ASSERT_EQ(SeenA, CountA)              << "merge saw partial A at iter " << Iter;
        ASSERT_EQ(SeenB, CountB)              << "merge saw partial B at iter " << Iter;
    }
}

// ----------------------------------------------------------------------------
// Nested parallelism (fiber yielding must not deadlock or lose work)
// ----------------------------------------------------------------------------

TEST(TaskSystem, NestedParallelFor)
{
    constexpr uint32 Outer = 64;
    constexpr uint32 Inner = 64;
    for (int Round = 0; Round < 50; ++Round)
    {
        std::atomic<uint64> Total{0};
        Task::ParallelFor(Outer, [&](uint32)
        {
            Task::ParallelFor(Inner, [&](uint32)
            {
                Total.fetch_add(1, std::memory_order_relaxed);
            }, 16);
        }, 8);
        ASSERT_EQ(Total.load(), (uint64)Outer * Inner) << "round " << Round;
    }
}

TEST(TaskSystem, ManyConcurrentParallelFors_Stress)
{
    constexpr int Rounds = 2000;
    for (int r = 0; r < Rounds; ++r)
    {
        const uint32 N = 1u + (uint32)(r % 4096);
        std::atomic<uint32> Count{0};
        Task::ParallelFor(N, [&](uint32) { Count.fetch_add(1, std::memory_order_relaxed); }, 1u + (r % 64));
        ASSERT_EQ(Count.load(), N) << "round " << r;
    }
}

// ----------------------------------------------------------------------------
// Performance smoke tests (report numbers; assert only loose bounds)
// ----------------------------------------------------------------------------

TEST(TaskSystem, Perf_EmptyParallelForSchedulingOverhead)
{
    const uint32 Chunks = GTaskSystem->GetNumWorkers() * 4u;
    using Clock = std::chrono::steady_clock;

    auto Empty = [](uint32) {};

    for (int i = 0; i < 2000; ++i)
    {
        Task::ParallelFor(Chunks, Empty, 1); // warmup
    }

    constexpr int Iters = 20000;
    auto T0 = Clock::now();
    for (int i = 0; i < Iters; ++i)
    {
        Task::ParallelFor(Chunks, Empty, 1);
    }
    auto T1 = Clock::now();

    const double NsPerCall = std::chrono::duration<double, std::nano>(T1 - T0).count() / Iters;
    LOG_DISPLAY("[JobBench] Empty ParallelFor ({} chunks): {:.0f} ns/call", Chunks, NsPerCall);

    // Generous bound so CI noise doesn't flake; real numbers are far lower.
    EXPECT_LT(NsPerCall, 200000.0);
}

TEST(TaskSystem, Perf_ParallelForScalesWithWork)
{
    constexpr uint32 N = 4'000'000;
    TVector<float> Data;
    Data.resize(N);
    for (uint32 i = 0; i < N; ++i) Data[i] = (float)(i & 1023);

    auto Heavy = [](float x) -> float
    {
        // A few FMAs so each element costs enough to amortize scheduling.
        float a = x;
        for (int k = 0; k < 16; ++k) a = a * 1.0000001f + 0.5f;
        return a;
    };

    using Clock = std::chrono::steady_clock;

    // Serial baseline.
    volatile float SerialSink = 0.0f;
    auto S0 = Clock::now();
    {
        float acc = 0.0f;
        for (uint32 i = 0; i < N; ++i) acc += Heavy(Data[i]);
        SerialSink = acc;
    }
    auto S1 = Clock::now();
    const double SerialMs = std::chrono::duration<double, std::milli>(S1 - S0).count();

    // Parallel: per-worker partials avoid contention.
    TVector<double> Partials;
    Partials.resize(GTaskSystem->GetNumTaskThreads(), 0.0);

    auto P0 = Clock::now();
    Task::ParallelFor(N, [&](const Task::FParallelRange& R)
    {
        float acc = 0.0f;
        for (uint32 i = R.Start; i < R.End; ++i) acc += Heavy(Data[i]);
        Partials[R.Thread] += acc;
    }, 8192);
    auto P1 = Clock::now();
    const double ParallelMs = std::chrono::duration<double, std::milli>(P1 - P0).count();

    double Combined = 0.0;
    for (double p : Partials) Combined += p;

    const double Speedup = SerialMs / ParallelMs;
    LOG_DISPLAY("[JobBench] ParallelFor scaling: serial={:.2f}ms parallel={:.2f}ms speedup={:.2f}x (workers={})",
        SerialMs, ParallelMs, Speedup, GTaskSystem->GetNumWorkers());

    EXPECT_GT(Combined, 0.0);
    // With many workers we expect a clear win; keep the bar low to avoid flakiness on busy CI.
    EXPECT_GT(Speedup, 1.5);
}

// ----------------------------------------------------------------------------
// Fiber scheduler specifics: park/resume, migration, multi-waiter wakeups
// ----------------------------------------------------------------------------

// Three nesting levels: every level above the leaf parks its fiber on the level below. Exercises
// park/resume depth and pool non-starvation well beyond the 2-level NestedParallelFor case.
TEST(TaskSystem, DeepNestedParallelFor)
{
    constexpr uint32 L0 = 16, L1 = 16, L2 = 16;
    for (int Round = 0; Round < 30; ++Round)
    {
        std::atomic<uint64> Total{0};
        Task::ParallelFor(L0, [&](uint32)
        {
            Task::ParallelFor(L1, [&](uint32)
            {
                Task::ParallelFor(L2, [&](uint32)
                {
                    Total.fetch_add(1, std::memory_order_relaxed);
                }, 4);
            }, 4);
        }, 2);
        ASSERT_EQ(Total.load(), (uint64)L0 * L1 * L2) << "round " << Round;
    }
}

// A job that parks (via a nested ParallelFor) may resume on a different worker. The worker slot read
// after the wait must still be a valid, in-range slot, the across-yield re-read contract. Also a
// liveness probe: the system must keep making progress across migration.
TEST(TaskSystem, WorkerIndexValidAcrossNestedWait)
{
    const uint32 Slots = GTaskSystem->GetNumTaskThreads();
    std::atomic<int> Bad{0};
    std::atomic<int> Ran{0};

    Task::ParallelFor(256u, [&](uint32)
    {
        // Force this fiber to park on inner work, then re-read the slot afterwards.
        Task::ParallelFor(64u, [&](uint32) {}, 4);

        const uint32 After = Jobs::GetWorkerIndex();
        if (After >= Slots)
        {
            Bad.fetch_add(1, std::memory_order_relaxed);
        }
        Ran.fetch_add(1, std::memory_order_relaxed);
    }, 1);

    EXPECT_EQ(Bad.load(), 0) << "GetWorkerIndex() out of range after a nested wait/migration";
    EXPECT_EQ(Ran.load(), 256);
}

// Wide fan-in: many sink nodes all depend on one root, so the root's completion must wake every
// dependent, and the graph counter has many decrements feeding a single Wait. Repeated to stress the
// multi-waiter wake path + counter recycle.
TEST(TaskSystem, TaskGraph_WideFanIn_Stress)
{
    constexpr int Iterations = 2000;
    constexpr int Width = 32;
    FTaskGraph Graph;

    for (int Iter = 0; Iter < Iterations; ++Iter)
    {
        Graph.Reset();

        std::atomic<int> RootRuns{0};
        std::atomic<int> LeafRuns{0};
        std::atomic<int> SinkRuns{0};
        std::atomic<bool> RootDoneBeforeLeaf{true};

        auto Root = Graph.Add([&] { RootRuns.fetch_add(1); });

        FTaskGraph::FNodeHandle Leaves[Width];
        for (int i = 0; i < Width; ++i)
        {
            Leaves[i] = Graph.Add([&]
            {
                if (RootRuns.load() < 1) RootDoneBeforeLeaf.store(false);
                LeafRuns.fetch_add(1);
            });
            Graph.AddDependency(Leaves[i], Root);
        }

        auto Sink = Graph.Add([&]
        {
            if (LeafRuns.load() != Width) RootDoneBeforeLeaf.store(false);
            SinkRuns.fetch_add(1);
        });
        for (int i = 0; i < Width; ++i)
        {
            Graph.AddDependency(Sink, Leaves[i]);
        }

        Graph.Dispatch();
        Graph.Wait();

        ASSERT_EQ(RootRuns.load(), 1)  << "iter " << Iter;
        ASSERT_EQ(LeafRuns.load(), Width) << "iter " << Iter;
        ASSERT_EQ(SinkRuns.load(), 1)  << "iter " << Iter;
        ASSERT_TRUE(RootDoneBeforeLeaf.load()) << "dependency ordering violated at iter " << Iter;
    }
}

// ----------------------------------------------------------------------------
// Fiber-aware mutex
// ----------------------------------------------------------------------------

TEST(FiberSync, Mutex_MutualExclusionUnderContention)
{
    // Many jobs hammer a shared counter guarded by the fiber mutex; if the lock leaks, the
    // non-atomic increment loses updates. Contention forces the worker-fiber park path.
    FFiberMutex Mutex;
    int64 Shared = 0;
    constexpr uint32 N = 20000;

    Task::ParallelFor(N, [&](uint32)
    {
        FFiberScopeLock Lock(Mutex);
        Shared += 1; // deliberately non-atomic, correctness comes from the lock
    }, 1);

    EXPECT_EQ(Shared, static_cast<int64>(N));
}

TEST(FiberSync, Mutex_TryLockReflectsState)
{
    FFiberMutex Mutex;
    ASSERT_TRUE(Mutex.TryLock());
    EXPECT_FALSE(Mutex.TryLock());
    Mutex.Unlock();
    ASSERT_TRUE(Mutex.TryLock());
    Mutex.Unlock();
}

// ----------------------------------------------------------------------------
// Fiber-aware semaphore
// ----------------------------------------------------------------------------

TEST(FiberSync, Semaphore_BoundsConcurrency)
{
    // Permit at most K holders at once; track the live count and assert it never exceeds K.
    constexpr int32 K = 3;
    FFiberSemaphore Sem(K);
    std::atomic<int32> Live{0};
    std::atomic<int32> MaxLive{0};
    std::atomic<int32> Bad{0};

    Task::ParallelFor(4000u, [&](uint32)
    {
        Sem.Acquire();
        const int32 Now = Live.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (Now > K) Bad.fetch_add(1, std::memory_order_relaxed);
        int32 Prev = MaxLive.load(std::memory_order_relaxed);
        while (Now > Prev && !MaxLive.compare_exchange_weak(Prev, Now)) {}
        Live.fetch_sub(1, std::memory_order_acq_rel);
        Sem.Release();
    }, 1);

    EXPECT_EQ(Bad.load(), 0) << "semaphore allowed more than " << K << " concurrent holders";
    EXPECT_LE(MaxLive.load(), K);
}

// ----------------------------------------------------------------------------
// Fiber-aware shared (reader/writer) mutex
// ----------------------------------------------------------------------------

TEST(FiberSync, SharedMutex_NoReaderWriterOverlap)
{
    FFiberSharedMutex Mutex;
    std::atomic<int32> ActiveReaders{0};
    std::atomic<int32> ActiveWriters{0};
    std::atomic<int32> Bad{0};
    int64 Protected = 0;

    Task::ParallelFor(8000u, [&](uint32 Index)
    {
        if ((Index % 8) == 0)
        {
            FFiberWriteScopeLock Lock(Mutex);
            const int32 W = ActiveWriters.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (W != 1 || ActiveReaders.load(std::memory_order_acquire) != 0)
            {
                Bad.fetch_add(1, std::memory_order_relaxed);
            }
            Protected += 1;
            ActiveWriters.fetch_sub(1, std::memory_order_acq_rel);
        }
        else
        {
            FFiberReadScopeLock Lock(Mutex);
            ActiveReaders.fetch_add(1, std::memory_order_acq_rel);
            if (ActiveWriters.load(std::memory_order_acquire) != 0)
            {
                Bad.fetch_add(1, std::memory_order_relaxed);
            }
            ActiveReaders.fetch_sub(1, std::memory_order_acq_rel);
        }
    }, 1);

    EXPECT_EQ(Bad.load(), 0) << "reader/writer exclusivity violated";
    EXPECT_EQ(Protected, 1000) << "8000/8 writer sections expected";
}

// ----------------------------------------------------------------------------
// Fiber-aware condition variable
// ----------------------------------------------------------------------------

TEST(FiberSync, ConditionVariable_ProducerConsumerHandoff)
{
    FFiberMutex Mutex;
    FFiberConditionVariable CV;
    int Value = 0;
    bool Ready = false;
    std::atomic<int> Observed{-1};

    FTaskHandle Consumer = GTaskSystem->ScheduleLambda(1, 0, [&](uint32, uint32, uint32)
    {
        FFiberScopeLock Lock(Mutex);
        CV.Wait(Mutex, [&] { return Ready; });
        Observed.store(Value, std::memory_order_release);
    });

    // Give the consumer time to park, then produce.
    Threading::Sleep(5);
    {
        FFiberScopeLock Lock(Mutex);
        Value = 42;
        Ready = true;
    }
    CV.NotifyOne();

    Consumer->Wait();
    EXPECT_EQ(Observed.load(), 42);
}

// ----------------------------------------------------------------------------
// Futures / promises
// ----------------------------------------------------------------------------

TEST(Future, PromiseSetValue_FutureGetsIt)
{
    TPromise<int> Promise;
    TFuture<int> Future = Promise.GetFuture();
    EXPECT_FALSE(Future.IsReady());

    Promise.SetValue(7);
    EXPECT_TRUE(Future.IsReady());
    EXPECT_EQ(Future.Get(), 7);
}

TEST(Future, WaitIsFiberAware_SetFromAnotherJob)
{
    // A worker fiber waits on a future that a sibling job fulfills, exercises the park-on-future path.
    TPromise<int> Promise;
    TFuture<int> Future = Promise.GetFuture();

    GTaskSystem->ScheduleLambda(1, 0, [Promise = Move(Promise)](uint32, uint32, uint32) mutable
    {
        Threading::Sleep(2);
        Promise.SetValue(99);
    });

    EXPECT_EQ(Future.Get(), 99); // blocks (assist-waits) on the main thread until set
}

TEST(Future, Async_ReturnsResult)
{
    TFuture<int> F = Task::Async([] { return 11 * 11; });
    EXPECT_EQ(F.Get(), 121);
}

TEST(Future, Then_ChainsValue)
{
    TFuture<int> F = Task::Async([] { return 10; })
        .Then([](int& V) { return V + 5; })
        .Then([](int& V) { return V * 2; });
    EXPECT_EQ(F.Get(), 30);
}

TEST(Future, VoidPromiseAndThen)
{
    TPromise<void> Promise;
    std::atomic<int> Ran{0};
    TFuture<int> F = Promise.GetFuture().Then([&] { Ran.fetch_add(1); return 5; });
    Promise.SetValue();
    EXPECT_EQ(F.Get(), 5);
    EXPECT_EQ(Ran.load(), 1);
}

TEST(Future, MakeReadyFuture_IsImmediatelyReady)
{
    TFuture<int> F = MakeReadyFuture(3);
    EXPECT_TRUE(F.IsReady());
    EXPECT_EQ(F.Get(), 3);
}

TEST(Future, WhenAll_CompletesAfterAll)
{
    TVector<TFuture<int>> Futures;
    for (int i = 0; i < 16; ++i)
    {
        Futures.push_back(Task::Async([i] { Threading::Sleep((i % 4)); return i; }));
    }
    TFuture<void> All = WhenAll(Futures);
    All.Get();
    for (int i = 0; i < 16; ++i)
    {
        EXPECT_TRUE(Futures[i].IsReady()) << "future " << i << " not ready after WhenAll";
    }
}

namespace
{
    TTask<int> CoConst()
    {
        co_return 42;
    }

    TTask<int> CoAdd(int A, int B)
    {
        co_return A + B;
    }

    // Deep recursive chain, unwound via symmetric transfer.
    TTask<uint64> CoSumTo(uint32 N)
    {
        if (N == 0)
        {
            co_return 0;
        }
        const uint64 Rest = co_await CoSumTo(N - 1);
        co_return Rest + N;
    }

    TTask<void> CoIncrement(std::atomic<int>* Counter)
    {
        Counter->fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    TTask<int> CoWhenAllFan(std::atomic<int>* Counter, int N)
    {
        TVector<TTask<void>> Tasks;
        for (int i = 0; i < N; ++i)
        {
            Tasks.push_back(CoIncrement(Counter));
        }
        co_await WhenAll(Move(Tasks));
        co_return Counter->load(std::memory_order_relaxed);
    }

    TTask<int> CoAwaitFuture(TFuture<int> F)
    {
        const int V = co_await Move(F);
        co_return V * 2;
    }

    TTask<uint64> CoParallelSum(uint32 N)
    {
        std::atomic<uint64> Sum{0};
        co_await Task::ParallelForAsync(N, 64, [&](uint32 i)
        {
            Sum.fetch_add(i, std::memory_order_relaxed);
        });
        co_return Sum.load(std::memory_order_relaxed);
    }

    TTask<void> CoSetPromise(TPromise<int> P, int Value)
    {
        P.SetValue(Value);
        co_return;
    }
}

TEST(Coro, SyncWait_ReturnsResult)
{
    EXPECT_EQ(SyncWait(CoConst()), 42);
    EXPECT_EQ(SyncWait(CoAdd(20, 22)), 42);
}

TEST(Coro, DeepChain_SymmetricTransfer)
{
    constexpr uint32 N = 4000;
    const uint64 Expected = (uint64)N * (N + 1) / 2;
    EXPECT_EQ(SyncWait(CoSumTo(N)), Expected);
}

TEST(Coro, WhenAll_RunsEveryChild)
{
    std::atomic<int> Counter{0};
    const int Result = SyncWait(CoWhenAllFan(&Counter, 128));
    EXPECT_EQ(Result, 128);
    EXPECT_EQ(Counter.load(), 128);
}

TEST(Coro, AwaitFuture_BridgesFuturesLayer)
{
    TPromise<int> Promise;
    TFuture<int>  Future = Promise.GetFuture();

    // Fulfil on a worker while the coroutine (driven by SyncWait) awaits the future.
    Task::Async([P = Move(Promise)]() mutable { P.SetValue(21); });

    EXPECT_EQ(SyncWait(CoAwaitFuture(Move(Future))), 42);
}

TEST(Coro, ParallelForAsync_SumsRange)
{
    constexpr uint32 N = 10000;
    const uint64 Expected = (uint64)(N - 1) * N / 2;
    EXPECT_EQ(SyncWait(CoParallelSum(N)), Expected);
}

TEST(Coro, ParallelForAsync_ZeroIsNoOp)
{
    EXPECT_EQ(SyncWait(CoParallelSum(0)), 0u);
}

TEST(Coro, LaunchDetached_RunsToCompletion)
{
    TPromise<int> Promise;
    TFuture<int>  Future = Promise.GetFuture();

    Launch(CoSetPromise(Move(Promise), 7));

    EXPECT_EQ(Future.Get(), 7);
}

// SyncWait from inside a worker fiber must not deadlock.
TEST(Coro, SyncWait_FromWorkerFiber)
{
    std::atomic<int> Ran{0};
    Task::ParallelFor(8u, [&](uint32)
    {
        if (SyncWait(CoConst()) == 42)
        {
            Ran.fetch_add(1, std::memory_order_relaxed);
        }
    }, 1);
    EXPECT_EQ(Ran.load(), 8);
}

TEST(Coro, ManyConcurrentWhenAll_Stress)
{
    for (int Round = 0; Round < 200; ++Round)
    {
        std::atomic<int> Counter{0};
        const int Result = SyncWait(CoWhenAllFan(&Counter, 64));
        ASSERT_EQ(Result, 64) << "round " << Round;
    }
}
