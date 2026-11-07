#include "MaterialNodeExpression.h"

#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

namespace Lumina
{
    void CMaterialExpression::BuildNode()
    {
        Output = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "", ENodePinDirection::Output));
        Output->SetShouldDrawEditor(false);
    }

    FString CMaterialExpression::GetNodeTitleText() const
    {
        // The accessor is non-const only for the rename, and nothing is written through it here.
        const FName* ParameterName = bDynamic ? const_cast<CMaterialExpression*>(this)->GetParameterName() : nullptr;
        return ParameterName != nullptr && !ParameterName->IsNone() ? ParameterName->ToString() : Super::GetNodeTitleText();
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
            ImGui::TextUnformatted(GetNodeDisplayName().GetData());
            return;
        }

        // A graph of nodes all titled Float says nothing about which one drives EmissivePower.
        ImGui::Text(LE_ICON_MATERIAL_DESIGN " %s", *GetNodeTitleText());

        if (ImGui::IsItemHovered() && GetOwningGraph() != nullptr)
        {
            GetOwningGraph()->GetEditor().SetTooltip(ParameterName != nullptr
                ? "Material parameter. F2 renames it here; its value lives in the \"Material Properties\" window."
                : "Edit this parameter in the \"Material Properties\" window.");
        }

        // Single selection only, or a marquee select drops every parameter node into edit mode.
        if (ParameterName != nullptr
            && ImGui::IsKeyPressed(ImGuiKey_F2, false)
            && GetOwningGraph() != nullptr
            && GetOwningGraph()->GetEditor().GetSelectedNodeCount() == 1
            && GetOwningGraph()->GetEditor().IsNodeSelected(GetNodeID()))
        {
            bRenamingParameter = true;
            bParameterRenameNeedsFocus = true;
            ParameterRenameText = ParameterName->IsNone() ? FString() : ParameterName->ToString();
        }
    }

    void CMaterialExpression::DrawParameterRename(FName& Out)
    {
        if (bParameterRenameNeedsFocus)
        {
            ImGui::SetKeyboardFocusHere();
            bParameterRenameNeedsFocus = false;
        }

        ImGui::SetNextItemWidth(140.0f);
        const bool bCommitted = ImGuiX::InputText("##ParameterRename", ParameterRenameText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

        // Escape first, since it also deactivates the item and abandoning beats a half-typed commit.
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

        const FName NewName(ParameterRenameText);
        if (NewName != Out)
        {
            Out = NewName;

            // The parameter name is baked into the shader, so a rename needs a recompile like a value edit.
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
