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

        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), LE_ICON_DATABASE " Select Data Asset Class");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::ClassCombo("##DataAssetClass", CDataAsset::StaticClass(), SelectedClass, false, LE_ICON_DATABASE);

        ImGui::Spacing();
        ImGui::Separator();

        const float ButtonWidth = 110.0f;
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ButtonWidth * 2 - Spacing);

        bool bConfirm = false;
        ImGui::BeginDisabled(SelectedClass == nullptr);
        if (ImGui::Button(LE_ICON_CHECK " Create", ImVec2(ButtonWidth, 0)))
        {
            bConfirm = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CLOSE " Cancel", ImVec2(ButtonWidth, 0)))
        {
            SelectedClass = nullptr;
            bShouldClose = true;
        }

        return bConfirm;
    }

    CObject* CDataAssetFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        // The dialogue is the only way in, so a null selection means it was dismissed without a pick;
        // fall back to the base rather than handing back nothing.
        CClass* Class = SelectedClass != nullptr ? SelectedClass : CDataAsset::StaticClass();
        SelectedClass = nullptr;

        return NewObject(Class, Package, Name);
    }
}
