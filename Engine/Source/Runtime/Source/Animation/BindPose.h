#pragma once

#include "Animation/Pose.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"

// Shared by the animation resource and the pose kernels, since identical file-scope copies collide in a unity build.

namespace Lumina::AnimPose
{
    // Reads the SoA cache when the skeleton has one, otherwise decomposes the local matrix.
    FORCEINLINE void GetBindLocalTRS(
        const FSkeletonResource* Skeleton, int32 BoneIndex, FVector3& OutT, FQuat& OutR, FVector3& OutS)
    {
        if (Skeleton->HasBindPoseCache())
        {
            OutT = Skeleton->BindLocalTranslations[BoneIndex];
            OutR = Skeleton->BindLocalRotations[BoneIndex];
            OutS = Skeleton->BindLocalScales[BoneIndex];
        }
        else
        {
            DecomposeTRS(Skeleton->GetBone(BoneIndex).LocalTransform, OutT, OutR, OutS);
        }
    }

    // A bone's bind-pose global matrix, from the SoA cache or by inverting its inverse-bind matrix.
    FORCEINLINE FMatrix4 GetBindGlobalMatrix(const FSkeletonResource* Skeleton, int32 BoneIndex)
    {
        return Skeleton->HasBindGlobalMatrices()
            ? Skeleton->BindGlobalMatrices[BoneIndex]
            : Math::Inverse(Skeleton->GetBone(BoneIndex).InvBindMatrix);
    }
}
