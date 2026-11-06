#include <gtest/gtest.h>

#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/TaskSystem.h"
#include "Core/Threading/Thread.h"
#include "Containers/Vector.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

// Task-system benchmarks, excluded by default; run with --gtest_filter=TaskBench.*

using namespace Lumina;

namespace
{
    double MsSince(Lumina::uint64 T0)
    {
        return Lumina::PlatformTime::ToMilliseconds(Lumina::PlatformTime::Cycles() - T0);
    }

    // Pure compute with no memory traffic, so poor overlap shows up as lost speedup.
    FORCENOINLINE double BusySpin(uint64 Iterations, double Seed)
    {
        double a = Seed;
        for (uint64 i = 0; i < Iterations; ++i)
        {
            a = a * 1.0000000001 + 0.5;
        }
        return a;
    }

    struct FStats
    {
        double Min, Median, Mean, P99, Max;
    };

    FStats Summarize(TVector<double>& Samples)
    {
        std::sort(Samples.begin(), Samples.end());
        const int32 N = Samples.Num();
        double Sum = 0.0;
        for (double S : Samples) Sum += S;
        FStats St;
        St.Min    = Samples[0];
        St.Max    = Samples.Last();
        St.Median = Samples[N / 2];
        St.Mean   = Sum / (double)N;
        St.P99    = Samples[std::min(N - 1, static_cast<int32>(0.99 * N))];
        return St;
    }

    void ReportDist(const char* Name, TVector<double>& Samples)
    {
        const FStats S = Summarize(Samples);
        std::printf("[TaskBench] %-40s  min %8.4f  med %8.4f  mean %8.4f  p99 %8.4f  max %8.4f ms  (spread x%.1f)\n",
            Name, S.Min, S.Median, S.Mean, S.P99, S.Max, S.Median > 0.0 ? S.Max / S.Median : 0.0);
        std::fflush(stdout);
    }
}

// 1. Scheduling-overhead floor, an empty fan-out with no per-element work.
TEST(TaskBench, EmptyDispatchOverhead)
{
    const uint32 Workers = Jobs::GetNumWorkers();
    const uint32 Chunks  = Workers * 4u;
    auto Empty = [](uint32) {};

    for (int i = 0; i < 2000; ++i) Jobs::ParallelFor(Chunks, Empty, 1); // warm

    constexpr int Iters = 50000;
    const auto T0 = Lumina::PlatformTime::Cycles();
    for (int i = 0; i < Iters; ++i) Jobs::ParallelFor(Chunks, Empty, 1);
    const double NsPerCall = (Lumina::PlatformTime::ToSeconds(Lumina::PlatformTime::Cycles() - T0) * 1e9) / Iters;

    std::printf("[TaskBench] %-40s  %8.0f ns  (%u chunks / %u workers)\n",
        "empty ParallelFor (ns/dispatch)", NsPerCall, Chunks, Workers);
    std::fflush(stdout);
    SUCCEED();
}

// 2. Strong scaling on heavy compute-bound work.
TEST(TaskBench, StrongScaling_HeavyCompute)
{
    const uint32 Workers   = Jobs::GetNumWorkers();
    const uint32 Executors = Workers + 1; // workers + the assisting submitter thread
    constexpr uint32 Chunks = 4096;
    constexpr uint64 PerChunk = 200000; // FMAs

    TVector<double> Partials;
    Partials.SetNum(Jobs::GetNumThreadSlots(), 0.0);

    // Serial baseline over the same total work, with a per-call seed to block memoization.
    volatile double SerialSink = 0.0;
    const auto S0 = Lumina::PlatformTime::Cycles();
    double SerialAcc = 0.0;
    for (uint32 c = 0; c < Chunks; ++c) SerialAcc += BusySpin(PerChunk, (double)c);
    SerialSink = SerialAcc;
    const double SerialMs = MsSince(S0);

    for (double& P : Partials) P = 0.0;
    const auto P0 = Lumina::PlatformTime::Cycles();
    Jobs::ParallelFor(Chunks, [&](const Jobs::FParallelRange& R)
    {
        double Acc = 0.0;
        for (uint32 c = R.Start; c < R.End; ++c) Acc += BusySpin(PerChunk, (double)c);
        Partials[R.Thread] += Acc;
    }, 1);
    const double ParallelMs = MsSince(P0);

    double Combined = 0.0;
    for (double P : Partials) Combined += P;

    const double Speedup    = SerialMs / ParallelMs;
    const double Efficiency = 100.0 * Speedup / (double)Executors;
    std::printf("[TaskBench] %-40s  serial %.2f ms  parallel %.2f ms  speedup %.2fx / %u  -> %.0f%% eff\n",
        "strong scaling (heavy)", SerialMs, ParallelMs, Speedup, Executors, Efficiency);
    std::fflush(stdout);
    EXPECT_GT(Combined, 0.0);
    EXPECT_NE(SerialSink, -1.0);
}

