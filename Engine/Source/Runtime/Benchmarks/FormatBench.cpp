#include <gtest/gtest.h>

#include <cstdio>
#include <format>
#include <iterator>
#include <string>

#include "Platform/Time/PlatformTime.h"
#include "Containers/Format.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaFormatBench
{
    using Lumina::FString;
    using Lumina::FStringBuilder;
    using Lumina::int32;
    using Lumina::uint64;

    volatile uint64 GBenchSink = 0;

    constexpr int kIterations = 200'000;
    constexpr int kRepeats    = 5;

    template <typename TBody>
    double BestMillisOf(int Repeats, TBody&& Body)
    {
        double Best = 1e30;
        for (int Attempt = 0; Attempt < Repeats; ++Attempt)
        {
            const Lumina::uint64 Start = Lumina::PlatformTime::Cycles();
            Body();
            const Lumina::uint64 Stop = Lumina::PlatformTime::Cycles();

            const double Millis = Lumina::PlatformTime::ToMilliseconds(Stop - Start);
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

    void Consume(const char* Data, size_t Size)
    {
        GBenchSink += Size != 0 ? static_cast<uint64>(Data[0]) : 0u;
    }

    #define BENCH_CASE(Title, StdExpr, StdToExpr, OursExpr)                                    \
        {                                                                                      \
            const double Std = BestMillisOf(kRepeats, [&]                                      \
            {                                                                                  \
                for (int32 Index = 0; Index < kIterations; ++Index)                            \
                {                                                                              \
                    const std::string Text = StdExpr;                                          \
                    Consume(Text.data(), Text.size());                                         \
                }                                                                              \
            });                                                                                \
            const double StdTo = BestMillisOf(kRepeats, [&]                                    \
            {                                                                                  \
                for (int32 Index = 0; Index < kIterations; ++Index)                            \
                {                                                                              \
                    FString Text;                                                              \
                    StdToExpr;                                                                 \
                    Consume(Text.data(), Text.size());                                         \
                }                                                                              \
            });                                                                                \
            const double Ours = BestMillisOf(kRepeats, [&]                                     \
            {                                                                                  \
                for (int32 Index = 0; Index < kIterations; ++Index)                            \
                {                                                                              \
                    const FString Text = OursExpr;                                             \
                    Consume(Text.data(), Text.size());                                         \
                }                                                                              \
            });                                                                                \
            ReportHeader(Title);                                                               \
            ReportRow("std::format -> std::string", Std, 0.0);                                 \
            ReportRow("std::format_to -> FString", StdTo, Std);                                \
            ReportRow("Lumina::Format -> FString", Ours, Std);                                 \
        }

    TEST(FormatBench, SingleInteger)
    {
        BENCH_CASE("200,000 x Format(\"{}\", int)",
            std::format("{}", Index),
            std::format_to(std::back_inserter(Text), "{}", Index),
            Lumina::Format("{}", Index));
        SUCCEED();
    }

    TEST(FormatBench, MixedArguments)
    {
        const char* Name = "subsystem";

        BENCH_CASE("200,000 x Format(\"{} {} {}\", int, double, str)",
            std::format("{} {} {}", Index, 3.5, Name),
            std::format_to(std::back_inserter(Text), "{} {} {}", Index, 3.5, Name),
            Lumina::Format("{} {} {}", Index, 3.5, Name));
        SUCCEED();
    }

    TEST(FormatBench, LogLine)
    {
        const char* Category = "Renderer";

        BENCH_CASE("200,000 x a typical log line",
            std::format("[{}] {} finished in {:.2f} ms ({} draws)", Category, "frame", 12.3456, Index),
            std::format_to(std::back_inserter(Text), "[{}] {} finished in {:.2f} ms ({} draws)",
                           Category, "frame", 12.3456, Index),
            Lumina::Format("[{}] {} finished in {:.2f} ms ({} draws)", Category, "frame", 12.3456, Index));
        SUCCEED();
    }

    TEST(FormatBench, FixedPrecisionFloat)
    {
        BENCH_CASE("200,000 x Format(\"{:.3f}\", double)",
            std::format("{:.3f}", 1234.56789),
            std::format_to(std::back_inserter(Text), "{:.3f}", 1234.56789),
            Lumina::Format("{:.3f}", 1234.56789));
        SUCCEED();
    }

    TEST(FormatBench, HexWithWidth)
    {
        BENCH_CASE("200,000 x Format(\"{:#010x}\", uint32)",
            std::format("{:#010x}", static_cast<Lumina::uint32>(Index)),
            std::format_to(std::back_inserter(Text), "{:#010x}", static_cast<Lumina::uint32>(Index)),
            Lumina::Format("{:#010x}", static_cast<Lumina::uint32>(Index)));
        SUCCEED();
    }

    TEST(FormatBench, AppendIntoAReusedBuffer)
    {
        const double StdTo = BestMillisOf(kRepeats, [&]
        {
            FString Text;
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                Text.clear();
                std::format_to(std::back_inserter(Text), "{}:{}", "key", Index);
                Consume(Text.data(), Text.size());
            }
        });

        const double Ours = BestMillisOf(kRepeats, [&]
        {
            FStringBuilder Builder;
            for (int32 Index = 0; Index < kIterations; ++Index)
            {
                Builder.Reset();
                Builder.AppendFormat("{}:{}", "key", Index);
                Consume(Builder.data(), Builder.size());
            }
        });

        ReportHeader("200,000 x append into a reused buffer");
        ReportRow("std::format_to -> FString", StdTo, 0.0);
        ReportRow("FStringBuilder::AppendFormat", Ours, StdTo);
        SUCCEED();
    }

    TEST(FormatBench, ObjectSizes)
    {
        std::printf("\nformat plumbing sizes\n");
        std::printf("  %-30s %10zu\n", "sizeof FFormatArg", sizeof(Lumina::Fmt::FFormatArg));
        std::printf("  %-30s %10zu\n", "sizeof FFormatSpec", sizeof(Lumina::Fmt::FFormatSpec));
        std::printf("  %-30s %10zu\n", "sizeof FStringBuilder", sizeof(FStringBuilder));
        SUCCEED();
    }
}
