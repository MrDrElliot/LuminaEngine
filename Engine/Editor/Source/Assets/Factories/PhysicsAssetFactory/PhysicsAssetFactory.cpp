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

        DrawCreationDialogueTitle(LE_ICON_BONE " Select Skeleton");

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##Skeleton", CSkeleton::StaticClass(), SelectedSkeletonGUID, LE_ICON_BONE);

        const bool bConfirm = DrawCreationDialogueButtons(SelectedSkeletonGUID.IsValid(), bShouldClose);
        if (bShouldClose)
        {
            SelectedSkeletonGUID.Invalidate();
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