// 3. Overlap efficiency and spike spread over balanced equal-cost chunks.
TEST(TaskBench, OverlapEfficiency_BalancedFanout)
{
    const uint32 Workers   = Jobs::GetNumWorkers();
    const uint32 Executors = Workers + 1;
    const uint32 Chunks    = Workers * 4u;      // a few chunks per worker
    constexpr uint64 PerChunk = 120000;          // equal cost each

    // Calibrate the ideal from one chunk's serial cost.
    volatile double Sink = 0.0;
    const auto C0 = Lumina::PlatformTime::Cycles();
    Sink = BusySpin(PerChunk, 1.0);
    const double OneChunkMs = MsSince(C0);
    const double IdealMs    = OneChunkMs * std::ceil((double)Chunks / (double)Executors);

    for (int i = 0; i < 200; ++i) // warm
    {
        Jobs::ParallelFor(Chunks, [&](uint32 Idx) { Sink = BusySpin(PerChunk, (double)Idx); }, 1);
    }

    constexpr int Runs = 300;
    TVector<double> Samples;
    Samples.Reserve(Runs);
    for (int r = 0; r < Runs; ++r)
    {
        std::atomic<int> Done{0};
        const auto T0 = Lumina::PlatformTime::Cycles();
        Jobs::ParallelFor(Chunks, [&](uint32 Idx)
        {
            volatile double S = BusySpin(PerChunk, (double)(Idx + r));
            (void)S;
            Done.fetch_add(1, std::memory_order_relaxed);
        }, 1);
        Samples.Add(MsSince(T0));
        ASSERT_EQ(Done.load(), (int)Chunks);
    }

    const FStats St = Summarize(Samples);
    const double EffMedian = 100.0 * IdealMs / St.Median;
    ReportDist("overlap balanced fan-out", Samples);
    std::printf("[TaskBench]   ideal %.4f ms (1 chunk %.4f x ceil(%u/%u))  -> median efficiency %.0f%%\n",
        IdealMs, OneChunkMs, Chunks, Executors, EffMedian);
    std::fflush(stdout);
    SUCCEED();
}

// 4. Cold-wake cadence, the real per-frame dispatch, idle, dispatch pattern.
TEST(TaskBench, ColdWakeCadence_PerFramePattern)
{
    const uint32 Workers = Jobs::GetNumWorkers();
    const uint32 Chunks  = Workers * 4u;
    constexpr uint64 PerChunk = 60000;
    volatile double Sink = 0.0;

    for (int i = 0; i < 100; ++i) Jobs::ParallelFor(Chunks, [&](uint32 Idx){ Sink = BusySpin(PerChunk, (double)Idx); }, 1);

    constexpr int Frames = 240;
    TVector<double> Samples;
    Samples.Reserve(Frames);
    for (int f = 0; f < Frames; ++f)
    {
        // Idle gap so workers drain and park (simulates the rest of a frame).
        Threading::Sleep(2);
        const auto T0 = Lumina::PlatformTime::Cycles();
        Jobs::ParallelFor(Chunks, [&](uint32 Idx){ Sink = BusySpin(PerChunk, (double)(Idx + f)); }, 1);
        Samples.Add(MsSince(T0));
    }
    ReportDist("cold-wake per-frame dispatch", Samples);
    SUCCEED();
}

