#include <gtest/gtest.h>

#include "Assets/AssetTypes/SpriteSheet/SpriteSheet.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/PropertyText.h"
#include "Input/InputAction.h"

using namespace Lumina;

namespace
{
    FProperty* FramesProperty()
    {
        return SSpriteAnimation::StaticStruct()->GetProperty(FName("Frames"));
    }

    FProperty* ActionsProperty()
    {
        return SInputMappingContext::StaticStruct()->GetProperty(FName("Actions"));
    }
}

TEST(PropertyTextArray, IntArrayRoundTrips)
{
    FProperty* Frames = FramesProperty();
    ASSERT_NE(Frames, nullptr);
    ASSERT_TRUE(Reflection::IsTextConvertible(Frames));

    SSpriteAnimation Clip;
    Clip.Frames = {3, -1, 42};
    EXPECT_EQ(Reflection::ToText(Frames, &Clip), "(3,-1,42)");

    SSpriteAnimation Parsed;
    Parsed.Frames = {100, 200, 300, 400};
    ASSERT_TRUE(Reflection::FromText(Frames, &Parsed, " ( 7 , 8,9 ) "));
    EXPECT_EQ(Parsed.Frames, (TVector<int32>{7, 8, 9}));

    ASSERT_TRUE(Reflection::FromText(Frames, &Parsed, "()"));
    EXPECT_TRUE(Parsed.Frames.empty());
    EXPECT_EQ(Reflection::ToText(Frames, &Parsed), "()");
}

TEST(PropertyTextArray, BadElementLeavesValueUntouched)
{
    FProperty* Frames = FramesProperty();
    ASSERT_NE(Frames, nullptr);

    SSpriteAnimation Clip;
    Clip.Frames = {1, 2};
    EXPECT_FALSE(Reflection::FromText(Frames, &Clip, "(4,five,6)"));
    EXPECT_FALSE(Reflection::FromText(Frames, &Clip, "4,5"));
    EXPECT_EQ(Clip.Frames, (TVector<int32>{1, 2}));
}

TEST(PropertyTextArray, TextElementsQuoteWhatWouldNotReadBack)
{
    FProperty* Actions = ActionsProperty();
    ASSERT_NE(Actions, nullptr);
    ASSERT_TRUE(Reflection::IsTextConvertible(Actions));

    SInputMappingContext Context;
    Context.Actions = {FName("Plain"), FName("Has,Comma"), FName("Say \"hi\""), FName(" padded "), FName("Paren)")};
    const FString Text = Reflection::ToText(Actions, &Context);
    EXPECT_EQ(Text, "(Plain,\"Has,Comma\",\"Say \"\"hi\"\"\",\" padded \",\"Paren)\")");

    SInputMappingContext Parsed;
    ASSERT_TRUE(Reflection::FromText(Actions, &Parsed, FStringView(Text.c_str(), Text.size())));
    EXPECT_EQ(Parsed.Actions, Context.Actions);
}

TEST(PropertyTextArray, UnclosedQuoteFails)
{
    FProperty* Actions = ActionsProperty();
    ASSERT_NE(Actions, nullptr);

    SInputMappingContext Context;
    EXPECT_FALSE(Reflection::FromText(Actions, &Context, "(\"open,b)"));
    EXPECT_FALSE(Reflection::FromText(Actions, &Context, "(\"a\"x,b)"));
    EXPECT_TRUE(Context.Actions.empty());
}
