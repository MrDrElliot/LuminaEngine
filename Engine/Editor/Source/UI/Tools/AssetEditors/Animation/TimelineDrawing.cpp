#include "TimelineDrawing.h"

#include <cmath>
#include <cstdio>
#include <imgui_internal.h>

namespace Lumina::Timeline
{
    namespace
    {
        constexpr ImU32 GridColor       = IM_COL32(255, 255, 255, 14);
        constexpr ImU32 TickMajorColor  = IM_COL32(190, 194, 206, 150);
        constexpr ImU32 TickMinorColor  = IM_COL32(190, 194, 206, 60);
        constexpr ImU32 RulerTextColor  = IM_COL32(170, 174, 186, 230);
        constexpr ImU32 OutOfRangeColor = IM_COL32(0, 0, 0, 80);
        constexpr ImU32 EndLineColor    = IM_COL32(230, 90, 80, 150);
        constexpr ImU32 TagColor        = IM_COL32(15, 16, 20, 205);
        constexpr ImU32 TagHoverColor   = IM_COL32(15, 16, 20, 240);

        const FVector4 Palette[] =
        {
            { 0.93f, 0.42f, 0.36f, 1.0f },
            { 0.40f, 0.75f, 0.95f, 1.0f },
            { 0.55f, 0.85f, 0.45f, 1.0f },
            { 0.95f, 0.78f, 0.35f, 1.0f },
            { 0.78f, 0.55f, 0.92f, 1.0f },
            { 0.40f, 0.88f, 0.80f, 1.0f },
        };

        int32 StepDecimals(float Step)
        {
            for (int32 Decimals = 0; Decimals < 3; ++Decimals)
            {
                const float Scaled = Step * powf(10.0f, (float)Decimals);
                if (fabsf(Scaled - roundf(Scaled)) < 1e-3f)
                {
                    return Decimals;
                }
            }
            return 3;
        }
    }

    FVector4 PaletteColor(int32 Index)
    {
        constexpr int32 Count = (int32)IM_ARRAYSIZE(Palette);
        return Palette[((Index % Count) + Count) % Count];
    }

