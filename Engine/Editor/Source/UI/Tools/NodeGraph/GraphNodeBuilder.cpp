#include "GraphNodeBuilder.h"

#include "imgui_internal.h"
#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"

namespace Lumina
{
    namespace
    {
        constexpr float kPinIconSize     = 24.0f;
        constexpr float kPinRadius       = 5.0f;
        constexpr float kPinRingWidth    = 1.75f;
        constexpr float kPinOutline      = 1.5f;
        constexpr ImU32 kPinOutlineColor = IM_COL32(10, 11, 13, 255);
        constexpr float kHeaderHeight    = 30.0f;
        constexpr float kHeaderLeadFade  = 0.3f;
        constexpr float kHeaderTrailFade = 0.85f;
        constexpr ImU32 kHeaderSeparator = IM_COL32(0, 0, 0, 110);
        constexpr float kRefusedPinFade  = 0.25f;
        constexpr float kAcceptRingGap   = 4.5f;
        constexpr float kExpandBarHeight = 14.0f;
        constexpr ImU32 kExpandBarHover  = IM_COL32(255, 255, 255, 14);

        // A stroke width of zero fills the shape.
        void DrawPinShape(ImDrawList* DrawList, EGraphPinShape Shape, ImVec2 Center, float Radius, ImU32 Color, float StrokeWidth)
        {
            if (Shape == EGraphPinShape::Circle)
            {
                if (StrokeWidth > 0.0f)
                {
                    DrawList->AddCircle(Center, Radius, Color, 0, StrokeWidth);
                }
                else
                {
                    DrawList->AddCircleFilled(Center, Radius, Color);
                }
                return;
            }

            // Clockwise in screen space, which the filled polygon's antialiasing expects.
            ImVec2 Points[6];
            int32  Count = 0;
            switch (Shape)
            {
                case EGraphPinShape::Diamond:
                {
                    const float Reach = Radius * 1.25f;
                    Points[0] = Center + ImVec2(0.0f, -Reach);
                    Points[1] = Center + ImVec2(Reach, 0.0f);
                    Points[2] = Center + ImVec2(0.0f, Reach);
                    Points[3] = Center + ImVec2(-Reach, 0.0f);
                    Count = 4;
                    break;
                }
                case EGraphPinShape::Triangle:
                {
                    const float Reach = Radius * 1.2f;
                    Points[0] = Center + ImVec2(-Reach * 0.8f, -Reach);
                    Points[1] = Center + ImVec2(Reach, 0.0f);
                    Points[2] = Center + ImVec2(-Reach * 0.8f, Reach);
                    Count = 3;
                    break;
                }
                case EGraphPinShape::Square:
                {
                    const float Reach = Radius * 0.9f;
                    Points[0] = Center + ImVec2(-Reach, -Reach);
                    Points[1] = Center + ImVec2(Reach, -Reach);
                    Points[2] = Center + ImVec2(Reach, Reach);
                    Points[3] = Center + ImVec2(-Reach, Reach);
                    Count = 4;
                    break;
                }
                default:
                {
                    const float Reach = Radius * 1.1f;
                    for (int32 Corner = 0; Corner < 6; ++Corner)
                    {
                        const float Angle = IM_PI / 3.0f * (float)Corner;
                        Points[Corner] = Center + ImVec2(Math::Cos(Angle), Math::Sin(Angle)) * Reach;
                    }
                    Count = 6;
                    break;
                }
            }

            if (StrokeWidth > 0.0f)
            {
                DrawList->AddPolyline(Points, Count, Color, StrokeWidth, ImDrawFlags_Closed);
            }
            else
            {
                DrawList->AddConvexPolyFilled(Points, Count, Color);
            }
        }
    }

    void FGraphNodeBuilder::PinIcon(const FGraphPinIcon& Icon)
    {
        const ImVec2 Min = ImGui::GetCursorScreenPos();
        if (ImGui::IsRectVisible(ImVec2(kPinIconSize, kPinIconSize)))
        {
            ImDrawList*  DrawList = ImGui::GetWindowDrawList();
            const ImVec2 Center(Min.x + kPinIconSize * 0.5f - 0.5f, Min.y + kPinIconSize * 0.5f);
            const float  Fade    = Icon.Highlight == EGraphPinHighlight::Refuses ? kRefusedPinFade : 1.0f;
            const ImU32  Color   = EditorColors::ScaleAlpha(Icon.Color, Fade);
            const ImU32  Outline = EditorColors::ScaleAlpha((kPinOutlineColor & ~IM_COL32_A_MASK) | (Icon.Color & IM_COL32_A_MASK), Fade);

            if (Icon.Highlight == EGraphPinHighlight::Accepts)
            {
                DrawList->AddCircle(Center, kPinRadius + kAcceptRingGap, EditorColors::U32(EditorColors::Success()), 0, kPinRingWidth);
            }

            // A dark rim keeps the pin readable where wires and headers of the same color run behind it.
            DrawPinShape(DrawList, Icon.Shape, Center, kPinRadius + kPinOutline, Outline, 0.0f);
            if (Icon.bConnected)
            {
                DrawPinShape(DrawList, Icon.Shape, Center, kPinRadius, Color, 0.0f);
            }
            else
            {
                if ((Icon.InnerColor & IM_COL32_A_MASK) != 0)
                {
                    DrawPinShape(DrawList, Icon.Shape, Center, kPinRadius, EditorColors::ScaleAlpha(Icon.InnerColor, Fade), 0.0f);
                }
                DrawPinShape(DrawList, Icon.Shape, Center, kPinRadius - kPinRingWidth * 0.5f, Color, kPinRingWidth);
            }
        }
        ImGui::Dummy(ImVec2(kPinIconSize, kPinIconSize));
    }

