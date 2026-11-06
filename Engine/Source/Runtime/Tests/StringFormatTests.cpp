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
        EXPECT_STREQ(*Result, "2 + 3 = 5");
    }

    TEST(StringFormat, FormatAcceptsEngineStringTypes)
    {
        const FString Name = "Mesh";
        const FStringView View = "Static";

        const FString Result = Lumina::Format("{}/{}", View, Name);
        EXPECT_STREQ(*Result, "Static/Mesh");
    }

    TEST(StringFormat, FormatHandlesWidthAndPrecision)
    {
        EXPECT_STREQ(*Lumina::Format("{:.2f}", 3.14159), "3.14");
        EXPECT_STREQ(*Lumina::Format("{:>6}", 42), "    42");
        EXPECT_STREQ(*Lumina::Format("{:x}", 255), "ff");
        EXPECT_STREQ(*Lumina::Format("100%"), "100%");
    }

    TEST(StringFormat, FormatToAppends)
    {
        FString Result = "prefix:";
        Lumina::AppendFormat(Result, " {} items", 7);
        EXPECT_STREQ(*Result, "prefix: 7 items");
    }

    TEST(StringFormat, IntegerFormattingMatchesTheOldToString)
    {
        EXPECT_STREQ(*Lumina::Format("{}", 0), "0");
        EXPECT_STREQ(*Lumina::Format("{}", -1234), "-1234");
        EXPECT_STREQ(*Lumina::Format("{}", static_cast<uint64>(1) << 40), "1099511627776");
    }

    TEST(StringBuilder, AppendsWithoutAllocatingForShortText)
    {
        FStringBuilder Builder;
        Builder.Append("count=").AppendFormat("{}", 12).Append('!');

        EXPECT_EQ(Builder.Len(), 9);
        EXPECT_STREQ(*Builder, "count=12!");
        EXPECT_EQ(Builder.View(), FStringView("count=12!"));
    }

    TEST(StringBuilder, TerminatorDoesNotBlockFurtherAppends)
    {
        FStringBuilder Builder;
        Builder.Append("one");
        EXPECT_STREQ(*Builder, "one");

        Builder.Append("-two");
        EXPECT_STREQ(*Builder, "one-two");
        EXPECT_EQ(Builder.Len(), 7);
    }

    TEST(StringBuilder, GrowsPastItsInlineBuffer)
    {
        Lumina::TStringBuilder<8> Builder;
        for (int Index = 0; Index < 100; ++Index)
        {
            Builder.AppendFormat("{},", Index);
        }

        const FString Result = Builder.ToString();
        EXPECT_EQ(Result.Len(), Builder.Len());
        EXPECT_EQ(Result.Find("0,1,2,"), 0);
        EXPECT_EQ(Result.FindLast(",99,"), Result.Len() - 4);
    }

    TEST(StringBuilder, ResetKeepsCapacity)
    {
        FStringBuilder Builder;
        Builder.Append("something");
        Builder.Reset();

        EXPECT_TRUE(Builder.IsEmpty());
        EXPECT_STREQ(*Builder, "");
    }

    TEST(StringBuilder, AppendLineJoinsRows)
    {
        FStringBuilder Builder;
        Builder.AppendLine("a").AppendLine("b");
        EXPECT_STREQ(*Builder, "a\nb\n");
    }
}
