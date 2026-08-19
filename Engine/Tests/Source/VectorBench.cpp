#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "Containers/Vector.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaVectorBench
{
    template <typename T, size_t InlineCapacity = 0>
    using TVector = Lumina::Containers::TVector<T, InlineCapacity>;

    using FBenchClock = std::chrono::steady_clock;

    volatile uint64 GBenchSink = 0;

    struct FRelocatable
    {
        FRelocatable() = default;
        explicit FRelocatable(int InValue) : Value(InValue), Payload(InValue * 3) {}
        FRelocatable(const FRelocatable& Other) : Value(Other.Value), Payload(Other.Payload) {}
        FRelocatable(FRelocatable&& Other) noexcept : Value(Other.Value), Payload(Other.Payload) {}
        FRelocatable& operator=(const FRelocatable&) = default;
        FRelocatable& operator=(FRelocatable&&) noexcept = default;
        ~FRelocatable() { Value = 0; }

        int    Value   = 0;
        int    Payload = 0;
        double Ballast = 1.5;
    };

    struct FNonRelocatable
    {
        FNonRelocatable() = default;
        explicit FNonRelocatable(int InValue) : Value(InValue), Payload(InValue * 3) {}
        FNonRelocatable(const FNonRelocatable& Other) : Value(Other.Value), Payload(Other.Payload) {}
        FNonRelocatable(FNonRelocatable&& Other) noexcept : Value(Other.Value), Payload(Other.Payload) {}
        FNonRelocatable& operator=(const FNonRelocatable&) = default;
        FNonRelocatable& operator=(FNonRelocatable&&) noexcept = default;
        ~FNonRelocatable() { Value = 0; }

        int    Value   = 0;
        int    Payload = 0;
        double Ballast = 1.5;
    };

    template <typename TBody>
    double BestMillisOf(int Repeats, TBody&& Body)
    {
        double Best = 1e30;
        for (int Attempt = 0; Attempt < Repeats; ++Attempt)
        {
            const FBenchClock::time_point Start = FBenchClock::now();
            Body();
            const FBenchClock::time_point Stop = FBenchClock::now();

            const double Millis = std::chrono::duration<double, std::milli>(Stop - Start).count();
            Best = Millis < Best ? Millis : Best;
        }
        return Best;
    }

    void ReportHeader(const char* Title)
    {
        std::printf("\n%s\n", Title);
        std::printf("  %-26s %10s %10s\n", "implementation", "best (ms)", "vs std");
        std::printf("  %-26s %10s %10s\n", "--------------------------", "---------", "--------");
    }

    void ReportRow(const char* Name, double Millis, double Baseline)
    {
        if (Baseline > 0.0)
        {
            std::printf("  %-26s %10.3f %9.2fx\n", Name, Millis, Baseline / Millis);
        }
        else
        {
            std::printf("  %-26s %10.3f %10s\n", Name, Millis, "-");
        }
    }
}

LUMINA_DECLARE_TRIVIALLY_RELOCATABLE(LuminaVectorBench::FRelocatable)

namespace LuminaVectorBench
{
    static_assert(Lumina::TIsTriviallyRelocatable_V<FRelocatable>);
    static_assert(!Lumina::TIsTriviallyRelocatable_V<FNonRelocatable>);
    static_assert(!std::is_trivially_copyable_v<FRelocatable>);

    constexpr int kIntCount    = 1'000'000;
    constexpr int kStructCount = 400'000;
    constexpr int kRepeats     = 5;

    TEST(VectorBench, Footprint)
    {
        std::printf("\nfootprint (bytes per container instance)\n");
        std::printf("  %-26s %10zu\n", "Lumina TVector<int>", sizeof(TVector<int>));
        std::printf("  %-26s %10zu\n", "TVector<int>", sizeof(TVector<int>));
        std::printf("  %-26s %10zu\n", "std::vector<int>", sizeof(std::vector<int>));
        SUCCEED();
    }

    TEST(VectorBench, PushBackTrivialNoReserve)
    {
        const double Baseline = BestMillisOf(kRepeats, []
        {
            std::vector<int> Values;
            for (int Index = 0; Index < kIntCount; ++Index)
            {
                Values.push_back(Index);
            }
            GBenchSink += Values.back();
        });

        const double Lumina = BestMillisOf(kRepeats, []
        {
            TVector<int> Values;
            for (int Index = 0; Index < kIntCount; ++Index)
            {
                Values.push_back(Index);
            }
            GBenchSink += Values.back();
        });

        ReportHeader("push_back 1,000,000 int, no reserve (growth + relocation)");
        ReportRow("std::vector", Baseline, 0.0);
        ReportRow("Lumina TVector", Lumina, Baseline);
        SUCCEED();
    }

