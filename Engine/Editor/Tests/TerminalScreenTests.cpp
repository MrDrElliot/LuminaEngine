#include <cstring>

#include <gtest/gtest.h>

#include "UI/Tools/Terminal/TerminalScreen.h"

using namespace Lumina;

TEST(TerminalScreen, ResetClearsTitleSetByOsc)
{
    FTerminalScreen Screen;
    Screen.Initialize(80, 24, 1000);

    const char* Osc = "\x1b]0;remote-host\x07";
    Screen.Write(reinterpret_cast<const uint8*>(Osc), static_cast<int32>(strlen(Osc)));
    ASSERT_EQ(Screen.GetTitle(), "remote-host");

    Screen.Reset();
    EXPECT_TRUE(Screen.GetTitle().IsEmpty());

    Screen.Write(reinterpret_cast<const uint8*>(Osc), static_cast<int32>(strlen(Osc)));
    ASSERT_EQ(Screen.GetTitle(), "remote-host");

    Screen.Initialize(80, 24, 1000);
    EXPECT_TRUE(Screen.GetTitle().IsEmpty());
}
