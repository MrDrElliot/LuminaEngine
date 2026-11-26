#pragma once

#include <cmath>

#define USE_IMGUI_API
#include <imgui.h>
#include "Core/Math/Math.h"
#include "Tools/ComponentVisualizers/ComponentVisualizerContext.h"
#include "World/Subsystems/TerrainSculptSystem.h"

// What the terrain and foliage brush modes share, so their panels, shortcuts and cursors look and behave alike.
namespace Lumina::BrushUI
{
    // One button of a mode's tool palette, whose color also tints that tool's cursor ring and sliders.
    struct FTool
    {
        const char* Icon;
        const char* Label;
        const char* Tip;
        ImU32       Color;
    };

    EDITOR_API ImU32 ScaleColor(ImU32 Color, float Scale, float Alpha);

    // Opens the settings panel under the viewport toolbar at Width before the UI scale, and like ImGui::Begin it always needs ImGui::End.
    EDITOR_API void BeginPanel(const char* Id, float Width);

    // Rows of four toggles, true when a click picked another tool.
    EDITOR_API bool ToolPalette(const FTool* Tools, int32 Count, int32& Selected);

    // Rows whose labels share one column, so the controls after them line up.
    EDITOR_API void RowLabel(const char* Label);
    EDITOR_API bool Slider(const char* Label, float* Value, float Min, float Max, const char* Format, ImU32 Color, bool bLogarithmic = false);
    EDITOR_API bool SliderInt(const char* Label, int32* Value, int32 Min, int32 Max, const char* Format, ImU32 Color);
    EDITOR_API bool Combo(const char* Label, int32* Index, const char* const Items[], int32 Count);

    // Size, strength as a percentage, and the falloff with its curve and a cross-section of the brush.
    EDITOR_API void BrushSection(float& Radius, float MaxRadius, float& Strength, float& Falloff, ETerrainBrushFalloff& Shape, ImU32 Color);

    // Disabled text that wraps at the panel edge.
    EDITOR_API void Hint(const char* Text);

    // [ and ] resize, and with Shift they change strength, true when either changed so the cursor can show the new values.
    EDITOR_API bool HandleBrushKeys(float& Radius, float MaxRadius, float& Strength);

    // Shadowed, so it reads over bright ground beside the cursor.
    EDITOR_API void CursorLabel(ImDrawList* Draw, ImVec2 Cursor, const char* Text, ImU32 Color);

    // Lay sets each ring point's height, so a ring laid on the ground reads true on slopes instead of cutting through them.
    template<typename TLay>
    void DrawRing(ImDrawList* Draw, const FVisualizerView& View, const FVector3& Center, float Radius, ImU32 Color, float Thickness, TLay&& Lay)
    {
        constexpr int32 Segments = 64;
        ImVec2 Points[Segments];
        bool   Visible[Segments];
        int32  FirstHidden = -1;
        for (int32 i = 0; i < Segments; ++i)
        {
            const float A = float(i) / float(Segments) * Math::TwoPi<float>();
            FVector3 P = Center + FVector3(std::cos(A) * Radius, 0.0f, std::sin(A) * Radius);
            Lay(P);
            Visible[i] = View.WorldToScreen(P, Points[i]);
            if (!Visible[i] && FirstHidden < 0)
            {
                FirstHidden = i;
            }
        }
        if (FirstHidden < 0)
        {
            Draw->AddPolyline(Points, Segments, Color, Thickness, ImDrawFlags_Closed);
            return;
        }

        // Only the arcs in front of the camera, since joining across the part behind it draws a line through the view.
        ImVec2 Run[Segments];
        int32  Count = 0;
        for (int32 Step = 1; Step <= Segments; ++Step)
        {
            const int32 i = (FirstHidden + Step) % Segments;
            if (Visible[i])
            {
                Run[Count++] = Points[i];
                continue;
            }
            if (Count >= 2)
            {
                Draw->AddPolyline(Run, Count, Color, Thickness);
            }
            Count = 0;
        }
    }
}
