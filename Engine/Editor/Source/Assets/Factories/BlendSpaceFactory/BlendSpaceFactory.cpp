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

        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), LE_ICON_BONE " Select Skeleton");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##Skeleton", CSkeleton::StaticClass(), SelectedSkeletonGUID, LE_ICON_BONE);

        ImGui::Spacing();
        ImGui::TextUnformatted("Axes");
        ImGui::RadioButton("One (speed)", &SelectedAxisCount, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Two (speed + direction)", &SelectedAxisCount, 2);

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
