#include <gtest/gtest.h>

#include "ImGuiTestHarness.h"
#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "Core/Object/ObjectCore.h"
#include "UI/Tools/AssetEditors/Animation/TimelineDrawing.h"
#include "UI/Tools/AssetEditors/AnimationMontage/AnimationMontageEditorTool.h"

namespace Lumina
{
    struct FAnimationMontageEditorToolTestAccess
    {
        static void DrawSlotTracks(FAnimationMontageEditorTool& Tool, CAnimationMontage* Montage, float Duration)
        {
            Tool.DrawSlotTracks(Montage, Duration);
        }
    };
}

using namespace Lumina;

namespace
{
    ImGuiWindow* FindWindowByNameSubstring(const char* Substring)
    {
        for (ImGuiWindow* Window : ImGui::GetCurrentContext()->Windows)
        {
            if (Window->Name && strstr(Window->Name, Substring) != nullptr)
            {
                return Window;
            }
        }
        return nullptr;
    }

    ImGuiWindow* FindActivePopupWindow()
    {
        for (ImGuiWindow* Window : ImGui::GetCurrentContext()->Windows)
        {
            if (Window->Active && (Window->Flags & ImGuiWindowFlags_Popup) != 0 && (Window->Flags & ImGuiWindowFlags_Tooltip) == 0)
            {
                return Window;
            }
        }
        return nullptr;
    }

    void MoveMouseTo(ImVec2 Position)
    {
        ImGui::GetIO().AddMousePosEvent(Position.x, Position.y);
    }

    void SetMouseButton(ImGuiMouseButton Button, bool bDown)
    {
        ImGui::GetIO().AddMouseButtonEvent(Button, bDown);
    }
}

// Right-clicking the second slot row and picking Delete Slot used to leave DrawSlotTracks indexing SlotTracks with its pre-deletion count.
TEST(AnimationMontageEditorTool, DeletingASlotTrackDoesNotIndexPastTheShrunkArray)
{
    FImGuiTestHarness Harness;

    CAnimationMontage* Montage = NewObject<CAnimationMontage>();
    Montage->SlotTracks.EmplaceGetRef().SlotName = "First";
    Montage->SlotTracks.EmplaceGetRef().SlotName = "Second";

    FAnimationMontageEditorTool Tool(nullptr, Montage);

    auto Frame = [&]
    {
        FImGuiTestHarness::BeginFrame();
        FAnimationMontageEditorToolTestAccess::DrawSlotTracks(Tool, Montage, 1.0f);
        FImGuiTestHarness::EndFrame();
    };

    MoveMouseTo(ImVec2(-100.0f, -100.0f));
    Frame();

    ImGuiWindow* Canvas = FindWindowByNameSubstring("MontageCanvas");
    ASSERT_NE(Canvas, nullptr);
    const ImVec2 CanvasPos = Canvas->DC.CursorStartPos;

    const ImVec2 SecondRowHeader(CanvasPos.x + 20.0f, CanvasPos.y + Timeline::RulerHeight + 34.0f + 44.0f * 1.5f);

    MoveMouseTo(SecondRowHeader);
    Frame();

    SetMouseButton(ImGuiMouseButton_Right, true);
    Frame();

    SetMouseButton(ImGuiMouseButton_Right, false);
    Frame();

    // The popup is hidden on the frame it first appears, to size itself without a visible flicker.
    Frame();

    ImGuiWindow* Popup = FindActivePopupWindow();
    ASSERT_NE(Popup, nullptr);

    // Delete Slot is the popup's last menu item, so a point near its bottom edge hits it no matter how tall the items above it are.
    const ImVec2 DeleteSlotItem(Popup->Pos.x + 20.0f, Popup->Pos.y + Popup->Size.y - 10.0f);

    MoveMouseTo(DeleteSlotItem);
    Frame();

    SetMouseButton(ImGuiMouseButton_Left, true);
    Frame();

    SetMouseButton(ImGuiMouseButton_Left, false);
    Frame();

    EXPECT_EQ(Montage->SlotTracks.Num(), 1);
    EXPECT_EQ(Montage->SlotTracks[0].SlotName, FName("First"));
}
