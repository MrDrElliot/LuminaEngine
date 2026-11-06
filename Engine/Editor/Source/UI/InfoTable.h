#pragma once

#include "imgui.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina::InfoTable
{
    // One label/value row of a two-column table, with an empty value shown as "-".
    inline void Row(const char* Label, FStringView Value, const ImVec4* Color = nullptr)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(Label);
        ImGui::TableSetColumnIndex(1);
        if (Color)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, *Color);
        }
        ImGuiX::TextUnformatted(Value.IsEmpty() ? FStringView("-") : Value);
        if (Color)
        {
            ImGui::PopStyleColor();
        }
    }
}