// 5. Iteration throughput with light per-item work and no shared atomic.
TEST(TaskBench, IterationThroughput)
{
    constexpr uint32 N = 64'000'000;
    TVector<uint64> Partials;
    Partials.SetNum(Jobs::GetNumThreadSlots(), 0);

    auto Body = [&](const Jobs::FParallelRange& R)
    {
        uint64 h = (uint64)R.Start * 2654435761ull + 1ull;
        for (uint32 i = R.Start; i < R.End; ++i)
        {
            h = h * 6364136223846793005ull + (uint64)i; // loop-carried LCG, no closed form
        }
        Partials[R.Thread] += h;
    };

    for (int i = 0; i < 10; ++i) Jobs::ParallelFor(4'000'000u, Body, 2048);
    for (uint64& P : Partials) P = 0;

    const auto T0 = Lumina::PlatformTime::Cycles();
    Jobs::ParallelFor(N, Body, 4096);
    const double Ms = MsSince(T0);

    uint64 Total = 0;
    for (uint64 P : Partials) Total += P;
    ASSERT_NE(Total, 0ull);
    const double MItemsPerSec = (double)N / (Ms * 1000.0);
    std::printf("[TaskBench] %-40s  %.2f ms  (%.0f M items/s)\n", "iteration throughput", Ms, MItemsPerSec);
    std::fflush(stdout);
    SUCCEED();
}

namespace
{
    Jobs::TTask<void> BenchProducer(uint32 Count, uint64 PerChunk, volatile double* Sink)
    {
        co_await Jobs::Call(Jobs::ForEach(Count, [Sink, PerChunk](uint32 Index) { *Sink = BusySpin(PerChunk, (double)Index); }, 1));
    }

    Jobs::TTask<double> BenchFanOutMerge(uint32 Workers, uint64 PerChunk, volatile double* Sink)
    {
        co_await Jobs::Fork(BenchProducer(Workers * 2u, PerChunk, Sink));
        co_await Jobs::Fork(BenchProducer(Workers * 2u, PerChunk, Sink));
        co_await Jobs::Fork(BenchProducer(Workers * 2u, PerChunk, Sink));
        co_await Jobs::Join();
        *Sink = BusySpin(PerChunk, *Sink);
        co_return *Sink;
    }
}

// 6. Fork-join fan-out into a merge, the CompileDrawCommands shape.
TEST(TaskBench, FanOutMerge_DrawCommandsShape)
{
    const uint32 Workers = Jobs::GetNumWorkers();
    constexpr uint64 PerChunk = 40000;
    volatile double Sink = 0.0;

    auto Run = [&]() -> double
    {
        const auto T0 = Lumina::PlatformTime::Cycles();
        Sink = Jobs::SyncWait(Jobs::EJobPriority::High, &BenchFanOutMerge, Workers, PerChunk, &Sink);
        return MsSince(T0);
    };

    for (int i = 0; i < 100; ++i) Run();

    constexpr int Runs = 240;
    TVector<double> Samples;
    Samples.Reserve(Runs);
    for (int r = 0; r < Runs; ++r) Samples.Add(Run());
    ReportDist("fork-join fan-out->merge", Samples);
    SUCCEED();
}

// 7. Nested parallelism throughput under fan-out.
TEST(TaskBench, NestedParallelThroughput)
{
    constexpr uint32 Outer = 256, Inner = 256;
    std::atomic<uint64> Total{0};

    for (int i = 0; i < 10; ++i)
    {
        Jobs::ParallelFor(64u, [&](uint32){ Jobs::ParallelFor(64u, [&](uint32){ Total.fetch_add(1, std::memory_order_relaxed); }, 8); }, 4);
    }
    Total.store(0);

    const auto T0 = Lumina::PlatformTime::Cycles();
    Jobs::ParallelFor(Outer, [&](uint32)
    {
        Jobs::ParallelFor(Inner, [&](uint32) { Total.fetch_add(1, std::memory_order_relaxed); }, 16);
    }, 4);
    const double Ms = MsSince(T0);

    ASSERT_EQ(Total.load(), (uint64)Outer * Inner);
    std::printf("[TaskBench] %-40s  %.3f ms  (%llu leaf tasks)\n", "nested parallel-for", Ms, (unsigned long long)Outer * Inner);
    std::fflush(stdout);
    SUCCEED();
}

