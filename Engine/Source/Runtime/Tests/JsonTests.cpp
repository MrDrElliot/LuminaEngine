#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "Containers/StringFormat.h"
#include "Core/Json/Json.h"

namespace LuminaJsonTests
{
    using Lumina::EJsonParseFlags;
    using Lumina::EJsonType;
    using Lumina::FJsonMember;
    using Lumina::FJsonParseError;
    using Lumina::FJsonValue;
    using Lumina::FString;
    using Lumina::FStringView;
    using Lumina::int32;
    using Lumina::int64;
    using Lumina::uint64;

    FJsonValue ParseOrFail(FStringView Text, EJsonParseFlags Flags = EJsonParseFlags::None)
    {
        FJsonValue Value;
        FJsonParseError Error;
        EXPECT_TRUE(FJsonValue::Parse(Text, Value, &Error, Flags)) << *Error.Message << " in " << *FString(Text);
        return Value;
    }

    bool Accepts(FStringView Text, EJsonParseFlags Flags = EJsonParseFlags::None)
    {
        FJsonValue Value;
        return FJsonValue::Parse(Text, Value, nullptr, Flags);
    }

    FString RoundTrip(FStringView Text)
    {
        return ParseOrFail(Text).ToString();
    }

    TEST(Json, AcceptsWhatRfc8259Allows)
    {
        const char* const Valid[] =
        {
            "null", "true", "false", "0", "-0", "1", "-1", "0.5", "1e5", "1E+5", "1e-5", "-1.25e-10", "123456789012345678",
            "\"\"", "\"a\"", "\"\\u0000\"", "\"\\uD834\\uDD1E\"", "\"\xF0\x9D\x84\x9E\"", "\"\\/\"", "[]", "{}", "[[[]]]",
            "{\"a\":{\"b\":[1,{\"c\":null}]}}", " \t\r\n[ 1 , 2 ]\n ", "{\"\":0}", "[1e308]", "[-1e-400]", "18446744073709551615",
            "-9223372036854775808", "184467440737095516160",
        };
        for (const char* Text : Valid)
        {
            EXPECT_TRUE(Accepts(Text)) << Text;
        }
    }