    ImU32 ToU32(const FVector4& Color)
    {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(Color.x, Color.y, Color.z, Color.w));
    }

    ImU32 WithAlpha(const FVector4& Color, float Alpha)
    {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(Color.x, Color.y, Color.z, Alpha));
    }

    void DrawRuler(ImDrawList* DrawList, const FView& View, float RulerY0, float TrackAreaY0, float CanvasY1, float Duration, int32 FrameRate)
    {
        static const float NiceSteps[] = { 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 15.0f, 30.0f, 60.0f };
        float Major = NiceSteps[IM_ARRAYSIZE(NiceSteps) - 1];
        for (float Step : NiceSteps)
        {
            if (Step * View.PPS >= 84.0f)
            {
                Major = Step;
                break;
            }
        }

        const float FrameSec = 1.0f / (float)Math::Max(FrameRate, 1);
        float Minor = 0.0f;
        if (FrameSec * View.PPS >= 7.0f && FrameSec < Major)
        {
            Minor = FrameSec;
        }
        else if (Major * 0.2f * View.PPS >= 7.0f)
        {
            Minor = Major * 0.2f;
        }

        const float VisibleT0 = Math::Max(View.XToTime(View.LaneX0), 0.0f);
        const float VisibleT1 = Math::Min(View.XToTime(View.LaneX1), Duration);

        DrawList->PushClipRect(ImVec2(View.LaneX0, RulerY0), ImVec2(View.LaneX1, CanvasY1), true);

        const float StartX = View.TimeToX(0.0f);
        const float EndX   = View.TimeToX(Duration);
        if (StartX > View.LaneX0)
        {
            DrawList->AddRectFilled(ImVec2(View.LaneX0, TrackAreaY0), ImVec2(StartX, CanvasY1), OutOfRangeColor);
        }
        if (EndX < View.LaneX1)
        {
            DrawList->AddRectFilled(ImVec2(EndX, TrackAreaY0), ImVec2(View.LaneX1, CanvasY1), OutOfRangeColor);
        }

        if (Minor > 0.0f)
        {
            const int64 First = (int64)ceilf(VisibleT0 / Minor - 1e-3f);
            const int64 Last  = (int64)floorf(VisibleT1 / Minor + 1e-3f);
            for (int64 K = First; K <= Last; ++K)
            {
                const float X = View.TimeToX((float)K * Minor);
                DrawList->AddLine(ImVec2(X, TrackAreaY0 - 5.0f), ImVec2(X, TrackAreaY0), TickMinorColor);
            }
        }

        const int32 Decimals = StepDecimals(Major);
        const int64 FirstMajor = (int64)ceilf(VisibleT0 / Major - 1e-3f);
        const int64 LastMajor  = (int64)floorf(VisibleT1 / Major + 1e-3f);
        for (int64 K = FirstMajor; K <= LastMajor; ++K)
        {
            const float T = (float)K * Major;
            const float X = View.TimeToX(T);
            DrawList->AddLine(ImVec2(X, TrackAreaY0), ImVec2(X, CanvasY1), GridColor);
            DrawList->AddLine(ImVec2(X, TrackAreaY0 - 10.0f), ImVec2(X, TrackAreaY0), TickMajorColor);

            char Label[32];
            ImFormatString(Label, sizeof(Label), "%.*fs", Decimals, T);
            const float LabelW = ImGui::CalcTextSize(Label).x;
            const float LabelX = (X + 4.0f + LabelW > View.LaneX1 - 2.0f) ? X - 4.0f - LabelW : X + 4.0f;
            DrawList->AddText(ImVec2(LabelX, RulerY0 + 4.0f), RulerTextColor, Label);
        }

        DrawList->AddLine(ImVec2(EndX, RulerY0 + 4.0f), ImVec2(EndX, CanvasY1), EndLineColor, 1.0f);
        DrawList->PopClipRect();
    }

    void DrawPlayhead(ImDrawList* DrawList, const FView& View, float RulerY0, float TrackAreaY0, float CanvasY1, float Time, int32 FrameRate)
    {
        const float PX = View.TimeToX(Time);
        if (PX < View.LaneX0 - 1.0f || PX > View.LaneX1 + 1.0f)
        {
            return;
        }

        DrawList->PushClipRect(ImVec2(View.LaneX0, RulerY0), ImVec2(View.LaneX1, CanvasY1), true);
        DrawList->AddLine(ImVec2(PX, TrackAreaY0), ImVec2(PX, CanvasY1), PlayheadColor, 1.5f);

        char FrameText[16];
        snprintf(FrameText, sizeof(FrameText), "%d", (int)roundf(Time * (float)FrameRate));
        const ImVec2 FrameSize = ImGui::CalcTextSize(FrameText);
        const float BoxW = Math::Max(FrameSize.x + 12.0f, 24.0f);
        const ImVec2 Box0(PX - BoxW * 0.5f, TrackAreaY0 - FrameSize.y - 8.0f);
        const ImVec2 Box1(PX + BoxW * 0.5f, TrackAreaY0 - 4.0f);
        DrawList->AddRectFilled(Box0, Box1, PlayheadColor, 4.0f);
        DrawList->AddTriangleFilled(ImVec2(PX - 4.0f, Box1.y), ImVec2(PX + 4.0f, Box1.y), ImVec2(PX, Box1.y + 4.0f), PlayheadColor);
        DrawList->AddText(ImVec2(PX - FrameSize.x * 0.5f, Box0.y + 2.0f), IM_COL32(24, 24, 28, 255), FrameText);
        DrawList->PopClipRect();
    }

    void DrawTrackHeader(ImDrawList* DrawList, const ImVec2& Min, const ImVec2& Max, const FVector4& Color, const char* Icon,
                         const char* Name, int32 Count, bool bSelected, bool bHovered)
    {
        const float FontSize = ImGui::GetFontSize();
        DrawList->AddRectFilled(Min, Max, bSelected ? HeaderSelectedColor : (bHovered ? HeaderHoverColor : HeaderColor));
        DrawList->AddRectFilled(ImVec2(Min.x, Min.y + 1.0f), ImVec2(Min.x + 4.0f, Max.y - 1.0f), ToU32(Color));

        const float TextY = Min.y + (Max.y - Min.y - FontSize) * 0.5f;
        float TextX = Min.x + 14.0f;
        if (Icon != nullptr)
        {
            DrawList->AddText(ImVec2(TextX, TextY), ToU32(Color), Icon);
            TextX += ImGui::CalcTextSize(Icon).x + 6.0f;
        }

        float NameLimit = Max.x - 10.0f;
        if (Count > 0)
        {
            char CountText[16];
            snprintf(CountText, sizeof(CountText), "%d", Count);
            const ImVec2 CountSize = ImGui::CalcTextSize(CountText);
            const float BadgeX1 = Max.x - 10.0f;
            const float BadgeX0 = BadgeX1 - CountSize.x - 14.0f;
            const float BadgeY0 = Min.y + (Max.y - Min.y - CountSize.y - 4.0f) * 0.5f;
            DrawList->AddRectFilled(ImVec2(BadgeX0, BadgeY0), ImVec2(BadgeX1, BadgeY0 + CountSize.y + 4.0f), IM_COL32(255, 255, 255, 16), (CountSize.y + 4.0f) * 0.5f);
            DrawList->AddText(ImVec2(BadgeX0 + 7.0f, BadgeY0 + 2.0f), MutedTextColor, CountText);
            NameLimit = BadgeX0 - 6.0f;
        }

        DrawList->PushClipRect(ImVec2(TextX, Min.y), ImVec2(NameLimit, Max.y), true);
        DrawList->AddText(ImVec2(TextX, TextY), bSelected ? ItemTextColor : TrackTextColor, Name);
        DrawList->PopClipRect();
    }

    FPointGeometry GetPointGeometry(float X, float RowY0, float RowY1, const char* Name, bool bPinnedToBar)
    {
        FPointGeometry Geometry;
        const ImVec2 NameSize = ImGui::CalcTextSize(Name);
        Geometry.CenterY = bPinnedToBar ? RowY0 + ItemInset : (RowY0 + RowY1) * 0.5f;
        Geometry.Tag0 = ImVec2(X + Geometry.Radius + 4.0f, Geometry.CenterY - NameSize.y * 0.5f - 3.0f);
        Geometry.Tag1 = ImVec2(Geometry.Tag0.x + NameSize.x + 12.0f, Geometry.CenterY + NameSize.y * 0.5f + 3.0f);
        return Geometry;
    }

    bool HitPoint(const FPointGeometry& Geometry, float X, const ImVec2& Mouse, bool bWithTag)
    {
        const bool bOverMarker = fabsf(Mouse.x - X) <= Geometry.Radius + 3.0f && fabsf(Mouse.y - Geometry.CenterY) <= Geometry.Radius + 3.0f;
        const bool bOverTag    = bWithTag && Mouse.x >= Geometry.Tag0.x && Mouse.x <= Geometry.Tag1.x && Mouse.y >= Geometry.Tag0.y && Mouse.y <= Geometry.Tag1.y;
        return bOverMarker || bOverTag;
    }

    bool Overlaps(const TVector<ImVec2>& Spans, float X0, float X1)
    {
        for (const ImVec2& Span : Spans)
        {
            if (X0 < Span.y && X1 > Span.x)
            {
                return true;
            }
        }
        return false;
    }

    ImVec2 RangeLabelSpan(const FRangeGeometry& Geometry, float X0, float X1, const char* Name)
    {
        if (!Geometry.bNameInside)
        {
            return ImVec2(Geometry.Tag0.x, Geometry.Tag1.x);
        }
        return ImVec2(X0 + 8.0f, Math::Min(X0 + 10.0f + ImGui::CalcTextSize(Name).x, X1 - 6.0f));
    }

    void DrawPoint(ImDrawList* DrawList, const FPointGeometry& Geometry, float X, float RowY0, float RowY1, const FVector4& Color,
                   const char* Name, bool bSelected, bool bHovered, float Flash, bool bShowTag)
    {
        const FVector4 C = Flash > 0.0f ? Math::Mix(Color, FVector4(1.0f), Flash * 0.6f) : Color;
        const float CY = Geometry.CenterY;

        DrawList->AddLine(ImVec2(X, RowY0 + 3.0f), ImVec2(X, RowY1 - 3.0f), WithAlpha(C, 0.45f), 1.0f);
        if (bShowTag)
        {
            DrawList->AddRectFilled(Geometry.Tag0, Geometry.Tag1, bSelected ? WithAlpha(C, 0.38f) : (bHovered ? TagHoverColor : TagColor), 4.0f);
            if (bSelected)
            {
                DrawList->AddRect(Geometry.Tag0, Geometry.Tag1, WithAlpha(C, 0.9f), 4.0f, 1.0f);
            }
            DrawList->AddText(ImVec2(Geometry.Tag0.x + 6.0f, CY - ImGui::GetFontSize() * 0.5f), ItemTextColor, Name);
        }

        const float R = (bSelected || bHovered) ? Geometry.Radius + 1.0f : Geometry.Radius;
        const ImVec2 P0(X, CY - R), P1(X + R, CY), P2(X, CY + R), P3(X - R, CY);
        DrawList->AddQuadFilled(P0, P1, P2, P3, ToU32(C));
        DrawList->AddQuad(P0, P1, P2, P3, bSelected ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 170), bSelected ? 2.0f : 1.2f);
        if (Flash > 0.0f)
        {
            DrawList->AddCircle(ImVec2(X, CY), R + 3.0f + (1.0f - Flash) * 7.0f, WithAlpha(C, Flash), 0, 2.0f);
        }
    }

    FRangeGeometry GetRangeGeometry(float X0, float X1, float RowY0, float RowY1, const char* Name)
    {
        FRangeGeometry Geometry;
        const float FontSize = ImGui::GetFontSize();
        const float NameW = ImGui::CalcTextSize(Name).x;
        Geometry.BarY0 = RowY0 + ItemInset;
        Geometry.BarY1 = RowY1 - ItemInset;
        Geometry.TextY = (Geometry.BarY0 + Geometry.BarY1 - FontSize) * 0.5f;
        Geometry.bNameInside = X1 - X0 >= NameW + 14.0f;
        Geometry.Tag0 = ImVec2(X1 + 5.0f, Geometry.TextY - 3.0f);
        Geometry.Tag1 = ImVec2(Geometry.Tag0.x + NameW + 12.0f, Geometry.TextY + FontSize + 3.0f);
        return Geometry;
    }

    void DrawRange(ImDrawList* DrawList, const FRangeGeometry& Geometry, float X0, float X1, const FVector4& Color, const char* Name,
                   const char* Detail, bool bSelected, bool bHovered, bool bLeftHandle, bool bRightHandle)
    {
        const float BarY0 = Geometry.BarY0;
        const float BarY1 = Geometry.BarY1;

        DrawList->AddRectFilled(ImVec2(X0, BarY0), ImVec2(X1, BarY1), WithAlpha(Color, bSelected ? 0.55f : (bHovered ? 0.46f : 0.34f)), 5.0f);
        DrawList->AddRectFilled(ImVec2(X0, BarY0), ImVec2(Math::Min(X0 + 4.0f, X1), BarY1), WithAlpha(Color, 1.0f), 5.0f, ImDrawFlags_RoundCornersLeft);
        DrawList->AddRect(ImVec2(X0, BarY0), ImVec2(X1, BarY1), bSelected ? IM_COL32(255, 255, 255, 235) : WithAlpha(Color, bHovered ? 1.0f : 0.75f), 5.0f, bSelected ? 2.0f : 1.0f);

        if (Geometry.bNameInside)
        {
            DrawList->PushClipRect(ImVec2(X0 + 8.0f, BarY0), ImVec2(X1 - 6.0f, BarY1), true);
            DrawList->AddText(ImVec2(X0 + 10.0f, Geometry.TextY), ItemTextColor, Name);
            if (Detail != nullptr)
            {
                const float NameW = ImGui::CalcTextSize(Name).x;
                const float DetailW = ImGui::CalcTextSize(Detail).x;
                if (X1 - X0 > NameW + DetailW + 34.0f)
                {
                    DrawList->AddText(ImVec2(X1 - 8.0f - DetailW, Geometry.TextY), MutedTextColor, Detail);
                }
            }
            DrawList->PopClipRect();
        }
        else
        {
            DrawList->AddRectFilled(Geometry.Tag0, Geometry.Tag1, bHovered ? TagHoverColor : TagColor, 4.0f);
            DrawList->AddText(ImVec2(Geometry.Tag0.x + 6.0f, Geometry.TextY), ItemTextColor, Name);
        }

        if (bLeftHandle)
        {
            DrawList->AddRectFilled(ImVec2(X0 + 1.0f, BarY0 + 5.0f), ImVec2(X0 + 4.0f, BarY1 - 5.0f), IM_COL32(255, 255, 255, 230), 2.0f);
        }
        if (bRightHandle)
        {
            DrawList->AddRectFilled(ImVec2(X1 - 4.0f, BarY0 + 5.0f), ImVec2(X1 - 1.0f, BarY1 - 5.0f), IM_COL32(255, 255, 255, 230), 2.0f);
        }
    }
}
