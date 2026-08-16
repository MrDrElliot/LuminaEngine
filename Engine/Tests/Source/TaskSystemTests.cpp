#include <gtest/gtest.h>

#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/TaskGraph.h"
#include "TaskSystem/Task.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
#include "TaskSystem/FiberSync.h"
#include "TaskSystem/Future.h"
#include "Containers/Array.h"
#include "Log/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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

// ----------------------------------------------------------------------------
// Assist-wait isolation (ETaskPriority::Background / EJobPriority::Background)
//
// An external thread that waits on a counter cannot park (it has no fiber), so it "assist-waits": it
// dequeues and runs queued jobs inline until the counter clears. That steal is UNTARGETED -- it takes
// the first job in any worker queue, with no relationship to the counter being awaited. Unbounded,
// that means a main thread waiting on the draw graph can adopt an unrelated multi-hundred-millisecond
// background build and run it to completion inside the wait (which Tracy then renders, correctly and
// very confusingly, as the build nesting under FTaskGraph::Wait).
//
// Background is the band excluded from that path. Both halves of the contract are pinned here: an
// assisting thread must NEVER run Background work, and must STILL run everything else -- the latter is
// what keeps an external wait from starving when the work it needs is queued behind busy workers.
//
// Neither test saturates the pool or spins without a bound. An earlier version tried to occupy every
// worker with blocking jobs and deadlocked when two blockers landed on the same worker: job placement
// rotates through a shared cursor, so "N jobs reach N distinct workers" is not something a test may assume.
// ----------------------------------------------------------------------------

namespace
{
    struct FAssistProbe
    {
        std::thread::id     WaitingThread;
        std::atomic<uint32> RanOnWaitingThread{0};
        std::atomic<uint32> RanTotal{0};
    };

    // Real work, so the queue stays deep enough that an assisting thread gets many chances to steal
    // from it. A trivial body would drain before the wait even starts and prove nothing.
    void AssistProbeJob(void* Arg, uint32)
    {
        FAssistProbe& P = *static_cast<FAssistProbe*>(Arg);

        const auto End = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
        while (std::chrono::steady_clock::now() < End)
        {
        }

        if (std::this_thread::get_id() == P.WaitingThread)
        {
            P.RanOnWaitingThread.fetch_add(1, std::memory_order_relaxed);
        }
        P.RanTotal.fetch_add(1, std::memory_order_relaxed);
    }

    constexpr uint32 kAssistProbeCount = 2048;
}

TEST(TaskSystem, AssistWaitNeverRunsBackgroundWork)
{
    FAssistProbe Probe;
    Probe.WaitingThread = std::this_thread::get_id();

    Jobs::FCounter* ProbeCounter = Jobs::AllocCounter(0);
    for (uint32 i = 0; i < kAssistProbeCount; ++i)
    {
        Jobs::RunJob(&AssistProbeJob, &Probe, Jobs::EJobPriority::Background, ProbeCounter, "AssistTest.Background");
    }

    // The wait is gated by an OS thread, NOT by the Background work itself. Waiting on that work from
    // this thread is the anti-pattern EJobPriority::Background explicitly warns about: this is exactly
    // the thread forbidden from running it, so the wait would make no progress of its own and rest
    // entirely on worker wake-ups. That is a fine thing to warn about and a terrible thing to build a
    // test on -- an earlier version did, and intermittently hung the suite.
    Jobs::FCounter* Gate = Jobs::AllocCounter(1);
    std::thread Releaser([Gate]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Jobs::DecrementCounter(Gate, 1);
    });

    // Assist-waits for ~50ms with a deep Background queue alongside. Pre-fix, this drained dozens of
    // them onto this thread.
    Jobs::WaitForCounter(Gate, 0);
    Releaser.join();

    EXPECT_EQ(Probe.RanOnWaitingThread.load(), 0u)
        << "an assist-waiting thread executed Background work; the exclusion in TryStealAny is not holding, "
           "so a frame-critical wait can again inline an unrelated background build";

    // Drained by polling rather than by waiting on the counter, for the same reason. Bounded, so a
    // stalled pool fails the assertion instead of hanging the suite.
    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Probe.RanTotal.load(std::memory_order_relaxed) < kAssistProbeCount
        && std::chrono::steady_clock::now() < Deadline)
    {
        std::this_thread::yield();
    }

    // Proves the assertion above did not pass vacuously: the work really was there to be stolen.
    EXPECT_EQ(Probe.RanTotal.load(), kAssistProbeCount) << "Background work never drained onto workers";
    EXPECT_EQ(Probe.RanOnWaitingThread.load(), 0u);

    Jobs::FreeCounter(Gate);
    if (Probe.RanTotal.load() == kAssistProbeCount)
    {
        // Only once every job that references it has finished.
        Jobs::FreeCounter(ProbeCounter);
    }
}

