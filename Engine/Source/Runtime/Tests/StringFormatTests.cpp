#include <gtest/gtest.h>

#include "Containers/StringFormat.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaStringFormatTests
{
    using Lumina::FString;
    using Lumina::FStringBuilder;
    using Lumina::FStringView;

    TEST(StringFormat, FormatProducesAnFString)
    {
        const FString Result = Lumina::Format("{} + {} = {}", 2, 3, 5);
        EXPECT_STREQ(Result.c_str(), "2 + 3 = 5");
    }

    TEST(StringFormat, FormatAcceptsEngineStringTypes)
    {
        const FString Name = "Mesh";
        const FStringView View = "Static";

        const FString Result = Lumina::Format("{}/{}", View, Name);
        EXPECT_STREQ(Result.c_str(), "Static/Mesh");
    }

    TEST(StringFormat, FormatHandlesWidthAndPrecision)
    {
        EXPECT_STREQ(Lumina::Format("{:.2f}", 3.14159).c_str(), "3.14");
        EXPECT_STREQ(Lumina::Format("{:>6}", 42).c_str(), "    42");
        EXPECT_STREQ(Lumina::Format("{:x}", 255).c_str(), "ff");
        EXPECT_STREQ(Lumina::Format("100%").c_str(), "100%");
    }

    TEST(StringFormat, FormatToAppends)
    {
        FString Result = "prefix:";
        Lumina::AppendFormat(Result, " {} items", 7);
        EXPECT_STREQ(Result.c_str(), "prefix: 7 items");
    }

    TEST(StringFormat, IntegerFormattingMatchesTheOldToString)
    {
        EXPECT_STREQ(Lumina::Format("{}", 0).c_str(), "0");
        EXPECT_STREQ(Lumina::Format("{}", -1234).c_str(), "-1234");
        EXPECT_STREQ(Lumina::Format("{}", static_cast<uint64>(1) << 40).c_str(), "1099511627776");
    }

    TEST(StringBuilder, AppendsWithoutAllocatingForShortText)
    {
        FStringBuilder Builder;
        Builder.Append("count=").AppendFormat("{}", 12).Append('!');

        EXPECT_EQ(Builder.size(), 9u);
        EXPECT_STREQ(Builder.c_str(), "count=12!");
        EXPECT_EQ(Builder.View(), FStringView("count=12!"));
    }

    TEST(StringBuilder, TerminatorDoesNotBlockFurtherAppends)
    {
        FStringBuilder Builder;
        Builder.Append("one");
        EXPECT_STREQ(Builder.c_str(), "one");

        Builder.Append("-two");
        EXPECT_STREQ(Builder.c_str(), "one-two");
        EXPECT_EQ(Builder.size(), 7u);
    }

    TEST(StringBuilder, GrowsPastItsInlineBuffer)
    {
        Lumina::TStringBuilder<8> Builder;
        for (int Index = 0; Index < 100; ++Index)
        {
            Builder.AppendFormat("{},", Index);
        }

        const FString Result = Builder.ToString();
        EXPECT_EQ(Result.size(), Builder.size());
        EXPECT_EQ(Result.find("0,1,2,"), 0u);
        EXPECT_EQ(Result.rfind(",99,"), Result.size() - 4);
    }

    TEST(StringBuilder, ResetKeepsCapacity)
    {
        FStringBuilder Builder;
        Builder.Append("something");
        Builder.Reset();

        EXPECT_TRUE(Builder.empty());
        EXPECT_STREQ(Builder.c_str(), "");
    }

    TEST(StringBuilder, AppendLineJoinsRows)
    {
        FStringBuilder Builder;
        Builder.AppendLine("a").AppendLine("b");
        EXPECT_STREQ(Builder.c_str(), "a\nb\n");
    }
}