namespace
{
    Jobs::TTask<int64> BenchFib(int32 N)
    {
        if (N < 2)
        {
            co_return N;
        }
        int64 Left = 0;
        co_await Jobs::Fork(&Left, BenchFib(N - 1));
        const int64 Right = co_await Jobs::Call(BenchFib(N - 2));
        co_await Jobs::Join();
        co_return Left + Right;
    }

    int64 SerialFib(int32 N)
    {
        return N < 2 ? N : SerialFib(N - 1) + SerialFib(N - 2);
    }

    Jobs::TTask<uint64> BenchReduce(const uint32* Data, uint32 Count)
    {
        if (Count <= 4096)
        {
            uint64 Sum = 0;
            for (uint32 Index = 0; Index < Count; ++Index)
            {
                Sum += Data[Index];
            }
            co_return Sum;
        }
        const uint32 Half = Count / 2;
        uint64 Left  = 0;
        uint64 Right = 0;
        co_await Jobs::Fork(&Left, BenchReduce(Data, Half));
        co_await Jobs::Fork(&Right, BenchReduce(Data + Half, Count - Half));
        co_await Jobs::Join();
        co_return Left + Right;
    }
}

// 8. Fork overhead, a recursive fib where every call is a fork and a join.
TEST(TaskBench, ForkOverhead_Fib)
{
    constexpr int32 N = 32;
    constexpr int Runs = 10;

    const auto S0 = Lumina::PlatformTime::Cycles();
    const int64 SerialResult = SerialFib(N);
    const double SerialMs = MsSince(S0);

    for (int i = 0; i < 2; ++i) (void)Jobs::SyncWait(&BenchFib, N);

    TVector<double> Samples;
    Samples.Reserve(Runs);
    int64 Result = 0;
    for (int r = 0; r < Runs; ++r)
    {
        const auto T0 = Lumina::PlatformTime::Cycles();
        Result = Jobs::SyncWait(&BenchFib, N);
        Samples.Add(MsSince(T0));
    }
    ASSERT_EQ(Result, SerialResult);

    const FStats St = Summarize(Samples);
    const double Calls = 2.0 * (double)SerialResult * 1.618 - 1.0;
    std::printf("[TaskBench] %-40s  serial %.2f ms  fork-join med %.2f ms  (%.1f ns/task, %.2fx serial)\n",
        "fork overhead (fib 32)", SerialMs, St.Median, (St.Median * 1e6) / Calls, St.Median / SerialMs);
    std::fflush(stdout);
    SUCCEED();
}

// 9. Tree reduction over a large array, fork-join against ParallelFor with per-slot partials.
TEST(TaskBench, Reduction_ForkJoinVsParallelFor)
{
    constexpr uint32 N = 64'000'000;
    TVector<uint32> Data;
    Data.SetNum(N);
    for (uint32 i = 0; i < N; ++i) Data[i] = i & 1023u;

    uint64 Expected = 0;
    for (uint32 Value : Data) Expected += Value;

    constexpr int Runs = 10;
    TVector<double> ForkSamples;
    TVector<double> ForSamples;
    TVector<uint64> Partials;
    for (int r = 0; r < Runs + 2; ++r)
    {
        const auto F0 = Lumina::PlatformTime::Cycles();
        const uint64 ForkSum = Jobs::SyncWait(&BenchReduce, Data.GetData(), N);
        const double ForkMs = MsSince(F0);

        Partials.Reset();
        Partials.SetNum(Jobs::GetNumThreadSlots(), 0);
        const auto P0 = Lumina::PlatformTime::Cycles();
        Jobs::ParallelFor(N, [&](const Jobs::FParallelRange& R)
        {
            uint64 Local = 0;
            for (uint32 i = R.Start; i < R.End; ++i) Local += Data[i];
            Partials[R.Thread] += Local;
        }, 4096);
        const double ForMs = MsSince(P0);

        uint64 ForSum = 0;
        for (uint64 P : Partials) ForSum += P;
        ASSERT_EQ(ForkSum, Expected);
        ASSERT_EQ(ForSum, Expected);
        if (r >= 2)
        {
            ForkSamples.Add(ForkMs);
            ForSamples.Add(ForMs);
        }
    }
    ReportDist("reduction (fork-join tree)", ForkSamples);
    ReportDist("reduction (ParallelFor partials)", ForSamples);
    SUCCEED();
}

