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

        DrawCreationDialogueTitle(LE_ICON_FORMAT_LIST_BULLETED_TYPE " Select Parent Material");

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##ParentMaterial", CMaterialInterface::StaticClass(), SelectedMaterialGUID, LE_ICON_PALETTE);

        const bool bConfirm = DrawCreationDialogueButtons(SelectedMaterialGUID.IsValid(), bShouldClose);
        if (bShouldClose)
        {
            SelectedMaterialGUID.Invalidate();
        }

        return bConfirm;
    }

    CObject* CMaterialInstanceFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CMaterialInstance* NewInstance = NewObject<CMaterialInstance>(Package, Name);

        if (SelectedMaterialGUID.IsValid())
        {
            NewInstance->SetParentMaterial(Cast<CMaterialInterface>(LoadObject<CObject>(SelectedMaterialGUID)));
            // Mirror reload-time PostLoad so Parameters/MaterialIndex are populated for the editor.
            NewInstance->PostLoad();
        }

        SelectedMaterialGUID.Invalidate();
        return NewInstance;
    }
}
