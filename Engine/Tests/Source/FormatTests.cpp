#include <gtest/gtest.h>

#include <format>
#include <string>

#include "Containers/Format.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "GUID/GUID.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaFormatTests
{
    namespace Fmt = Lumina::Fmt;
    using Lumina::Format;
    using Lumina::FormatAs;
    using Lumina::FormatTo;
    using Lumina::AppendFormat;
    using Lumina::FString;
    using Lumina::FStringView;
    using Lumina::FFixedString;
    using Lumina::FStringBuilder;
    using Lumina::FName;
    using Lumina::int32;
    using Lumina::int64;
    using Lumina::uint32;
    using Lumina::uint64;

    struct FPoint
    {
        int32 X = 0;
        int32 Y = 0;
    };

    void FormatArgument(Fmt::FFormatBuffer& Out, const FPoint& Point, const Fmt::FFormatSpec& Spec)
    {
        Lumina::AppendFormat(Out, "({}, {})", Point.X, Point.Y);
        (void)Spec;
    }

    // The standard is the reference implementation for everything both of them claim to support.
    #define EXPECT_MATCHES_STD(Fmt, ...) \
        EXPECT_EQ(Format(Fmt, __VA_ARGS__), FString(std::format(Fmt, __VA_ARGS__).c_str())) << "spec: " << Fmt

    TEST(Format, CopiesLiteralsAndUnescapesBraces)
    {
        EXPECT_EQ(Format("plain"), "plain");
        EXPECT_EQ(Format(""), "");
        EXPECT_EQ(Format("{{}}"), "{}");
        EXPECT_EQ(Format("a{{b}}c"), "a{b}c");
        EXPECT_EQ(Format("{{{}}}", 7), "{7}");
    }

    TEST(Format, SubstitutesAutomaticAndManualIndices)
    {
        EXPECT_EQ(Format("{} {} {}", 1, 2, 3), "1 2 3");
        EXPECT_EQ(Format("{2} {1} {0}", 1, 2, 3), "3 2 1");
        EXPECT_EQ(Format("{0} {0} {0}", 9), "9 9 9");
    }

    TEST(Format, MatchesTheStandardOnIntegers)
    {
        EXPECT_MATCHES_STD("{}", 0);
        EXPECT_MATCHES_STD("{}", -1);
        EXPECT_MATCHES_STD("{}", 1234567890);
        EXPECT_MATCHES_STD("{}", int64(-9223372036854775807ll - 1));
        EXPECT_MATCHES_STD("{}", uint64(18446744073709551615ull));
        EXPECT_MATCHES_STD("{:d}", 42);
        EXPECT_MATCHES_STD("{:x}", 48879);
        EXPECT_MATCHES_STD("{:X}", 48879);
        EXPECT_MATCHES_STD("{:#x}", 48879);
        EXPECT_MATCHES_STD("{:#X}", 48879);
        EXPECT_MATCHES_STD("{:o}", 64);
        EXPECT_MATCHES_STD("{:#o}", 64);
        EXPECT_MATCHES_STD("{:#o}", 0);
        EXPECT_MATCHES_STD("{:b}", 5);
        EXPECT_MATCHES_STD("{:#b}", 5);
        EXPECT_MATCHES_STD("{:#B}", 5);
    }

    TEST(Format, MatchesTheStandardOnIntegerPaddingAndSigns)
    {
        EXPECT_MATCHES_STD("{:8}", 42);
        EXPECT_MATCHES_STD("{:<8}", 42);
        EXPECT_MATCHES_STD("{:>8}", 42);
        EXPECT_MATCHES_STD("{:^8}", 42);
        EXPECT_MATCHES_STD("{:*^9}", 42);
        EXPECT_MATCHES_STD("{:08}", 42);
        EXPECT_MATCHES_STD("{:08}", -42);
        EXPECT_MATCHES_STD("{:+}", 42);
        EXPECT_MATCHES_STD("{: }", 42);
        EXPECT_MATCHES_STD("{:+}", -42);
        EXPECT_MATCHES_STD("{:+08}", 42);
        EXPECT_MATCHES_STD("{:#010x}", 48879);
        EXPECT_MATCHES_STD("{:3}", 1234567);
    }

    TEST(Format, MatchesTheStandardOnFloats)
    {
        EXPECT_MATCHES_STD("{}", 0.0);
        EXPECT_MATCHES_STD("{}", -0.0);
        EXPECT_MATCHES_STD("{}", 1.0);
        EXPECT_MATCHES_STD("{}", 0.1);
        EXPECT_MATCHES_STD("{}", 3.14159265358979);
        EXPECT_MATCHES_STD("{}", 1e300);
        EXPECT_MATCHES_STD("{}", 1e-300);
        EXPECT_MATCHES_STD("{:.2f}", 3.14159);
        EXPECT_MATCHES_STD("{:.0f}", 2.5);
        EXPECT_MATCHES_STD("{:f}", 1.5);
        EXPECT_MATCHES_STD("{:.3e}", 1234.5678);
        EXPECT_MATCHES_STD("{:.3E}", 1234.5678);
        EXPECT_MATCHES_STD("{:.4g}", 1234.5678);
        EXPECT_MATCHES_STD("{:.4G}", 0.000012345);
        EXPECT_MATCHES_STD("{:10.2f}", 3.14159);
        EXPECT_MATCHES_STD("{:<10.2f}", 3.14159);
        EXPECT_MATCHES_STD("{:010.2f}", -3.14159);
        EXPECT_MATCHES_STD("{:+.2f}", 3.14159);
    }

    TEST(Format, MatchesTheStandardOnFloatSpecialValues)
    {
        const double Infinity = std::numeric_limits<double>::infinity();
        const double NotANumber = std::numeric_limits<double>::quiet_NaN();

        EXPECT_MATCHES_STD("{}", Infinity);
        EXPECT_MATCHES_STD("{}", -Infinity);
        EXPECT_MATCHES_STD("{}", NotANumber);
        EXPECT_MATCHES_STD("{:8}", Infinity);
        EXPECT_MATCHES_STD("{:F}", Infinity);
    }

    TEST(Format, MatchesTheStandardOnStringsAndCharacters)
    {
        EXPECT_MATCHES_STD("{}", "text");
        EXPECT_MATCHES_STD("{:8}", "text");
        EXPECT_MATCHES_STD("{:>8}", "text");
        EXPECT_MATCHES_STD("{:^8}", "text");
        EXPECT_MATCHES_STD("{:.2}", "text");
        EXPECT_MATCHES_STD("{:8.2}", "text");
        EXPECT_MATCHES_STD("{}", 'a');
        EXPECT_MATCHES_STD("{:>4}", 'a');
        EXPECT_MATCHES_STD("{}", true);
        EXPECT_MATCHES_STD("{}", false);
        EXPECT_MATCHES_STD("{:d}", true);
        EXPECT_MATCHES_STD("{:>7}", true);
    }

    TEST(Format, HandlesEveryEngineStringType)
    {
        const FString Owned("owned");
        const FStringView View("viewed");
        const FFixedString Fixed("fixed");
        const std::string Standard("standard");
        const char* Pointer = "pointer";

        EXPECT_EQ(Format("{}", Owned), "owned");
        EXPECT_EQ(Format("{}", View), "viewed");
        EXPECT_EQ(Format("{}", Fixed), "fixed");
        EXPECT_EQ(Format("{}", Standard), "standard");
        EXPECT_EQ(Format("{}", Pointer), "pointer");
        EXPECT_EQ(Format("{:>8}", Owned), "   owned");
        EXPECT_EQ(Format("{:.3}", Owned), "own");
    }

    TEST(Format, FormatsPointers)
    {
        const void* Address = reinterpret_cast<const void*>(uintptr_t(0xdeadbeef));

        EXPECT_EQ(Format("{}", Address), "0xdeadbeef");
        EXPECT_EQ(Format("{:p}", Address), "0xdeadbeef");
        EXPECT_EQ(Format("{:P}", Address), "0XDEADBEEF");
        EXPECT_EQ(Format("{}", static_cast<const void*>(nullptr)), "0x0");
    }

    TEST(Format, ReadsWidthAndPrecisionFromArguments)
    {
        EXPECT_EQ(Format("{:{}}", 42, 6), "    42");
        EXPECT_EQ(Format("{:.{}f}", 3.14159, 3), "3.142");
        EXPECT_EQ(Format("{:{}.{}f}", 3.14159, 9, 2), "     3.14");
    }

    TEST(Format, ReachesACustomTypeThroughFormatArgument)
    {
        const FPoint Point{ 3, -4 };

        EXPECT_EQ(Format("{}", Point), "(3, -4)");
        EXPECT_EQ(Format("at {} and {}", Point, FPoint{ 0, 0 }), "at (3, -4) and (0, 0)");
    }

    TEST(Format, FormatsEngineValueTypes)
    {
        const FName Name("SomeName");
        EXPECT_EQ(Format("{}", Name), FString(Name.c_str()));

        const Lumina::FGuid Guid = Lumina::FGuid::New();
        EXPECT_EQ(Format("{}", Guid), Guid.ToString());
    }

    TEST(Format, FormatsIntoAChosenStringType)
    {
        const FFixedString Fixed = FormatAs<FFixedString>("{}-{}", 1, 2);
        EXPECT_EQ(Fixed, "1-2");

        FString Appended("head");
        AppendFormat(Appended, "/{}", "tail");
        EXPECT_EQ(Appended, "head/tail");

        FString Replaced("stale");
        FormatTo(Replaced, "{}", 5);
        EXPECT_EQ(Replaced, "5");
    }

    TEST(Format, FormatsIntoACallerOwnedArray)
    {
        char Small[8] = {};
        const size_t Written = Lumina::FormatToBuffer(Small, sizeof(Small), "{}", 1234567890);

        EXPECT_EQ(Written, 8u);
        EXPECT_EQ(FStringView(Small, Written), "12345678");

        char Roomy[32] = {};
        const size_t Fits = Lumina::FormatToBuffer(Roomy, sizeof(Roomy), "{}-{}", 1, 2);
        EXPECT_EQ(FStringView(Roomy, Fits), "1-2");
    }

    TEST(Format, GrowsPastTheInlineBuffer)
    {
        const FString Long(600, 'x');
        const FString Result = Format("{}{}", Long, Long);

        EXPECT_EQ(Result.size(), 1200u);
        EXPECT_EQ(Result[0], 'x');
        EXPECT_EQ(Result[1199], 'x');
    }

    TEST(Format, AcceptsAFormatStringThatOnlyExistsAtRuntime)
    {
        const FString Pattern("{} and {}");
        EXPECT_EQ(Format(Lumina::RuntimeFormat(Pattern), 1, 2), "1 and 2");
    }

    TEST(Format, ReportsRatherThanCrashesOnABadRuntimeString)
    {
        EXPECT_EQ(Format(Lumina::RuntimeFormat("{"), 1), "<unclosed {>");
        EXPECT_EQ(Format(Lumina::RuntimeFormat("}"), 1), "<stray }>");
        EXPECT_EQ(Format(Lumina::RuntimeFormat("{5}"), 1), "<missing arg>");
    }

    TEST(StringBuilder, AppendsTextAndFormattedValues)
    {
        FStringBuilder Builder;
        Builder.Append("start");
        Builder.AppendFormat(" {}:{}", "key", 42);
        Builder.Append('!');

        EXPECT_EQ(Builder.View(), "start key:42!");
        EXPECT_EQ(Builder.size(), 13u);
        EXPECT_STREQ(Builder.c_str(), "start key:42!");

        Builder.Reset();
        EXPECT_TRUE(Builder.empty());
    }

    TEST(StringBuilder, KeepsWorkingPastItsInlineCapacity)
    {
        Lumina::TStringBuilder<16> Builder;
        for (int32 Index = 0; Index < 100; ++Index)
        {
            Builder.AppendFormat("{},", Index);
        }

        EXPECT_GT(Builder.size(), 16u);
        EXPECT_EQ(Builder.View().substr(0, 6), "0,1,2,");
    }
}
