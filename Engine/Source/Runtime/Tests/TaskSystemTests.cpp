#include <gtest/gtest.h>

#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/TaskSystem.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

using namespace Lumina;

namespace
{
    Jobs::TTask<int64> Fib(int32 N)
    {
        if (N < 2)
        {
            co_return N;
        }
        int64 Left = 0;
        co_await Jobs::Fork(&Left, Fib(N - 1));
        const int64 Right = co_await Jobs::Call(Fib(N - 2));
        co_await Jobs::Join();
        co_return Left + Right;
    }

    Jobs::TTask<int64> SumRange(const int32* Data, uint32 Count)
    {
        if (Count <= 64)
        {
            int64 Sum = 0;
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                Sum += Data[Index];
            }
            co_return Sum;
        }
        const uint32 Half = Count / 2;
        int64 Left  = 0;
        int64 Right = 0;
        co_await Jobs::Fork(&Left, SumRange(Data, Half));
        co_await Jobs::Fork(&Right, SumRange(Data + Half, Count - Half));
        co_await Jobs::Join();
        co_return Left + Right;
    }

    Jobs::TTask<int32> Square(int32 Value)
    {
        co_return Value * Value;
    }

    Jobs::TTask<void> FanOutSquares(int32* Out, int32 Count)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            co_await Jobs::Fork(&Out[Index], Square(Index));
        }
        co_await Jobs::Join();
    }

    Jobs::TTask<int64> NestedSyncWait(int32 N)
    {
        co_return Jobs::SyncWait(&Fib, N);
    }

    Jobs::TTask<int64> ForkNestedSyncWaits(int32 Count, int32 N)
    {
        TVector<int64> Results;
        Results.SetNum(Count, 0);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            co_await Jobs::Fork(&Results[Index], NestedSyncWait(N));
        }
        co_await Jobs::Join();
        int64 Total = 0;
        for (int64 Value : Results)
        {
            Total += Value;
        }
        co_return Total;
    }

    Jobs::TTask<void> ForEachInTask(std::atomic<uint32>* Hits, uint32 Num)
    {
        co_await Jobs::Call(Jobs::ForEach(Num, [Hits](uint32 Index) { Hits[Index].fetch_add(1, std::memory_order_relaxed); }, 16));
    }

    Jobs::TTask<void> ForEachSlotsInTask(std::atomic<uint32>* BadSlots, uint32 Num)
    {
        const uint32 Slots = Jobs::GetNumThreadSlots();
        co_await Jobs::Call(Jobs::ForEach(Num, [BadSlots, Slots](uint32, uint32 Thread)
        {
            if (Thread >= Slots)
            {
                BadSlots->fetch_add(1, std::memory_order_relaxed);
            }
        }, 8));
    }

    Jobs::TTask<void> FillAndSignal(int32* Out, int32 Value, Jobs::FTaskEvent* Done)
    {
        *Out = Value;
        Done->Signal();
        co_return;
    }

    Jobs::TTask<void> CountAndSignal(std::atomic<int32>* Count, Jobs::FTaskEvent* Done)
    {
        Count->fetch_add(1, std::memory_order_relaxed);
        Done->Signal();
        co_return;
    }

    Jobs::TTask<void> AwaitThenCount(Jobs::FTaskEvent* Gate, std::atomic<int32>* Resumed)
    {
        co_await *Gate;
        Resumed->fetch_add(1, std::memory_order_relaxed);
    }

    Jobs::TTask<void> ForkWaiters(Jobs::FTaskEvent* Gate, std::atomic<int32>* Resumed, int32 Count, Jobs::FTaskEvent* Done)
    {
        for (int32 Index = 0; Index < Count; ++Index)
        {
            co_await Jobs::Fork(AwaitThenCount(Gate, Resumed));
        }
        co_await Jobs::Join();
        Done->Signal();
    }

    Jobs::TTask<void> CountUp(std::atomic<int32>* Counter, Jobs::FLatch* Latch)
    {
        Counter->fetch_add(1, std::memory_order_acq_rel);
        Latch->CountDown();
        co_return;
    }

    Jobs::TTask<void> ObserveAfterLatch(Jobs::FLatch* Latch, std::atomic<int32>* Counter, std::atomic<int32>* Observed, Jobs::FTaskEvent* Done)
    {
        co_await *Latch;
        Observed->store(Counter->load(std::memory_order_acquire), std::memory_order_release);
        Done->Signal();
    }

    Jobs::TTask<void> AwaitAdd(Jobs::FTaskEvent* Gate, std::atomic<int32>* Sum, int32 Value)
    {
        co_await *Gate;
        Sum->fetch_add(Value, std::memory_order_relaxed);
    }

    Jobs::TTask<void> ForkAndAwaitSelf(Jobs::FTaskEvent* Gate, std::atomic<int32>* Sum, Jobs::FTaskEvent* Done)
    {
        co_await Jobs::Fork(AwaitAdd(Gate, Sum, 1));
        co_await *Gate;
        Sum->fetch_add(10, std::memory_order_relaxed);
        co_await Jobs::Join();
        Done->Signal();
    }

    Jobs::TTask<void> ReportWorkerThread(std::atomic<int32>* State)
    {
        State->store(Jobs::IsWorkerThread() && Jobs::GetThreadSlot() < Jobs::GetNumWorkers() ? 1 : 2, std::memory_order_release);
        co_return;
    }

    Jobs::TTask<int32> PriorityValue(int32 Value)
    {
        co_return Value;
    }

    Jobs::TTask<void> BumpAndSignal(std::atomic<int32>* Count, int32 Total, Jobs::FTaskEvent* Done)
    {
        if (Count->fetch_add(1, std::memory_order_acq_rel) + 1 == Total)
        {
            Done->Signal();
        }
        co_return;
    }

    struct FAssistProbe
    {
        std::thread::id     WaitingThread;
        std::atomic<uint32> RanTotal{0};
        std::atomic<uint32> RanOnWaitingThread{0};

        void Run()
        {
            RanTotal.fetch_add(1, std::memory_order_relaxed);
            if (std::this_thread::get_id() == WaitingThread)
            {
                RanOnWaitingThread.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    constexpr uint32 kAssistProbeCount = 2048;

    Jobs::TTask<void> RunProbe(FAssistProbe* Probe)
    {
        Probe->Run();
        co_return;
    }

    Jobs::TTask<void> ProbeFanOut(FAssistProbe* Probe)
    {
        co_await Jobs::Call(Jobs::ForEach(kAssistProbeCount, [Probe](uint32) { Probe->Run(); }, 1));
    }

    bool WaitForCount(const std::atomic<uint32>& Counter, uint32 Target, double TimeoutSeconds)
    {
        const double Deadline = PlatformTime::Seconds() + TimeoutSeconds;
        while (Counter.load(std::memory_order_relaxed) < Target)
        {
            if (PlatformTime::Seconds() >= Deadline)
            {
                return false;
            }
            PlatformTime::YieldThread();
        }
        return true;
    }

    uint32 FreeExternalSlots()
    {
        (void)Jobs::GetThreadSlot();
        return Jobs::GetNumThreadSlots() - Jobs::GetNumWorkers() - 1;
    }
}

TEST(TaskSystem, StartRuntime_ClampsSlotsToMax)
{
    Jobs::Shutdown();
    Jobs::FRuntimeConfig Config;
    Config.NumWorkers         = Jobs::kMaxThreadSlots * 2;
    Config.NumExternalThreads = Jobs::kMaxThreadSlots;
    Jobs::StartRuntime(Config);

    const uint32 Slots   = Jobs::GetNumThreadSlots();
    const uint32 Workers = Jobs::GetNumWorkers();
    const int64 Result   = Jobs::SyncWait(&Fib, 20);
    Jobs::Shutdown();
    Jobs::Initialize();

    EXPECT_EQ(Slots, Jobs::kMaxThreadSlots);
    EXPECT_GE(Workers, 1u);
    EXPECT_EQ(Result, 6765);
}

TEST(TaskSystem, ParallelFor_RunsEachIndexExactlyOnce)
{
    constexpr uint32 N = 100000;
    TVector<int> Visited;
    Visited.SetNum(N, 0);

    Jobs::ParallelFor(N, [&](uint32 Index)
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
    Visited.SetNum(N, 0);

    std::atomic<uint32> RangeCount{0};

    Jobs::ParallelFor(N, [&](const Jobs::FParallelRange& Range)
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
    Jobs::ParallelFor(0u, [&](uint32) { Calls.fetch_add(1); });
    EXPECT_EQ(Calls.load(), 0);
}

TEST(TaskSystem, ParallelFor_SingleRunsOnce)
{
    std::atomic<int> Calls{0};
    Jobs::ParallelFor(1u, [&](uint32 Index)
    {
        EXPECT_EQ(Index, 0u);
        Calls.fetch_add(1);
    });
    EXPECT_EQ(Calls.load(), 1);
}

TEST(TaskSystem, ParallelFor_ThreadSlotInRange)
{
    const uint32 Slots = Jobs::GetNumThreadSlots();
    std::atomic<int> Bad{0};
    Jobs::ParallelFor(50000u, [&](uint32, uint32 Thread)
    {
        if (Thread >= Slots || Thread != Jobs::GetThreadSlot())
        {
            Bad.fetch_add(1, std::memory_order_relaxed);
        }
    }, 64);
    EXPECT_EQ(Bad.load(), 0);
}

TEST(TaskSystem, ParallelFor_RunsOnEveryBand)
{
    for (Jobs::EJobPriority Band : { Jobs::EJobPriority::High, Jobs::EJobPriority::Normal, Jobs::EJobPriority::Low, Jobs::EJobPriority::Background })
    {
        std::atomic<uint32> Count{0};
        Jobs::ParallelFor(5000u, [&](uint32) { Count.fetch_add(1, std::memory_order_relaxed); }, 32, Band);
        EXPECT_EQ(Count.load(), 5000u) << "band " << static_cast<uint32>(Band);
    }
}

TEST(TaskSystem, ParallelForEach_VisitsAndMutates)
{
    TVector<int> Data;
    Data.SetNum(20000);
    for (int i = 0; i < Data.Num(); ++i)
    {
        Data[i] = i;
    }

    Jobs::ParallelForEach(Data.begin(), Data.end(), [](int& Value)
    {
        Value *= 2;
    });

    for (int i = 0; i < Data.Num(); ++i)
    {
        ASSERT_EQ(Data[i], i * 2);
    }
}

TEST(TaskSystem, ParallelForOrSerial_BelowThresholdRunsOneInlineRange)
{
    const std::thread::id Caller = std::this_thread::get_id();
    uint32 Calls = 0;
    bool bOffThread = false;
    Jobs::ParallelForOrSerial(100, 1000, [&](const Jobs::FParallelRange& Range)
    {
        ++Calls;
        bOffThread |= std::this_thread::get_id() != Caller;
        EXPECT_EQ(Range.Start, 0u);
        EXPECT_EQ(Range.End, 100u);
        EXPECT_EQ(Range.Thread, Jobs::GetThreadSlot());
    });
    EXPECT_EQ(Calls, 1u);
    EXPECT_FALSE(bOffThread);
}

TEST(TaskSystem, ParallelForOrSerial_VisitsEveryIndexOnBothSidesOfThreshold)
{
    for (uint32 Num : { 10u, 50000u })
    {
        std::vector<std::atomic<int>> Visits(Num);
        Jobs::ParallelForOrSerial(Num, 1000, [&](uint32 Index)
        {
            Visits[Index].fetch_add(1, std::memory_order_relaxed);
        }, 64);

        for (uint32 i = 0; i < Num; ++i)
        {
            ASSERT_EQ(Visits[i].load(), 1) << "Num " << Num << " index " << i;
        }
    }
}

TEST(TaskSystem, ForEach_InsideATaskCoversEveryIndex)
{
    constexpr uint32 Num = 30000;
    std::vector<std::atomic<uint32>> Hits(Num);
    Jobs::SyncWait(&ForEachInTask, Hits.data(), Num);

    for (uint32 i = 0; i < Num; ++i)
    {
        ASSERT_EQ(Hits[i].load(), 1u) << "index " << i;
    }
}

TEST(TaskSystem, ForEach_ThreadSlotsStayInRange)
{
    std::atomic<uint32> BadSlots{0};
    Jobs::SyncWait(&ForEachSlotsInTask, &BadSlots, 20000u);
    EXPECT_EQ(BadSlots.load(), 0u);
}

TEST(TaskSystem, NestedParallelFor)
{
    constexpr uint32 Outer = 64;
    constexpr uint32 Inner = 64;
    for (int Round = 0; Round < 50; ++Round)
    {
        std::atomic<uint64> Total{0};
        Jobs::ParallelFor(Outer, [&](uint32)
        {
            Jobs::ParallelFor(Inner, [&](uint32)
            {
                Total.fetch_add(1, std::memory_order_relaxed);
            }, 16);
        }, 8);
        ASSERT_EQ(Total.load(), (uint64)Outer * Inner) << "round " << Round;
    }
}

TEST(TaskSystem, DeepNestedParallelFor)
{
    constexpr uint32 L0 = 16, L1 = 16, L2 = 16;
    for (int Round = 0; Round < 30; ++Round)
    {
        std::atomic<uint64> Total{0};
        Jobs::ParallelFor(L0, [&](uint32)
        {
            Jobs::ParallelFor(L1, [&](uint32)
            {
                Jobs::ParallelFor(L2, [&](uint32)
                {
                    Total.fetch_add(1, std::memory_order_relaxed);
                }, 4);
            }, 4);
        }, 2);
        ASSERT_EQ(Total.load(), (uint64)L0 * L1 * L2) << "round " << Round;
    }
}

TEST(TaskSystem, ManyParallelFors_Stress)
{
    for (int r = 0; r < 2000; ++r)
    {
        const uint32 N = 1u + (uint32)(r % 4096);
        std::atomic<uint32> Count{0};
        Jobs::ParallelFor(N, [&](uint32) { Count.fetch_add(1, std::memory_order_relaxed); }, 1u + (r % 64));
        ASSERT_EQ(Count.load(), N) << "round " << r;
    }
}

TEST(TaskSystem, ThreadSlotValidAfterNestedWait)
{
    const uint32 Slots = Jobs::GetNumThreadSlots();
    std::atomic<int> Bad{0};
    std::atomic<int> Ran{0};

    Jobs::ParallelFor(256u, [&](uint32)
    {
        Jobs::ParallelFor(64u, [&](uint32) {}, 4);

        if (Jobs::GetThreadSlot() >= Slots)
        {
            Bad.fetch_add(1, std::memory_order_relaxed);
        }
        Ran.fetch_add(1, std::memory_order_relaxed);
    }, 1);

    EXPECT_EQ(Bad.load(), 0);
    EXPECT_EQ(Ran.load(), 256);
}

TEST(TaskSystem, ForkJoin_FibonacciResults)
{
    EXPECT_EQ(Jobs::SyncWait(&Fib, 0), 0);
    EXPECT_EQ(Jobs::SyncWait(&Fib, 1), 1);
    EXPECT_EQ(Jobs::SyncWait(&Fib, 2), 1);
    EXPECT_EQ(Jobs::SyncWait(&Fib, 20), 6765);
    EXPECT_EQ(Jobs::SyncWait(&Fib, 27), 196418);
}

TEST(TaskSystem, ForkJoin_RepeatedFibonacciStress)
{
    for (int Round = 0; Round < 300; ++Round)
    {
        ASSERT_EQ(Jobs::SyncWait(&Fib, 16), 987) << "round " << Round;
    }
}

TEST(TaskSystem, ForkJoin_TreeReductionMatchesSerial)
{
    TVector<int32> Data;
    Data.SetNum(200003);
    int64 Expected = 0;
    for (int32 i = 0; i < Data.Num(); ++i)
    {
        Data[i] = (i * 7) % 1000 - 500;
        Expected += Data[i];
    }

    EXPECT_EQ(Jobs::SyncWait(&SumRange, Data.GetData(), static_cast<uint32>(Data.Num())), Expected);
}

TEST(TaskSystem, ForkJoin_FanOutWritesEveryResult)
{
    constexpr int32 Count = 512;
    TVector<int32> Out;
    Out.SetNum(Count, -1);
    Jobs::SyncWait(&FanOutSquares, Out.GetData(), Count);

    for (int32 i = 0; i < Count; ++i)
    {
        ASSERT_EQ(Out[i], i * i) << "index " << i;
    }
}

TEST(TaskSystem, SyncWait_NestedInsideATask)
{
    EXPECT_EQ(Jobs::SyncWait(&NestedSyncWait, 18), 2584);
    EXPECT_EQ(Jobs::SyncWait(&ForkNestedSyncWaits, 16, 15), 16 * 610);
}

TEST(TaskSystem, SyncWait_InsideAParallelForBody)
{
    std::atomic<int64> Total{0};
    Jobs::ParallelFor(64u, [&](uint32)
    {
        Total.fetch_add(Jobs::SyncWait(&Fib, 12), std::memory_order_relaxed);
    }, 1);
    EXPECT_EQ(Total.load(), 64 * 144);
}

TEST(TaskSystem, SyncWait_RunsOnEveryBandAndReturnsValue)
{
    EXPECT_EQ(Jobs::SyncWait(Jobs::EJobPriority::High, &PriorityValue, 1), 1);
    EXPECT_EQ(Jobs::SyncWait(Jobs::EJobPriority::Normal, &PriorityValue, 2), 2);
    EXPECT_EQ(Jobs::SyncWait(Jobs::EJobPriority::Low, &PriorityValue, 3), 3);
    EXPECT_EQ(Jobs::SyncWait(Jobs::EJobPriority::Background, &PriorityValue, 4), 4);
}

TEST(TaskSystem, Spawn_RunsOnAWorkerThread)
{
    std::atomic<int32> State{0};
    Jobs::Spawn(Jobs::EJobPriority::Normal, &ReportWorkerThread, &State);
    const double Deadline = PlatformTime::Seconds() + 10.0;
    while (State.load(std::memory_order_acquire) == 0 && PlatformTime::Seconds() < Deadline)
    {
        PlatformTime::YieldThread();
    }
    EXPECT_EQ(State.load(), 1);
    EXPECT_FALSE(Jobs::IsWorkerThread());
    Jobs::WaitForSpawned();
}

TEST(TaskSystem, Spawn_WithAnEventDeliversTheResult)
{
    int32 Out = 0;
    Jobs::FTaskEvent Done;
    Jobs::Spawn(Jobs::EJobPriority::Normal, &FillAndSignal, &Out, 42, &Done);
    Done.Wait();

    EXPECT_TRUE(Done.IsSignaled());
    EXPECT_EQ(Out, 42);
}

TEST(TaskSystem, Spawn_ManyTasksAllRunBeforeWaitForSpawnedReturns)
{
    constexpr int32 Total = 5000;
    std::atomic<int32> Count{0};
    Jobs::FTaskEvent Done;
    for (int32 i = 0; i < Total; ++i)
    {
        Jobs::Spawn(Jobs::EJobPriority::Normal, &BumpAndSignal, &Count, Total, &Done);
    }
    Jobs::WaitForSpawned();

    EXPECT_EQ(Count.load(), Total);
    EXPECT_TRUE(Done.IsSignaled());
}

TEST(TaskSystem, Spawn_RunsOnEveryBand)
{
    constexpr int32 PerBand = 200;
    std::atomic<int32> Count{0};
    Jobs::FTaskEvent Done;
    for (int32 i = 0; i < PerBand; ++i)
    {
        Jobs::Spawn(Jobs::EJobPriority::High, &BumpAndSignal, &Count, PerBand * 4, &Done);
        Jobs::Spawn(Jobs::EJobPriority::Normal, &BumpAndSignal, &Count, PerBand * 4, &Done);
        Jobs::Spawn(Jobs::EJobPriority::Low, &BumpAndSignal, &Count, PerBand * 4, &Done);
        Jobs::Spawn(Jobs::EJobPriority::Background, &BumpAndSignal, &Count, PerBand * 4, &Done);
    }
    Jobs::WaitForSpawned();
    EXPECT_EQ(Count.load(), PerBand * 4);
}

TEST(TaskSystem, Event_SignalResetAndIsSignaled)
{
    Jobs::FTaskEvent Event;
    EXPECT_FALSE(Event.IsSignaled());
    Event.Signal();
    EXPECT_TRUE(Event.IsSignaled());
    Event.Wait();
    Event.Reset();
    EXPECT_FALSE(Event.IsSignaled());
    Event.Signal();
    EXPECT_TRUE(Event.IsSignaled());
}

TEST(TaskSystem, Event_AlreadySignaledDoesNotSuspendATask)
{
    Jobs::FTaskEvent Gate;
    Gate.Signal();
    std::atomic<int32> Resumed{0};
    Jobs::SyncWait(&AwaitThenCount, &Gate, &Resumed);
    EXPECT_EQ(Resumed.load(), 1);
}

TEST(TaskSystem, Event_ResumesEveryTaskParkedOnIt)
{
    constexpr int32 Waiters = 32;
    Jobs::FTaskEvent Gate;
    Jobs::FTaskEvent Done;
    std::atomic<int32> Resumed{0};

    Jobs::Spawn(Jobs::EJobPriority::Normal, &ForkWaiters, &Gate, &Resumed, Waiters, &Done);
    PlatformTime::SleepMilliseconds(20);
    EXPECT_EQ(Resumed.load(), 0);
    EXPECT_FALSE(Done.IsSignaled());

    Gate.Signal();
    Done.Wait();
    EXPECT_EQ(Resumed.load(), Waiters);
}

TEST(TaskSystem, Event_SignaledFromAnExternalThreadResumesATask)
{
    Jobs::FTaskEvent Gate;
    Jobs::FTaskEvent Done;
    std::atomic<int32> Resumed{0};

    Jobs::Spawn(Jobs::EJobPriority::Normal, &ForkWaiters, &Gate, &Resumed, 4, &Done);
    FThread Signaler([&Gate]
    {
        PlatformTime::SleepMilliseconds(30);
        Gate.Signal();
        Jobs::ReleaseThreadSlot();
    });
    Done.Wait();
    Signaler.join();
    EXPECT_EQ(Resumed.load(), 4);
}

TEST(TaskSystem, Event_ParentAndForkedChildBothParkOnTheSameEvent)
{
    for (int Round = 0; Round < 50; ++Round)
    {
        Jobs::FTaskEvent Gate;
        Jobs::FTaskEvent Done;
        std::atomic<int32> Sum{0};

        Jobs::Spawn(Jobs::EJobPriority::Normal, &ForkAndAwaitSelf, &Gate, &Sum, &Done);
        PlatformTime::SleepMilliseconds(1);
        Gate.Signal();
        Done.Wait();
        ASSERT_EQ(Sum.load(), 11) << "round " << Round;
    }
}

TEST(TaskSystem, Latch_ReleasesTasksAfterTheLastCountDown)
{
    constexpr int32 Counters = 64;
    constexpr int32 Waiters  = 4;
    Jobs::FLatch Latch(Counters);
    std::atomic<int32> Counter{0};
    std::atomic<int32> Observed[Waiters];
    Jobs::FTaskEvent Done[Waiters];

    for (int32 i = 0; i < Waiters; ++i)
    {
        Observed[i].store(-1);
        Jobs::Spawn(Jobs::EJobPriority::Normal, &ObserveAfterLatch, &Latch, &Counter, &Observed[i], &Done[i]);
    }
    for (int32 i = 0; i < Counters; ++i)
    {
        Jobs::Spawn(Jobs::EJobPriority::Normal, &CountUp, &Counter, &Latch);
    }

    for (int32 i = 0; i < Waiters; ++i)
    {
        Done[i].Wait();
        EXPECT_EQ(Observed[i].load(), Counters) << "waiter " << i;
    }
    Latch.Wait();
    EXPECT_TRUE(Latch.IsDone());
}

TEST(TaskSystem, Latch_ZeroCountIsAlreadyDone)
{
    Jobs::FLatch Latch(0);
    EXPECT_TRUE(Latch.IsDone());
    Latch.Wait();
}

TEST(TaskSystem, Priority_BackgroundIsNeverRunByAForegroundWait)
{
    FAssistProbe Probe;
    Probe.WaitingThread = std::this_thread::get_id();

    for (uint32 i = 0; i < kAssistProbeCount; ++i)
    {
        Jobs::Spawn(Jobs::EJobPriority::Background, &RunProbe, &Probe);
    }

    Jobs::FTaskEvent Gate;
    FThread Releaser([&Gate]
    {
        PlatformTime::SleepMilliseconds(50);
        Gate.Signal();
        Jobs::ReleaseThreadSlot();
    });
    Gate.Wait();
    Releaser.join();

    EXPECT_EQ(Probe.RanOnWaitingThread.load(), 0u);
    EXPECT_TRUE(WaitForCount(Probe.RanTotal, kAssistProbeCount, 10.0));
    Jobs::WaitForSpawned();
    EXPECT_EQ(Probe.RanOnWaitingThread.load(), 0u);
}

TEST(TaskSystem, Priority_WaitRunsTheBackgroundWorkItIsBlockedOn)
{
    FAssistProbe Probe;
    Probe.WaitingThread = std::this_thread::get_id();

    Jobs::SyncWait(Jobs::EJobPriority::Background, &ProbeFanOut, &Probe);

    EXPECT_EQ(Probe.RanTotal.load(), kAssistProbeCount);
    EXPECT_GT(Probe.RanOnWaitingThread.load(), 0u);
}

TEST(TaskSystem, Priority_WaitStillRunsForegroundWork)
{
    FAssistProbe Probe;
    Probe.WaitingThread = std::this_thread::get_id();

    Jobs::SyncWait(Jobs::EJobPriority::Normal, &ProbeFanOut, &Probe);

    EXPECT_EQ(Probe.RanTotal.load(), kAssistProbeCount);
    EXPECT_GT(Probe.RanOnWaitingThread.load(), 0u);
}

TEST(TaskSystem, ExternalThreads_ManyRunTasksConcurrently)
{
    const uint32 Threads = std::min(FreeExternalSlots(), 6u);
    if (Threads < 2)
    {
        GTEST_SKIP() << "not enough external thread slots";
    }

    std::atomic<uint32> Failures{0};
    std::vector<FThread> Pool;
    for (uint32 t = 0; t < Threads; ++t)
    {
        Pool.emplace_back([&Failures]
        {
            for (int Round = 0; Round < 40; ++Round)
            {
                if (Jobs::SyncWait(&Fib, 15) != 610)
                {
                    Failures.fetch_add(1);
                }

                std::atomic<uint32> Count{0};
                Jobs::ParallelFor(2000u, [&](uint32) { Count.fetch_add(1, std::memory_order_relaxed); }, 16);
                if (Count.load() != 2000u)
                {
                    Failures.fetch_add(1);
                }

                int32 Out = 0;
                Jobs::FTaskEvent Done;
                Jobs::Spawn(Jobs::EJobPriority::Normal, &FillAndSignal, &Out, Round, &Done);
                Done.Wait();
                if (Out != Round)
                {
                    Failures.fetch_add(1);
                }
            }
            Jobs::ReleaseThreadSlot();
        });
    }
    for (FThread& Thread : Pool)
    {
        Thread.join();
    }
    EXPECT_EQ(Failures.load(), 0u);
}

TEST(TaskSystem, ExternalThreads_SlotsAreRecycled)
{
    const uint32 Workers  = Jobs::GetNumWorkers();
    const uint32 Externals = FreeExternalSlots();
    ASSERT_GT(Externals, 0u);

    for (uint32 Round = 0; Round < (Externals + 1) * 4; ++Round)
    {
        FThread Thread([Workers]
        {
            const uint32 Slot = Jobs::GetThreadSlot();
            EXPECT_GE(Slot, Workers);
            EXPECT_LT(Slot, Jobs::GetNumThreadSlots());
            Jobs::ReleaseThreadSlot();
        });
        Thread.join();
    }

    std::vector<uint32> Slots(Externals, ~0u);
    std::vector<FThread> Pool;
    std::atomic<uint32> Arrived{0};
    for (uint32 i = 0; i < Externals; ++i)
    {
        Pool.emplace_back([&, i]
        {
            Slots[i] = Jobs::GetThreadSlot();
            Arrived.fetch_add(1, std::memory_order_acq_rel);
            while (Arrived.load(std::memory_order_acquire) < Externals)
            {
                PlatformTime::YieldThread();
            }
            Jobs::ReleaseThreadSlot();
        });
    }
    for (FThread& Thread : Pool)
    {
        Thread.join();
    }

    std::vector<uint32> Sorted = Slots;
    std::sort(Sorted.begin(), Sorted.end());
    for (uint32 i = 0; i < Externals; ++i)
    {
        EXPECT_LT(Sorted[i], Jobs::GetNumThreadSlots());
        if (i > 0)
        {
            EXPECT_NE(Sorted[i], Sorted[i - 1]) << "two live threads shared a slot";
        }
    }
}

TEST(TaskSystem, Perf_EmptyParallelForSchedulingOverhead)
{
    const uint32 Chunks = Jobs::GetNumWorkers() * 4u;
    auto Empty = [](uint32) {};

    for (int i = 0; i < 2000; ++i)
    {
        Jobs::ParallelFor(Chunks, Empty, 1);
    }

    constexpr int Iters = 20000;
    auto T0 = PlatformTime::Cycles();
    for (int i = 0; i < Iters; ++i)
    {
        Jobs::ParallelFor(Chunks, Empty, 1);
    }
    auto T1 = PlatformTime::Cycles();

    const double NsPerCall = (PlatformTime::ToSeconds(T1 - T0) * 1e9) / Iters;
    LOG_DISPLAY("[JobBench] Empty ParallelFor ({} chunks): {:.0f} ns/call", Chunks, NsPerCall);
    EXPECT_LT(NsPerCall, 200000.0);
}

TEST(TaskSystem, DISABLED_Perf_ParallelForScalesWithWork)
{
    constexpr uint32 N = 4'000'000;
    TVector<float> Data;
    Data.SetNum(N);
    for (uint32 i = 0; i < N; ++i)
    {
        Data[i] = (float)(i & 1023);
    }

    auto Heavy = [](float x) -> float
    {
        float a = x;
        for (int k = 0; k < 16; ++k)
        {
            a = a * 1.0000001f + 0.5f;
        }
        return a;
    };

    volatile float SerialSink = 0.0f;
    auto S0 = PlatformTime::Cycles();
    {
        float acc = 0.0f;
        for (uint32 i = 0; i < N; ++i)
        {
            acc += Heavy(Data[i]);
        }
        SerialSink = acc;
    }
    auto S1 = PlatformTime::Cycles();
    (void)SerialSink;
    const double SerialMs = PlatformTime::ToMilliseconds(S1 - S0);

    TVector<double> Partials;
    Partials.SetNum(Jobs::GetNumThreadSlots(), 0.0);

    auto P0 = PlatformTime::Cycles();
    Jobs::ParallelFor(N, [&](const Jobs::FParallelRange& R)
    {
        float acc = 0.0f;
        for (uint32 i = R.Start; i < R.End; ++i)
        {
            acc += Heavy(Data[i]);
        }
        Partials[R.Thread] += acc;
    }, 8192);
    auto P1 = PlatformTime::Cycles();
    const double ParallelMs = PlatformTime::ToMilliseconds(P1 - P0);

    double Combined = 0.0;
    for (double p : Partials)
    {
        Combined += p;
    }

    const double Speedup = SerialMs / ParallelMs;
    LOG_DISPLAY("[JobBench] ParallelFor scaling: serial={:.2f}ms parallel={:.2f}ms speedup={:.2f}x (workers={})",
        SerialMs, ParallelMs, Speedup, Jobs::GetNumWorkers());

    EXPECT_GT(Combined, 0.0);
    EXPECT_GT(Speedup, 1.5);
}

TEST(JobsCallable, SpawnOwnsCapturingLambda)
{
    std::atomic<int> Sum{0};
    Jobs::FTaskEvent Done;
    {
        const int Base = 40;
        TVector<int> Values = { 1, 1 };
        Jobs::Spawn(Jobs::EJobPriority::Normal, Done, [&Sum, Base, Values]() -> Jobs::TTask<void>
        {
            Sum.fetch_add(Base + Values[0] + Values[1]);
            co_return;
        });
    }
    Done.Wait();
    EXPECT_EQ(Sum.load(), 42);
}

TEST(JobsCallable, SpawnRunsPlainLambda)
{
    std::atomic<int> Ran{0};
    Jobs::FTaskEvent Done;
    Jobs::Spawn(Jobs::EJobPriority::Normal, Done, [&Ran] { Ran.fetch_add(1); });
    Done.Wait();
    EXPECT_EQ(Ran.load(), 1);
}

TEST(JobsCallable, SyncWaitReturnsFromPlainAndTaskLambdas)
{
    const int Factor = 6;
    EXPECT_EQ(Jobs::SyncWait(Jobs::EJobPriority::Normal, [Factor] { return Factor * 7; }), 42);
    EXPECT_EQ(Jobs::SyncWait(Jobs::EJobPriority::Normal, [Factor]() -> Jobs::TTask<int> { co_return Factor * 7; }), 42);
}

TEST(JobsCallable, ForkAndCallTakePlainLambdas)
{
    const int Result = Jobs::SyncWait(Jobs::EJobPriority::Normal, []() -> Jobs::TTask<int>
    {
        int Left  = 0;
        int Right = 0;
        co_await Jobs::Fork(&Left, [] { return 20; });
        co_await Jobs::Fork([&Right] { Right = 20; });
        co_await Jobs::Join();
        const int Extra = co_await Jobs::Call([] { return 2; });
        co_return Left + Right + Extra;
    });
    EXPECT_EQ(Result, 42);
}

TEST(JobsCallable, LatchSpawnHoldsUntilEveryTreeEnds)
{
    std::atomic<int> Ran{0};
    Jobs::FLatch Done(1);
    for (int Index = 0; Index < 64; ++Index)
    {
        Jobs::Spawn(Jobs::EJobPriority::Normal, Done, [&Ran]() -> Jobs::TTask<void>
        {
            co_await Jobs::Call(Jobs::ForEach(16, [&Ran](uint32) { Ran.fetch_add(1); }));
        });
    }
    Done.CountDown();
    Done.Wait();
    EXPECT_EQ(Ran.load(), 64 * 16);
}

TEST(JobsCallable, WaitForSpawnedInsideATaskReturns)
{
    Jobs::FTaskEvent Done;
    Jobs::Spawn(Jobs::EJobPriority::Normal, Done, [] { Jobs::WaitForSpawned(); });
    Done.Wait();
    EXPECT_TRUE(Done.IsSignaled());
}

namespace TaskSystemTestsInternal
{
    thread_local bool GtInIsolatedRegion = false;
}

TEST(JobsCallable, IsolatedWaitNeverRunsUnrelatedTasks)
{
    using namespace TaskSystemTestsInternal;
    std::atomic<int> RanInside{0};
    std::atomic<int> Ran{0};
    Jobs::FLatch Done(1);
    for (int Index = 0; Index < 512; ++Index)
    {
        Jobs::Spawn(Jobs::EJobPriority::Low, Done, [&RanInside, &Ran]
        {
            RanInside.fetch_add(GtInIsolatedRegion ? 1 : 0);
            const uint64 Start = PlatformTime::Cycles();
            while (PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start) < 0.1)
            {
            }
            Ran.fetch_add(1);
        });
    }

    std::atomic<uint64> Sum{0};
    {
        Jobs::FTaskIsolation Isolation;
        GtInIsolatedRegion = true;
        for (int Round = 0; Round < 32; ++Round)
        {
            Jobs::ParallelFor(4096, [&Sum](uint32 Index) { Sum.fetch_add(Index, std::memory_order_relaxed); }, 16);
        }
        GtInIsolatedRegion = false;
    }

    Done.CountDown();
    Done.Wait();
    EXPECT_EQ(Ran.load(), 512);
    EXPECT_EQ(RanInside.load(), 0);
    EXPECT_EQ(Sum.load(), 32ull * (4095ull * 4096ull / 2ull));
}
