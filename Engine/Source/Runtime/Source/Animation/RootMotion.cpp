#include "RuntimePCH.h"
#include "RootMotion.h"

#include "Animation/Pose.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Math/Transform.h"
#include "Renderer/MeshData.h"

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
            Pose.Translations[RootIndex] = Skeleton->BindLocalTranslations[RootIndex];
            Pose.Rotations[RootIndex]    = Skeleton->BindLocalRotations[RootIndex];
            Pose.Scales[RootIndex]       = Skeleton->BindLocalScales[RootIndex];
            return;
        }

        AnimPose::DecomposeTRS(Skeleton->GetBone(RootIndex).LocalTransform,
                               Pose.Translations[RootIndex], Pose.Rotations[RootIndex], Pose.Scales[RootIndex]);
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

        // Component-space (left/fixed-frame) delta D = M_cur * M_prev^-1. The entity then absorbs it as
        // E_new = E_prev * D, reproducing the root's world motion. Using the body-frame delta
        // (M_prev^-1 * M_cur) instead would rotate the translation by the root bone's rest orientation --
        // DCC exports often bake a tilt into the root -- turning forward motion into vertical drift.
        if (bLooping && CurTime < PrevTime)
        {
            // Playhead wrapped this frame: accumulate [Prev, Duration] then [0, Cur] so a full
            // loop contributes the clip's whole root displacement.
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

    FRootMotionDelta RootMotion::BlendRootMotion(const FRootMotionDelta& A, const FRootMotionDelta& B, float Alpha)
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
