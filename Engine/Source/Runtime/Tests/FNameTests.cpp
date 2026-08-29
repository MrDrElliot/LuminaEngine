#include <gtest/gtest.h>
#include "Containers/Name.h"

using namespace Lumina;

TEST(FNameTests, NameNone)
{
    EXPECT_TRUE(FName{}.IsNone());
    EXPECT_TRUE(FName(NAME_None).IsNone());
}

TEST(FNameTests, ConstructFromCString)
{
    FName a("Test");
    FName b("Test");

    EXPECT_EQ(a, b);
    EXPECT_FALSE(a.IsNone());
    EXPECT_NE(a.GetID(), 0u);
}

TEST(FNameTests, SameStringProducesSameID)
{
    FName a("Engine");
    FName b("Engine");

    EXPECT_EQ(a.GetID(), b.GetID());
    EXPECT_EQ(a, b);
}

TEST(FNameTests, DifferentStringsProduceDifferentIDs)
{
    FName a("Engine");
    FName b("Renderer");

    EXPECT_NE(a.GetID(), b.GetID());
    EXPECT_NE(a, b);
}

TEST(FNameTests, ConstructFromStringView)
{
    FStringView view("Physics");
    FName a(view);

    EXPECT_FALSE(a.IsNone());
    EXPECT_NE(a.GetID(), 0u);
    EXPECT_EQ(a.ToString(), "Physics");
}

TEST(FNameTests, ConstructFromStdString)
{
    FString str = "Audio";
    FName a(str);

    EXPECT_EQ(a.ToString(), "Audio");
}

TEST(FNameTests, ConstructFromWideString)
{
    FWString wstr = StringUtils::ToWideString("Render");
    FName a(wstr);

    EXPECT_EQ(a.ToString(), "Render");
}

TEST(FNameTests, ConstructFromTCHAR)
{
    const TCHAR* w = StringUtils::ToWideString("Input").c_str();
    FName a(w);

    EXPECT_EQ(a.ToString(), "Input");
}

TEST(FNameTests, CopyConstructorAndAssignment)
{
    FName a("Core");
    FName b = a;
    FName c;
    c = a;

    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c);
}

TEST(FNameTests, EqualityOperators)
{
    FName a("Mesh");
    FName b("Mesh");
    FName c("Material");

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
    EXPECT_TRUE(a != c);
}

TEST(FNameTests, ComparisonOperators)
{
    FName a("A");
    FName b("B");

    EXPECT_TRUE((a < b) || (a > b));
    EXPECT_NE(a, b);
}

TEST(FNameTests, EnumConversion)
{
    FName a(NAME_None);

    EXPECT_TRUE(a == NAME_None);
    EXPECT_TRUE(a.IsNone());
}

TEST(FNameTests, UInt64Constructor)
{
    FName a("Test");
    uint64 id = a.GetID();

    FName b(id);

    EXPECT_EQ(a, b);
}

TEST(FNameTests, DereferenceOperatorReturnsString)
{
    FName a("Shader");

    const char* str = *a;

    EXPECT_NE(str, nullptr);
    EXPECT_STREQ(str, "Shader");
}

TEST(FNameTests, LengthConsistency)
{
    FName a("Transform");

    EXPECT_EQ(a.Length(), strlen(a.c_str()));
    EXPECT_EQ(a.length(), a.Length());
}

TEST(FNameTests, HashStability)
{
    FName a("StableName");
    FName b("StableName");

    EXPECT_EQ(a.Hash(), b.Hash());
}

TEST(FNameTests, AtAccessValid)
{
    FName a("ABC");

    EXPECT_EQ(a.At(0), 'A');
    EXPECT_EQ(a.At(1), 'B');
    EXPECT_EQ(a.At(2), 'C');
}

TEST(FNameTests, ToStringRoundTrip)
{
    FName a("RoundTrip");

    FString s = a.ToString();
    FName b(s);

    EXPECT_EQ(a, b);
}

TEST(FNameTests, ExplicitNumberConstructor)
{
    FName a("Entity", 3);

    EXPECT_TRUE(a.HasNumber());
    EXPECT_EQ(a.GetNumber(), 3u);
    EXPECT_EQ(a.ToString(), "Entity_3");
}

TEST(FNameTests, ExplicitNumberZero)
{
    FName a("Entity", 0);

    EXPECT_TRUE(a.HasNumber());
    EXPECT_EQ(a.GetNumber(), 0u);
    EXPECT_EQ(a.ToString(), "Entity_0");
}

TEST(FNameTests, ParsesTrailingNumber)
{
    FName a("Cube_42");

    EXPECT_TRUE(a.HasNumber());
    EXPECT_EQ(a.GetNumber(), 42u);
    EXPECT_EQ(a.ToString(), "Cube_42");
    EXPECT_EQ(a, FName("Cube", 42));
}

TEST(FNameTests, NumberedNamesShareBase)
{
    FName a("Cube_1");
    FName b("Cube_2");
    FName base("Cube");

    // Same pooled base string, distinct numbers.
    EXPECT_EQ(a.GetID(), b.GetID());
    EXPECT_EQ(a.GetID(), base.GetID());
    EXPECT_NE(a, b);
    EXPECT_EQ(a.GetBaseName(), base);
}

TEST(FNameTests, LeadingZeroNotTreatedAsNumber)
{
    FName a("Item_05");

    EXPECT_FALSE(a.HasNumber());
    EXPECT_EQ(a.ToString(), "Item_05");
}

TEST(FNameTests, TrailingZeroIsNumber)
{
    FName a("Item_0");

    EXPECT_TRUE(a.HasNumber());
    EXPECT_EQ(a.GetNumber(), 0u);
    EXPECT_EQ(a.ToString(), "Item_0");
}

TEST(FNameTests, UnderscoreWithoutDigitsIsNotNumber)
{
    FName a("My_Name");

    EXPECT_FALSE(a.HasNumber());
    EXPECT_EQ(a.ToString(), "My_Name");
}

TEST(FNameTests, NumberedCStrIncludesSuffix)
{
    FName a("Light", 7);

    EXPECT_STREQ(a.c_str(), "Light_7");
    EXPECT_EQ(a.Length(), strlen("Light_7"));
}

TEST(FNameTests, NumberedRoundTrip)
{
    FName a("Spawn_128");

    FName b(a.ToString());
    EXPECT_EQ(a, b);
}

TEST(FNameTests, CaseInsensitiveEquality)
{
    FName a("Hello");
    FName b("HELLO");
    FName c("hello");

    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c);
    EXPECT_EQ(a.GetID(), b.GetID());
}

TEST(FNameTests, DisplayPreservesFirstSeenCasing)
{
    // Unique base so this test is the first to intern it process-wide.
    FName first("ZxMixedCaseName");
    FName second("ZXMIXEDCASENAME");

    EXPECT_EQ(first, second);
    EXPECT_EQ(second.ToString(), "ZxMixedCaseName");
}

TEST(FNameTests, CaseInsensitiveWithNumber)
{
    FName a("Cube_5");
    FName b("CUBE_5");

    EXPECT_EQ(a, b);
    EXPECT_EQ(a.GetNumber(), 5u);
}
