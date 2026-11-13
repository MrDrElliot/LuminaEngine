#include "AnimPreview.h"

#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Containers/StringFormat.h"
#include "Containers/Algorithm.h"
#include "Core/Math/Math.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Renderer/SkeletonResource.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "imgui.h"

namespace Lumina
{
    namespace AnimPreview
    {
        constexpr float kMinSharedBoneFraction = 0.9f;

        bool SharesBoneNames(const FSkeletonResource& A, const FSkeletonResource& B)
        {
            const int32 SmallerBoneCount = Math::Min(A.GetNumBones(), B.GetNumBones());
            if (SmallerBoneCount == 0)
            {
                return false;
            }

            int32 SharedBones = 0;
            for (const FSkeletonResource::FBoneInfo& Bone : A.Bones)
            {
                if (B.FindBoneIndex(Bone.Name) != Constants::kIndexNone)
                {
                    ++SharedBones;
                }
            }
            return (float)SharedBones >= (float)SmallerBoneCount * kMinSharedBoneFraction;
        }

        bool AreSkeletonsCompatible(const CSkeleton& A, const CSkeleton& B)
        {
            if (&A == &B)
            {
                return true;
            }

            const FSkeletonResource* BonesA = A.GetSkeletonResource();
            const FSkeletonResource* BonesB = B.GetSkeletonResource();
            return BonesA != nullptr && BonesB != nullptr && SharesBoneNames(*BonesA, *BonesB);
        }

        bool IsAssetCompatible(const FAssetData& Data, const CSkeleton& Skeleton, THashMap<FGuid, bool>& SkeletonVerdicts)
        {
            static const FName SkeletonClassName = CSkeleton::StaticClass()->GetName();

            for (const FAssetDependency& Dependency : Data.Dependencies)
            {
                if (Dependency.TargetGUID == Skeleton.GetGUID())
                {
                    return true;
                }

                const FAssetDataPtr Target = FAssetRegistry::Get().GetAssetByGUID(Dependency.TargetGUID);
                if (Target == nullptr || Target->AssetClass != SkeletonClassName)
                {
                    continue;
                }

                const bool* bVerdict = SkeletonVerdicts.Find(Dependency.TargetGUID);
                if (bVerdict == nullptr)
                {
                    const CSkeleton* Candidate = Cast<CSkeleton>(LoadObject<CObject>(Dependency.TargetGUID));
                    bVerdict = &SkeletonVerdicts.Add(Dependency.TargetGUID, Candidate != nullptr && AreSkeletonsCompatible(*Candidate, Skeleton));
                }

                if (*bVerdict)
                {
                    return true;
                }
            }
            return false;
        }
    }

    FCompatibleAssetList::FCompatibleAssetList(CClass* InAssetClass)
        : AssetClass(InAssetClass)
    {
    }


    const TVector<FCompatibleAssetList::FEntry>& FCompatibleAssetList::Get(const CSkeleton* Skeleton)
    {
        if (!StaleLatch.Refresh(Skeleton, CachedSkeleton))
        {
            return Entries;
        }

        Entries.Reset();
        if (Skeleton == nullptr || AssetClass == nullptr)
        {
            return Entries;
        }

        const FName ClassName = AssetClass->GetName();
        TVector<FAssetDataPtr> Candidates = FAssetRegistry::Get().FindByPredicate([&ClassName](const FAssetData& Data)
        {
            return Data.AssetClass == ClassName;
        });

        THashMap<FGuid, bool> SkeletonVerdicts;
        for (const FAssetDataPtr& Data : Candidates)
        {
            if (AnimPreview::IsAssetCompatible(*Data, *Skeleton, SkeletonVerdicts))
            {
                Entries.Emplace(Data->AssetGUID, Data->AssetName.ToString());
            }
        }

        Algo::Sort(Entries, [](const FEntry& A, const FEntry& B) { return A.Name < B.Name; });
        return Entries;
    }

    FPreviewMeshSelection::FPreviewMeshSelection()
        : Meshes(CSkeletalMesh::StaticClass())
    {
    }

    CSkeletalMesh* FPreviewMeshSelection::Resolve(const CSkeleton* Skeleton) const
    {
        if (Override.IsValid())
        {
            return Override.Get();
        }
        return Skeleton != nullptr ? Skeleton->PreviewMesh.Get() : nullptr;
    }

    bool FPreviewMeshSelection::DrawMenu(CSkeleton* Skeleton)
    {
        if (!ImGui::BeginMenu(LE_ICON_HUMAN " Preview Mesh"))
        {
            return false;
        }

        if (Skeleton == nullptr)
        {
            ImGui::TextDisabled("Assign a skeleton to pick a preview mesh.");
            ImGui::EndMenu();
            return false;
        }

        CSkeletalMesh* const Previous = Resolve(Skeleton);
        CSkeletalMesh* const SkeletonDefault = Skeleton->PreviewMesh.Get();

        const FString DefaultLabel = SkeletonDefault != nullptr
            ? Format("Skeleton Default ({})", *SkeletonDefault->GetName())
            : FString("Skeleton Default (none)");
        if (ImGui::MenuItem(*DefaultLabel, nullptr, !Override.IsValid()))
        {
            Override = nullptr;
        }

        ImGui::Separator();

        const TVector<FCompatibleAssetList::FEntry>& Entries = Meshes.Get(Skeleton);
        if (Entries.IsEmpty())
        {
            ImGui::TextDisabled("No skeletal meshes match this skeleton's bones.");
        }

        for (const FCompatibleAssetList::FEntry& Entry : Entries)
        {
            const bool bSelected = Override.IsValid() && Override->GetGUID() == Entry.GUID;
            if (ImGui::MenuItem(*Entry.Name, nullptr, bSelected) && !bSelected)
            {
                if (CSkeletalMesh* Picked = Cast<CSkeletalMesh>(LoadObject<CObject>(Entry.GUID)))
                {
                    Override = Picked;
                }
            }
        }

        ImGui::Separator();

        const bool bCanPromote = Override.IsValid() && Override.Get() != SkeletonDefault;
        if (ImGui::MenuItem(LE_ICON_CONTENT_SAVE " Set as Skeleton Default", nullptr, false, bCanPromote))
        {
            Skeleton->PreviewMesh = Override.Get();
            if (CPackage* Package = Skeleton->GetPackage())
            {
                Package->MarkDirty();
            }
            Override = nullptr;
        }
        ImGuiX::TextTooltip("{}", "Store this mesh on the skeleton, so every editor using the skeleton opens with it.");

        ImGui::EndMenu();
        return Resolve(Skeleton) != Previous;
    }
}
