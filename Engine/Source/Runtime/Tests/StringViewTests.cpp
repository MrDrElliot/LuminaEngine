#include <gtest/gtest.h>

#include <format>
#include <string>

#include "Containers/StringFormat.h"
#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/Vector.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaStringViewTests
{
    using FView = Lumina::FStringView;
    using FCView = Lumina::FCStringView;
    using FStr = Lumina::FString;

    TEST(StringViewLayout, IsAPointerAndALength)
    {
        static_assert(sizeof(FView) == sizeof(void*) + sizeof(size_t));
        static_assert(std::is_trivially_copyable_v<FView>);
        static_assert(std::is_trivially_copyable_v<FCView>);
        SUCCEED();
    }

    TEST(StringViewBasics, DefaultIsEmpty)
    {
        constexpr FView View;
        static_assert(View.IsEmpty());
        static_assert(View.Len() == 0);
        EXPECT_EQ(View.GetData(), nullptr);
    }

    TEST(StringViewBasics, ConstructFromLiteralAndRange)
    {
        constexpr FView FromLiteral("hello");
        static_assert(FromLiteral.Len() == 5);
        EXPECT_EQ(FromLiteral[0], 'h');
        EXPECT_EQ(FromLiteral[0], 'h');
        EXPECT_EQ(FromLiteral.Last(), 'o');

        const FView FromRange("hello world", 5);
        EXPECT_EQ(FromRange, FromLiteral);
    }

    TEST(StringViewBasics, ConvertsFromOwningString)
    {
        const FStr Owner("borrowed");
        const FView View = Owner;
        EXPECT_EQ(View.Len(), Owner.Len());
        EXPECT_EQ(View.GetData(), Owner.GetData());
    }

    TEST(StringViewBasics, ConvertsToStdView)
    {
        const FView View("interop");
        const std::string_view Std = View;
        EXPECT_EQ(Std, "interop");

        const FView Back(std::string_view("round trip"));
        EXPECT_EQ(Back.Len(), 10);
    }

    TEST(StringViewSlicing, SubstrLeftRight)
    {
        constexpr FView View("abcdef");
        static_assert(View.Mid(2) == FView("cdef"));
        static_assert(View.Mid(1, 3) == FView("bcd"));
        static_assert(View.Mid(2, 100) == FView("cdef"));
        static_assert(View.Left(2) == FView("ab"));
        static_assert(View.Right(2) == FView("ef"));
        static_assert(View.Right(99) == View);
        SUCCEED();
    }

    TEST(StringViewSlicing, RemovePrefixAndSuffix)
    {
        FView View("  padded  ");
        View.RemovePrefix(2);
        View.RemoveSuffix(2);
        EXPECT_EQ(View, FView("padded"));
    }

    TEST(StringViewSearch, FindFamilyMatchesTheStandard)
    {
        constexpr FView View("the quick brown fox");
        static_assert(View.Find(FView("quick")) == 4);
        static_assert(View.Find(FView("missing")) == Lumina::Constants::kIndexNone);
        static_assert(View.Find('q') == 4);
        static_assert(View.Find('q', 5) == Lumina::Constants::kIndexNone);
        static_assert(View.FindLast('o') == 17);
        static_assert(View.Find(FView("")) == 0);

        const std::string_view Reference("the quick brown fox");
        EXPECT_EQ(View.Find(FView("o")), Reference.find("o"));
        EXPECT_EQ(View.FindLast(FView("o")), Reference.rfind("o"));
        EXPECT_EQ(View.FindFirstOf(FView("xq")), Reference.find_first_of("xq"));
        EXPECT_EQ(View.FindLastOf(FView("xq")), Reference.find_last_of("xq"));
        EXPECT_EQ(View.FindFirstNotOf(FView("teh ")), Reference.find_first_not_of("teh "));
        EXPECT_EQ(View.FindLastNotOf(FView("xof ")), Reference.find_last_not_of("xof "));
    }

    TEST(StringViewSearch, SearchOnEmptyNeverReadsPastTheEnd)
    {
        constexpr FView Empty;
        static_assert(Empty.Find('a') == Lumina::Constants::kIndexNone);
        static_assert(Empty.FindLast('a') == Lumina::Constants::kIndexNone);
        static_assert(Empty.FindLastOf(FView("abc")) == Lumina::Constants::kIndexNone);
        static_assert(Empty.FindLastNotOf(FView("abc")) == Lumina::Constants::kIndexNone);
        static_assert(!Empty.StartsWith('a'));
        static_assert(!Empty.EndsWith('a'));
        SUCCEED();
    }

    TEST(StringViewCompare, OrderingAndPredicates)
    {
        constexpr FView Path("Engine/Source/Runtime");
        static_assert(Path.StartsWith(FView("Engine/")));
        static_assert(Path.EndsWith(FView("Runtime")));
        static_assert(Path.Contains(FView("Source")));
        static_assert(!Path.Contains(FView("Editor")));

        EXPECT_LT(FView("abc"), FView("abd"));
        EXPECT_LT(FView("abc"), FView("abcd"));
        EXPECT_EQ(FView("abc"), FView("abc"));
        EXPECT_NE(FView("abc"), FView("abC"));
    }

    TEST(StringViewCompare, CaseFoldedHelpers)
    {
        EXPECT_TRUE(Lumina::EqualsIgnoreCase(FView("Content/Meshes"), FView("content/MESHES")));
        EXPECT_FALSE(Lumina::EqualsIgnoreCase(FView("Content"), FView("Contents")));
        EXPECT_LT(Lumina::CompareIgnoreCase(FView("Apple"), FView("banana")), 0);
        EXPECT_EQ(Lumina::CompareIgnoreCase(FView("Apple"), FView("APPLE")), 0);
    }

    TEST(StringViewUTF8, DecodesCodepointsAndReplacesBadInput)
    {
        // A, e-acute, euro sign, U+1F600, stray continuation, truncated 2-byte lead before Z, invalid 0xFF lead.
        const FView Text("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\x80\xC3Z\xFF");
        const Lumina::TVector<uint32> Expected = { 'A', 0xE9, 0x20AC, 0x1F600, 0xFFFD, 0xFFFD, 'Z', 0xFFFD };
        Lumina::TVector<uint32> Decoded;
        for (int32 i = 0; i < Text.Len(); )
        {
            Decoded.Add(Lumina::DecodeUTF8Codepoint(Text, i));
        }
        EXPECT_EQ(Decoded, Expected);

        int32 Index = 0;
        EXPECT_EQ(Lumina::DecodeUTF8Codepoint(FView("\xE2\x82"), Index), 0xFFFDu);
        EXPECT_EQ(Index, 2);
    }

    TEST(StringViewInterop, FormatterAndHash)
    {
        EXPECT_EQ(Lumina::Format("[{}]", FView("view")), "[view]");
        EXPECT_EQ(Lumina::Format("{:>6}", FView("ab")), "    ab");

        Lumina::THashSet<FView> Set;
        Set.Emplace("one");
        Set.Emplace("one");
        Set.Emplace("two");
        EXPECT_EQ(Set.Num(), 2);
    }

    TEST(CStringViewBasics, DefaultIsAnEmptyTerminatedString)
    {
        constexpr FCView View;
        static_assert(View.IsEmpty());
        static_assert(View.Len() == 0);
        EXPECT_STREQ(*View, "");
    }

    TEST(CStringViewBasics, FromLiteralKeepsTheTerminator)
    {
        constexpr FCView View("literal");
        static_assert(View.Len() == 7);
        EXPECT_STREQ(*View, "literal");
        EXPECT_EQ((*View)[View.Len()], '\0');
    }

    TEST(CStringViewBasics, FromOwningStringKeepsTheTerminator)
    {
        const FStr Owner("owned value that outgrows the inline buffer");
        const FCView View = Owner.CView();
        EXPECT_EQ(View.Len(), Owner.Len());
        EXPECT_STREQ(*View, *Owner);
        EXPECT_EQ((*View)[View.Len()], '\0');
    }

    TEST(CStringViewBasics, ConvertsToPlainView)
    {
        const FCView Terminated("path/to/asset");
        const FView View = Terminated;
        EXPECT_EQ(View, FView("path/to/asset"));
        EXPECT_TRUE(Terminated.StartsWith(FView("path/")));
        EXPECT_TRUE(Terminated.EndsWith(FView("asset")));
    }

    TEST(CStringViewSlicing, RemovePrefixStaysTerminated)
    {
        FCView View("prefix/tail");
        View.RemovePrefix(7);
        EXPECT_STREQ(*View, "tail");
        EXPECT_EQ(View.Len(), 4);
    }

    TEST(CStringViewInterop, FormatterAndHash)
    {
        EXPECT_EQ(Lumina::Format("[{}]", FCView("cstr")), "[cstr]");

        Lumina::THashSet<FCView> Set;
        Set.Emplace("one");
        Set.Emplace("one");
        EXPECT_EQ(Set.Num(), 1);
    }

    TEST(CStringViewInterop, PassesStraightToACApi)
    {
        const FStr Owner("no copy needed");
        EXPECT_EQ(std::strlen(*Owner.CView()), Owner.Len());
    }
}
