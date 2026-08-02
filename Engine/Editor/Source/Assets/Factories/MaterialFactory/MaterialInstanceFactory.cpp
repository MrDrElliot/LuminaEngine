#include "EditorPCH.h"
#include "MaterialInstanceFactory.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "imgui.h"


namespace Lumina
{
    bool CMaterialInstanceFactory::HasCreationDialogue() const
    {
        return true;
    }

    bool CMaterialInstanceFactory::DrawCreationDialogue(FStringView Path, bool& bShouldClose)
    {
        if (ImGui::IsWindowAppearing())
        {
            SelectedMaterialGUID.Invalidate();
        }

        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), LE_ICON_FORMAT_LIST_BULLETED_TYPE " Select Parent Material");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##ParentMaterial", CMaterial::StaticClass(), SelectedMaterialGUID, LE_ICON_PALETTE);

        ImGui::Spacing();
        ImGui::Separator();

        const float ButtonWidth = 110.0f;
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ButtonWidth * 2 - Spacing);

        bool bConfirm = false;
        ImGui::BeginDisabled(!SelectedMaterialGUID.IsValid());
        if (ImGui::Button(LE_ICON_CHECK " Create", ImVec2(ButtonWidth, 0)))
        {
            bConfirm = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CLOSE " Cancel", ImVec2(ButtonWidth, 0)))
        {
            SelectedMaterialGUID.Invalidate();
            bShouldClose = true;
        }

        return bConfirm;
    }

    CObject* CMaterialInstanceFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CMaterialInstance* NewInstance = NewObject<CMaterialInstance>(Package, Name);

        if (SelectedMaterialGUID.IsValid())
        {
            NewInstance->Material = Cast<CMaterial>(LoadObject<CObject>(SelectedMaterialGUID));
            // Mirror reload-time PostLoad so Parameters/MaterialIndex are populated for the editor.
            NewInstance->PostLoad();
        }

        SelectedMaterialGUID.Invalidate();
        return NewInstance;
    }
}