TEST(TaskSystem, AssistWaitStillRunsNonBackgroundWork)
{
    // The complement, guarding against over-correcting the above into "assist nothing". The assist path
    // is what stops an external thread's wait from starving while its own fan-out sits queued behind
    // busy workers; if this regresses, every ParallelFor issued from the main thread gets slower and
    // WaitForCounter can stall for as long as the pool stays saturated.
    FAssistProbe Probe;
    Probe.WaitingThread = std::this_thread::get_id();

    Jobs::FCounter* Counter = Jobs::AllocCounter(0);
    for (uint32 i = 0; i < kAssistProbeCount; ++i)
    {
        Jobs::RunJob(&AssistProbeJob, &Probe, Jobs::EJobPriority::Normal, Counter, "AssistTest.Normal");
    }

    Jobs::WaitForCounter(Counter, 0);

    EXPECT_EQ(Probe.RanTotal.load(), kAssistProbeCount);
    EXPECT_GT(Probe.RanOnWaitingThread.load(), 0u)
        << "the assist path ran no work inline during a fan-out-and-wait; an external wait can now starve";

    Jobs::FreeCounter(Counter);
}

// ----------------------------------------------------------------------------
// Fiber pool saturation
//
// A fiber is held for as long as its job is BLOCKED, not just while it runs, so the pool has to cover
// peak simultaneous blocked jobs. Exceed it and the scheduler does not merely slow down: the queued work
// that would release the parked fibers needs a fiber to run on, and the only fibers are the ones parked
// waiting for it. Nothing in the scheduler breaks that cycle, which is why the pool grows on demand.
//
// Both tests deliberately poll instead of waiting on a counter. An external WaitForCounter assist-runs
// queued jobs inline, which papers over the whole failure -- and the paths that actually hang in
// practice (FTaskCompletion::Wait, a fire-and-forget submit nobody waits on) have no such rescue.
// ----------------------------------------------------------------------------

namespace
{
    struct FSaturateProbe
    {
        Jobs::FCounter*     Gate = nullptr;
        std::atomic<uint32> Entered{0};
        std::atomic<uint32> Finished{0};
    };

    void SaturateBlockingJob(void* Arg, uint32 /*Worker*/)
    {
        FSaturateProbe* P = static_cast<FSaturateProbe*>(Arg);
        P->Entered.fetch_add(1, std::memory_order_relaxed);
        Jobs::WaitForCounter(P->Gate, 0); // parks this fiber and holds it until the gate opens
        P->Finished.fetch_add(1, std::memory_order_relaxed);
    }

