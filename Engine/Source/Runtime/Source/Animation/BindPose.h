#pragma once

#include "Animation/Pose.h"
#include "Renderer/MeshData.h"

// Bind-pose access shared by the animation resource and the pose kernels. Its own header rather
// than a copy in each .cpp: the two copies were identical, and identical file-scope definitions
// compile fine one translation unit at a time but collide the moment those files share one.
// It also keeps the weight where it belongs, since neither Pose.h nor MeshData.h has to learn
// about the other to declare it.

namespace Lumina::AnimPose
{
    // A bone's bind-pose local TRS, from the SoA cache when the skeleton has one and by decomposing
    // its local matrix when it does not.
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
}
