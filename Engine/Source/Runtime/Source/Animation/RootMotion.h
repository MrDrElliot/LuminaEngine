#pragma once

#include "Containers/Name.h"
#include "Core/Math/AABB.h"
#include "Core/Math/Scalar.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    class CAnimation;
    struct FPose;
    struct FSkeletonResource;

    // In the root bone's local space, so apply it to the owning entity then strip the root from the in-place pose.
    struct FRootMotionDelta
    {
        FVector3 Translation = FVector3(0.0f);
        FQuat    Rotation    = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        bool     bHasMotion  = false;
    };

    namespace RootMotion
    {
        // Capped so a hitch on a near-zero duration clip cannot spin the per-loop root motion and notify work.
        inline int32 WholeLoopsIn(float Cycles)
        {
            return (int32)Math::Clamp(Cycles, 0.0f, 1024.0f);
        }

        // Uses NameOverride when set and present, else the first bone with ParentIndex below 0.
        RUNTIME_API int32 ResolveRootBoneIndex(const FSkeletonResource* Skeleton, const FName& NameOverride);

        // Pins the root bone to its bind-pose local transform so the mesh never drifts (the "lock").
        RUNTIME_API void PinRootToBindPose(FPose& Pose, const FSkeletonResource* Skeleton, int32 RootIndex);

        // Component-space delta applied as E_new = E_prev * Delta, plus one full cycle per whole loop a looping advance spanned.
        RUNTIME_API FRootMotionDelta ExtractRootDelta(const CAnimation* Animation,
                                                      FSkeletonResource* Skeleton,
                                                      int32 RootIndex,
                                                      float PrevTime,
                                                      float CurTime,
                                                      bool bLooping,
                                                      float Duration,
                                                      int32 WholeLoops = 0);

        // Identity stands in for a motionless side so blending fades motion in and out, and bHasMotion propagates from either side.
        RUNTIME_API FRootMotionDelta BlendRootMotion(const FRootMotionDelta& A, const FRootMotionDelta& B, float Alpha);
    }
}