    TEST(VectorBench, PushBackTrivialReserved)
    {
        const double Baseline = BestMillisOf(kRepeats, []
        {
            std::vector<int> Values;
            Values.reserve(kIntCount);
            for (int Index = 0; Index < kIntCount; ++Index)
            {
                Values.push_back(Index);
            }
            GBenchSink += Values.back();
        });

        const double Lumina = BestMillisOf(kRepeats, []
        {
            TVector<int> Values;
            Values.Reserve(kIntCount);
            for (int Index = 0; Index < kIntCount; ++Index)
            {
                Values.push_back(Index);
            }
            GBenchSink += Values.back();
        });

        ReportHeader("push_back 1,000,000 int, reserved (pure store path)");
        ReportRow("std::vector", Baseline, 0.0);
        ReportRow("Lumina TVector", Lumina, Baseline);
        SUCCEED();
    }

    TEST(VectorBench, GrowthOfRelocatableStruct)
    {
        const double Baseline = BestMillisOf(kRepeats, []
        {
            std::vector<FNonRelocatable> Values;
            for (int Index = 0; Index < kStructCount; ++Index)
            {
                Values.push_back(FNonRelocatable(Index));
            }
            GBenchSink += static_cast<uint64>(Values.back().Value);
        });

        const double Std = BestMillisOf(kRepeats, []
        {
            std::vector<FNonRelocatable> Values;
            for (int Index = 0; Index < kStructCount; ++Index)
            {
                Values.push_back(FNonRelocatable(Index));
            }
            GBenchSink += static_cast<uint64>(Values.back().Value);
        });

        const double Lumina = BestMillisOf(kRepeats, []
        {
            TVector<FRelocatable> Values;
            for (int Index = 0; Index < kStructCount; ++Index)
            {
                Values.push_back(FRelocatable(Index));
            }
            GBenchSink += static_cast<uint64>(Values.back().Value);
        });

        const double LuminaNoTrait = BestMillisOf(kRepeats, []
        {
            TVector<FNonRelocatable> Values;
            for (int Index = 0; Index < kStructCount; ++Index)
            {
                Values.push_back(FNonRelocatable(Index));
            }
            GBenchSink += static_cast<uint64>(Values.back().Value);
        });

        ReportHeader("push_back 400,000 non-trivial struct, no reserve");
        ReportRow("std::vector", Baseline, 0.0);
        ReportRow("std::vector", Std, Baseline);
        ReportRow("Lumina, no relocate trait", LuminaNoTrait, Baseline);
        ReportRow("Lumina, relocate trait", Lumina, Baseline);
        SUCCEED();
    }

    TEST(VectorBench, InsertAtFront)
    {
        constexpr int kInserts = 40'000;

        const double Baseline = BestMillisOf(kRepeats, []
        {
            std::vector<int> Values;
            for (int Index = 0; Index < kInserts; ++Index)
            {
                Values.insert(Values.begin(), Index);
            }
            GBenchSink += Values.front();
        });

        const double Lumina = BestMillisOf(kRepeats, []
        {
            TVector<int> Values;
            for (int Index = 0; Index < kInserts; ++Index)
            {
                Values.insert(Values.begin(), Index);
            }
            GBenchSink += Values.front();
        });

        ReportHeader("insert at front 40,000 times (shift cost)");
        ReportRow("std::vector", Baseline, 0.0);
        ReportRow("Lumina TVector", Lumina, Baseline);
        SUCCEED();
    }

