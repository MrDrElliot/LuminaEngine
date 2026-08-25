#include "RuntimePCH.h"
#include "RootMotion.h"

#include "Animation/Pose.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Math/Transform.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina::RootMotion
{
    namespace
    {
        // Root bone's local (== component) transform at a clip time, translation + rotation only.
        FTransform SampleRoot(const CAnimation* Animation, FSkeletonResource* Skeleton, int32 RootIndex, float Time)
        {
            FVector3 T, S;
            FQuat R;
            Animation->SampleBoneLocal(Time, Skeleton, RootIndex, T, R, S);

            FTransform Out;
            Out.SetLocation(T);
            Out.SetRotation(R);
            Out.SetScale(FVector3(1.0f));
            return Out;
        }
    }

    int32 ResolveRootBoneIndex(const FSkeletonResource* Skeleton, const FName& NameOverride)
    {
        if (Skeleton == nullptr || Skeleton->GetNumBones() == 0)
        {
            return INDEX_NONE;
        }

        if (!NameOverride.IsNone())
        {
            const int32 Named = Skeleton->FindBoneIndex(NameOverride);
            if (Named != INDEX_NONE)
            {
                return Named;
            }
        }

        for (int32 i = 0; i < Skeleton->GetNumBones(); ++i)
        {
            if (Skeleton->GetBone(i).ParentIndex < 0)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    void PinRootToBindPose(FPose& Pose, const FSkeletonResource* Skeleton, int32 RootIndex)
    {
        if (Skeleton == nullptr || RootIndex < 0 || RootIndex >= Pose.GetNumBones())
        {
            return;
        }

        if (Skeleton->HasBindPoseCache())
        {
            Pose.SetBone(RootIndex,
                         Skeleton->BindLocalTranslations[RootIndex],
                         Skeleton->BindLocalRotations[RootIndex],
                         Skeleton->BindLocalScales[RootIndex]);
            return;
        }

        FVector3 T, S;
        FQuat R;
        AnimPose::DecomposeTRS(Skeleton->GetBone(RootIndex).LocalTransform, T, R, S);
        Pose.SetBone(RootIndex, T, R, S);
    }

    FRootMotionDelta ExtractRootDelta(const CAnimation* Animation,
                                      FSkeletonResource* Skeleton,
                                      int32 RootIndex,
                                      float PrevTime,
                                      float CurTime,
                                      bool bLooping,
                                      float Duration)
    {
        FRootMotionDelta Delta;

        if (Animation == nullptr || Skeleton == nullptr || RootIndex < 0 || Duration <= 0.0f)
        {
            return Delta;
        }

        FTransform Combined;
        Combined.SetScale(FVector3(1.0f));

        // A body-frame delta would rotate translation by the root's rest tilt, turning forward into drift.
        if (bLooping && CurTime < PrevTime)
        {
            // The playhead wrapped, so accumulate both spans and a full loop contributes the whole displacement.
            const FTransform Prev  = SampleRoot(Animation, Skeleton, RootIndex, PrevTime);
            const FTransform End   = SampleRoot(Animation, Skeleton, RootIndex, Duration);
            const FTransform Start = SampleRoot(Animation, Skeleton, RootIndex, 0.0f);
            const FTransform Cur   = SampleRoot(Animation, Skeleton, RootIndex, CurTime);

            const FTransform Seg1 = End * Prev.Inverse();
            const FTransform Seg2 = Cur * Start.Inverse();
            Combined = Seg1 * Seg2;
        }
        else
        {
            const FTransform Prev = SampleRoot(Animation, Skeleton, RootIndex, PrevTime);
            const FTransform Cur  = SampleRoot(Animation, Skeleton, RootIndex, CurTime);
            Combined = Cur * Prev.Inverse();
        }

        Delta.Translation = Combined.GetLocation();
        Delta.Rotation    = Combined.GetRotation();
        Delta.bHasMotion  = true;
        return Delta;
    }

    FRootMotionDelta BlendRootMotion(const FRootMotionDelta& A, const FRootMotionDelta& B, float Alpha)
    {
        if (!A.bHasMotion && !B.bHasMotion)
        {
            return FRootMotionDelta();
        }

        Alpha = Math::Clamp(Alpha, 0.0f, 1.0f);

        FRootMotionDelta Out;
        Out.Translation = Math::Mix(A.Translation, B.Translation, Alpha);
        Out.Rotation    = Math::Normalize(Math::Slerp(A.Rotation, B.Rotation, Alpha));
        Out.bHasMotion  = true;
        return Out;
    }
}
