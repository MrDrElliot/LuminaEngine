#include <gtest/gtest.h>

#include "ImGuiTestHarness.h"
#include "AI/StateTree/StateTreeTestTypes.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/Customizations/CoreTypeCustomization.h"

using namespace Lumina;

namespace
{
    // Also reports the combo's screen rect and open state so a test can drive the popup from outside.
    EPropertyChangeOp DrawFrame(FEnumPropertyCustomization& Customization, const TSharedPtr<FPropertyHandle>& Handle, ImVec2* OutComboMin = nullptr, ImVec2* OutComboMax = nullptr, bool* OutComboOpen = nullptr)
    {
        FImGuiTestHarness::BeginFrame();
        const FPropertyDrawArgs Args;
        EPropertyChangeOp Result = Customization.UpdateAndDraw(Handle, Args);
        if (OutComboMin != nullptr) { *OutComboMin = ImGui::GetItemRectMin(); }
        if (OutComboMax != nullptr) { *OutComboMax = ImGui::GetItemRectMax(); }
        if (OutComboOpen != nullptr)
        {
            // Mirrors UpdateAndDraw's own PushID(Property.get()) so the hashed popup id matches.
            ImGui::PushID(Handle.get());
            *OutComboOpen = ImGuiX::IsSearchableComboOpen("##enum");
            ImGui::PopID();
        }
        FImGuiTestHarness::EndFrame();
        return Result;
    }

    void MouseDown(ImVec2 Pos)
    {
        ImGui::GetIO().AddMousePosEvent(Pos.x, Pos.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    }

    void MouseUp()
    {
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    }
}

// Re-selecting the already-active enum value must report no change, while a real change still commits.
TEST(EnumCustomization, RepickingTheCurrentValueReportsNoChangeButADifferentValueStillCommits)
{
    FImGuiTestHarness Harness;
    RegisterPendingTypes();

    SStateTreeTestTask Task;
    Task.Result = EStateTreeRunStatus::Succeeded;
    FProperty* Property = SStateTreeTestTask::StaticStruct()->GetProperty("Result");
    ASSERT_NE(Property, nullptr);
    TSharedPtr<FPropertyHandle> Handle = MakeShared<FPropertyHandle>(&Task, Property);

    FEnumPropertyCustomization Customization;

    ImGui::GetIO().AddMousePosEvent(-100.0f, -100.0f);
    ImVec2 ComboMin, ComboMax;
    // ImGui needs a couple of settled frames before mouse interaction is reliable in a fresh context.
    DrawFrame(Customization, Handle);
    EXPECT_EQ(DrawFrame(Customization, Handle, &ComboMin, &ComboMax), EPropertyChangeOp::None);
    const ImVec2 ComboCenter((ComboMin.x + ComboMax.x) * 0.5f, (ComboMin.y + ComboMax.y) * 0.5f);

    // The list is drawn unfiltered, so ClickFirstRow below always lands on its topmost entry, Running.
    auto Open = [&]
    {
        MouseDown(ComboCenter);
        DrawFrame(Customization, Handle);
        MouseUp();
        bool bOpen = false;
        DrawFrame(Customization, Handle, nullptr, nullptr, &bOpen);
        ASSERT_TRUE(bOpen);
    };

    // The popup only closes once a click actually lands on the topmost row, not on empty space.
    auto ClickFirstRow = [&]() -> EPropertyChangeOp
    {
        EPropertyChangeOp Result = EPropertyChangeOp::None;
        bool bOpen = true;
        for (float Y = ComboMax.y + 4.0f; bOpen && Y < ComboMax.y + 200.0f; Y += 4.0f)
        {
            MouseDown(ImVec2(ComboMin.x + 10.0f, Y));
            DrawFrame(Customization, Handle);
            MouseUp();
            Result = DrawFrame(Customization, Handle, nullptr, nullptr, &bOpen);
        }
        EXPECT_FALSE(bOpen);
        return Result;
    };

    // Starting value is Succeeded, so the topmost row (Running) is a real change that must commit.
    Open();
    EXPECT_EQ(ClickFirstRow(), EPropertyChangeOp::Started);
    Customization.UpdatePropertyValue(Handle);
    EXPECT_EQ(Task.Result, EStateTreeRunStatus::Running);
    EXPECT_EQ(DrawFrame(Customization, Handle), EPropertyChangeOp::Finished);

    // Current value is now Running, so re-picking the topmost row must report no change at all.
    Open();
    EXPECT_EQ(ClickFirstRow(), EPropertyChangeOp::None);
    EXPECT_EQ(DrawFrame(Customization, Handle), EPropertyChangeOp::None);
    EXPECT_EQ(Task.Result, EStateTreeRunStatus::Running);
}
