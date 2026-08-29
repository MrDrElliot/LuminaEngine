#include <gtest/gtest.h>

#include <format>
#include <string>

#include "Containers/StringFormat.h"
#include "Containers/HashTable.h"
#include "Containers/String.h"

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
        static_assert(View.empty());
        static_assert(View.size() == 0);
        EXPECT_EQ(View.data(), nullptr);
    }

    TEST(StringViewBasics, ConstructFromLiteralAndRange)
    {
        constexpr FView FromLiteral("hello");
        static_assert(FromLiteral.size() == 5);
        EXPECT_EQ(FromLiteral[0], 'h');
        EXPECT_EQ(FromLiteral.front(), 'h');
        EXPECT_EQ(FromLiteral.back(), 'o');

        const FView FromRange("hello world", 5);
        EXPECT_EQ(FromRange, FromLiteral);
    }

    TEST(StringViewBasics, ConvertsFromOwningString)
    {
        const FStr Owner("borrowed");
        const FView View = Owner;
        EXPECT_EQ(View.size(), Owner.size());
        EXPECT_EQ(View.data(), Owner.data());
    }

    TEST(StringViewBasics, ConvertsToStdView)
    {
        const FView View("interop");
        const std::string_view Std = View;
        EXPECT_EQ(Std, "interop");

        const FView Back(std::string_view("round trip"));
        EXPECT_EQ(Back.size(), 10u);
    }

    TEST(StringViewSlicing, SubstrLeftRight)
    {
        constexpr FView View("abcdef");
        static_assert(View.substr(2) == FView("cdef"));
        static_assert(View.substr(1, 3) == FView("bcd"));
        static_assert(View.substr(2, 100) == FView("cdef"));
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
        static_assert(View.find(FView("quick")) == 4);
        static_assert(View.find(FView("missing")) == FView::npos);
        static_assert(View.find('q') == 4);
        static_assert(View.find('q', 5) == FView::npos);
        static_assert(View.rfind('o') == 17);
        static_assert(View.find(FView("")) == 0);

        const std::string_view Reference("the quick brown fox");
        EXPECT_EQ(View.find(FView("o")), Reference.find("o"));
        EXPECT_EQ(View.rfind(FView("o")), Reference.rfind("o"));
        EXPECT_EQ(View.find_first_of(FView("xq")), Reference.find_first_of("xq"));
        EXPECT_EQ(View.find_last_of(FView("xq")), Reference.find_last_of("xq"));
        EXPECT_EQ(View.find_first_not_of(FView("teh ")), Reference.find_first_not_of("teh "));
        EXPECT_EQ(View.find_last_not_of(FView("xof ")), Reference.find_last_not_of("xof "));
    }

    TEST(StringViewSearch, SearchOnEmptyNeverReadsPastTheEnd)
    {
        constexpr FView Empty;
        static_assert(Empty.find('a') == FView::npos);
        static_assert(Empty.rfind('a') == FView::npos);
        static_assert(Empty.find_last_of(FView("abc")) == FView::npos);
        static_assert(Empty.find_last_not_of(FView("abc")) == FView::npos);
        static_assert(!Empty.starts_with('a'));
        static_assert(!Empty.ends_with('a'));
        SUCCEED();
    }

    TEST(StringViewCompare, OrderingAndPredicates)
    {
        constexpr FView Path("Engine/Source/Runtime");
        static_assert(Path.starts_with(FView("Engine/")));
        static_assert(Path.ends_with(FView("Runtime")));
        static_assert(Path.contains(FView("Source")));
        static_assert(!Path.contains(FView("Editor")));

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

    TEST(StringViewInterop, FormatterAndHash)
    {
        EXPECT_EQ(Lumina::Format("[{}]", FView("view")), "[view]");
        EXPECT_EQ(Lumina::Format("{:>6}", FView("ab")), "    ab");

        Lumina::THashSet<FView> Set;
        Set.insert(FView("one"));
        Set.insert(FView("one"));
        Set.insert(FView("two"));
        EXPECT_EQ(Set.size(), 2u);
    }

    TEST(CStringViewBasics, DefaultIsAnEmptyTerminatedString)
    {
        constexpr FCView View;
        static_assert(View.empty());
        static_assert(View.size() == 0);
        EXPECT_STREQ(View.c_str(), "");
    }

    TEST(CStringViewBasics, FromLiteralKeepsTheTerminator)
    {
        constexpr FCView View("literal");
        static_assert(View.size() == 7);
        EXPECT_STREQ(View.c_str(), "literal");
        EXPECT_EQ(View.c_str()[View.size()], '\0');
    }

    TEST(CStringViewBasics, FromOwningStringKeepsTheTerminator)
    {
        const FStr Owner("owned value that outgrows the inline buffer");
        const FCView View = Owner.CView();
        EXPECT_EQ(View.size(), Owner.size());
        EXPECT_STREQ(View.c_str(), Owner.c_str());
        EXPECT_EQ(View.c_str()[View.size()], '\0');
    }

    TEST(CStringViewBasics, ConvertsToPlainView)
    {
        const FCView Terminated("path/to/asset");
        const FView View = Terminated;
        EXPECT_EQ(View, FView("path/to/asset"));
        EXPECT_TRUE(Terminated.starts_with(FView("path/")));
        EXPECT_TRUE(Terminated.ends_with(FView("asset")));
    }

    TEST(CStringViewSlicing, RemovePrefixStaysTerminated)
    {
        FCView View("prefix/tail");
        View.RemovePrefix(7);
        EXPECT_STREQ(View.c_str(), "tail");
        EXPECT_EQ(View.size(), 4u);
    }

    TEST(CStringViewInterop, FormatterAndHash)
    {
        EXPECT_EQ(Lumina::Format("[{}]", FCView("cstr")), "[cstr]");

        Lumina::THashSet<FCView> Set;
        Set.insert(FCView("one"));
        Set.insert(FCView("one"));
        EXPECT_EQ(Set.size(), 1u);
    }

    TEST(CStringViewInterop, PassesStraightToACApi)
    {
        const FStr Owner("no copy needed");
        EXPECT_EQ(std::strlen(Owner.CView().c_str()), Owner.size());
    }
}
