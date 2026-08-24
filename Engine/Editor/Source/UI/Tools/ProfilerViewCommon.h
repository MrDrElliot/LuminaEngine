#pragma once

#include "imgui.h"
#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"

// Inline rather than per-file statics, so a unity build cannot end up with two of the same helper.
namespace Lumina::ProfilerView
{
    /** Hashed from the name, so a pass or job keeps its color across runs without a registry to maintain. */
    inline ImU32 ScopeColor(const char* Name)
    {
        uint32 H = 2166136261u;
        for (const char* P = Name ? Name : "?"; *P; ++P) { H ^= (uint8)*P; H *= 16777619u; }
        float R, G, B;
        ImGui::ColorConvertHSVtoRGB((H % 360) / 360.0f, 0.55f, 0.88f, R, G, B);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(R, G, B, 1.0f));
    }

    inline ImU32 DimColor(ImU32 Color, float Scale)
    {
        const ImVec4 C = ImGui::ColorConvertU32ToFloat4(Color);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(C.x * Scale, C.y * Scale, C.z * Scale, C.w));
    }

    /** Row height that always fits a label, whatever the font scale or DPI. */
    inline float RowHeight(float Preferred)
    {
        return Math::Max(Preferred, ImGui::GetTextLineHeight() + 12.0f);
    }

    /** Vertically centers a label inside a row of the given height. */
    inline float LabelY(float RowTop, float RowHeightPx)
    {
        return RowTop + (RowHeightPx - ImGui::GetTextLineHeight()) * 0.5f;
    }

    /** Eases Current toward Target, keeping Keep of the previous value. */
    inline double Ease(double Current, double Target, double Keep)
    {
        return Current * Keep + Target * (1.0 - Keep);
    }

    inline void Divider()
    {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
    }
}
