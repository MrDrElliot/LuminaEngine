#include "BrushModeUI.h"

#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina::BrushUI
{
    namespace
    {
        // Width of the label column every row shares, before the UI scale.
        constexpr float kLabelColumn = 84.0f;

        constexpr int32 kToolsPerRow = 4;

        constexpr const char* kFalloffNames[] = { "Smooth", "Linear", "Sphere", "Tip" };
        static_assert(IM_ARRAYSIZE(kFalloffNames) == size_t(ETerrainBrushFalloff::Num));

        // The fill takes the tool's color, so the panel matches the cursor ring.
        ImGuiX::FSliderStyle ToolSliderStyle(ImU32 Color)
        {
            ImGuiX::FSliderStyle Style;
            Style.FillColor = ScaleColor(Color, 0.8f, 1.0f);
            return Style;
        }

        void FalloffCurve(float Falloff, ETerrainBrushFalloff Shape, ImU32 Color)
        {
            const ImVec2 Size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * 1.6f);
            const ImVec2 Min = ImGui::GetCursorScreenPos();
            ImGui::Dummy(Size);

            ImDrawList* Draw = ImGui::GetWindowDrawList();
            Draw->AddRectFilled(Min, ImVec2(Min.x + Size.x, Min.y + Size.y), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);

            constexpr int32 Samples = 48;
            ImVec2 Points[Samples + 1];
            for (int32 i = 0; i <= Samples; ++i)
            {
                const float X = float(i) / float(Samples) * 2.0f - 1.0f;
                const float W = FTerrainSculptSystem::FalloffWeight(Math::Abs(X), 1.0f, Falloff, Shape);
                Points[i] = ImVec2(Min.x + 4.0f + (X * 0.5f + 0.5f) * (Size.x - 8.0f), Min.y + Size.y - 3.0f - W * (Size.y - 6.0f));
            }
            Draw->AddPolyline(Points, Samples + 1, Color, 2.0f);
        }
    }

    ImU32 ScaleColor(ImU32 Color, float Scale, float Alpha)
    {
        const ImVec4 C = ImGui::ColorConvertU32ToFloat4(Color);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(C.x * Scale, C.y * Scale, C.z * Scale, Alpha));
    }

    void BeginPanel(const char* Id, float Width)
    {
        ImVec2 Anchor = ImGui::GetWindowPos();
        Anchor.y += ImGui::GetWindowSize().y + 4.0f;
        const float Scaled = Width * ImGuiX::GetUIScale();
        ImGui::SetNextWindowPos(Anchor);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::SetNextWindowSizeConstraints(ImVec2(Scaled, 0.0f), ImVec2(Scaled, FLT_MAX));
        ImGui::Begin(Id, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    }

    bool ToolPalette(const FTool* Tools, int32 Count, int32& Selected)
    {
        ImGui::SeparatorText("Tool");

        // Toggles, since a combo costs two clicks and hides the other tools behind a label.
        const int32 PerRow = Math::Min(Count, kToolsPerRow);
        const float Width  = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * float(PerRow - 1)) / float(PerRow);
        bool bChanged = false;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FTool& Tool = Tools[Index];
            if (Index % PerRow != 0)
            {
                ImGui::SameLine();
            }

            char Label[48];
            ImFormatString(Label, sizeof(Label), "%s %s", Tool.Icon, Tool.Label);
            ImGui::PushStyleColor(ImGuiCol_Button, Selected == Index ? ScaleColor(Tool.Color, 0.55f, 1.0f) : ImGui::GetColorU32(ImGuiCol_Button));
            if (ImGui::Button(Label, ImVec2(Width, 0.0f)) && Selected != Index)
            {
                Selected = Index;
                bChanged = true;
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", Tool.Tip);
        }
        return bChanged;
    }

    void RowLabel(const char* Label)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Label);
        ImGui::SameLine(kLabelColumn * ImGuiX::GetUIScale());
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    bool Slider(const char* Label, float* Value, float Min, float Max, const char* Format, ImU32 Color, bool bLogarithmic)
    {
        RowLabel(Label);
        const ImGuiX::FSliderStyle Style = ToolSliderStyle(Color);
        ImGui::PushID(Label);
        const bool bChanged = ImGuiX::SliderFloat("##Value", Value, Min, Max,
            ImGuiX::ESliderFlags::AlwaysValue | (bLogarithmic ? ImGuiX::ESliderFlags::Logarithmic : ImGuiX::ESliderFlags::None), Format, &Style);
        ImGui::PopID();
        return bChanged;
    }

    bool SliderInt(const char* Label, int32* Value, int32 Min, int32 Max, const char* Format, ImU32 Color)
    {
        RowLabel(Label);
        const ImGuiX::FSliderStyle Style = ToolSliderStyle(Color);
        ImGui::PushID(Label);
        const bool bChanged = ImGuiX::SliderInt("##Value", Value, Min, Max, ImGuiX::ESliderFlags::AlwaysValue, Format, &Style);
        ImGui::PopID();
        return bChanged;
    }

    bool Combo(const char* Label, int32* Index, const char* const Items[], int32 Count)
    {
        RowLabel(Label);
        ImGui::PushID(Label);
        const bool bChanged = ImGui::Combo("##Value", Index, Items, Count);
        ImGui::PopID();
        return bChanged;
    }

    void BrushSection(float& Radius, float MaxRadius, float& Strength, float& Falloff, ETerrainBrushFalloff& Shape, ImU32 Color)
    {
        ImGui::SeparatorText("Brush");
        Slider("Size", &Radius, 1.0f, MaxRadius, "%.0f m", Color, true);

        float Percent = Strength * 100.0f;
        if (Slider("Strength", &Percent, 1.0f, 100.0f, "%.0f%%", Color))
        {
            Strength = Percent / 100.0f;
        }

        Slider("Falloff", &Falloff, 0.0f, 1.0f, "%.2f", Color);
        int32 ShapeIndex = int32(Shape);
        if (Combo("Curve", &ShapeIndex, kFalloffNames, IM_ARRAYSIZE(kFalloffNames)))
        {
            Shape = ETerrainBrushFalloff(ShapeIndex);
        }
        FalloffCurve(Falloff, Shape, Color);
    }

    void Hint(const char* Text)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", Text);
        ImGui::PopTextWrapPos();
    }

    bool HandleBrushKeys(float& Radius, float MaxRadius, float& Strength)
    {
        if (ImGui::GetIO().WantTextInput)
        {
            return false;
        }

        // Wired to ImGui's repeat, so holding a bracket key keeps scrubbing.
        const int32 Steps = int32(ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) - int32(ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true));
        if (Steps == 0)
        {
            return false;
        }

        if (ImGui::GetIO().KeyShift)
        {
            Strength = Math::Clamp(Strength + 0.05f * float(Steps), 0.01f, 1.0f);
        }
        else
        {
            Radius = Math::Clamp(Radius * (Steps > 0 ? 1.1f : 1.0f / 1.1f), 1.0f, MaxRadius);
        }
        return true;
    }

    void CursorLabel(ImDrawList* Draw, ImVec2 Cursor, const char* Text, ImU32 Color)
    {
        const ImVec2 Pos(Cursor.x + 14.0f, Cursor.y + 10.0f);
        Draw->AddText(ImVec2(Pos.x + 1.0f, Pos.y + 1.0f), IM_COL32(0, 0, 0, 200), Text);
        Draw->AddText(Pos, Color, Text);
    }
}