    TEST(Json, RejectsWhatRfc8259Forbids)
    {
        const char* const Invalid[] =
        {
            "", " ", "nul", "tru", "nulll", "True", "01", "-01", "+1", ".5", "5.", "1e", "1e+", "-", "0x10", "1.0e400", "NaN", "Infinity",
            "\"", "\"abc", "\"\\x\"", "\"\\u12\"", "\"\\uZZZZ\"", "\"\\uD834\"", "\"\\uDD1E\"", "\"\\uD834\\u0041\"", "\"tab\there\"",
            "\"\xFF\"", "\"\xC0\xAF\"", "\"\xED\xA0\x80\"", "\"\xF4\x90\x80\x80\"", "\"\xE2\x82\"",
            "[", "]", "[1,]", "[,1]", "[1 2]", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{a:1}", "{\"a\" 1}", "{1:1}", "[1]]", "{}{}",
            "/* comment */ 1", "1 // comment", "[1,2,3", "\"\\", "\x01",
        };
        for (const char* Text : Invalid)
        {
            EXPECT_FALSE(Accepts(Text)) << "[" << Text << "]";
        }
    }

    TEST(Json, ReportsWhereParsingFailed)
    {
        FJsonValue Value = FJsonValue::MakeObject();
        FJsonParseError Error;
        EXPECT_FALSE(FJsonValue::Parse("{\n  \"a\": 1,\n  \"b\" 2\n}", Value, &Error));
        EXPECT_TRUE(Value.IsNull());
        EXPECT_EQ(Error.Line, 3);
        EXPECT_EQ(Error.Column, 7);
        EXPECT_EQ(Error.Offset, 18);
        EXPECT_NE(Error.Message.Find("expected ':'"), Lumina::Constants::kIndexNone) << *Error.Message;
        EXPECT_NE(Error.Message.Find("line 3, column 7"), Lumina::Constants::kIndexNone) << *Error.Message;
    }

    TEST(Json, OptionalCommentsAndTrailingCommas)
    {
        const char* Text = "// header\n{ /* a */ \"a\": [1, 2,], \"b\": 3, // tail\n}";
        EXPECT_FALSE(Accepts(Text));
        EXPECT_FALSE(Accepts(Text, EJsonParseFlags::AllowComments));
        const FJsonValue Value = ParseOrFail(Text, EJsonParseFlags::AllowComments | EJsonParseFlags::AllowTrailingCommas);
        EXPECT_EQ(Value.ToString(), "{\"a\":[1,2],\"b\":3}");
        EXPECT_FALSE(Accepts("/* never closed", EJsonParseFlags::AllowComments));
        EXPECT_FALSE(Accepts("[1,,2]", EJsonParseFlags::AllowTrailingCommas));
    }

    TEST(Json, KeepsIntegerWidthsAndKinds)
    {
        EXPECT_EQ(ParseOrFail("18446744073709551615").GetType(), EJsonType::UInt);
        EXPECT_EQ(ParseOrFail("18446744073709551615").AsUInt64(), std::numeric_limits<uint64>::max());
        EXPECT_EQ(ParseOrFail("-9223372036854775808").AsInt64(), std::numeric_limits<int64>::min());
        EXPECT_EQ(ParseOrFail("-9223372036854775808").GetType(), EJsonType::Int);
        EXPECT_EQ(ParseOrFail("7").GetType(), EJsonType::UInt);
        EXPECT_EQ(ParseOrFail("-7").GetType(), EJsonType::Int);
        EXPECT_EQ(ParseOrFail("18446744073709551616").GetType(), EJsonType::Double);
        EXPECT_EQ(ParseOrFail("-9223372036854775809").GetType(), EJsonType::Double);
        EXPECT_EQ(ParseOrFail("1e-400").AsDouble(), 0.0);
        EXPECT_TRUE(std::signbit(ParseOrFail("-1e-400").AsDouble()));
        EXPECT_EQ(ParseOrFail("-0").AsInt64(), 0);
        EXPECT_EQ(ParseOrFail("2.5").AsInt64(), 2);
        EXPECT_EQ(ParseOrFail("1e300").AsInt64(), std::numeric_limits<int64>::max());
        EXPECT_EQ(ParseOrFail("-3.5").AsUInt64(), 0u);
        EXPECT_FLOAT_EQ(ParseOrFail("0.1").AsFloat(), 0.1f);
    }

    TEST(Json, WritesShortestRoundTripNumbers)
    {
        EXPECT_EQ(FJsonValue(3.0).ToString(), "3.0");
        EXPECT_EQ(FJsonValue(-0.0).ToString(), "-0.0");
        EXPECT_EQ(FJsonValue(0.0).ToString(), "0.0");
        EXPECT_EQ(FJsonValue(0.1).ToString(), "0.1");
        EXPECT_EQ(FJsonValue(0.0001).ToString(), "0.0001");
        EXPECT_EQ(FJsonValue(0.00001).ToString(), "1e-05");
        EXPECT_EQ(FJsonValue(1e14).ToString(), "100000000000000.0");
        EXPECT_EQ(FJsonValue(1e15).ToString(), "1e+15");
        EXPECT_EQ(FJsonValue(1e16).ToString(), "1e+16");
        EXPECT_EQ(FJsonValue(123456.789).ToString(), "123456.789");
        EXPECT_EQ(FJsonValue(2.4e11).ToString(), "240000000000.0");
        EXPECT_EQ(FJsonValue(1.5e300).ToString(), "1.5e+300");
        EXPECT_EQ(FJsonValue(5e-324).ToString(), "5e-324");
        EXPECT_EQ(FJsonValue(0.1f).ToString(), "0.10000000149011612");
        EXPECT_EQ(FJsonValue(std::numeric_limits<double>::infinity()).ToString(), "null");
        EXPECT_EQ(FJsonValue(std::numeric_limits<double>::quiet_NaN()).ToString(), "null");
        EXPECT_EQ(FJsonValue(std::numeric_limits<int64>::min()).ToString(), "-9223372036854775808");
        EXPECT_EQ(FJsonValue(std::numeric_limits<uint64>::max()).ToString(), "18446744073709551615");

        for (const double Value : { 0.1, 1.0 / 3.0, 2.0 / 3.0, 1e-7, 6.02214076e23, 1.7976931348623157e308, 2.2250738585072014e-308 })
        {
            EXPECT_EQ(ParseOrFail(FJsonValue(Value).ToString().View()).AsDouble(), Value);
        }
    }

    TEST(Json, EscapesAndDecodesStrings)
    {
        EXPECT_EQ(RoundTrip("\"\\u0041\\u00e9\\u20ac\\ud83d\\ude00\""), "\"A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"");
        EXPECT_EQ(RoundTrip("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\""), "\"\\\"\\\\/\\b\\f\\n\\r\\t\"");
        EXPECT_EQ(RoundTrip("\"\\u0001\\u001f\\u007f\""), "\"\\u0001\\u001f\x7F\"");
        EXPECT_EQ(FJsonValue(FStringView("a\0b", 3)).ToString(), "\"a\\u0000b\"");
        EXPECT_EQ(FJsonValue(FStringView("bad \xFF byte \xC3")).ToString(), "\"bad \xEF\xBF\xBD byte \xEF\xBF\xBD\"");
        EXPECT_EQ(ParseOrFail("\"\\u0000\"").AsString().Len(), 1);

        const FString Long = "a string long enough to leave the inline buffer, with \"quotes\" and \\ in it";
        const FJsonValue Value(Long);
        EXPECT_EQ(ParseOrFail(Value.ToString().View()).AsString(), Long.View());
        EXPECT_STREQ(ParseOrFail("\"short\"").AsCString(), "short");
        EXPECT_STREQ(Value.AsCString(), *Long);
    }

    TEST(Json, PrettyPrintsWithIndentedMembers)
    {
        const FJsonValue Value = ParseOrFail("{\"a\":[1,{\"b\":null}],\"c\":{},\"d\":[]}");
        EXPECT_EQ(Value.ToString(2), "{\n  \"a\": [\n    1,\n    {\n      \"b\": null\n    }\n  ],\n  \"c\": {},\n  \"d\": []\n}");
        EXPECT_EQ(Value.ToString(0), "{\n\"a\": [\n1,\n{\n\"b\": null\n}\n],\n\"c\": {},\n\"d\": []\n}");
        EXPECT_EQ(Value.ToString(), "{\"a\":[1,{\"b\":null}],\"c\":{},\"d\":[]}");
    }

    TEST(Json, KeepsInsertionOrderAndTheLastOfARepeatedKey)
    {
        EXPECT_EQ(RoundTrip("{\"z\":1,\"a\":2,\"m\":3}"), "{\"z\":1,\"a\":2,\"m\":3}");
        EXPECT_EQ(RoundTrip("{\"a\":1,\"b\":2,\"a\":3}"), "{\"a\":3,\"b\":2}");

        FString Wide = "{";
        for (int32 Index = 0; Index < 40; ++Index)
        {
            Wide += Lumina::Format("\"k{}\":{},", Index, Index);
        }
        Wide += "\"k3\":\"again\",\"k39\":\"last\"}";
        const FJsonValue Value = ParseOrFail(Wide.View());
        EXPECT_EQ(Value.Num(), 40);
        EXPECT_EQ(Value["k3"].AsString(), "again");
        EXPECT_EQ(Value["k39"].AsString(), "last");
        EXPECT_EQ(Value.Members()[3].Key, "k3");
        EXPECT_EQ(Value.Members()[39].Key, "k39");
        for (int32 Index = 0; Index < 40; ++Index)
        {
            if (Index != 3 && Index != 39)
            {
                EXPECT_EQ(Value[*Lumina::Format("k{}", Index)].AsInt32(), Index);
            }
        }
    }

    TEST(Json, BuildsDocumentsWithoutInsertingOnRead)
    {
        FJsonValue Root;
        Root.Set("name", "Lumina");
        Root.Set("version", 168);
        Root.FindOrAdd("tags").Add("engine");
        Root.FindOrAdd("tags").Add(FJsonValue::MakeArray({ 1, 2.5, true, nullptr }));
        Root.FindOrAdd("nested").FindOrAdd("deep").Set("value", -1);
        EXPECT_EQ(Root.ToString(), "{\"name\":\"Lumina\",\"version\":168,\"tags\":[\"engine\",[1,2.5,true,null]],\"nested\":{\"deep\":{\"value\":-1}}}");

        const FJsonValue& Reader = Root;
        EXPECT_TRUE(Reader["missing"]["deeper"][3].IsNull());
        EXPECT_FALSE(Root.Contains("missing"));
        EXPECT_EQ(Reader["tags"][1][1].AsDouble(), 2.5);
        EXPECT_EQ(Reader["tags"][7].GetType(), EJsonType::Null);

        Root.Set("version", "replaced");
        EXPECT_EQ(Root.Num(), 4);
        EXPECT_EQ(Root["version"].AsString(), "replaced");

        const FJsonValue Made = FJsonValue::MakeObject({ { "a", 1 }, { "b", "two" }, { "a", 3 } });
        EXPECT_EQ(Made.ToString(), "{\"a\":3,\"b\":\"two\"}");
    }

    TEST(Json, TypedAccessReturnsDefaultsInsteadOfThrowing)
    {
        const FJsonValue Value = ParseOrFail("{\"n\":5,\"s\":\"text\",\"b\":true,\"f\":1.5,\"neg\":-2}");
        EXPECT_EQ(Value.GetValue("n", 0), 5);
        EXPECT_EQ(Value.GetValue("missing", 42), 42);
        EXPECT_EQ(Value.GetValue("s", 7), 7);
        EXPECT_EQ(Value.GetValue("b", false), true);
        EXPECT_EQ(Value.GetValue("n", false), false);
        EXPECT_FLOAT_EQ(Value.GetValue("f", 0.0f), 1.5f);
        EXPECT_EQ(Value.GetValue("neg", int64(0)), -2);
        EXPECT_EQ(Value.GetString("s"), "text");
        EXPECT_EQ(Value.GetString("n", "fallback"), "fallback");
        EXPECT_EQ(Value["s"].AsInt32(9), 9);
        EXPECT_EQ(Value["n"].AsString("none"), "none");
        EXPECT_FALSE(Value["n"].AsBool());

        FString Text;
        EXPECT_TRUE(Value["s"].TryGet(Text));
        EXPECT_EQ(Text, "text");
        int32 Number = 0;
        EXPECT_FALSE(Value["s"].TryGet(Number));
        EXPECT_TRUE(Value["f"].TryGet(Number));
        EXPECT_EQ(Number, 1);
    }

    TEST(Json, CopiesDeepAndSurvivesAliasing)
    {
        FJsonValue Original = ParseOrFail("{\"a\":{\"b\":[1,2,3]},\"long-key-past-inline\":\"long value past the inline buffer\"}");
        FJsonValue Copy = Original;
        Copy.FindOrAdd("a").FindOrAdd("b").Add(4);
        EXPECT_EQ(Original["a"]["b"].Num(), 3);
        EXPECT_EQ(Copy["a"]["b"].Num(), 4);
        EXPECT_FALSE(Copy == Original);

        FJsonValue Self = Original;
        Self = Self["a"];
        EXPECT_EQ(Self.ToString(), "{\"b\":[1,2,3]}");

        FJsonValue Moved = Original;
        Moved = std::move(*Moved.Find("a"));
        EXPECT_EQ(Moved.ToString(), "{\"b\":[1,2,3]}");

        FJsonValue Keys = ParseOrFail("{\"first\":1}");
        Keys.Set(Keys.Members()[0].Key.View(), 2);
        EXPECT_EQ(Keys.ToString(), "{\"first\":2}");

        FJsonValue Grow = FJsonValue::MakeArray();
        Grow.Add("seed long enough to live on the heap for this test");
        for (int32 Index = 0; Index < 100; ++Index)
        {
            Grow.Add(Grow[0]);
        }
        EXPECT_EQ(Grow[100].AsString(), Grow[0].AsString());

        const FJsonValue Moving = std::move(Copy);
        EXPECT_TRUE(Copy.IsNull());
        EXPECT_EQ(Moving["a"]["b"].Num(), 4);
    }

    TEST(Json, RemovesAndKeepsLookupsWorking)
    {
        FJsonValue Object = FJsonValue::MakeObject();
        for (int32 Index = 0; Index < 30; ++Index)
        {
            Object.Set(*Lumina::Format("key{}", Index), Index);
        }
        EXPECT_TRUE(Object.Remove("key4"));
        EXPECT_FALSE(Object.Remove("key4"));
        Object.RemoveAt(0);
        EXPECT_EQ(Object.Num(), 28);
        EXPECT_FALSE(Object.Contains("key0"));
        EXPECT_EQ(Object.Members()[0].Key, "key1");
        for (int32 Index = 1; Index < 30; ++Index)
        {
            EXPECT_EQ(Object.Contains(*Lumina::Format("key{}", Index)), Index != 4) << Index;
        }

        FJsonValue Array = ParseOrFail("[0,1,2,3]");
        Array.RemoveAt(1);
        EXPECT_EQ(Array.ToString(), "[0,2,3]");
    }

    TEST(Json, MergePatchFollowsRfc7386)
    {
        const char* const Cases[][3] =
        {
            { "{\"a\":\"b\"}", "{\"a\":\"c\"}", "{\"a\":\"c\"}" },
            { "{\"a\":\"b\"}", "{\"b\":\"c\"}", "{\"a\":\"b\",\"b\":\"c\"}" },
            { "{\"a\":\"b\"}", "{\"a\":null}", "{}" },
            { "{\"a\":\"b\",\"b\":\"c\"}", "{\"a\":null}", "{\"b\":\"c\"}" },
            { "{\"a\":[\"b\"]}", "{\"a\":\"c\"}", "{\"a\":\"c\"}" },
            { "{\"a\":{\"b\":\"c\"}}", "{\"a\":{\"b\":\"d\",\"c\":null}}", "{\"a\":{\"b\":\"d\"}}" },
            { "[1,2]", "{\"a\":\"b\",\"c\":null}", "{\"a\":\"b\"}" },
            { "{\"e\":null}", "{\"a\":1}", "{\"e\":null,\"a\":1}" },
            { "{}", "{\"a\":{\"bb\":{\"ccc\":null}}}", "{\"a\":{\"bb\":{}}}" },
        };
        for (const auto& Case : Cases)
        {
            FJsonValue Target = ParseOrFail(Case[0]);
            Target.MergePatch(ParseOrFail(Case[1]));
            EXPECT_EQ(Target.ToString(), Case[2]) << Case[0] << " + " << Case[1];
        }
    }

    TEST(Json, ComparesByValue)
    {
        EXPECT_TRUE(ParseOrFail("1") == ParseOrFail("1.0"));
        EXPECT_TRUE(ParseOrFail("-1") == FJsonValue(-1.0));
        EXPECT_FALSE(ParseOrFail("-1") == ParseOrFail("18446744073709551615"));
        EXPECT_TRUE(ParseOrFail("{\"a\":1,\"b\":[true]}") == ParseOrFail("{\"b\":[true],\"a\":1}"));
        EXPECT_FALSE(ParseOrFail("[1,2]") == ParseOrFail("[2,1]"));
        EXPECT_FALSE(ParseOrFail("\"1\"") == ParseOrFail("1"));
        EXPECT_TRUE(FJsonValue() == ParseOrFail("null"));
    }

    TEST(Json, BoundsNestingDepth)
    {
        FString Deep;
        for (int32 Index = 0; Index < 512; ++Index)
        {
            Deep += "[";
        }
        for (int32 Index = 0; Index < 512; ++Index)
        {
            Deep += "]";
        }
        EXPECT_TRUE(Accepts(Deep.View()));
        EXPECT_FALSE(Accepts(("[" + Deep + "]").View()));

        FString Hostile;
        for (int32 Index = 0; Index < 100000; ++Index)
        {
            Hostile += "{\"a\":";
        }
        EXPECT_FALSE(Accepts(Hostile.View()));
    }

    TEST(Json, EmptyContainersStayDistinctFromNull)
    {
        FJsonValue Array = FJsonValue::MakeArray();
        FJsonValue Object = FJsonValue::MakeObject();
        EXPECT_TRUE(Array.IsArray());
        EXPECT_TRUE(Object.IsObject());
        EXPECT_EQ(Array.ToString(), "[]");
        EXPECT_EQ(Object.ToString(), "{}");
        EXPECT_FALSE(Array == Object);
        EXPECT_TRUE(Array.AsArray().Num() == 0 && Object.Members().Num() == 0);
        EXPECT_FALSE(Object.Remove("x"));
        EXPECT_EQ(FJsonValue(Array).ToString(), "[]");
        EXPECT_EQ(ParseOrFail("{\"a\":[],\"b\":{}}").ToString(), "{\"a\":[],\"b\":{}}");
    }
}
