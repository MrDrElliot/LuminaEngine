#include <gtest/gtest.h>

#include <format>
#include <unordered_set>

#include "Containers/String.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaBasicStringTests
{
    using FStr = Lumina::Containers::FString;
    using FWStr = Lumina::Containers::FWString;

    template <size_t N>
    using TFixed = Lumina::Containers::TFixedString<N>;

    constexpr size_t kSso = FStr::InlineCapacityChars;

    TEST(BasicStringLayout, DefaultStringIsSixteenBytes)
    {
        static_assert(sizeof(FStr) == 16);
        static_assert(kSso == 15);
        static_assert(alignof(FStr) == alignof(void*));
        SUCCEED();
    }

    TEST(BasicStringLayout, FixedCapacityWidensTheBuffer)
    {
        static_assert(sizeof(TFixed<63>) == 64);
        static_assert(TFixed<63>::InlineCapacityChars == 63);
        SUCCEED();
    }

    TEST(BasicStringBasics, DefaultIsEmptyAndTerminated)
    {
        const FStr Text;
        EXPECT_TRUE(Text.empty());
        EXPECT_EQ(Text.size(), 0u);
        EXPECT_EQ(Text.capacity(), kSso);
        EXPECT_TRUE(Text.IsInline());
        EXPECT_STREQ(Text.c_str(), "");
    }

    TEST(BasicStringBasics, ConstructFromLiteralAndView)
    {
        const FStr FromLiteral("hello");
        EXPECT_EQ(FromLiteral.size(), 5u);
        EXPECT_STREQ(FromLiteral.c_str(), "hello");

        const FStr FromView(std::string_view("world"));
        EXPECT_STREQ(FromView.c_str(), "world");

        const FStr FromRange("abcdef", 3);
        EXPECT_STREQ(FromRange.c_str(), "abc");

        const FStr Repeated(4, 'x');
        EXPECT_STREQ(Repeated.c_str(), "xxxx");
    }

    TEST(BasicStringBasics, StaysInlineUpToTheSsoCapacity)
    {
        FStr Text;
        for (size_t Index = 0; Index < kSso; ++Index)
        {
            Text.push_back('a');
            ASSERT_TRUE(Text.IsInline()) << "spilled at " << Index;
        }

        EXPECT_EQ(Text.size(), kSso);
        EXPECT_STREQ(Text.c_str(), "aaaaaaaaaaaaaaa");

        Text.push_back('b');
        EXPECT_FALSE(Text.IsInline());
        EXPECT_EQ(Text.size(), kSso + 1);
        EXPECT_EQ(Text.back(), 'b');
        EXPECT_STREQ(Text.c_str(), "aaaaaaaaaaaaaaab");
    }

    TEST(BasicStringBasics, TerminatorSurvivesAFullInlineBuffer)
    {
        FStr Text(kSso, 'z');
        ASSERT_TRUE(Text.IsInline());
        EXPECT_EQ(Text.size(), kSso);
        EXPECT_EQ(std::char_traits<char>::length(Text.c_str()), kSso);
    }

    TEST(BasicStringBasics, HeapGrowthKeepsContent)
    {
        FStr Text;
        for (int Index = 0; Index < 5000; ++Index)
        {
            Text.push_back(static_cast<char>('a' + (Index % 26)));
        }

        ASSERT_EQ(Text.size(), 5000u);
        EXPECT_FALSE(Text.IsInline());
        for (int Index = 0; Index < 5000; ++Index)
        {
            ASSERT_EQ(Text[static_cast<size_t>(Index)], static_cast<char>('a' + (Index % 26)));
        }
        EXPECT_EQ(std::char_traits<char>::length(Text.c_str()), 5000u);
    }

    TEST(BasicStringBasics, ResizeAndClear)
    {
        FStr Text("abcdef");
        Text.resize(3);
        EXPECT_STREQ(Text.c_str(), "abc");

        Text.resize(6, '!');
        EXPECT_STREQ(Text.c_str(), "abc!!!");

        Text.clear();
        EXPECT_TRUE(Text.empty());
        EXPECT_STREQ(Text.c_str(), "");
    }

    TEST(BasicStringBasics, ReserveAndShrinkToFit)
    {
        FStr Text("short");
        Text.reserve(500);
        EXPECT_GE(Text.capacity(), 500u);
        EXPECT_FALSE(Text.IsInline());
        EXPECT_STREQ(Text.c_str(), "short");

        Text.shrink_to_fit();
        EXPECT_TRUE(Text.IsInline());
        EXPECT_STREQ(Text.c_str(), "short");
        EXPECT_EQ(Text.capacity(), kSso);
    }

    TEST(BasicStringAppend, AppendAcrossTheSsoBoundary)
    {
        FStr Text;
        Text.append("0123456789");
        EXPECT_TRUE(Text.IsInline());

        Text += "abcdefghij";
        EXPECT_FALSE(Text.IsInline());
        EXPECT_STREQ(Text.c_str(), "0123456789abcdefghij");

        Text += '!';
        EXPECT_EQ(Text.back(), '!');

        Text.append(3, '.');
        EXPECT_STREQ(Text.c_str(), "0123456789abcdefghij!...");
    }

    TEST(BasicStringAppend, OperatorPlusCombinations)
    {
        const FStr Left("left");
        EXPECT_EQ(Left + "-right", FStr("left-right"));
        EXPECT_EQ("pre-" + Left, FStr("pre-left"));
        EXPECT_EQ(Left + '!', FStr("left!"));
        EXPECT_EQ(Left + std::string_view("-view"), FStr("left-view"));
    }

    TEST(BasicStringAliasing, AppendingOwnContentSurvivesGrowth)
    {
        FStr Text("0123456789");
        ASSERT_TRUE(Text.IsInline());

        Text.append(Text.c_str(), Text.size());
        EXPECT_STREQ(Text.c_str(), "01234567890123456789");
    }

    TEST(BasicStringAliasing, AssignFromOwnContentSurvivesGrowth)
    {
        FStr Text("abcdefghijklmnopqrstuvwxyz");
        ASSERT_FALSE(Text.IsInline());

        Text.assign(Text.c_str() + 1, 5);
        EXPECT_STREQ(Text.c_str(), "bcdef");
    }

    TEST(BasicStringAliasing, InsertOfOwnContent)
    {
        FStr Text("0123456789");
        Text.insert(0, Text.View());
        EXPECT_STREQ(Text.c_str(), "01234567890123456789");
    }

    TEST(BasicStringEdit, InsertEraseReplaceSubstr)
    {
        FStr Text("hello world");

        Text.insert(5, ",");
        EXPECT_STREQ(Text.c_str(), "hello, world");

        Text.erase(5, 1);
        EXPECT_STREQ(Text.c_str(), "hello world");

        Text.replace(6, 5, "there");
        EXPECT_STREQ(Text.c_str(), "hello there");

        EXPECT_EQ(Text.substr(6), FStr("there"));
        EXPECT_EQ(Text.substr(0, 5), FStr("hello"));
    }

    TEST(BasicStringEdit, EraseWithIterators)
    {
        FStr Text("abcdef");
        Text.erase(Text.begin() + 1, Text.begin() + 3);
        EXPECT_STREQ(Text.c_str(), "adef");

        Text.erase(Text.begin());
        EXPECT_STREQ(Text.c_str(), "def");
    }

    TEST(BasicStringSearch, FindFamily)
    {
        const FStr Text("/Game/Meshes/Rock.lasset");

        EXPECT_EQ(Text.find("Meshes"), 6u);
        EXPECT_EQ(Text.find('/'), 0u);
        EXPECT_EQ(Text.find("missing"), FStr::npos);
        EXPECT_EQ(Text.rfind('/'), 12u);
        EXPECT_EQ(Text.find_last_of("."), 17u);
        EXPECT_EQ(Text.find_first_of("GM"), 1u);
        EXPECT_EQ(Text.find_first_not_of("/"), 1u);
    }

    TEST(BasicStringSearch, PredicatesTheEastlStringNeverHad)
    {
        const FStr Text("/Game/Meshes/Rock.lasset");

        EXPECT_TRUE(Text.starts_with("/Game/"));
        EXPECT_FALSE(Text.starts_with("/Engine/"));
        EXPECT_TRUE(Text.ends_with(".lasset"));
        EXPECT_TRUE(Text.contains("Meshes"));
        EXPECT_TRUE(Text.contains('.'));
        EXPECT_FALSE(Text.contains("Textures"));
    }

    TEST(BasicStringCase, FoldingAndComparison)
    {
        FStr Text("MiXeD");
        Text.ToLower();
        EXPECT_STREQ(Text.c_str(), "mixed");

        Text.ToUpper();
        EXPECT_STREQ(Text.c_str(), "MIXED");

        EXPECT_TRUE(FStr("Hello").EqualsIgnoreCase("hELLO"));
        EXPECT_FALSE(FStr("Hello").EqualsIgnoreCase("hELLO!"));
    }

    TEST(BasicStringCase, Trimming)
    {
        FStr Padded("  \t spaced out \n ");
        Padded.Trim();
        EXPECT_STREQ(Padded.c_str(), "spaced out");

        FStr OnlySpace("    ");
        OnlySpace.Trim();
        EXPECT_TRUE(OnlySpace.empty());
    }

    TEST(BasicStringCopyMove, CopyIsIndependent)
    {
        const FStr Source("a string long enough to live on the heap");
        FStr Copy = Source;

        ASSERT_FALSE(Copy.IsInline());
        EXPECT_NE(Copy.data(), Source.data());
        EXPECT_EQ(Copy, Source);

        Copy[0] = 'A';
        EXPECT_EQ(Source[0], 'a');
    }

    TEST(BasicStringCopyMove, MoveStealsTheHeapBlock)
    {
        FStr Source("a string long enough to live on the heap");
        const char* Base = Source.c_str();

        FStr Moved = std::move(Source);
        EXPECT_EQ(Moved.c_str(), Base);
        EXPECT_TRUE(Source.empty());
        EXPECT_TRUE(Source.IsInline());
        EXPECT_STREQ(Source.c_str(), "");
    }

    TEST(BasicStringCopyMove, MoveFromInlineCopiesCharacters)
    {
        FStr Source("short");
        ASSERT_TRUE(Source.IsInline());

        FStr Moved = std::move(Source);
        EXPECT_STREQ(Moved.c_str(), "short");
        EXPECT_TRUE(Source.empty());
    }

    TEST(BasicStringCopyMove, MoveAssignReleasesTheOldBlock)
    {
        FStr Target("a long target string that certainly allocates");
        FStr Source("a long source string that certainly allocates");

        Target = std::move(Source);
        EXPECT_STREQ(Target.c_str(), "a long source string that certainly allocates");
    }

    TEST(BasicStringCopyMove, SelfAssignmentIsSafe)
    {
        FStr Text("a long string that certainly allocates on the heap");
        FStr& Alias = Text;

        Text = Alias;
        EXPECT_STREQ(Text.c_str(), "a long string that certainly allocates on the heap");

        Text = std::move(Alias);
        EXPECT_STREQ(Text.c_str(), "a long string that certainly allocates on the heap");
    }

    TEST(BasicStringCopyMove, SwapAcrossInlineAndHeap)
    {
        FStr Small("tiny");
        FStr Large("a string long enough to live on the heap");

        Small.swap(Large);
        EXPECT_STREQ(Small.c_str(), "a string long enough to live on the heap");
        EXPECT_STREQ(Large.c_str(), "tiny");
    }

    TEST(BasicStringCompare, EqualityAndOrdering)
    {
        const FStr A("apple");
        const FStr B("banana");

        EXPECT_EQ(A, FStr("apple"));
        EXPECT_EQ(A, "apple");
        EXPECT_NE(A, B);
        EXPECT_LT(A, B);
        EXPECT_GT(B, A);
        EXPECT_LT(A, "apples");
        EXPECT_EQ(A.compare("apple"), 0);
    }

    TEST(BasicStringInterop, FormatterAndHash)
    {
        const FStr Text("formatted");
        EXPECT_EQ(std::format("[{}]", Text), "[formatted]");
        EXPECT_EQ(std::format("{:>12}", Text), "   formatted");

        std::unordered_set<FStr> Seen;
        Seen.insert(FStr("one"));
        Seen.insert(FStr("two"));
        Seen.insert(FStr("one"));
        EXPECT_EQ(Seen.size(), 2u);
    }

    TEST(BasicStringInterop, RangeForAndStdAlgorithms)
    {
        const FStr Text("abc");

        int Sum = 0;
        for (char Character : Text)
        {
            Sum += Character;
        }
        EXPECT_EQ(Sum, 'a' + 'b' + 'c');

        const Lumina::FStringView View = Text;
        EXPECT_EQ(View, Lumina::FStringView("abc"));

        const std::string_view StdView = View;
        EXPECT_EQ(StdView, "abc");
    }

    TEST(BasicStringWide, WideCharacterStringsWork)
    {
        FWStr Text(L"wide");
        EXPECT_EQ(Text.size(), 4u);
        EXPECT_TRUE(Text.IsInline());

        Text += L" characters that push this past the inline buffer";
        EXPECT_FALSE(Text.IsInline());
        EXPECT_EQ(Text.View().substr(0, 4), std::wstring_view(L"wide"));
        EXPECT_EQ(std::char_traits<wchar_t>::length(Text.c_str()), Text.size());
    }

    TEST(BasicStringFixed, LargerInlineBufferAvoidsTheHeap)
    {
        TFixed<255> Path("/Game/Some/Reasonably/Long/Asset/Path/Rock.lasset");
        EXPECT_TRUE(Path.IsInline());
        EXPECT_EQ(Path.capacity(), 255u);

        Path.append(FStr(220, 'x').View());
        EXPECT_FALSE(Path.IsInline());
        EXPECT_EQ(Path.size(), 49u + 220u);
    }
}
