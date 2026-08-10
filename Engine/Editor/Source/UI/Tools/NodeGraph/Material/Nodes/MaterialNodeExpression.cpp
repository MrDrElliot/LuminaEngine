#include "MaterialNodeExpression.h"

#include "imgui-node-editor/imgui_node_editor.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Memory/Memcpy.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

namespace Lumina
{
    namespace
    {
        // F2 is gated on a single selected node, so at most one rename is live at a time and the edit
        // buffer can be shared instead of costing every expression a string.
        char GParameterRenameBuffer[64] = {};
        bool GParameterRenameNeedsFocus = false;
    }

    void CMaterialExpression::BuildNode()
    {
        Output = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "", ENodePinDirection::Output));
        Output->SetShouldDrawEditor(false);
    }

    void CMaterialExpression::DrawNodeTitleBar()
    {
        FName* ParameterName = GetParameterName();

        if (bRenamingParameter && ParameterName != nullptr)
        {
            DrawParameterRename(*ParameterName);
            return;
        }
        bRenamingParameter = false;

        if (!bDynamic)
        {
            ImGui::TextUnformatted(GetNodeDisplayName().data());
            return;
        }

        // A dynamic expression IS a material parameter, so its name is the useful label -- a graph of nodes
        // all titled Float says nothing about which drives EmissivePower. Unnamed ones fall back to the type.
        const bool bNamed = ParameterName != nullptr && !ParameterName->IsNone();
        const FString Label = bNamed ? ParameterName->ToString() : FString(GetNodeDisplayName());
        ImGui::Text(LE_ICON_MATERIAL_DESIGN " %s", Label.c_str());

        ImGuiX::TextTooltip("{}", ParameterName != nullptr
            ? "Material parameter. F2 renames it here; its value lives in the \"Material Properties\" window."
            : "Edit this parameter in the \"Material Properties\" window.");

        // Single selection only: a marquee select must not drop every parameter node into edit mode.
        if (ParameterName != nullptr
            && ImGui::IsKeyPressed(ImGuiKey_F2, false)
            && ax::NodeEditor::GetSelectedObjectCount() == 1
            && ax::NodeEditor::IsNodeSelected(GetNodeID()))
        {
            bRenamingParameter = true;
            GParameterRenameNeedsFocus = true;

            const FString Current = ParameterName->IsNone() ? FString() : ParameterName->ToString();
            constexpr size_t MaxLength = sizeof(GParameterRenameBuffer) - 1;
            const size_t Length = Current.size() < MaxLength ? Current.size() : MaxLength;
            Memory::Memcpy(GParameterRenameBuffer, Current.c_str(), Length);
            GParameterRenameBuffer[Length] = '\0';
        }
    }

    void CMaterialExpression::DrawParameterRename(FName& Out)
    {
        // Claim the editor's Ctrl+C/V/X for the text field. DrawGraph re-arms shortcuts at the top of
        // every frame, so this is a per-frame opt-out and needs no matching restore.
        ax::NodeEditor::EnableShortcuts(false);

        if (GParameterRenameNeedsFocus)
        {
            ImGui::SetKeyboardFocusHere();
            GParameterRenameNeedsFocus = false;
        }

        ImGui::SetNextItemWidth(140.0f);
        const bool bCommitted = ImGui::InputText("##ParameterRename", GParameterRenameBuffer, sizeof(GParameterRenameBuffer),
                                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

        // Escape first: it also deactivates the item, and abandoning beats committing a half-typed name.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            bRenamingParameter = false;
            return;
        }

        // Clicking away commits, matching how the rest of the editor's inline fields behave.
        if (!bCommitted && !ImGui::IsItemDeactivatedAfterEdit())
        {
            return;
        }

        const FName NewName(GParameterRenameBuffer);
        if (NewName != Out)
        {
            Out = NewName;

            // The parameter name is baked into the generated shader, so a rename needs a recompile
            // just as much as a value edit does.
            NotifyValueEdited();

            if (CPackage* Package = GetPackage())
            {
                Package->MarkDirty();
            }
        }

        bRenamingParameter = false;
    }

    void CMaterialExpression_Math::BuildNode()
    {
        Super::BuildNode();
    }
}
