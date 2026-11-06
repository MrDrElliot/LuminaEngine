#include "EditorPCH.h"
#include "BlendSpaceFactory.h"

#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "imgui.h"

namespace Lumina
{
    bool CBlendSpaceFactory::HasCreationDialogue() const
    {
        return true;
    }

    bool CBlendSpaceFactory::DrawCreationDialogue(FStringView Path, bool& bShouldClose)
    {
        if (ImGui::IsWindowAppearing())
        {
            SelectedSkeletonGUID.Invalidate();
            SelectedAxisCount = 2;
        }

        DrawCreationDialogueTitle(LE_ICON_BONE " Select Skeleton");

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##Skeleton", CSkeleton::StaticClass(), SelectedSkeletonGUID, LE_ICON_BONE);

        ImGui::Spacing();
        ImGui::TextUnformatted("Axes");
        ImGui::RadioButton("One (speed)", &SelectedAxisCount, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Two (speed + direction)", &SelectedAxisCount, 2);

        const bool bConfirm = DrawCreationDialogueButtons(SelectedSkeletonGUID.IsValid(), bShouldClose);
        if (bShouldClose)
        {
            SelectedSkeletonGUID.Invalidate();
        }

        return bConfirm;
    }

    CObject* CBlendSpaceFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CBlendSpace* NewBlendSpace = NewObject<CBlendSpace>(Package, Name);

        if (SelectedSkeletonGUID.IsValid())
        {
            if (CSkeleton* Skeleton = Cast<CSkeleton>(LoadObject<CObject>(SelectedSkeletonGUID)))
            {
                NewBlendSpace->Skeleton = Skeleton;
            }
        }

        NewBlendSpace->AxisCount = (SelectedAxisCount == 1) ? EBlendSpaceAxes::One : EBlendSpaceAxes::Two;

        // Defaults that read as locomotion, which is what almost every blend space is for.
        NewBlendSpace->AxisX.Name = "Speed";
        NewBlendSpace->AxisX.Min = 0.0f;
        NewBlendSpace->AxisX.Max = 600.0f;
        NewBlendSpace->AxisY.Name = "Direction";
        NewBlendSpace->AxisY.Min = -180.0f;
        NewBlendSpace->AxisY.Max = 180.0f;

        SelectedSkeletonGUID.Invalidate();
        return NewBlendSpace;
    }
}