    TEST(VectorBench, EraseFrontVersusRemoveAtSwap)
    {
        constexpr int kElements = 60'000;

        const double Baseline = BestMillisOf(kRepeats, []
        {
            std::vector<int> Values;
            Values.reserve(kElements);
            for (int Index = 0; Index < kElements; ++Index)
            {
                Values.push_back(Index);
            }
            while (!Values.empty())
            {
                Values.erase(Values.begin());
            }
            GBenchSink += Values.size();
        });

        const double LuminaErase = BestMillisOf(kRepeats, []
        {
            TVector<int> Values;
            Values.Reserve(kElements);
            for (int Index = 0; Index < kElements; ++Index)
            {
                Values.push_back(Index);
            }
            while (!Values.empty())
            {
                Values.erase(Values.begin());
            }
            GBenchSink += Values.size();
        });

        const double LuminaSwap = BestMillisOf(kRepeats, []
        {
            TVector<int> Values;
            Values.Reserve(kElements);
            for (int Index = 0; Index < kElements; ++Index)
            {
                Values.push_back(Index);
            }
            while (!Values.empty())
            {
                Values.RemoveAtSwap(0);
            }
            GBenchSink += Values.size();
        });

        ReportHeader("drain 60,000 elements from the front");
        ReportRow("TVector erase", Baseline, 0.0);
        ReportRow("Lumina TVector erase", LuminaErase, Baseline);
        ReportRow("Lumina RemoveAtSwap", LuminaSwap, Baseline);
        SUCCEED();
    }

    TEST(VectorBench, CopyConstruct)
    {
        std::vector<int> BaselineSource;
        TVector<int> LuminaSource;
        for (int Index = 0; Index < kIntCount; ++Index)
        {
            BaselineSource.push_back(Index);
            LuminaSource.push_back(Index);
        }

        const double Baseline = BestMillisOf(kRepeats, [&BaselineSource]
        {
            for (int Attempt = 0; Attempt < 20; ++Attempt)
            {
                std::vector<int> Copy(BaselineSource);
                GBenchSink += Copy.back();
            }
        });

        const double Lumina = BestMillisOf(kRepeats, [&LuminaSource]
        {
            for (int Attempt = 0; Attempt < 20; ++Attempt)
            {
                TVector<int> Copy(LuminaSource);
                GBenchSink += Copy.back();
            }
        });

        ReportHeader("copy-construct a 1,000,000 int vector, 20 times");
        ReportRow("std::vector", Baseline, 0.0);
        ReportRow("Lumina TVector", Lumina, Baseline);
        SUCCEED();
    }

    TEST(VectorBench, InlineCapacityAvoidsTheAllocator)
    {
        constexpr int kRounds = 400'000;

        const double Baseline = BestMillisOf(kRepeats, []
        {
            uint64 Total = 0;
            for (int Round = 0; Round < kRounds; ++Round)
            {
                std::vector<int> Values;
                Values.push_back(Round);
                Values.push_back(Round + 1);
                Values.push_back(Round + 2);
                Total += Values.back();
            }
            GBenchSink += Total;
        });

        const double Lumina = BestMillisOf(kRepeats, []
        {
            uint64 Total = 0;
            for (int Round = 0; Round < kRounds; ++Round)
            {
                Lumina::Containers::TInlineVector<int, 4> Values;
                Values.push_back(Round);
                Values.push_back(Round + 1);
                Values.push_back(Round + 2);
                Total += Values.back();
            }
            GBenchSink += Total;
        });

        ReportHeader("400,000 short-lived 3-element vectors");
        ReportRow("std::vector", Baseline, 0.0);
        ReportRow("Lumina TInlineVector<4>", Lumina, Baseline);
        SUCCEED();
    }

    TEST(VectorBench, SequentialRead)
    {
        TVector<int> EastlValues;
        TVector<int> LuminaValues;
        for (int Index = 0; Index < kIntCount; ++Index)
        {
            EastlValues.push_back(Index);
            LuminaValues.push_back(Index);
        }

        const double Baseline = BestMillisOf(kRepeats, [&EastlValues]
        {
            uint64 Total = 0;
            for (int Attempt = 0; Attempt < 20; ++Attempt)
            {
                for (int Value : EastlValues)
                {
                    Total += static_cast<uint64>(Value);
                }
            }
            GBenchSink += Total;
        });

        const double Lumina = BestMillisOf(kRepeats, [&LuminaValues]
        {
            uint64 Total = 0;
            for (int Attempt = 0; Attempt < 20; ++Attempt)
            {
                for (int Value : LuminaValues)
                {
                    Total += static_cast<uint64>(Value);
                }
            }
            GBenchSink += Total;
        });

        ReportHeader("sequential read of 1,000,000 int, 20 passes");
        ReportRow("std::vector", Baseline, 0.0);
        ReportRow("Lumina TVector", Lumina, Baseline);
        SUCCEED();
    }
}
