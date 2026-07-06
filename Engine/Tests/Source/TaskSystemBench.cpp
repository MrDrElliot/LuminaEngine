#include <gtest/gtest.h>

#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/TaskGraph.h"
#include "TaskSystem/Task.h"
#include "TaskSystem/Future.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
#include "Core/Threading/Thread.h"
#include "Containers/Array.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>

// ============================================================================
// Task-system benchmarks. Excluded from the default test run (main.cpp filters
// out *Bench*); run explicitly:
//     Tests.exe --gtest_filter=TaskBench.*
// They don't assert on timing (CI-noise-proof), they print numbers so the
// scheduler can be measured and tuned. The metrics that matter for the fiber
// scheduler are OVERLAP EFFICIENCY (do all workers engage on a fan-out?) and
// its run-to-run SPREAD (the "random spikes" a staggered ramp produces), so
// the distribution (min/median/p99/max), not just the mean, is reported.
//
// Bodies are written to defeat the optimizer: BusySpin takes a per-call Seed
// (so identical-argument calls can't be memoized/CSE'd into one), and the
// iteration body is a loop-carried LCG (no closed form). Without this the
// compiler folds the "work" away and the numbers are meaningless.
// ============================================================================

using namespace Lumina;

namespace
{
    using Clock = std::chrono::steady_clock;

    double MsSince(Clock::time_point T0)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - T0).count();
    }

    // Pure compute, no memory traffic, isolates scheduling + core parallelism from memory bandwidth so
    // a poor overlap shows up as lost speedup rather than being masked by a saturated bus. Seed varies the
    // result per call so the compiler can't memoize identical-argument calls into one. Returns a sink.
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
        const size_t N = Samples.size();
        double Sum = 0.0;
        for (double S : Samples) Sum += S;
        FStats St;
        St.Min    = Samples.front();
        St.Max    = Samples.back();
        St.Median = Samples[N / 2];
        St.Mean   = Sum / (double)N;
        St.P99    = Samples[(size_t)std::min<double>((double)N - 1, std::floor(0.99 * (double)N))];
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

// ----------------------------------------------------------------------------
// 1. Scheduling-overhead floor: empty fan-out, no per-element work. ns/dispatch.
// ----------------------------------------------------------------------------
TEST(TaskBench, EmptyDispatchOverhead)
{
    const uint32 Workers = GTaskSystem->GetNumWorkers();
    const uint32 Chunks  = Workers * 4u;
    auto Empty = [](uint32) {};

    for (int i = 0; i < 2000; ++i) Task::ParallelFor(Chunks, Empty, 1); // warm

    constexpr int Iters = 50000;
    const auto T0 = Clock::now();
    for (int i = 0; i < Iters; ++i) Task::ParallelFor(Chunks, Empty, 1);
    const double NsPerCall = std::chrono::duration<double, std::nano>(Clock::now() - T0).count() / Iters;

    std::printf("[TaskBench] %-40s  %8.0f ns  (%u chunks / %u workers)\n",
        "empty ParallelFor (ns/dispatch)", NsPerCall, Chunks, Workers);
    std::fflush(stdout);
    SUCCEED();
}

