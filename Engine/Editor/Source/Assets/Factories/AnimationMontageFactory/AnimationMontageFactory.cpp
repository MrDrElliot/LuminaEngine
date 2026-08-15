#include "EditorPCH.h"
#include "AnimationMontageFactory.h"

#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "imgui.h"

namespace Lumina
{
    bool CAnimationMontageFactory::HasCreationDialogue() const
    {
        return true;
    }

    bool CAnimationMontageFactory::DrawCreationDialogue(FStringView Path, bool& bShouldClose)
    {
        if (ImGui::IsWindowAppearing())
        {
            SelectedSkeletonGUID.Invalidate();
            SelectedAnimationGUID.Invalidate();
        }

        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), LE_ICON_BONE " Select Skeleton");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##Skeleton", CSkeleton::StaticClass(), SelectedSkeletonGUID, LE_ICON_BONE);

        ImGui::Spacing();
        ImGui::TextUnformatted("First Clip (optional)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##Animation", CAnimation::StaticClass(), SelectedAnimationGUID, LE_ICON_ANIMATION);

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
            SelectedAnimationGUID.Invalidate();
            bShouldClose = true;
        }

        return bConfirm;
    }

    CObject* CAnimationMontageFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CAnimationMontage* NewMontage = NewObject<CAnimationMontage>(Package, Name);

        if (SelectedSkeletonGUID.IsValid())
        {
            if (CSkeleton* Skeleton = Cast<CSkeleton>(LoadObject<CObject>(SelectedSkeletonGUID)))
            {
                NewMontage->Skeleton = Skeleton;
            }
        }

        SAnimMontageSlotTrack& Track = NewMontage->SlotTracks.emplace_back();
        Track.SlotName = "DefaultSlot";

        if (SelectedAnimationGUID.IsValid())
        {
            if (CAnimation* Animation = Cast<CAnimation>(LoadObject<CObject>(SelectedAnimationGUID)))
            {
                SAnimMontageSegment& Segment = Track.Segments.emplace_back();
                Segment.Animation = Animation;
            }
        }

        SAnimMontageSection& Section = NewMontage->Sections.emplace_back();
        Section.Name = "Default";
        Section.StartTime = 0.0f;

        NewMontage->EnsureNotifyTracks();

        SelectedSkeletonGUID.Invalidate();
        SelectedAnimationGUID.Invalidate();
        return NewMontage;
    }
}
