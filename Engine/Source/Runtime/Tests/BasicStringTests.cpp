#include <gtest/gtest.h>

#include <format>

#include "Containers/StringFormat.h"
#include "Containers/HashTable.h"
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
        EXPECT_TRUE(Text.IsEmpty());
        EXPECT_EQ(Text.Len(), 0);
        EXPECT_EQ(Text.GetCapacity(), kSso);
        EXPECT_TRUE(Text.IsInline());
        EXPECT_STREQ(*Text, "");
    }

    TEST(BasicStringBasics, ConstructFromLiteralAndView)
    {
        const FStr FromLiteral("hello");
        EXPECT_EQ(FromLiteral.Len(), 5);
        EXPECT_STREQ(*FromLiteral, "hello");

        const FStr FromView(std::string_view("world"));
        EXPECT_STREQ(*FromView, "world");

        const FStr FromRange("abcdef", 3);
        EXPECT_STREQ(*FromRange, "abc");

        const FStr Repeated(4, 'x');
        EXPECT_STREQ(*Repeated, "xxxx");
    }

    TEST(BasicStringBasics, StaysInlineUpToTheSsoCapacity)
    {
        FStr Text;
        for (size_t Index = 0; Index < kSso; ++Index)
        {
            Text.AppendChar('a');
            ASSERT_TRUE(Text.IsInline()) << "spilled at " << Index;
        }

        EXPECT_EQ(Text.Len(), kSso);
        EXPECT_STREQ(*Text, "aaaaaaaaaaaaaaa");

        Text.AppendChar('b');
        EXPECT_FALSE(Text.IsInline());
        EXPECT_EQ(Text.Len(), kSso + 1);
        EXPECT_EQ(Text.Last(), 'b');
        EXPECT_STREQ(*Text, "aaaaaaaaaaaaaaab");
    }

    TEST(BasicStringBasics, TerminatorSurvivesAFullInlineBuffer)
    {
        FStr Text(kSso, 'z');
        ASSERT_TRUE(Text.IsInline());
        EXPECT_EQ(Text.Len(), kSso);
        EXPECT_EQ(std::char_traits<char>::length(*Text), kSso);
    }

    TEST(BasicStringBasics, HeapGrowthKeepsContent)
    {
        FStr Text;
        for (int Index = 0; Index < 5000; ++Index)
        {
            Text.AppendChar(static_cast<char>('a' + (Index % 26)));
        }

        ASSERT_EQ(Text.Len(), 5000);
        EXPECT_FALSE(Text.IsInline());
        for (int Index = 0; Index < 5000; ++Index)
        {
            ASSERT_EQ(Text[Index], static_cast<char>('a' + (Index % 26)));
        }
        EXPECT_EQ(std::char_traits<char>::length(*Text), 5000u);
    }

    TEST(BasicStringBasics, ResizeAndClear)
    {
        FStr Text("abcdef");
        Text.SetLen(3);
        EXPECT_STREQ(*Text, "abc");

        Text.SetLen(6, '!');
        EXPECT_STREQ(*Text, "abc!!!");

        Text.Reset();
        EXPECT_TRUE(Text.IsEmpty());
        EXPECT_STREQ(*Text, "");
    }

    TEST(BasicStringBasics, ReserveAndShrinkToFit)
    {
        FStr Text("short");
        Text.Reserve(500);
        EXPECT_GE(Text.GetCapacity(), 500);
        EXPECT_FALSE(Text.IsInline());
        EXPECT_STREQ(*Text, "short");

        Text.Shrink();
        EXPECT_TRUE(Text.IsInline());
        EXPECT_STREQ(*Text, "short");
        EXPECT_EQ(Text.GetCapacity(), kSso);
    }

    TEST(BasicStringAppend, AppendAcrossTheSsoBoundary)
    {
        FStr Text;
        Text.Append("0123456789");
        EXPECT_TRUE(Text.IsInline());

        Text += "abcdefghij";
        EXPECT_FALSE(Text.IsInline());
        EXPECT_STREQ(*Text, "0123456789abcdefghij");

        Text += '!';
        EXPECT_EQ(Text.Last(), '!');

        Text.AppendChars('.', 3);
        EXPECT_STREQ(*Text, "0123456789abcdefghij!...");
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

        Text.Append(*Text, Text.Len());
        EXPECT_STREQ(*Text, "01234567890123456789");
    }

    TEST(BasicStringAliasing, AssignFromOwnContentSurvivesGrowth)
    {
        FStr Text("abcdefghijklmnopqrstuvwxyz");
        ASSERT_FALSE(Text.IsInline());

        Text = Lumina::FStringView(*Text + 1, 5);
        EXPECT_STREQ(*Text, "bcdef");
    }

    TEST(BasicStringAliasing, InsertOfOwnContent)
    {
        FStr Text("0123456789");
        Text.InsertAt(0, Text.View());
        EXPECT_STREQ(*Text, "01234567890123456789");
    }

    TEST(BasicStringEdit, InsertEraseReplaceSubstr)
    {
        FStr Text("hello world");

        Text.InsertAt(5, ",");
        EXPECT_STREQ(*Text, "hello, world");

        Text.RemoveAt(5, 1);
        EXPECT_STREQ(*Text, "hello world");

        Text.RemoveAt(6, 5).InsertAt(6, "there");
        EXPECT_STREQ(*Text, "hello there");

        EXPECT_EQ(Text.Mid(6), FStr("there"));
        EXPECT_EQ(Text.Mid(0, 5), FStr("hello"));
    }

    TEST(BasicStringEdit, EraseWithIterators)
    {
        FStr Text("abcdef");
        Text.RemoveAt(1, 2);
        EXPECT_STREQ(*Text, "adef");

        Text.RemoveAt(0);
        EXPECT_STREQ(*Text, "def");
    }

    TEST(BasicStringSearch, FindFamily)
    {
        const FStr Text("/Game/Meshes/Rock.lasset");

        EXPECT_EQ(Text.Find("Meshes"), 6);
        EXPECT_EQ(Text.Find('/'), 0);
        EXPECT_EQ(Text.Find("missing"), Lumina::Constants::kIndexNone);
        EXPECT_EQ(Text.FindLast('/'), 12);
        EXPECT_EQ(Text.FindLastOf("."), 17);
        EXPECT_EQ(Text.FindFirstOf("GM"), 1);
        EXPECT_EQ(Text.FindFirstNotOf("/"), 1);
    }

    TEST(BasicStringSearch, PredicatesTheEastlStringNeverHad)
    {
        const FStr Text("/Game/Meshes/Rock.lasset");

        EXPECT_TRUE(Text.StartsWith("/Game/"));
        EXPECT_FALSE(Text.StartsWith("/Engine/"));
        EXPECT_TRUE(Text.EndsWith(".lasset"));
        EXPECT_TRUE(Text.Contains("Meshes"));
        EXPECT_TRUE(Text.Contains('.'));
        EXPECT_FALSE(Text.Contains("Textures"));
    }

    TEST(BasicStringCase, FoldingAndComparison)
    {
        FStr Text("MiXeD");
        Text.ToLower();
        EXPECT_STREQ(*Text, "mixed");

        Text.ToUpper();
        EXPECT_STREQ(*Text, "MIXED");

        EXPECT_TRUE(FStr("Hello").EqualsIgnoreCase("hELLO"));
        EXPECT_FALSE(FStr("Hello").EqualsIgnoreCase("hELLO!"));
    }

    TEST(BasicStringCase, Trimming)
    {
        FStr Padded("  \t spaced out \n ");
        Padded.Trim();
        EXPECT_STREQ(*Padded, "spaced out");

        FStr OnlySpace("    ");
        OnlySpace.Trim();
        EXPECT_TRUE(OnlySpace.IsEmpty());
    }

    TEST(BasicStringCopyMove, CopyIsIndependent)
    {
        const FStr Source("a string long enough to live on the heap");
        FStr Copy = Source;

        ASSERT_FALSE(Copy.IsInline());
        EXPECT_NE(Copy.GetData(), Source.GetData());
        EXPECT_EQ(Copy, Source);

        Copy[0] = 'A';
        EXPECT_EQ(Source[0], 'a');
    }

    TEST(BasicStringCopyMove, MoveStealsTheHeapBlock)
    {
        FStr Source("a string long enough to live on the heap");
        const char* Base = *Source;

        FStr Moved = std::move(Source);
        EXPECT_EQ(*Moved, Base);
        EXPECT_TRUE(Source.IsEmpty());
        EXPECT_TRUE(Source.IsInline());
        EXPECT_STREQ(*Source, "");
    }

    TEST(BasicStringCopyMove, MoveFromInlineCopiesCharacters)
    {
        FStr Source("short");
        ASSERT_TRUE(Source.IsInline());

        FStr Moved = std::move(Source);
        EXPECT_STREQ(*Moved, "short");
        EXPECT_TRUE(Source.IsEmpty());
    }

    TEST(BasicStringCopyMove, MoveAssignReleasesTheOldBlock)
    {
        FStr Target("a long target string that certainly allocates");
        FStr Source("a long source string that certainly allocates");

        Target = std::move(Source);
        EXPECT_STREQ(*Target, "a long source string that certainly allocates");
    }

    TEST(BasicStringCopyMove, SelfAssignmentIsSafe)
    {
        FStr Text("a long string that certainly allocates on the heap");
        FStr& Alias = Text;

        Text = Alias;
        EXPECT_STREQ(*Text, "a long string that certainly allocates on the heap");

        Text = std::move(Alias);
        EXPECT_STREQ(*Text, "a long string that certainly allocates on the heap");
    }

    TEST(BasicStringCopyMove, SwapAcrossInlineAndHeap)
    {
        FStr Small("tiny");
        FStr Large("a string long enough to live on the heap");

        swap(Small, Large);
        EXPECT_STREQ(*Small, "a string long enough to live on the heap");
        EXPECT_STREQ(*Large, "tiny");
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
        EXPECT_EQ(A.Compare("apple"), 0);
    }

    TEST(BasicStringInterop, FormatterAndHash)
    {
        const FStr Text("formatted");
        EXPECT_EQ(Lumina::Format("[{}]", Text), "[formatted]");
        EXPECT_EQ(Lumina::Format("{:>12}", Text), "   formatted");

        Lumina::THashSet<FStr> Seen;
        Seen.Emplace("one");
        Seen.Emplace("two");
        Seen.Emplace("one");
        EXPECT_EQ(Seen.Num(), 2);
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
        static_assert(sizeof(FWStr) == 16);
        static_assert(FWStr::InlineCapacityChars == 16 / sizeof(wchar_t) - 1);

        FWStr Text(L"wid");
        EXPECT_EQ(Text.Len(), 3);
        EXPECT_TRUE(Text.IsInline());

        Text += L" characters that push this past the inline buffer";
        EXPECT_FALSE(Text.IsInline());
        EXPECT_EQ(Text.View().Mid(0, 3), std::wstring_view(L"wid"));
        EXPECT_EQ(std::char_traits<wchar_t>::length(*Text), Text.Len());
    }

    TEST(BasicStringFixed, LargerInlineBufferAvoidsTheHeap)
    {
        TFixed<255> Path("/Game/Some/Reasonably/Long/Asset/Path/Rock.lasset");
        EXPECT_TRUE(Path.IsInline());
        EXPECT_EQ(Path.GetCapacity(), 255);

        Path.Append(FStr(220, 'x').View());
        EXPECT_FALSE(Path.IsInline());
        EXPECT_EQ(Path.Len(), 49 + 220);
    }
}
