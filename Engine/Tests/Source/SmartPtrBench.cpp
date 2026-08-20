#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "Platform/Time/PlatformTime.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaSmartPtrBench
{
    namespace PlatformTime = Lumina::PlatformTime;
    using Lumina::TSharedPtr;
    using Lumina::TWeakPtr;
    using Lumina::TUniquePtr;
    using Lumina::MakeShared;
    using Lumina::MakeUnique;
    using Lumina::FThread;
    using Lumina::int32;
    using Lumina::uint32;
    using Lumina::uint64;

    volatile uint64 GBenchSink = 0;

    constexpr int32 kIterations = 500'000;
    constexpr int32 kRepeats    = 5;

    struct FPayload
    {
        int32 A = 1;
        int32 B = 2;
        double C = 3.0;
    };

    template <typename TBody>
    double BestMillisOf(int32 Repeats, TBody&& Body)
    {
        double Best = 1e30;
        for (int32 Attempt = 0; Attempt < Repeats; ++Attempt)
        {
            const uint64 Start = PlatformTime::Cycles();
            Body();
            const double Millis = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start);
            Best = Millis < Best ? Millis : Best;
        }

        return Best;
    }

    void ReportHeader(const char* Title)
    {
        std::printf("\n%s\n", Title);
        std::printf("  %-30s %10s %10s\n", "implementation", "best (ms)", "vs std");
        std::printf("  %-30s %10s %10s\n", "------------------------------", "---------", "--------");
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

    TEST(SmartPtrBench, MakeAndDestroy)
    {
        const double Std = BestMillisOf(kRepeats, []
        {
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                const std::shared_ptr<FPayload> Ptr = std::make_shared<FPayload>();
                GBenchSink += Ptr->A;
            }
        });

        const double Ours = BestMillisOf(kRepeats, []
        {
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                const TSharedPtr<FPayload> Ptr = MakeShared<FPayload>();
                GBenchSink += Ptr->A;
            }
        });

        ReportHeader("500,000 x make a shared pointer and drop it");
        ReportRow("std::make_shared", Std, 0.0);
        ReportRow("Lumina::MakeShared", Ours, Std);
        SUCCEED();
    }

    TEST(SmartPtrBench, CopyAndDestroy)
    {
        const std::shared_ptr<FPayload> StdRoot = std::make_shared<FPayload>();
        const TSharedPtr<FPayload> OurRoot = MakeShared<FPayload>();

        const double Std = BestMillisOf(kRepeats, [&StdRoot]
        {
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                const std::shared_ptr<FPayload> Copy = StdRoot;
                GBenchSink += Copy->A;
            }
        });

        const double Ours = BestMillisOf(kRepeats, [&OurRoot]
        {
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                const TSharedPtr<FPayload> Copy = OurRoot;
                GBenchSink += Copy->A;
            }
        });

        ReportHeader("500,000 x copy a shared pointer and drop it");
        ReportRow("std::shared_ptr", Std, 0.0);
        ReportRow("Lumina::TSharedPtr", Ours, Std);
        SUCCEED();
    }

    TEST(SmartPtrBench, ContendedCopy)
    {
        constexpr uint32 kThreads = 4;
        constexpr int32 kPerThread = 200'000;

        const std::shared_ptr<FPayload> StdRoot = std::make_shared<FPayload>();
        const TSharedPtr<FPayload> OurRoot = MakeShared<FPayload>();

        const double Std = BestMillisOf(3, [&StdRoot]
        {
            std::vector<FThread> Threads;
            Threads.reserve(kThreads);
            for (uint32 Index = 0; Index < kThreads; ++Index)
            {
                Threads.emplace_back([&StdRoot]
                {
                    for (int32 Step = 0; Step < kPerThread; ++Step)
                    {
                        const std::shared_ptr<FPayload> Copy = StdRoot;
                        GBenchSink += Copy->A;
                    }
                });
            }
            for (FThread& Thread : Threads) { Thread.Join(); }
        });

        const double Ours = BestMillisOf(3, [&OurRoot]
        {
            std::vector<FThread> Threads;
            Threads.reserve(kThreads);
            for (uint32 Index = 0; Index < kThreads; ++Index)
            {
                Threads.emplace_back([&OurRoot]
                {
                    for (int32 Step = 0; Step < kPerThread; ++Step)
                    {
                        const TSharedPtr<FPayload> Copy = OurRoot;
                        GBenchSink += Copy->A;
                    }
                });
            }
            for (FThread& Thread : Threads) { Thread.Join(); }
        });

        ReportHeader("4 threads x 200,000 contended copies");
        ReportRow("std::shared_ptr", Std, 0.0);
        ReportRow("Lumina::TSharedPtr", Ours, Std);
        SUCCEED();
    }

    TEST(SmartPtrBench, WeakPin)
    {
        const std::shared_ptr<FPayload> StdRoot = std::make_shared<FPayload>();
        const std::weak_ptr<FPayload> StdWeak = StdRoot;

        const TSharedPtr<FPayload> OurRoot = MakeShared<FPayload>();
        const TWeakPtr<FPayload> OurWeak = OurRoot;

        const double Std = BestMillisOf(kRepeats, [&StdWeak]
        {
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                const std::shared_ptr<FPayload> Pinned = StdWeak.lock();
                GBenchSink += Pinned->A;
            }
        });

        const double Ours = BestMillisOf(kRepeats, [&OurWeak]
        {
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                const TSharedPtr<FPayload> Pinned = OurWeak.Pin();
                GBenchSink += Pinned->A;
            }
        });

        ReportHeader("500,000 x pin a weak pointer");
        ReportRow("std::weak_ptr::lock", Std, 0.0);
        ReportRow("TWeakPtr::Pin", Ours, Std);
        SUCCEED();
    }

    TEST(SmartPtrBench, UniqueMakeAndDestroy)
    {
        const double Std = BestMillisOf(kRepeats, []
        {
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                const std::unique_ptr<FPayload> Ptr = std::make_unique<FPayload>();
                GBenchSink += Ptr->A;
            }
        });

        const double Ours = BestMillisOf(kRepeats, []
        {
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                const TUniquePtr<FPayload> Ptr = MakeUnique<FPayload>();
                GBenchSink += Ptr->A;
            }
        });

        ReportHeader("500,000 x make a unique pointer and drop it");
        ReportRow("std::make_unique", Std, 0.0);
        ReportRow("Lumina::MakeUnique", Ours, Std);
        SUCCEED();
    }

    TEST(SmartPtrBench, ObjectSizes)
    {
        std::printf("\nsmart pointer sizes\n");
        std::printf("  %-30s %10zu %10zu\n", "shared (std / ours)",
                    sizeof(std::shared_ptr<FPayload>), sizeof(TSharedPtr<FPayload>));
        std::printf("  %-30s %10zu %10zu\n", "weak (std / ours)",
                    sizeof(std::weak_ptr<FPayload>), sizeof(TWeakPtr<FPayload>));
        std::printf("  %-30s %10zu %10zu\n", "unique (std / ours)",
                    sizeof(std::unique_ptr<FPayload>), sizeof(TUniquePtr<FPayload>));
        SUCCEED();
    }
}