namespace
{
    constexpr uint64 kNarrowJobIters = 300000;

    struct FNarrowShared
    {
        std::atomic<uint32> Done{0};
        std::atomic<double> Sink{0.0};
    };

    Jobs::TTask<void> NarrowJob(FNarrowShared* Shared)
    {
        Shared->Sink.store(BusySpin(kNarrowJobIters, 1.0), std::memory_order_relaxed);
        Shared->Done.fetch_add(1, std::memory_order_release);
        co_return;
    }
}

TEST(TaskBench, NarrowColdSubmit_WakeFanout)
{
    const uint32 Workers = Jobs::GetNumWorkers();
    const uint32 K       = std::max(2u, Workers / 4u);

    volatile double OneSink = 0.0;
    double OneJobMs = 0.0;
    {
        TVector<double> One;
        for (int i = 0; i < 32; ++i)
        {
            const auto T0 = Lumina::PlatformTime::Cycles();
            OneSink = BusySpin(kNarrowJobIters, (double)i);
            One.Add(MsSince(T0));
        }
        OneJobMs = Summarize(One).Median;
    }

    FNarrowShared S;
    auto RunOnce = [&]() -> double
    {
        S.Done.store(0, std::memory_order_relaxed);
        const auto T0 = Lumina::PlatformTime::Cycles();
        for (uint32 i = 0; i < K; ++i)
        {
            Jobs::Spawn(Jobs::EJobPriority::Normal, &NarrowJob, &S);
        }
        while (S.Done.load(std::memory_order_acquire) < K)
        {
        }
        return MsSince(T0);
    };

    for (int i = 0; i < 20; ++i) RunOnce();

    constexpr int Runs = 240;
    TVector<double> Samples;
    Samples.Reserve(Runs);
    for (int r = 0; r < Runs; ++r)
    {
        Threading::Sleep(2);
        Samples.Add(RunOnce());
    }

    const FStats St = Summarize(Samples);
    ReportDist("narrow cold submit (K single tasks)", Samples);
    std::printf("[TaskBench]   K=%u tasks / %u workers, 1 task %.4f ms -> median %.2fx one task "
                "(1.0x = perfect fan-out, %.1fx = fully serialized on one worker)\n",
        K, Workers, OneJobMs, St.Median / OneJobMs, (double)K);
    std::fflush(stdout);
    SUCCEED();
}

namespace
{
    Jobs::TTask<void> ParkOnGate(Jobs::FTaskEvent* Gate, std::atomic<uint32>* Resumed)
    {
        co_await *Gate;
        Resumed->fetch_add(1, std::memory_order_release);
    }

    Jobs::TTask<void> ParkMany(Jobs::FTaskEvent* Gate, std::atomic<uint32>* Resumed, uint32 Count, Jobs::FTaskEvent* Done)
    {
        for (uint32 i = 0; i < Count; ++i)
        {
            co_await Jobs::Fork(ParkOnGate(Gate, Resumed));
        }
        co_await Jobs::Join();
        Done->Signal();
    }
}

// 10. Many tasks parked on one event, released together.
TEST(TaskBench, ResumeStorm_ParkedTaskWake)
{
    constexpr uint32 N = 200;

    auto RunOnce = [&]() -> double
    {
        Jobs::FTaskEvent Gate;
        Jobs::FTaskEvent Done;
        std::atomic<uint32> Resumed{0};
        Jobs::Spawn(Jobs::EJobPriority::Normal, &ParkMany, &Gate, &Resumed, N, &Done);
        Threading::Sleep(1);

        const auto T0 = Lumina::PlatformTime::Cycles();
        Gate.Signal();
        Done.Wait();
        const double Ms = MsSince(T0);
        EXPECT_EQ(Resumed.load(), N);
        return Ms;
    };

    for (int i = 0; i < 10; ++i) RunOnce();

    constexpr int Runs = 120;
    TVector<double> Samples;
    Samples.Reserve(Runs);
    for (int r = 0; r < Runs; ++r) Samples.Add(RunOnce());

    ReportDist("resume storm (200 parked tasks)", Samples);
    SUCCEED();
}