// ----------------------------------------------------------------------------
// 2. Strong scaling on heavy compute-bound work. speedup + per-core efficiency.
// ----------------------------------------------------------------------------
TEST(TaskBench, StrongScaling_HeavyCompute)
{
    const uint32 Workers   = GTaskSystem->GetNumWorkers();
    const uint32 Executors = Workers + 1; // workers + the assisting submitter thread
    constexpr uint32 Chunks = 4096;
    constexpr uint64 PerChunk = 200000; // FMAs

    TVector<double> Partials;
    Partials.resize(GTaskSystem->GetNumTaskThreads(), 0.0);

    // Serial baseline: same total work. Per-call seed (c) blocks memoization; sink keeps the loop alive.
    volatile double SerialSink = 0.0;
    const auto S0 = Clock::now();
    double SerialAcc = 0.0;
    for (uint32 c = 0; c < Chunks; ++c) SerialAcc += BusySpin(PerChunk, (double)c);
    SerialSink = SerialAcc;
    const double SerialMs = MsSince(S0);

    for (double& P : Partials) P = 0.0;
    const auto P0 = Clock::now();
    Task::ParallelFor(Chunks, [&](const Task::FParallelRange& R)
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

// ----------------------------------------------------------------------------
// 3. Overlap efficiency + spike spread: balanced equal-cost chunks, many runs.
//    This is the original complaint, do all workers engage on a fan-out, and
//    how consistent is it run to run? Reports the wall-time DISTRIBUTION.
// ----------------------------------------------------------------------------
TEST(TaskBench, OverlapEfficiency_BalancedFanout)
{
    const uint32 Workers   = GTaskSystem->GetNumWorkers();
    const uint32 Executors = Workers + 1;
    const uint32 Chunks    = Workers * 4u;      // a few chunks per worker
    constexpr uint64 PerChunk = 120000;          // equal cost each

    // Calibrate ideal: one chunk's serial cost.
    volatile double Sink = 0.0;
    const auto C0 = Clock::now();
    Sink = BusySpin(PerChunk, 1.0);
    const double OneChunkMs = MsSince(C0);
    const double IdealMs    = OneChunkMs * std::ceil((double)Chunks / (double)Executors);

    for (int i = 0; i < 200; ++i) // warm
    {
        Task::ParallelFor(Chunks, [&](uint32 Idx) { Sink = BusySpin(PerChunk, (double)Idx); }, 1);
    }

    constexpr int Runs = 300;
    TVector<double> Samples;
    Samples.reserve(Runs);
    for (int r = 0; r < Runs; ++r)
    {
        std::atomic<int> Done{0};
        const auto T0 = Clock::now();
        Task::ParallelFor(Chunks, [&](uint32 Idx)
        {
            volatile double S = BusySpin(PerChunk, (double)(Idx + r));
            (void)S;
            Done.fetch_add(1, std::memory_order_relaxed);
        }, 1);
        Samples.push_back(MsSince(T0));
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

// ----------------------------------------------------------------------------
// 4. Cold-wake cadence: dispatch, idle a beat (workers park), dispatch again,
//    the real per-frame pattern. Captures the re-engagement ramp the keep-hot
//    spin targets. Reports the per-dispatch distribution incl. p99/max.
// ----------------------------------------------------------------------------
TEST(TaskBench, ColdWakeCadence_PerFramePattern)
{
    const uint32 Workers = GTaskSystem->GetNumWorkers();
    const uint32 Chunks  = Workers * 4u;
    constexpr uint64 PerChunk = 60000;
    volatile double Sink = 0.0;

    for (int i = 0; i < 100; ++i) Task::ParallelFor(Chunks, [&](uint32 Idx){ Sink = BusySpin(PerChunk, (double)Idx); }, 1);

    constexpr int Frames = 240;
    TVector<double> Samples;
    Samples.reserve(Frames);
    for (int f = 0; f < Frames; ++f)
    {
        // Idle gap so workers drain and park (simulates the rest of a frame).
        Threading::Sleep(2);
        const auto T0 = Clock::now();
        Task::ParallelFor(Chunks, [&](uint32 Idx){ Sink = BusySpin(PerChunk, (double)(Idx + f)); }, 1);
        Samples.push_back(MsSince(T0));
    }
    ReportDist("cold-wake per-frame dispatch", Samples);
    SUCCEED();
}

// ----------------------------------------------------------------------------
// 5. Iteration throughput: chew through N items with light per-item work. The
//    body is a loop-carried LCG (no closed form) accumulated per-thread (no
//    shared atomic), so this measures dispatch + iteration, not contention.
// ----------------------------------------------------------------------------
TEST(TaskBench, IterationThroughput)
{
    constexpr uint32 N = 64'000'000;
    TVector<uint64> Partials;
    Partials.resize(GTaskSystem->GetNumTaskThreads(), 0);

    auto Body = [&](const Task::FParallelRange& R)
    {
        uint64 h = (uint64)R.Start * 2654435761ull + 1ull;
        for (uint32 i = R.Start; i < R.End; ++i)
        {
            h = h * 6364136223846793005ull + (uint64)i; // LCG: loop-carried, no closed form
        }
        Partials[R.Thread] += h;
    };

    for (int i = 0; i < 10; ++i) Task::ParallelFor(4'000'000u, Body, 2048);
    for (uint64& P : Partials) P = 0;

    const auto T0 = Clock::now();
    Task::ParallelFor(N, Body, 4096);
    const double Ms = MsSince(T0);

    uint64 Total = 0;
    for (uint64 P : Partials) Total += P;
    ASSERT_NE(Total, 0ull);
    const double MItemsPerSec = (double)N / (Ms * 1000.0);
    std::printf("[TaskBench] %-40s  %.2f ms  (%.0f M items/s)\n", "iteration throughput", Ms, MItemsPerSec);
    std::fflush(stdout);
    SUCCEED();
}

// ----------------------------------------------------------------------------
// 6. Graph fan-out -> merge, the CompileDrawCommands shape: parallel producers
//    + a merge node gated on all of them. Wall-time distribution.
// ----------------------------------------------------------------------------
TEST(TaskBench, GraphFanOutMerge_DrawCommandsShape)
{
    const uint32 Workers = GTaskSystem->GetNumWorkers();
    constexpr uint64 PerChunk = 40000;
    volatile double Sink = 0.0;

    FTaskGraph Graph;
    auto BuildAndRun = [&]() -> double
    {
        Graph.Reset();
        auto MakeProducer = [&](uint32 Count)
        {
            return Graph.AddParallelFor(Count, 1, [&](const Task::FParallelRange& R)
            {
                for (uint32 i = R.Start; i < R.End; ++i) Sink = BusySpin(PerChunk, (double)i);
            }, ETaskPriority::High);
        };
        auto A = MakeProducer(Workers * 2u);
        auto B = MakeProducer(Workers * 2u);
        auto C = MakeProducer(Workers * 2u);
        auto Merge = Graph.Add([&]{ Sink = BusySpin(PerChunk, Sink); }, ETaskPriority::High);
        Graph.AddDependency(Merge, A);
        Graph.AddDependency(Merge, B);
        Graph.AddDependency(Merge, C);

        const auto T0 = Clock::now();
        Graph.Dispatch();
        Graph.Wait();
        return MsSince(T0);
    };

    for (int i = 0; i < 100; ++i) BuildAndRun(); // warm

    constexpr int Runs = 240;
    TVector<double> Samples;
    Samples.reserve(Runs);
    for (int r = 0; r < Runs; ++r) Samples.push_back(BuildAndRun());
    ReportDist("graph fan-out->merge", Samples);
    SUCCEED();
}

// ----------------------------------------------------------------------------
// 7. Nested parallelism throughput (fiber park/resume under fan-out).
// ----------------------------------------------------------------------------
TEST(TaskBench, NestedParallelThroughput)
{
    constexpr uint32 Outer = 256, Inner = 256;
    std::atomic<uint64> Total{0};

    for (int i = 0; i < 10; ++i)
    {
        Task::ParallelFor(64u, [&](uint32){ Task::ParallelFor(64u, [&](uint32){ Total.fetch_add(1, std::memory_order_relaxed); }, 8); }, 4);
    }
    Total.store(0);

    const auto T0 = Clock::now();
    Task::ParallelFor(Outer, [&](uint32)
    {
        Task::ParallelFor(Inner, [&](uint32) { Total.fetch_add(1, std::memory_order_relaxed); }, 16);
    }, 4);
    const double Ms = MsSince(T0);

    ASSERT_EQ(Total.load(), (uint64)Outer * Inner);
    std::printf("[TaskBench] %-40s  %.3f ms  (%llu leaf tasks)\n", "nested parallel-for", Ms, (unsigned long long)Outer * Inner);
    std::fflush(stdout);
    SUCCEED();
}

// Coroutine fan-out then merge, the same shape as GraphFanOutMerge.
namespace
{
    TTask<void> CoBenchProducer(uint32 Count, uint64 PerChunk, volatile double* Sink)
    {
        co_await Task::ParallelForAsync(Count, 1, [Sink, PerChunk](uint32 i)
        {
            *Sink = BusySpin(PerChunk, (double)i);
        }, ETaskPriority::High);
    }

    TTask<double> CoBenchFanOutMerge(uint32 Workers, uint64 PerChunk, volatile double* Sink)
    {
        TVector<TTask<void>> Producers;
        Producers.push_back(CoBenchProducer(Workers * 2u, PerChunk, Sink));
        Producers.push_back(CoBenchProducer(Workers * 2u, PerChunk, Sink));
        Producers.push_back(CoBenchProducer(Workers * 2u, PerChunk, Sink));
        co_await WhenAll(Move(Producers));
        *Sink = BusySpin(PerChunk, *Sink);
        co_return *Sink;
    }
}

TEST(TaskBench, CoroFanOutMerge_DrawCommandsShape)
{
    const uint32 Workers = GTaskSystem->GetNumWorkers();
    constexpr uint64 PerChunk = 40000;
    volatile double Sink = 0.0;

    auto Run = [&]() -> double
    {
        const auto T0 = Clock::now();
        Sink = SyncWait(CoBenchFanOutMerge(Workers, PerChunk, &Sink));
        return MsSince(T0);
    };

    for (int i = 0; i < 100; ++i) Run(); // warm

    constexpr int Runs = 240;
    TVector<double> Samples;
    Samples.reserve(Runs);
    for (int r = 0; r < Runs; ++r) Samples.push_back(Run());
    ReportDist("coro fan-out->merge", Samples);
    SUCCEED();
}

// Many coroutines suspended in-flight at once, past the 256-fiber pool cap.
namespace
{
    TTask<void> CoBenchWaitOn(TFuture<void> Gate)
    {
        co_await Move(Gate);
        co_return;
    }

    TTask<int> CoBenchManyWaiters(int N, TFuture<void> Gate)
    {
        TVector<TTask<void>> Waiters;
        Waiters.reserve(N);
        for (int i = 0; i < N; ++i)
        {
            Waiters.push_back(CoBenchWaitOn(Gate)); // copies the shared future state
        }
        co_await WhenAll(Move(Waiters));
        co_return N;
    }
}

TEST(TaskBench, CoroManyInFlightWaiters)
{
    constexpr int N = 5000; // far past the 256-fiber cap

    auto Run = [&]() -> double
    {
        TPromise<void> GateP;
        TFuture<void>  Gate = GateP.GetFuture();
        // Release the gate shortly after the waiters have all suspended.
        Task::Async([P = Move(GateP)]() mutable { Threading::Sleep(2); P.SetValue(); });

        const auto T0 = Clock::now();
        const int Done = SyncWait(CoBenchManyWaiters(N, Move(Gate)));
        const double Ms = MsSince(T0);
        (void)Done;
        return Ms;
    };

    for (int i = 0; i < 5; ++i) Run(); // warm

    constexpr int Runs = 40;
    TVector<double> Samples;
    Samples.reserve(Runs);
    for (int r = 0; r < Runs; ++r) Samples.push_back(Run());
    ReportDist("coro 5000 in-flight waiters", Samples);
    std::printf("[TaskBench] %-40s  %d coroutines suspended at once (fiber pool = 256)\n", "in-flight scale", N);
    std::fflush(stdout);
    SUCCEED();
}
