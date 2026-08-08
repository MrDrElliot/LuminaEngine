#pragma once

#include "Containers/Name.h"
#include "Core/Math/AABB.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    class CAnimation;
    struct FPose;
    struct FSkeletonResource;

    // One frame of root-bone motion extracted from a clip, expressed in the root bone's local
    // (component) space. Apply to the owning entity, then strip the root from the in-place pose.
    struct FRootMotionDelta
    {
        FVector3 Translation = FVector3(0.0f);
        FQuat    Rotation    = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        bool     bHasMotion  = false;
    };

    namespace RootMotion
    {
        // Resolves the motion root bone: the named bone if NameOverride is set and present, else the
        // first bone with ParentIndex < 0. Returns INDEX_NONE when the skeleton has no root.
        RUNTIME_API int32 ResolveRootBoneIndex(const FSkeletonResource* Skeleton, const FName& NameOverride);

        // Pins the root bone to its bind-pose local transform so the mesh never drifts (the "lock").
        RUNTIME_API void PinRootToBindPose(FPose& Pose, const FSkeletonResource* Skeleton, int32 RootIndex);

        // Extracts the root bone's motion between PrevTime and CurTime (loop-wrap aware), returning the
        // component-space (fixed-frame) translation + rotation delta to apply as E_new = E_prev * Delta.
        // Does not modify the pose; callers strip the root afterward via PinRootToBindPose. Scale is
        // intentionally not extracted.
        RUNTIME_API FRootMotionDelta ExtractRootDelta(const CAnimation* Animation,
                                                      FSkeletonResource* Skeleton,
                                                      int32 RootIndex,
                                                      float PrevTime,
                                                      float CurTime,
                                                      bool bLooping,
                                                      float Duration);

        // Blends two deltas by Alpha (translation lerp, rotation slerp); identity stands in for a
        // motionless side so blending fades motion in/out. bHasMotion propagates if either side has
        // it, marking the branch as root-motion driven through the graph.
        RUNTIME_API FRootMotionDelta BlendRootMotion(const FRootMotionDelta& A, const FRootMotionDelta& B, float Alpha);
    }
}
