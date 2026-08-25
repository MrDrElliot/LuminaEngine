#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "Animation.h"

#include "Animation/BindPose.h"
#include "Animation/Pose.h"
#include "Memory/Memcpy.h"
#include "Renderer/MeshData.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"


namespace Lumina
{
    namespace Detail
    {
        static constexpr uint8 TouchedT = 1u << 0;
        static constexpr uint8 TouchedR = 1u << 1;
        static constexpr uint8 TouchedS = 1u << 2;
        static constexpr uint8 TouchedAll = TouchedT | TouchedR | TouchedS;

        struct FDecodedBone
        {
            FVector3 T;
            FQuat R;
            FVector3 S;
            uint8 Touched;
        };

        static FDecodedBone DecodeBone(const FCompressedAnimData& Data, const FCompressedAnimBone& Bone,
                                       uint32 Frame0, uint32 Frame1, float Alpha)
        {
            FDecodedBone Out;
            Out.Touched = 0;

            if (Bone.Translation.Format != EAnimTrackFormat::None)
            {
                Out.T = Data.DecodeTranslation(Bone.Translation, Frame0, Frame1, Alpha);
                Out.Touched |= TouchedT;
            }
            if (Bone.Rotation.Format != EAnimTrackFormat::None)
            {
                Out.R = Data.DecodeRotation(Bone.Rotation, Frame0, Frame1, Alpha);
                Out.Touched |= TouchedR;
            }
            if (Bone.Scale.Format != EAnimTrackFormat::None)
            {
                Out.S = Data.DecodeScale(Bone.Scale, Frame0, Frame1, Alpha);
                Out.Touched |= TouchedS;
            }

            return Out;
        }
    }

    const FAnimationResource::FResolvedSkeleton* FAnimationResource::GetResolvedSkeleton(const FSkeletonResource* Skeleton)
    {
        const FResolvedSkeleton* Active = ActiveResolvedSkeleton.load(std::memory_order_acquire);
        if (Active && Active->Skeleton == Skeleton && Active->Generation == Skeleton->BindPoseGeneration)
        {
            return Active;
        }

        FScopeLock Lock(ResolveMutex);

        for (const TUniquePtr<FResolvedSkeleton>& Resolved : ResolvedSkeletons)
        {
            if (Resolved->Skeleton == Skeleton && Resolved->Generation == Skeleton->BindPoseGeneration)
            {
                ActiveResolvedSkeleton.store(Resolved.get(), std::memory_order_release);
                return Resolved.get();
            }
        }

        TUniquePtr<FResolvedSkeleton> NewSet = MakeUnique<FResolvedSkeleton>();
        NewSet->Skeleton   = Skeleton;
        NewSet->Generation = Skeleton->BindPoseGeneration;

        const int32 NumBones = Skeleton->GetNumBones();
        NewSet->SkeletonToCompressed.assign(NumBones, INDEX_NONE);
        NewSet->CompressedBones.reserve(Compressed.Bones.size());

        int32 NumUnmatched = 0;
        for (int32 i = 0; i < (int32)Compressed.Bones.size(); ++i)
        {
            const int32 BoneIndex = Skeleton->FindBoneIndex(Compressed.Bones[i].BoneName);
            NumUnmatched += BoneIndex < 0 ? 1 : 0;
            NewSet->CompressedBones.push_back(BoneIndex);

            if (BoneIndex >= 0 && BoneIndex < NumBones)
            {
                NewSet->SkeletonToCompressed[BoneIndex] = i;
            }
        }

        // Unmatched bones silently freeze at bind pose, the telltale of the wrong skeleton.
        if (NumUnmatched > 0)
        {
            LOG_WARN("Animation '{}': {}/{} bones are missing from the skeleton (name mismatch or wrong skeleton)",
                     Name.c_str(), NumUnmatched, (int32)Compressed.Bones.size());
        }

        const FResolvedSkeleton* Result = NewSet.get();
        ResolvedSkeletons.push_back(std::move(NewSet));
        ActiveResolvedSkeleton.store(Result, std::memory_order_release);
        return Result;
    }