    // Poll a predicate up to a deadline. Never assists, so a wedged pool shows up as a failed
    // expectation instead of being rescued by the waiting thread (or hanging the suite).
    template<typename FPred>
    bool PollUntil(FPred&& Pred, std::chrono::milliseconds Timeout)
    {
        const auto Deadline = std::chrono::steady_clock::now() + Timeout;
        while (!Pred())
        {
            if (std::chrono::steady_clock::now() >= Deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
}

TEST(TaskSystem, FiberPoolSaturation_MoreBlockedJobsThanFibers)
{
    Jobs::FJobLiveStats Stats;
    Jobs::GetLiveStats(Stats);

    // Past the pool by more than the workers could have in flight transiently, so the excess genuinely
    // cannot be bound without the pool growing. Deliberately NOT guarded against MaxWorkFibers: a build
    // whose ceiling sits at the starting size is one where this deadlocks, and that has to fail here
    // rather than skip. The test recovers either way -- the gate opens after the deadline, which frees
    // the parked fibers and lets the stalled jobs through.
    const uint32 Blockers = Stats.NumWorkFibers + Stats.NumWorkers * 2 + 64;

    // Static so a timed-out run cannot leave jobs pointing at a dead stack frame.
    static FSaturateProbe Probe;
    Probe.Entered.store(0);
    Probe.Finished.store(0);
    Probe.Gate = Jobs::AllocCounter(1);

    for (uint32 i = 0; i < Blockers; ++i)
    {
        Jobs::RunJob(&SaturateBlockingJob, &Probe, Jobs::EJobPriority::Normal, nullptr, "Saturate.Block");
    }

    // Every job must get a fiber even though far more of them are blocked at once than the pool started
    // with. Before on-demand growth this stalled at NumWorkFibers forever, with every worker pinned at
    // 100% in the starvation spin.
    const bool AllEntered = PollUntil([&] { return Probe.Entered.load() == Blockers; },
        std::chrono::seconds(10));
    EXPECT_TRUE(AllEntered) << "only " << Probe.Entered.load() << " of " << Blockers
        << " blocking jobs ever started; the fiber pool wedged instead of growing";

    Jobs::DecrementCounter(Probe.Gate, 1);

    const bool AllFinished = PollUntil([&] { return Probe.Finished.load() == Blockers; },
        std::chrono::seconds(10));
    EXPECT_TRUE(AllFinished) << "only " << Probe.Finished.load() << " of " << Blockers
        << " parked fibers resumed after the gate opened";

    if (AllFinished)
    {
        Jobs::FreeCounter(Probe.Gate); // only once nothing references it
    }

    Jobs::GetLiveStats(Stats);
    EXPECT_GE(Stats.NumWorkFibers, Blockers)
        << "the pool did not grow to cover the blocked set";
}

TEST(TaskSystem, ManyBlockingAsyncTasks_AllComplete)
{
    // The same failure through the public API, in the shape it actually shows up as: a pile of
    // fire-and-forget tasks (asset imports, thumbnails, shader compiles) that each block, then fan out.
    //
    // The gate is what makes it deterministic. Left to race, the outer tasks drain about as fast as they
    // pile up and the pool is never provably exceeded -- an earlier version of this test passed on a
    // 256-fiber build for exactly that reason. Holding every task at the same barrier forces all Outer
    // of them to be blocked, and holding a fiber, simultaneously.
    constexpr uint32 Outer = 400;
    constexpr uint32 Inner = 256;

    struct FProbe
    {
        Jobs::FCounter*     Gate = nullptr;
        std::atomic<uint32> Entered{0};
        std::atomic<uint32> Done{0};
        std::atomic<uint64> Sum{0};
    };
    static FProbe Probe;
    Probe.Entered.store(0);
    Probe.Done.store(0);
    Probe.Sum.store(0);
    Probe.Gate = Jobs::AllocCounter(1);

    for (uint32 i = 0; i < Outer; ++i)
    {
        Task::AsyncTask(1, 1, [](uint32, uint32, uint32)
        {
            Probe.Entered.fetch_add(1, std::memory_order_relaxed);
            Jobs::WaitForCounter(Probe.Gate, 0);

            Task::ParallelFor(Inner, [](uint32)
            {
                Probe.Sum.fetch_add(1, std::memory_order_relaxed);
            }, 8);
            Probe.Done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    const bool AllEntered = PollUntil([&] { return Probe.Entered.load() == Outer; },
        std::chrono::seconds(10));
    EXPECT_TRUE(AllEntered) << "only " << Probe.Entered.load() << " of " << Outer
        << " async tasks ever started; the fiber pool wedged instead of growing";

    Jobs::DecrementCounter(Probe.Gate, 1);

    const bool AllDone = PollUntil([&] { return Probe.Done.load() == Outer; }, std::chrono::seconds(20));
    EXPECT_TRUE(AllDone) << "only " << Probe.Done.load() << " of " << Outer
        << " blocking async tasks completed";
    EXPECT_EQ(Probe.Sum.load(), (uint64)Outer * Inner);

    if (AllDone)
    {
        Jobs::FreeCounter(Probe.Gate);
    }
}

// ----------------------------------------------------------------------------
// External thread slots
// ----------------------------------------------------------------------------

TEST(TaskSystem, ExternalThreadSlotsAreRecycled)
{
    const uint32 Workers = Jobs::GetNumWorkers();
    const uint32 Slots   = Jobs::GetNumThreadSlots() - Workers;
    ASSERT_GT(Slots, 1u);

    // Churn well past the supply. Handing slots out with a monotonic counter leaked one per cycle, and
    // once the supply ran dry every later thread aliased a live one -- silently, since the index is what
    // sizes and indexes every per-thread array in the engine.
    for (uint32 Round = 0; Round < Slots * 4; ++Round)
    {
        std::thread T([&]
        {
            const uint32 Index = Jobs::RegisterExternalThread();
            EXPECT_GE(Index, Workers);
            EXPECT_LT(Index, Jobs::GetNumThreadSlots());
            Jobs::UnregisterExternalThread();
        });
        T.join();
    }

    // After all that churn, concurrently live external threads must still get distinct slots. The main
    // thread holds one of its own, so it is Slots - 1 that are actually available.
    const uint32 Concurrent = Slots - 1;
    std::vector<uint32>      Indices(Concurrent, ~0u);
    std::vector<std::thread> Threads;
    std::atomic<uint32>      Arrived{0};

    Threads.reserve(Concurrent);
    for (uint32 i = 0; i < Concurrent; ++i)
    {
        Threads.emplace_back([&, i]
        {
            Indices[i] = Jobs::RegisterExternalThread();
            // Hold the slot until every thread has one, so the claims genuinely overlap.
            Arrived.fetch_add(1, std::memory_order_acq_rel);
            while (Arrived.load(std::memory_order_acquire) < Concurrent)
            {
                std::this_thread::yield();
            }
            Jobs::UnregisterExternalThread();
        });
    }
    for (std::thread& T : Threads)
    {
        T.join();
    }

    std::vector<uint32> Sorted = Indices;
    std::sort(Sorted.begin(), Sorted.end());
    EXPECT_EQ(std::adjacent_find(Sorted.begin(), Sorted.end()), Sorted.end())
        << "two concurrently live external threads were handed the same slot; per-thread arrays "
           "indexed by GetWorkerIndex() are racing";
}

// Wedged pool recovery: a pool at its ceiling with every fiber parked used to deadlock forever, workers must now drain the queued backlog by running jobs inline on the scheduler fiber.
namespace
{
    struct FWedgeProbe
    {
        Jobs::FCounter*     Gate = nullptr;
        std::atomic<uint32> Finished{0};
        std::atomic<bool>   Released{false};
    };

    void WedgeBlockingJob(void* Arg, uint32 /*Worker*/)
    {
        FWedgeProbe* P = static_cast<FWedgeProbe*>(Arg);
        Jobs::WaitForCounter(P->Gate, 0);
        P->Finished.fetch_add(1, std::memory_order_relaxed);
    }

    void WedgeReleaseJob(void* Arg, uint32 /*Worker*/)
    {
        FWedgeProbe* P = static_cast<FWedgeProbe*>(Arg);
        P->Released.store(true, std::memory_order_relaxed);
        Jobs::DecrementCounter(P->Gate, 1);
    }

    // The wedge needs the ceiling reachable, so these tests cycle the scheduler to a tiny FIXED pool.
    void CycleToWedgeScheduler(uint32& SavedWorkers, uint32& SavedExternal)
    {
        SavedWorkers  = Jobs::GetNumWorkers();
        SavedExternal = Jobs::GetNumThreadSlots() - SavedWorkers;
        Jobs::WaitForAll();
        Jobs::UnregisterExternalThread();
        Jobs::Shutdown();

        Jobs::FConfig Small;
        Small.NumWorkerThreads   = 4;
        Small.NumExternalThreads = SavedExternal;
        Small.NumWorkFibers      = 8;
        Small.MaxWorkFibers      = 8;
        Jobs::Initialize(Small);
        Jobs::RegisterExternalThread();
    }

    void RestoreSchedulerAfterWedge(uint32 SavedWorkers, uint32 SavedExternal)
    {
        Jobs::WaitForAll();
        Jobs::UnregisterExternalThread();
        Jobs::Shutdown();

        Jobs::FConfig Config;
        Config.NumWorkerThreads   = SavedWorkers;
        Config.NumExternalThreads = SavedExternal;
        Jobs::Initialize(Config);
        Jobs::RegisterExternalThread();
    }

    void RunWedgeRecoveryScenario(FWedgeProbe& Probe, Jobs::EJobPriority ReleaserPriority)
    {
        Probe.Finished.store(0);
        Probe.Released.store(false);
        Probe.Gate = Jobs::AllocCounter(1);

        // Far more blocked jobs than the 8-fiber ceiling, so the pool provably wedges before the fix.
        constexpr uint32 Blockers = 32;
        for (uint32 i = 0; i < Blockers; ++i)
        {
            Jobs::RunJob(&WedgeBlockingJob, &Probe, Jobs::EJobPriority::Normal, nullptr, "Wedge.Block");
        }
        Jobs::RunJob(&WedgeReleaseJob, &Probe, ReleaserPriority, nullptr, "Wedge.Release");

        // Nobody opens the gate from outside; recovery must come from workers running queued jobs inline.
        const bool Recovered = PollUntil([&] { return Probe.Finished.load() == Blockers; },
            std::chrono::seconds(15));
        EXPECT_TRUE(Recovered) << "only " << Probe.Finished.load() << " of " << Blockers
            << " blocked jobs completed; the wedged pool never ran the queued release job inline";

        if (!Recovered)
        {
            Jobs::DecrementCounter(Probe.Gate, 1);
            PollUntil([&] { return Probe.Finished.load() == Blockers; }, std::chrono::seconds(10));
        }
        Jobs::WaitForAll();
        Jobs::FreeCounter(Probe.Gate);
    }
}

TEST(TaskSystem, WedgedFiberPool_RecoversByRunningJobsInline)
{
    uint32 SavedWorkers = 0, SavedExternal = 0;
    CycleToWedgeScheduler(SavedWorkers, SavedExternal);

    static FWedgeProbe Probe;
    RunWedgeRecoveryScenario(Probe, Jobs::EJobPriority::Normal);

    RestoreSchedulerAfterWedge(SavedWorkers, SavedExternal);
}

TEST(TaskSystem, WedgedFiberPool_BackgroundReleaseJobStillRuns)
{
    uint32 SavedWorkers = 0, SavedExternal = 0;
    CycleToWedgeScheduler(SavedWorkers, SavedExternal);

    // Assist waits normally refuse Background; wedge recovery must take it anyway or the wedge is permanent.
    static FWedgeProbe Probe;
    RunWedgeRecoveryScenario(Probe, Jobs::EJobPriority::Background);

    RestoreSchedulerAfterWedge(SavedWorkers, SavedExternal);
}
