#include "EditorPCH.h"
#include "PhysicsAssetFactory.h"

#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "imgui.h"

namespace Lumina
{
    bool CPhysicsAssetFactory::HasCreationDialogue() const
    {
        return true;
    }

    bool CPhysicsAssetFactory::DrawCreationDialogue(FStringView Path, bool& bShouldClose)
    {
        if (ImGui::IsWindowAppearing())
        {
            SelectedSkeletonGUID.Invalidate();
        }

        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), LE_ICON_BONE " Select Skeleton");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##Skeleton", CSkeleton::StaticClass(), SelectedSkeletonGUID, LE_ICON_BONE);

        ImGui::Spacing();
        ImGui::Separator();

        const float ButtonWidth = 110.0f;
        const float Spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ButtonWidth * 2 - Spacing);

        bool bConfirm = false;
        ImGui::BeginDisabled(!SelectedSkeletonGUID.IsValid());
        if (ImGui::Button(LE_ICON_CHECK " Create", ImVec2(ButtonWidth, 0)))
        {
            bConfirm = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_CLOSE " Cancel", ImVec2(ButtonWidth, 0)))
        {
            SelectedSkeletonGUID.Invalidate();
            bShouldClose = true;
        }

        return bConfirm;
    }

    CObject* CPhysicsAssetFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CPhysicsAsset* NewPhysicsAsset = NewObject<CPhysicsAsset>(Package, Name);

        if (SelectedSkeletonGUID.IsValid())
        {
            if (CSkeleton* Skeleton = Cast<CSkeleton>(LoadObject<CObject>(SelectedSkeletonGUID)))
            {
                NewPhysicsAsset->Skeleton = Skeleton;
            }
        }

        SelectedSkeletonGUID.Invalidate();
        return NewPhysicsAsset;
    }
}
