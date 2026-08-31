#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <execution>
#include <vector>

#include "Containers/Algorithm.h"
#include "Containers/Vector.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/ParallelSort.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaParallelSortBench
{
    namespace Algo = Lumina::Algo;
    namespace PlatformTime = Lumina::PlatformTime;
    using Lumina::TVector;
    using Lumina::int32;
    using Lumina::uint32;
    using Lumina::uint64;

    volatile uint64 GBenchSink = 0;

    constexpr int32 kRepeats = 5;

    TVector<int32> MakeRandom(size_t Count, uint64 Seed)
    {
        uint64 State = Seed;
        TVector<int32> Values;
        Values.reserve(Count);
        for (size_t Index = 0; Index < Count; ++Index)
        {
            State = State * 6364136223846793005ull + 1442695040888963407ull;
            Values.push_back(static_cast<int32>(State >> 33));
        }

        return Values;
    }

    template <typename TBody>
    double BestMillisOf(int32 Repeats, const TVector<int32>& Source, TBody&& Body)
    {
        double Best = 1e30;
        for (int32 Attempt = 0; Attempt < Repeats; ++Attempt)
        {
            TVector<int32> Scratch = Source;

            const uint64 Start = PlatformTime::Cycles();
            Body(Scratch);
            const double Millis = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start);

            GBenchSink += static_cast<uint64>(Scratch[0]);
            Best = Millis < Best ? Millis : Best;
        }

        return Best;
    }

    void ReportHeader(const char* Title)
    {
        std::printf("\n%s\n", Title);
        std::printf("  %-30s %10s %10s\n", "implementation", "best (ms)", "vs serial");
        std::printf("  %-30s %10s %10s\n", "------------------------------", "---------", "---------");
    }

    void ReportRow(const char* Name, double Millis, double Baseline)
    {
        if (Baseline > 0.0)
        {
            std::printf("  %-30s %10.3f %9.2fx\n", Name, Millis, Baseline / Millis);
        }
        else
        {
            std::printf("  %-30s %10.3f %10s\n", Name, Millis, "-");
        }
    }

    void RunCase(const char* Title, size_t Count)
    {
        const TVector<int32> Source = MakeRandom(Count, Count + 1);

        const double Serial = BestMillisOf(kRepeats, Source, [](TVector<int32>& Values)
        {
            Algo::Sort(Values);
        });

        const double StdPar = BestMillisOf(kRepeats, Source, [](TVector<int32>& Values)
        {
            std::sort(std::execution::par, Values.begin(), Values.end());
        });

        const double Ours = BestMillisOf(kRepeats, Source, [](TVector<int32>& Values)
        {
            Lumina::Task::ParallelSort(Values.begin(), Values.end());
        });

        ReportHeader(Title);
        ReportRow("Algo::Sort (serial)", Serial, 0.0);
        ReportRow("std::sort(execution::par)", StdPar, Serial);
        ReportRow("Task::ParallelSort", Ours, Serial);
    }

    TEST(ParallelSortBench, AcrossSizes)
    {
        RunCase("sort 4,096 int32", 4096);
        RunCase("sort 100,000 int32", 100000);
        RunCase("sort 1,000,000 int32", 1000000);
        RunCase("sort 8,000,000 int32", 8000000);
        SUCCEED();
    }
}
