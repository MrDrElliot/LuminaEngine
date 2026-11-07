#include <gtest/gtest.h>

#include "ImGuiTestHarness.h"
#include "UI/CurveEditor/CurveEditorWidget.h"

using namespace Lumina;

namespace
{
    constexpr int32 kTooltipDelayFrames = 30;

    bool IsTooltipShown()
    {
        for (const ImGuiWindow* Window : ImGui::GetCurrentContext()->Windows)
        {
            if ((Window->Flags & ImGuiWindowFlags_Tooltip) != 0 && Window->Active)
            {
                return true;
            }
        }
        return false;
    }
}

// The toolbar tooltips come from ImGuiX in the Runtime DLL, which reads that DLL's own ImGui context.
TEST(CurveEditorWidget, HoveringAToolbarButtonShowsItsTooltip)
{
    FImGuiTestHarness Harness;
    SKeyedCurve Curve;
    Curve.AddKey(0.0f, 0.0f);
    Curve.AddKey(1.0f, 1.0f);

    FCurveEditorWidget Widget;
    Widget.SetCurve(&Curve);

    ImGui::GetIO().AddMousePosEvent(ImGui::GetStyle().FramePadding.x + 2.0f, ImGui::GetStyle().FramePadding.y + 2.0f);
    for (int32 Frame = 0; Frame < kTooltipDelayFrames; ++Frame)
    {
        FImGuiTestHarness::BeginFrame();
        Widget.Draw("Curve");
        FImGuiTestHarness::EndFrame();
    }

    EXPECT_TRUE(IsTooltipShown());
    EXPECT_EQ(Curve.NumKeys(), 2);
}
