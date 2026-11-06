#include "EditorPCH.h"
#include "DataAssetFactory.h"

#include "Assets/AssetTypes/DataAsset/DataAsset.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "imgui.h"

namespace Lumina
{
    bool CDataAssetFactory::HasCreationDialogue() const
    {
        return true;
    }

    bool CDataAssetFactory::DrawCreationDialogue(FStringView Path, bool& bShouldClose)
    {
        if (ImGui::IsWindowAppearing())
        {
            SelectedClass = nullptr;
        }

        DrawCreationDialogueTitle(LE_ICON_DATABASE " Select Data Asset Class");

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::ClassCombo("##DataAssetClass", CDataAsset::StaticClass(), SelectedClass, false, LE_ICON_DATABASE);

        const bool bConfirm = DrawCreationDialogueButtons(SelectedClass != nullptr, bShouldClose);
        if (bShouldClose)
        {
            SelectedClass = nullptr;
        }

        return bConfirm;
    }

    CObject* CDataAssetFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        // The dialogue is the only way in, so a null selection means it was dismissed without a pick.
        CClass* Class = SelectedClass != nullptr ? SelectedClass : CDataAsset::StaticClass();
        SelectedClass = nullptr;

        return NewObject(Class, Package, Name);
    }
}
