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

        DrawCreationDialogueTitle(LE_ICON_BONE " Select Skeleton");

        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##Skeleton", CSkeleton::StaticClass(), SelectedSkeletonGUID, LE_ICON_BONE);

        ImGui::Spacing();
        ImGui::TextUnformatted("First Clip (optional)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGuiX::AssetReferenceCombo("##Animation", CAnimation::StaticClass(), SelectedAnimationGUID, LE_ICON_ANIMATION);

        const bool bConfirm = DrawCreationDialogueButtons(SelectedSkeletonGUID.IsValid(), bShouldClose);
        if (bShouldClose)
        {
            SelectedSkeletonGUID.Invalidate();
            SelectedAnimationGUID.Invalidate();
        }

        return bConfirm;
    }

    void CAnimationMontageFactory::InitializeMontage(CAnimationMontage& Montage, CSkeleton* Skeleton, CAnimation* FirstClip)
    {
        Montage.Skeleton = Skeleton;

        SAnimMontageSlotTrack& Track = Montage.SlotTracks.EmplaceGetRef();
        Track.SlotName = "DefaultSlot";

        if (FirstClip != nullptr)
        {
            SAnimMontageSegment& Segment = Track.Segments.EmplaceGetRef();
            Segment.Animation = FirstClip;
        }

        SAnimMontageSection& Section = Montage.Sections.EmplaceGetRef();
        Section.Name = "Default";
        Section.StartTime = 0.0f;

        Montage.EnsureNotifyTracks();
    }

    CObject* CAnimationMontageFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CAnimationMontage* NewMontage = NewObject<CAnimationMontage>(Package, Name);

        CSkeleton* Skeleton = SelectedSkeletonGUID.IsValid() ? Cast<CSkeleton>(LoadObject<CObject>(SelectedSkeletonGUID)) : nullptr;
        CAnimation* Animation = SelectedAnimationGUID.IsValid() ? Cast<CAnimation>(LoadObject<CObject>(SelectedAnimationGUID)) : nullptr;
        InitializeMontage(*NewMontage, Skeleton, Animation);

        SelectedSkeletonGUID.Invalidate();
        SelectedAnimationGUID.Invalidate();
        return NewMontage;
    }
}