// Minimum effective granularity, the crossover that decides whether to fan out at all.
TEST(TaskBench, GranularityCrossover)
{
    constexpr uint32 N        = 1u << 20;
    constexpr uint64 PerItem  = 24;
    constexpr int    Runs     = 15;

    auto RunSerial = [&]
    {
        const auto T0 = Lumina::PlatformTime::Cycles();
        double Sink = 0.0;
        for (uint32 i = 0; i < N; ++i)
        {
            Sink += BusySpin(PerItem, (double)i);
        }
        const double Ms = MsSince(T0);
        return Sink == 1.0 ? Ms + 1e-9 : Ms;
    };

    auto RunGrain = [&](uint32 Grain)
    {
        const auto T0 = Lumina::PlatformTime::Cycles();
        Jobs::ParallelFor(N, [&](const Jobs::FParallelRange& R)
        {
            double Local = 0.0;
            for (uint32 i = R.Start; i < R.End; ++i)
            {
                Local += BusySpin(PerItem, (double)i);
            }
            if (Local == 1.0) { std::printf(" "); }
        }, Grain);
        return MsSince(T0);
    };

    for (int i = 0; i < 3; ++i) { (void)RunSerial(); (void)RunGrain(4096); }

    TVector<double> SerialSamples;
    SerialSamples.Reserve(Runs);
    for (int r = 0; r < Runs; ++r) SerialSamples.Add(RunSerial());
    const double SerialMed = Summarize(SerialSamples).Median;

    std::printf("[TaskBench] %-40s  serial %8.4f ms over %u items\n", "granularity sweep", SerialMed, N);

    const uint32 Grains[] = { 32u, 128u, 512u, 2048u, 8192u, 32768u, 131072u };
    for (uint32 Grain : Grains)
    {
        TVector<double> Samples;
        Samples.Reserve(Runs);
        for (int r = 0; r < Runs; ++r) Samples.Add(RunGrain(Grain));
        const FStats S = Summarize(Samples);
        const double Speedup = SerialMed / S.Median;
        std::printf("[TaskBench]   grain %6u: med %8.4f ms  speedup %5.2fx  efficiency %5.1f%%  (p99 %8.4f)\n",
            Grain, S.Median, Speedup, 100.0 * Speedup / (double)Jobs::GetNumWorkers(), S.P99);
    }
    std::fflush(stdout);
    SUCCEED();
}

// Independent producers, measuring queue contention rather than steals.
TEST(TaskBench, ConcurrentProducers)
{
    const uint32 W = Jobs::GetNumWorkers();
    constexpr uint32 Producers = 4;
    constexpr uint32 Iters     = 400;

    auto Body = [](uint32 i) { (void)BusySpin(16, (double)i); };

    auto MeasureOne = [&]
    {
        const auto T0 = Lumina::PlatformTime::Cycles();
        for (uint32 i = 0; i < Iters; ++i)
        {
            Jobs::ParallelFor(W, Body, 1);
        }
        return MsSince(T0) / (double)Iters;
    };

    for (int i = 0; i < 3; ++i) { (void)MeasureOne(); }
    const double SoloMs = MeasureOne();

    TAtomic<uint32> Ready{0};
    TAtomic<uint64> TotalUs{0};
    TVector<FThread> Threads;
    Threads.Reserve(Producers);

    for (uint32 p = 0; p < Producers; ++p)
    {
        Threads.Emplace([&]
        {
            (void)Jobs::GetThreadSlot();
            Ready.fetch_add(1, std::memory_order_release);
            while (Ready.load(std::memory_order_acquire) < Producers)
            {
                Threading::ThreadYield();
            }
            const double PerCallMs = MeasureOne();
            TotalUs.fetch_add((uint64)(PerCallMs * 1000.0), std::memory_order_relaxed);
            Jobs::ReleaseThreadSlot();
        });
    }
    for (FThread& T : Threads)
    {
        T.Join();
    }

    const double SharedMs = ((double)TotalUs.load(std::memory_order_relaxed) / Producers) / 1000.0;
    std::printf("[TaskBench] %-40s  solo %8.4f ms  %u producers %8.4f ms  (x%.2f contention)\n",
        "concurrent producers", SoloMs, Producers, SharedMs, SoloMs > 0.0 ? SharedMs / SoloMs : 0.0);
    std::fflush(stdout);
    SUCCEED();
}