    void FGraphNodeBuilder::Begin(int64 InNodeID, ImVec2& InOutPosition)
    {
        NodeID        = InNodeID;
        bHasHeader    = false;
        bHasExpandBar = false;

        Editor.BeginNode(NodeID, InOutPosition);
        NodePosition = InOutPosition;

        SetStage(EStage::Begin);
    }

    void FGraphNodeBuilder::End(bool bWantsTitlebar)
    {
        SetStage(EStage::End);
        Editor.EndNode();

        const ImVec2 NodeMax = NodePosition + Editor.GetNodeSize(NodeID);
        if (bHasExpandBar && ImGui::IsItemVisible())
        {
            DrawExpandBar(Editor.GetNodeBackgroundDrawList(NodeID), NodeMax);
        }

        if (bWantsTitlebar && ImGui::IsItemVisible())
        {
            const FGraphNodeStyle Style;
            const float  Inset   = Style.BorderWidth * 0.5f;
            const ImVec2 Min     = NodePosition + ImVec2(Inset, Inset);
            const ImVec2 Max(NodeMax.x - Inset, NodePosition.y + kHeaderHeight);

            // The category color leads at the left and settles into the body color across the title.
            const ImVec4 Category = ImGui::ColorConvertU32ToFloat4(HeaderColor);
            const ImVec4 Body     = EditorColors::WithAlpha(ImGui::ColorConvertU32ToFloat4(Style.Background), Category.w);
            const ImU32  Lead     = EditorColors::U32(ImLerp(Category, Body, kHeaderLeadFade));
            const ImU32  Trail    = EditorColors::U32(ImLerp(Category, Body, kHeaderTrailFade));

            ImDrawList* DrawList    = Editor.GetNodeBackgroundDrawList(NodeID);
            const int32 FirstVertex = DrawList->VtxBuffer.Size;
            DrawList->AddRectFilled(Min, Max, ImGui::GetColorU32(HeaderColor), Style.Rounding, ImDrawFlags_RoundCornersTop);
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(DrawList, FirstVertex, DrawList->VtxBuffer.Size, Min, Max, Lead, Trail);
            DrawList->AddLine(ImVec2(Min.x, Max.y), ImVec2(Max.x, Max.y), ImGui::GetColorU32(kHeaderSeparator));
        }

        NodeID = 0;
        SetStage(EStage::Invalid);
    }

    void FGraphNodeBuilder::Header(ImU32 Color)
    {
        HeaderColor = Color;
        SetStage(EStage::Header);
    }

    void FGraphNodeBuilder::EndHeader()
    {
        SetStage(EStage::Content);
    }

    void FGraphNodeBuilder::Input(uint32 PinID, ImU32 Color)
    {
        if (Stage == EStage::Begin)
        {
            SetStage(EStage::Content);
        }

        const bool bAnotherInput = Stage == EStage::Input;
        SetStage(EStage::Input);
        if (bAnotherInput)
        {
            ImGui::Spring(0);
        }

        Editor.BeginPin(PinID, EGraphPinKind::Input, Color);
        ImGui::BeginHorizontal((const void*)(uintptr_t)PinID);
    }

    void FGraphNodeBuilder::EndInput()
    {
        ImGui::EndHorizontal();
        Editor.EndPin();
    }

    void FGraphNodeBuilder::Middle()
    {
        if (Stage == EStage::Begin)
        {
            SetStage(EStage::Content);
        }
        SetStage(EStage::Middle);
    }

    void FGraphNodeBuilder::Output(uint32 PinID, ImU32 Color)
    {
        if (Stage == EStage::Begin)
        {
            SetStage(EStage::Content);
        }

        const bool bAnotherOutput = Stage == EStage::Output;
        SetStage(EStage::Output);
        if (bAnotherOutput)
        {
            ImGui::Spring(0);
        }

        Editor.BeginPin(PinID, EGraphPinKind::Output, Color);
        ImGui::BeginHorizontal((const void*)(uintptr_t)PinID);
    }

