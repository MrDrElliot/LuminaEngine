#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    void CSkeletalMesh::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Meshes");
        CMesh::Serialize(Ar);
    }

    void CSkeletalMesh::PostLoad()
    {
        CMesh::PostLoad();
        SkeletonJointIndicesAddress = Skeleton;
    }

    void CSkeletalMesh::PostPropertyChange(FProperty* ChangedProperty)
    {
        CMesh::PostPropertyChange(ChangedProperty);

        // Undo restores Skeleton alone, so the indices have to follow it back from here.
        RemapJointIndicesToSkeleton();
    }

    void CSkeletalMesh::RemapJointIndicesToSkeleton()
    {
        if (Skeleton == SkeletonJointIndicesAddress || Skeleton == nullptr)
        {
            return;
        }

        const FSkeletonResource* From = SkeletonJointIndicesAddress != nullptr ? SkeletonJointIndicesAddress->GetSkeletonResource() : nullptr;
        const FSkeletonResource* To   = Skeleton->GetSkeletonResource();
        if (From == nullptr || To == nullptr)
        {
            SkeletonJointIndicesAddress = Skeleton;
            return;
        }

        // Bones are parents-before-children, so an unmatched bone inherits its parent's already-resolved target.
        TVector<uint32> FromToTo(From->GetNumBones(), 0u);
        uint32 MatchedBones = 0;
        FName FirstUnmatched;
        for (int32 i = 0; i < From->GetNumBones(); ++i)
        {
            const FSkeletonResource::FBoneInfo& Bone = From->GetBone(i);
            const int32 Target = To->FindBoneIndex(Bone.Name);
            if (Target >= 0)
            {
                FromToTo[i] = (uint32)Target;
                ++MatchedBones;
                continue;
            }

            FromToTo[i] = Bone.ParentIndex >= 0 ? FromToTo[Bone.ParentIndex] : 0u;
            if (FirstUnmatched.IsNone())
            {
                FirstUnmatched = Bone.Name;
            }
        }

        // Left addressing the old skeleton, so assigning it back is still lossless.
        if (MatchedBones == 0)
        {
            LOG_WARN("[SkeletalMesh] '{}': no bone of '{}' matches '{}' by name, joint indices left unchanged; it will skin to the wrong bones.",
                     GetName(), SkeletonJointIndicesAddress->GetName(), Skeleton->GetName());
            return;
        }

        if (MatchedBones < (uint32)From->GetNumBones())
        {
            LOG_WARN("[SkeletalMesh] '{}': {} bone(s) of '{}' missing from '{}' (first: '{}'); their vertices now follow the nearest matching parent.",
                     GetName(), From->GetNumBones() - (int32)MatchedBones, SkeletonJointIndicesAddress->GetName(),
                     Skeleton->GetName(), FirstUnmatched);
        }

        TVector<FMatrix4> FromBindPose;
        TVector<FMatrix4> ToBindPose;
        SkeletonJointIndicesAddress->ComputeBindPoseSkinningMatrices(FromBindPose);
        Skeleton->ComputeBindPoseSkinningMatrices(ToBindPose);

        float WorstBindPoseDelta = 0.0f;
        uint32 WorstBone = 0;
        for (uint32& GlobalBone : GetMeshResource().MeshletData.MeshletBoneIndices)
        {
            const uint32 OldBone = GlobalBone;
            GlobalBone = OldBone < FromToTo.size() ? FromToTo[OldBone] : 0u;
            if (OldBone >= FromBindPose.size())
            {
                continue;
            }

            for (int32 Column = 0; Column < 4; ++Column)
            {
                for (int32 Row = 0; Row < 4; ++Row)
                {
                    const float Delta = Math::Abs(FromBindPose[OldBone][Column][Row] - ToBindPose[GlobalBone][Column][Row]);
                    if (Delta > WorstBindPoseDelta)
                    {
                        WorstBindPoseDelta = Delta;
                        WorstBone = OldBone;
                    }
                }
            }
        }

        // Remapping fixes bone order only; a different bind pose or import scale still deforms the mesh.
        if (WorstBindPoseDelta > 1e-3f)
        {
            LOG_WARN("[SkeletalMesh] '{}': bind pose of '{}' differs from '{}' by up to {} at bone '{}'; the mesh will deform. Import both from the same file at the same scale.",
                     GetName(), Skeleton->GetName(), SkeletonJointIndicesAddress->GetName(), WorstBindPoseDelta,
                     From->GetBone((int32)WorstBone).Name);        }

        SkeletonJointIndicesAddress = Skeleton;
        GenerateGPUBuffers();
    }
}