    void FAnimationResource::InvalidateResolvedSkeletons()
    {
        FScopeLock Lock(ResolveMutex);
        ActiveResolvedSkeleton.store(nullptr, std::memory_order_release);
        ResolvedSkeletons.clear();
    }

    CAnimation::CAnimation()
        : AnimationResource(MakeUnique<FAnimationResource>())
    {
    }

    void CAnimation::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Animation");
        CObject::Serialize(Ar);

        if (!AnimationResource)
        {
            AnimationResource = MakeUnique<FAnimationResource>();
        }

        Ar << *AnimationResource;

        if (Ar.IsReading())
        {
            // Clips saved before the cutover still carry channels; compressing here spares a re-import.
            if (!AnimationResource->Compressed.IsValid() && !AnimationResource->Channels.empty())
            {
                AnimCompression::Build(*AnimationResource);
            }

            AnimationResource->Channels.clear();
            AnimationResource->Channels.shrink_to_fit();
            AnimationResource->InvalidateResolvedSkeletons();
        }
    }

    int32 CAnimation::FindCurveIndex(const FName& CurveName) const
    {
        const TVector<FAnimationCurve>& Curves = AnimationResource->Curves;
        for (int32 i = 0; i < (int32)Curves.size(); ++i)
        {
            if (Curves[i].Name == CurveName)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    float CAnimation::EvaluateCurve(const FName& CurveName, float Time, float Default) const
    {
        const int32 Index = FindCurveIndex(CurveName);
        return Index != INDEX_NONE ? AnimationResource->Curves[Index].Curve.Evaluate(Time) : Default;
    }

    void CAnimation::SamplePose(float Time, FSkeletonResource* RESTRICT InSkeleton, TVector<FMatrix4>& RESTRICT OutBoneTransforms) const
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumBones = InSkeleton->GetNumBones();
        OutBoneTransforms.resize(NumBones);

        if (NumBones == 0)
        {
            return;
        }

        // Per-thread scratch reused across frames; thread_local required since SamplePose runs in ParallelFor.
        thread_local TVector<FVector3> ScratchT;
        thread_local TVector<FQuat> ScratchR;
        thread_local TVector<FVector3> ScratchS;
        thread_local TVector<uint8>     ScratchTouched;

        if ((int32)ScratchT.size() < NumBones)
        {
            ScratchT.resize(NumBones);
            ScratchR.resize(NumBones);
            ScratchS.resize(NumBones);
            ScratchTouched.resize(NumBones);
        }

        Memory::Memset(ScratchTouched.data(), 0, (size_t)NumBones * sizeof(uint8));

        // Pass 1 gathers per-bone TRS overrides with bone indices pre-resolved.
        const FAnimationResource::FResolvedSkeleton* Resolved = AnimationResource->GetResolvedSkeleton(InSkeleton);
        const FCompressedAnimData& Compressed = AnimationResource->Compressed;

        uint32 Frame0, Frame1;
        float Alpha;
        Compressed.GetFrameBlend(Time, AnimationResource->Duration, Frame0, Frame1, Alpha);

        for (SIZE_T b = 0; b < Compressed.Bones.size(); ++b)
        {
            const int32 BoneIdx = Resolved->CompressedBones[b];
            if (BoneIdx < 0 || BoneIdx >= NumBones)
            {
                continue;
            }

            const Detail::FDecodedBone Decoded = Detail::DecodeBone(Compressed, Compressed.Bones[b], Frame0, Frame1, Alpha);
            ScratchT[BoneIdx]       = Decoded.T;
            ScratchR[BoneIdx]       = Decoded.R;
            ScratchS[BoneIdx]       = Decoded.S;
            ScratchTouched[BoneIdx] = Decoded.Touched;
        }

        // Bones[] is parents-before-children, so local matrices fuse with FK in one linear pass.
        for (int32 i = 0; i < NumBones; ++i)
        {
            const FSkeletonResource::FBoneInfo& Bone = InSkeleton->GetBone(i);
            const uint8 Touched = ScratchTouched[i];

            FMatrix4 Local;
            if (Touched == 0)
            {
                Local = Bone.LocalTransform;
            }
            else
            {
                FVector3 T, S;
                FQuat R;
                if (Touched == Detail::TouchedAll)
                {
                    T = ScratchT[i];
                    R = ScratchR[i];
                    S = ScratchS[i];
                }
                else
                {
                    AnimPose::GetBindLocalTRS(InSkeleton, i, T, R, S);
                    if (Touched & Detail::TouchedT) T = ScratchT[i];
                    if (Touched & Detail::TouchedR) R = ScratchR[i];
                    if (Touched & Detail::TouchedS) S = ScratchS[i];
                }
                Local = AnimPose::ComposeTRS(T, R, S);
            }

            OutBoneTransforms[i] = Bone.ParentIndex != INDEX_NONE ? OutBoneTransforms[Bone.ParentIndex] * Local : Local;
        }

        // Pass 3 folds in InvBind to produce the GPU skinning matrix.
        for (int32 i = 0; i < NumBones; ++i)
        {
            OutBoneTransforms[i] = OutBoneTransforms[i] * InSkeleton->GetBone(i).InvBindMatrix;
        }
    }

    CAnimation* CAnimation::GetAdditiveBaseAnimation() const
    {
        if (AdditiveBasePoseType == EAdditiveBasePoseType::RefPose)
        {
            return nullptr;
        }

        CAnimation* Base = AdditiveBaseAnimation.Get();
        return Base != this ? Base : nullptr;
    }

    void CAnimation::SampleLocalPose(float Time, FSkeletonResource* RESTRICT InSkeleton, FPose& RESTRICT OutPose, int32 MaxBones) const
    {
        if (IsAdditive())
        {
            SampleAdditiveDelta(Time, InSkeleton, OutPose, MaxBones);
            return;
        }

        SampleRawLocalPose(Time, InSkeleton, OutPose, MaxBones);
    }

    float CAnimation::GetAdditiveBaseTime(float Time) const
    {
        const CAnimation* BaseClip = GetAdditiveBaseAnimation();
        if (BaseClip == nullptr || AdditiveBasePoseType != EAdditiveBasePoseType::AnimScaled)
        {
            return AdditiveBaseFrameTime;
        }

        const float Duration = AnimationResource->Duration;
        return Duration > 0.0f ? (Time / Duration) * BaseClip->GetDuration() : 0.0f;
    }

    void CAnimation::SampleAdditiveBasePose(float Time, FSkeletonResource* RESTRICT InSkeleton, FPose& RESTRICT OutBase, int32 MaxBones) const
    {
        const CAnimation* BaseClip = GetAdditiveBaseAnimation();
        if (BaseClip == nullptr)
        {
            OutBase.ResetToBindPose(InSkeleton);
            return;
        }

        BaseClip->SampleRawLocalPose(GetAdditiveBaseTime(Time), InSkeleton, OutBase, MaxBones);
    }

    void CAnimation::SampleAdditiveDelta(float Time, FSkeletonResource* RESTRICT InSkeleton, FPose& RESTRICT OutDelta, int32 MaxBones) const
    {
        LUMINA_PROFILE_SCOPE();

        // Per-thread scratch, since the executor samples inside a ParallelFor.
        thread_local FPose SourceScratch;
        thread_local FPose BaseScratch;

        SampleRawLocalPose(Time, InSkeleton, SourceScratch, MaxBones);

        const bool bMeshSpace = AdditiveAnimType == EAdditiveAnimType::MeshSpace;
        if (GetAdditiveBaseAnimation() == nullptr)
        {
            if (bMeshSpace)
            {
                AnimPose::MakeAdditiveMeshSpace(SourceScratch, InSkeleton, OutDelta, MaxBones);
            }
            else
            {
                AnimPose::MakeAdditive(SourceScratch, InSkeleton, OutDelta, MaxBones);
            }
            return;
        }

        SampleAdditiveBasePose(Time, InSkeleton, BaseScratch, MaxBones);

        if (bMeshSpace)
        {
            AnimPose::MakeAdditiveMeshSpace(SourceScratch, BaseScratch, InSkeleton, OutDelta, MaxBones);
        }
        else
        {
            AnimPose::MakeAdditiveFromBase(SourceScratch, BaseScratch, OutDelta, MaxBones);
        }
    }

    void CAnimation::SampleRawLocalPose(float Time, FSkeletonResource* RESTRICT InSkeleton, FPose& RESTRICT OutPose, int32 MaxBones) const
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumBones = InSkeleton->GetNumBones();
        OutPose.SetNumBones(NumBones);
        OutPose.AdditiveSpace = EPoseAdditiveSpace::None;

        if (NumBones == 0)
        {
            return;
        }

        const int32 ActiveBones = (MaxBones >= 0 && MaxBones < NumBones) ? MaxBones : NumBones;

        // Three bulk copies instead of a per-bone decompose, and the LOD tail keeps its bind-pose locals.
        OutPose.ResetToBindPose(InSkeleton);

        const FAnimationResource::FResolvedSkeleton* Resolved = AnimationResource->GetResolvedSkeleton(InSkeleton);
        const FCompressedAnimData& Compressed = AnimationResource->Compressed;

        uint32 Frame0, Frame1;
        float Alpha;
        Compressed.GetFrameBlend(Time, AnimationResource->Duration, Frame0, Frame1, Alpha);

        for (SIZE_T b = 0; b < Compressed.Bones.size(); ++b)
        {
            const int32 BoneIdx = Resolved->CompressedBones[b];
            if (BoneIdx < 0 || BoneIdx >= ActiveBones)
            {
                continue;
            }

            const Detail::FDecodedBone Decoded = Detail::DecodeBone(Compressed, Compressed.Bones[b], Frame0, Frame1, Alpha);
            if (Decoded.Touched & Detail::TouchedT) OutPose.SetTranslation(BoneIdx, Decoded.T);
            if (Decoded.Touched & Detail::TouchedR) OutPose.SetRotation(BoneIdx, Decoded.R);
            if (Decoded.Touched & Detail::TouchedS) OutPose.SetScale(BoneIdx, Decoded.S);
        }
    }

    void CAnimation::SampleBoneLocal(float Time, FSkeletonResource* RESTRICT InSkeleton, int32 BoneIndex,
                                     FVector3& OutT, FQuat& OutR, FVector3& OutS) const
    {
        AnimPose::GetBindLocalTRS(InSkeleton, BoneIndex, OutT, OutR, OutS);

        const FAnimationResource::FResolvedSkeleton* Resolved = AnimationResource->GetResolvedSkeleton(InSkeleton);
        const FCompressedAnimData& Compressed = AnimationResource->Compressed;

        if (BoneIndex < 0 || BoneIndex >= (int32)Resolved->SkeletonToCompressed.size())
        {
            return;
        }

        const int32 CompressedIndex = Resolved->SkeletonToCompressed[BoneIndex];
        if (CompressedIndex == INDEX_NONE)
        {
            return;
        }

        uint32 Frame0, Frame1;
        float Alpha;
        Compressed.GetFrameBlend(Time, AnimationResource->Duration, Frame0, Frame1, Alpha);

        const Detail::FDecodedBone Decoded = Detail::DecodeBone(Compressed, Compressed.Bones[CompressedIndex], Frame0, Frame1, Alpha);
        if (Decoded.Touched & Detail::TouchedT) OutT = Decoded.T;
        if (Decoded.Touched & Detail::TouchedR) OutR = Decoded.R;
        if (Decoded.Touched & Detail::TouchedS) OutS = Decoded.S;
    }
}
