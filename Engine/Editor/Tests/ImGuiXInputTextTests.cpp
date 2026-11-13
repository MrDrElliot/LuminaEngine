#include <gtest/gtest.h>

#include "ImGuiTestHarness.h"
#include "Tools/UI/ImGui/ImGuiX.h"

using namespace Lumina;

// Mirrors the console variable window, which reseeds its edit string from the live value every frame.
TEST(ImGuiXInputText, EnterCommitsTextLongerThanAnyFixedBuffer)
{
    FImGuiTestHarness Harness;
    FString Committed;
    const FString Typed(700, 'x');

    auto Frame = [&]
    {
        FImGuiTestHarness::BeginFrame();
        if (ImGui::GetActiveID() == 0)
        {
            ImGui::SetKeyboardFocusHere();
        }
        FString Edit(Committed);
        if (ImGuiX::InputText("##Val", Edit, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            Committed = Edit;
        }
        FImGuiTestHarness::EndFrame();
    };

    Frame();
    Frame();
    ImGui::GetIO().AddInputCharactersUTF8(*Typed);
    Frame();
    EXPECT_TRUE(Committed.IsEmpty());

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    Frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    Frame();

    EXPECT_EQ(Committed.Len(), Typed.Len());
    EXPECT_EQ(Committed, Typed);
}
