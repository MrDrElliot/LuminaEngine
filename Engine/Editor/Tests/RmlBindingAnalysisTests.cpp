#include <gtest/gtest.h>

#include "UI/Tools/AssetEditors/RmlUiEditor/RmlBindingAnalysis.h"

using namespace Lumina;

TEST(RmlBindingAnalysis, ModelEnclosingFindsTheInnermostDataModel)
{
    const std::string_view Text =
        "<body data-model=\"Outer\">"
        "<span data-text=\"Score\"/>"
        "<div data-model=\"Inner\"><p data-if=\"Ready\">{{ Name }}</p></div>"
        "<em data-text=\"Lives\"></em>"
        "</body><footer/>";

    EXPECT_EQ(RmlBinding::ModelEnclosing(Text, Text.find("<span")), "Outer");
    EXPECT_EQ(RmlBinding::ModelEnclosing(Text, Text.find("<div")), "Inner");
    EXPECT_EQ(RmlBinding::ModelEnclosing(Text, Text.find("<p")), "Inner");
    EXPECT_EQ(RmlBinding::ModelEnclosing(Text, Text.find("Name")), "Inner");
    EXPECT_EQ(RmlBinding::ModelEnclosing(Text, Text.find("<em")), "Outer");
    EXPECT_EQ(RmlBinding::ModelEnclosing(Text, Text.find("<footer")), "");
}
