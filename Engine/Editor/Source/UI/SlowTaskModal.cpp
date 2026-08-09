#include "SlowTaskModal.h"

#include <cfloat>
#include <cmath>
#include <imgui.h>

#include "Containers/Array.h"
#include "Containers/String.h"
#include "Core/Progress/SlowTask.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"

namespace Lumina::SlowTaskModal
{
    namespace
    {
        constexpr const char* GPopupID = "##SlowTaskModal";

        constexpr ImVec4 GAccent      = ImVec4(0.11f, 0.64f, 0.92f, 1.00f);
        constexpr ImVec4 GAccentTrack = ImVec4(0.11f, 0.64f, 0.92f, 0.18f);
        constexpr ImVec4 GMutedText   = ImVec4(0.62f, 0.63f, 0.66f, 1.00f);
        constexpr ImVec4 GHeaderText  = ImVec4(0.55f, 0.56f, 0.60f, 1.00f);

        // A rotating three-quarter arc, animated off ImGui::GetTime().
        void DrawSpinner(float Radius, float Thickness, ImU32 Color)
        {
            const ImVec2 TopLeft = ImGui::GetCursorScreenPos();
            const ImVec2 Center  = ImVec2(TopLeft.x + Radius, TopLeft.y + Radius);

            ImDrawList* DrawList = ImGui::GetWindowDrawList();
            constexpr int Segments = 32;
            const float Time     = static_cast<float>(ImGui::GetTime());
            const float Start    = Time * 5.0f;
            const float ArcSpan  = IM_PI * 1.5f;

            DrawList->PathClear();
            for (int i = 0; i <= Segments; ++i)
            {
                const float Angle = Start + (static_cast<float>(i) / Segments) * ArcSpan;
                DrawList->PathLineTo(ImVec2(Center.x + std::cos(Angle) * Radius,
                                            Center.y + std::sin(Angle) * Radius));
            }
            DrawList->PathStroke(Color, ImDrawFlags_None, Thickness);

            ImGui::Dummy(ImVec2(Radius * 2.0f, Radius * 2.0f));
        }

        void DrawTask(const FSlowTaskProgress& Task)
        {
            constexpr float SpinnerRadius = 8.0f;
            constexpr float TextIndent    = SpinnerRadius * 2.0f + 12.0f;

            // Header row: spinner + bold title, vertically centered against each other.
            const float RowStartY = ImGui::GetCursorPosY();
            DrawSpinner(SpinnerRadius, 2.5f, ImGui::GetColorU32(GAccent));

            ImGui::SameLine(0.0f, 12.0f);
            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::MediumBold);
            const float TitleOffset = (SpinnerRadius * 2.0f - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorPosY(RowStartY + ImMax(0.0f, TitleOffset));
            ImGui::TextUnformatted(Task.Title.c_str());
            ImGuiX::Font::PopFont();

            // Message, indented to sit under the title.
            if (!Task.Message.empty())
            {
                ImGui::SetCursorPosY(RowStartY + SpinnerRadius * 2.0f + 4.0f);
                ImGui::Indent(TextIndent);
                ImGui::PushStyleColor(ImGuiCol_Text, GMutedText);
                ImGui::TextUnformatted(Task.Message.c_str());
                ImGui::PopStyleColor();
                ImGui::Unindent(TextIndent);
            }

            ImGui::Spacing();

            // Progress bar: accent fill on a faint accent track, rounded.
            FFixedString Overlay(FFixedString::CtorSprintf(), "%.0f%%", Task.Fraction * 100.0f);

            // ProgressBar draws the overlay percentage INSIDE the bar, clipped to it, so the bar has to be
            // at least a line of text plus padding tall or the digits lose their top and bottom. 14 keeps
            // the slim look at 1x; the text term is what makes it survive DPI scaling.
            constexpr float BarPaddingY = 3.0f;
            const float BarHeight = ImMax(14.0f, ImGui::GetTextLineHeight() + BarPaddingY * 2.0f);

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, GAccent);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, GAccentTrack);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, BarPaddingY));
            ImGui::ProgressBar(Task.Fraction, ImVec2(-FLT_MIN, BarHeight), Overlay.c_str());
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }
    }

    void Render()
    {
        TVector<FSlowTaskProgress> Tasks = SlowTasks::GetSnapshot();

        const bool bWantOpen = !Tasks.empty();
        const bool bIsOpen   = ImGui::IsPopupOpen(GPopupID);

        if (bWantOpen && !bIsOpen)
        {
            ImGui::OpenPopup(GPopupID);
        }

        if (!bWantOpen)
        {
            // Drain the popup so it actually closes once every task has finished.
            if (bIsOpen && ImGui::BeginPopupModal(GPopupID, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            return;
        }

        const ImGuiViewport* Viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);

        constexpr ImGuiWindowFlags Flags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));

        if (ImGui::BeginPopupModal(GPopupID, nullptr, Flags))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, GHeaderText);
            ImGui::TextUnformatted(LE_ICON_TIMER_SAND "  WORKING");
            ImGui::PopStyleColor();
            ImGui::Spacing();

            for (size_t i = 0; i < Tasks.size(); ++i)
            {
                if (i > 0)
                {
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                }
                DrawTask(Tasks[i]);
            }

            ImGui::EndPopup();
        }

        ImGui::PopStyleVar(2);
    }
}