    void FGraphNodeBuilder::EndOutput()
    {
        ImGui::EndHorizontal();
        Editor.EndPin();
    }

    void FGraphNodeBuilder::Footer()
    {
        SetStage(EStage::Footer);
    }

    bool FGraphNodeBuilder::ExpandToggle(bool bInExpanded)
    {
        SetStage(EStage::Footer);

        const FGraphNodeStyle Style;
        bExpanded     = bInExpanded;
        bHasExpandBar = true;
        ExpandBarTop  = ImGui::GetCursorScreenPos().y;
        ImGui::Dummy(ImVec2(0.0f, kExpandBarHeight));

        const float  Width = Editor.GetNodeSize(NodeID).x;
        const ImRect Bar(ImVec2(NodePosition.x, ExpandBarTop), ImVec2(NodePosition.x + Width, ExpandBarTop + kExpandBarHeight + Style.Padding.w));
        const ImGuiID ID = ImGui::GetID("##ExpandToggle");

        // Kept alive instead of added, because the stack layout measures added items and the bar would widen the node every frame.
        ImGui::KeepAliveID(ID);
        bool bHeld = false;
        return ImGui::ButtonBehavior(Bar, ID, &bExpandBarHovered, &bHeld);
    }

    void FGraphNodeBuilder::DrawExpandBar(ImDrawList* DrawList, const ImVec2& NodeMax) const
    {
        const FGraphNodeStyle Style;
        const float  Inset = Style.BorderWidth * 0.5f;
        const ImVec2 Min(NodePosition.x + Inset, ExpandBarTop);
        const ImVec2 Max(NodeMax.x - Inset, NodeMax.y - Inset);
        if (bExpandBarHovered)
        {
            DrawList->AddRectFilled(Min, Max, kExpandBarHover, Style.Rounding, ImDrawFlags_RoundCornersBottom);
        }

        const char*  Icon     = bExpanded ? LE_ICON_CHEVRON_UP : LE_ICON_CHEVRON_DOWN;
        const ImVec2 IconSize = ImGui::CalcTextSize(Icon);
        const ImVec2 Center((Min.x + Max.x) * 0.5f, ExpandBarTop + kExpandBarHeight * 0.5f);
        DrawList->AddText(Center - IconSize * 0.5f, ImGui::GetColorU32(bExpandBarHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled), Icon);
    }

    void FGraphNodeBuilder::SetStage(EStage NewStage)
    {
        if (NewStage == Stage)
        {
            return;
        }

        const EStage OldStage = Stage;
        Stage = NewStage;

        switch (OldStage)
        {
            case EStage::Header:
            {
                ImGui::EndHorizontal();
                ImGui::Spring(0, ImGui::GetStyle().ItemSpacing.y * 2.0f);
                break;
            }
            case EStage::Input:
            case EStage::Output:
            {
                ImGui::Spring(1, 0);
                ImGui::EndVertical();
                break;
            }
            case EStage::Middle:
            {
                ImGui::EndVertical();
                break;
            }
            default:
                break;
        }

        switch (NewStage)
        {
            case EStage::Begin:
            {
                ImGui::BeginVertical("node");
                break;
            }
            case EStage::Header:
            {
                bHasHeader = true;
                ImGui::BeginHorizontal("header");
                break;
            }
            case EStage::Content:
            {
                if (OldStage == EStage::Begin)
                {
                    ImGui::Spring(0);
                }
                ImGui::BeginHorizontal("content");
                ImGui::Spring(0, 0);
                break;
            }
            case EStage::Input:
            {
                ImGui::BeginVertical("inputs", ImVec2(0, 0), 0.0f);
                if (!bHasHeader)
                {
                    ImGui::Spring(1, 0);
                }
                break;
            }
            case EStage::Middle:
            {
                ImGui::Spring(1);
                ImGui::BeginVertical("middle", ImVec2(0, 0), 1.0f);
                break;
            }
            case EStage::Output:
            {
                ImGui::Spring(1, OldStage == EStage::Middle || OldStage == EStage::Input ? -1.0f : 0.0f);
                ImGui::BeginVertical("outputs", ImVec2(0, 0), 1.0f);
                if (!bHasHeader)
                {
                    ImGui::Spring(1, 0);
                }
                break;
            }
            case EStage::Footer:
            case EStage::End:
            {
                if (OldStage == EStage::Input)
                {
                    ImGui::Spring(1, 0);
                }
                if (OldStage != EStage::Begin && OldStage != EStage::Footer)
                {
                    ImGui::EndHorizontal();
                }
                if (NewStage == EStage::End)
                {
                    ImGui::EndVertical();
                }
                break;
            }
            default:
                break;
        }
    }
}
