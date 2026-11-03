#pragma once

#include <imgui.h>

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Timeline
{
    inline constexpr float HeaderWidth = 200.0f;
    inline constexpr float RulerHeight = 42.0f;
    inline constexpr float TrackHeight = 40.0f;
    inline constexpr float LanePad     = 16.0f;
    inline constexpr float ItemInset   = 7.0f;

    inline constexpr ImU32 CanvasColor         = IM_COL32(21, 22, 26, 255);
    inline constexpr ImU32 HeaderColor         = IM_COL32(29, 30, 36, 255);
    inline constexpr ImU32 HeaderHoverColor    = IM_COL32(35, 37, 44, 255);
    inline constexpr ImU32 HeaderSelectedColor = IM_COL32(42, 46, 58, 255);
    inline constexpr ImU32 RulerColor          = IM_COL32(33, 34, 41, 255);
    inline constexpr ImU32 RowColor            = IM_COL32(24, 25, 30, 255);
    inline constexpr ImU32 RowAltColor         = IM_COL32(27, 28, 34, 255);
    inline constexpr ImU32 RowHoverColor       = IM_COL32(255, 255, 255, 7);
    inline constexpr ImU32 RowLineColor        = IM_COL32(0, 0, 0, 110);
    inline constexpr ImU32 SeparatorColor      = IM_COL32(0, 0, 0, 170);
    inline constexpr ImU32 MutedTextColor      = IM_COL32(135, 139, 152, 255);
    inline constexpr ImU32 TrackTextColor      = IM_COL32(205, 208, 218, 255);
    inline constexpr ImU32 ItemTextColor       = IM_COL32(240, 242, 248, 255);
    inline constexpr ImU32 PlayheadColor       = IM_COL32(255, 204, 64, 255);

    struct FView
    {
        float LaneX0    = 0.0f;
        float LaneX1    = 0.0f;
        float ContentX0 = 0.0f;
        float PPS       = 1.0f;
        float Pan       = 0.0f;

        float TimeToX(float Time) const { return ContentX0 + (Time - Pan) * PPS; }
        float XToTime(float X) const { return Pan + (X - ContentX0) / PPS; }
    };

    struct FPointGeometry
    {
        float  CenterY = 0.0f;
        float  Radius  = 6.5f;
        ImVec2 Tag0;
        ImVec2 Tag1;
    };

    struct FRangeGeometry
    {
        float  BarY0 = 0.0f;
        float  BarY1 = 0.0f;
        float  TextY = 0.0f;
        bool   bNameInside = true;
        ImVec2 Tag0;
        ImVec2 Tag1;
    };

    FVector4 PaletteColor(int32 Index);
    ImU32 ToU32(const FVector4& Color);
    ImU32 WithAlpha(const FVector4& Color, float Alpha);

    void DrawRuler(ImDrawList* DrawList, const FView& View, float RulerY0, float TrackAreaY0, float CanvasY1, float Duration, int32 FrameRate);
    void DrawPlayhead(ImDrawList* DrawList, const FView& View, float RulerY0, float TrackAreaY0, float CanvasY1, float Time, int32 FrameRate);
    void DrawTrackHeader(ImDrawList* DrawList, const ImVec2& Min, const ImVec2& Max, const FVector4& Color, const char* Icon,
                         const char* Name, int32 Count, bool bSelected, bool bHovered);

    FPointGeometry GetPointGeometry(float X, float RowY0, float RowY1, const char* Name, bool bPinnedToBar);
    bool HitPoint(const FPointGeometry& Geometry, float X, const ImVec2& Mouse, bool bWithTag);
    void DrawPoint(ImDrawList* DrawList, const FPointGeometry& Geometry, float X, float RowY0, float RowY1, const FVector4& Color,
                   const char* Name, bool bSelected, bool bHovered, float Flash, bool bShowTag);

    bool Overlaps(const TVector<ImVec2>& Spans, float X0, float X1);
    ImVec2 RangeLabelSpan(const FRangeGeometry& Geometry, float X0, float X1, const char* Name);

    FRangeGeometry GetRangeGeometry(float X0, float X1, float RowY0, float RowY1, const char* Name);
    void DrawRange(ImDrawList* DrawList, const FRangeGeometry& Geometry, float X0, float X1, const FVector4& Color, const char* Name,
                   const char* Detail, bool bSelected, bool bHovered, bool bLeftHandle, bool bRightHandle);
}
