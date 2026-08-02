#include "EditorPCH.h"
#include "DataAssetFactory.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/DataAsset/DataAsset.h"
#include "Assets/AssetTypes/DataAsset/DataAssetSchema.h"
#include "Core/Object/Cast.h"
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
            SelectedSchemaGUID.Invalidate();
        }

        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), LE_ICON_DATABASE " Select Data Asset Schema");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##Schema", CDataAssetSchema::StaticClass(), SelectedSchemaGUID, LE_ICON_DATABASE);

        ImGui::Spacing();
        ImGui::Separator();

        const float ButtonWidth = 110.0f;
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ButtonWidth * 2 - Spacing);

        bool bConfirm = false;
        ImGui::BeginDisabled(!SelectedSchemaGUID.IsValid());
        if (ImGui::Button(LE_ICON_CHECK " Create", ImVec2(ButtonWidth, 0)))
        {
            bConfirm = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CLOSE " Cancel", ImVec2(ButtonWidth, 0)))
        {
            SelectedSchemaGUID.Invalidate();
            bShouldClose = true;
        }

        return bConfirm;
    }

    CObject* CDataAssetFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CDataAsset* NewDataAsset = NewObject<CDataAsset>(Package, Name);

        if (SelectedSchemaGUID.IsValid())
        {
            if (CDataAssetSchema* Schema = Cast<CDataAssetSchema>(LoadObject<CObject>(SelectedSchemaGUID)))
            {
                NewDataAsset->SetSchema(Schema);
            }
        }

        SelectedSchemaGUID.Invalidate();
        return NewDataAsset;
    }
}
