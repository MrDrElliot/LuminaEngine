#include "RuntimePCH.h"
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
        static FVector3 SampleVec3(const TVector<float>& Times, const TVector<FVector3>& Values, float Time)
        {
            const size_t N = Times.size();
            if (N == 0 || Values.empty())
            {
                return FVector3(0.0f);
            }
            if (N == 1 || Time <= Times[0])
            {
                return Values[0];
            }
            if (Time >= Times[N - 1])
            {
                return Values[N - 1];
            }

            // Binary search keyframe interval; clips routinely have hundreds of keys.
            size_t Lo = 0;
            size_t Hi = N - 1;
            while (Lo + 1 < Hi)
            {
                const size_t Mid = (Lo + Hi) >> 1;
                (Time < Times[Mid] ? Hi : Lo) = Mid;
            }

            const float Dt = Times[Lo + 1] - Times[Lo];
            const float t  = Dt > 0.0f ? (Time - Times[Lo]) / Dt : 0.0f;
            return Math::Mix(Values[Lo], Values[Lo + 1], t);
        }

        static FQuat SampleQuat(const TVector<float>& Times, const TVector<FQuat>& Values, float Time)
        {
            const size_t N = Times.size();
            if (N == 0 || Values.empty())
            {
                return FQuat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            if (N == 1 || Time <= Times[0])
            {
                return Values[0];
            }
            if (Time >= Times[N - 1])
            {
                return Values[N - 1];
            }

            size_t Lo = 0;
            size_t Hi = N - 1;
            while (Lo + 1 < Hi)
            {
                const size_t Mid = (Lo + Hi) >> 1;
                (Time < Times[Mid] ? Hi : Lo) = Mid;
            }

            const float Dt = Times[Lo + 1] - Times[Lo];
            const float t  = Dt > 0.0f ? (Time - Times[Lo]) / Dt : 0.0f;

            FQuat Q0 = Values[Lo];
            FQuat Q1 = Values[Lo + 1];
            if (Math::Dot(Q0, Q1) < 0.0f)
            {
                Q1 = -Q1;
            }
            // nlerp, NOT slerp: adjacent keyframes are dense, so a normalized lerp is visually identical while
            // avoiding acos + 3x sin per channel. Q0/Q1 are hemisphere-aligned above.
            return Math::Normalize(Q0 * (1.0f - t) + Q1 * t);
        }

        static constexpr uint8 TouchedT = 1u << 0;
        static constexpr uint8 TouchedR = 1u << 1;
        static constexpr uint8 TouchedS = 1u << 2;
        static constexpr uint8 TouchedAll = TouchedT | TouchedR | TouchedS;
    }

    const FAnimationResource::FResolvedChannelSet* FAnimationResource::GetResolvedChannelSet(const FSkeletonResource* Skeleton)
    {
        const FResolvedChannelSet* Active = ActiveChannelSet.load(std::memory_order_acquire);
        if (Active && Active->Skeleton == Skeleton && Active->Generation == Skeleton->BindPoseGeneration)
        {
            return Active;
        }

        FScopeLock Lock(ChannelSetMutex);

        for (const TUniquePtr<FResolvedChannelSet>& Set : ChannelSets)
        {
            if (Set->Skeleton == Skeleton && Set->Generation == Skeleton->BindPoseGeneration)
            {
                ActiveChannelSet.store(Set.get(), std::memory_order_release);
                return Set.get();
            }
        }

        TUniquePtr<FResolvedChannelSet> NewSet = MakeUnique<FResolvedChannelSet>();
        NewSet->Skeleton   = Skeleton;
        NewSet->Generation = Skeleton->BindPoseGeneration;
        NewSet->BoneIndices.reserve(Channels.size());
        int32 NumUnmatched = 0;
        for (const FAnimationChannel& Channel : Channels)
        {
            const int32 BoneIndex = Skeleton->FindBoneIndex(Channel.TargetBone);
            NumUnmatched += BoneIndex < 0 ? 1 : 0;
            NewSet->BoneIndices.push_back(BoneIndex);
        }

        // Unmatched channels silently freeze their bones at bind pose -- the telltale of an animation
        // sampled against the wrong skeleton. Once per (clip, skeleton) pairing, since sets are cached.
        if (NumUnmatched > 0)
        {
            LOG_WARN("Animation '{}': {}/{} channels target bones missing from the skeleton (name mismatch or wrong skeleton)",
                     Name.c_str(), NumUnmatched, (int32)Channels.size());
        }

        const FResolvedChannelSet* Result = NewSet.get();
        ChannelSets.push_back(eastl::move(NewSet));
        ActiveChannelSet.store(Result, std::memory_order_release);
        return Result;
    }

    void FAnimationResource::InvalidateResolvedChannelSets()
    {
        FScopeLock Lock(ChannelSetMutex);
        ActiveChannelSet.store(nullptr, std::memory_order_release);
        ChannelSets.clear();
    }

    void CAnimation::Serialize(FArchive& Ar)
    {
        CObject::Serialize(Ar);

        if (!AnimationResource)
        {
            AnimationResource = MakeUnique<FAnimationResource>();
        }

        Ar << *AnimationResource;
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

        // Pass 1: gather per-bone TRS overrides; one sample per channel, bone indices pre-resolved.
        const FAnimationResource::FResolvedChannelSet* Resolved = AnimationResource->GetResolvedChannelSet(InSkeleton);
        const TVector<FAnimationChannel>& Channels = AnimationResource->Channels;

        for (SIZE_T c = 0; c < Channels.size(); ++c)
        {
            const int32 BoneIdx = Resolved->BoneIndices[c];
            if (BoneIdx < 0 || BoneIdx >= NumBones)
            {
                continue;
            }

            const FAnimationChannel& Channel = Channels[c];
            switch (Channel.TargetPath)
            {
            case FAnimationChannel::ETargetPath::Translation:
                ScratchT[BoneIdx]       = Detail::SampleVec3(Channel.Timestamps, Channel.Translations, Time);
                ScratchTouched[BoneIdx] |= Detail::TouchedT;
                break;
            case FAnimationChannel::ETargetPath::Rotation:
                ScratchR[BoneIdx]       = Detail::SampleQuat(Channel.Timestamps, Channel.Rotations, Time);
                ScratchTouched[BoneIdx] |= Detail::TouchedR;
                break;
            case FAnimationChannel::ETargetPath::Scale:
                ScratchS[BoneIdx]       = Detail::SampleVec3(Channel.Timestamps, Channel.Scales, Time);
                ScratchTouched[BoneIdx] |= Detail::TouchedS;
                break;
            default:
                break;
            }
        }

        // Pass 2: local matrices fused with FK; Bones[] is parents-before-children so a single linear pass works.
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

        // Pass 3: fold in InvBind to produce the GPU skinning matrix.
        for (int32 i = 0; i < NumBones; ++i)
        {
            OutBoneTransforms[i] = OutBoneTransforms[i] * InSkeleton->GetBone(i).InvBindMatrix;
        }
    }

    void CAnimation::SampleLocalPose(float Time, FSkeletonResource* RESTRICT InSkeleton, FPose& RESTRICT OutPose, int32 MaxBones) const
    {
        LUMINA_PROFILE_SCOPE();

        const int32 NumBones = InSkeleton->GetNumBones();
        OutPose.SetNumBones(NumBones);

        if (NumBones == 0)
        {
            return;
        }

        const int32 ActiveBones = (MaxBones >= 0 && MaxBones < NumBones) ? MaxBones : NumBones;

        // Start from the bind pose, then overwrite whatever the channels animate: three bulk copies instead
        // of a per-bone decompose. Bones past the LOD cut keep their bind-pose locals.
        OutPose.ResetToBindPose(InSkeleton);

        const FAnimationResource::FResolvedChannelSet* Resolved = AnimationResource->GetResolvedChannelSet(InSkeleton);
        const TVector<FAnimationChannel>& Channels = AnimationResource->Channels;

        for (SIZE_T c = 0; c < Channels.size(); ++c)
        {
            const int32 BoneIdx = Resolved->BoneIndices[c];
            if (BoneIdx < 0 || BoneIdx >= ActiveBones)
            {
                continue;
            }

            const FAnimationChannel& Channel = Channels[c];
            switch (Channel.TargetPath)
            {
            case FAnimationChannel::ETargetPath::Translation:
                OutPose.Translations[BoneIdx] = Detail::SampleVec3(Channel.Timestamps, Channel.Translations, Time);
                break;
            case FAnimationChannel::ETargetPath::Rotation:
                OutPose.Rotations[BoneIdx] = Detail::SampleQuat(Channel.Timestamps, Channel.Rotations, Time);
                break;
            case FAnimationChannel::ETargetPath::Scale:
                OutPose.Scales[BoneIdx] = Detail::SampleVec3(Channel.Timestamps, Channel.Scales, Time);
                break;
            default:
                break;
            }
        }
    }

    void CAnimation::SampleBoneLocal(float Time, FSkeletonResource* RESTRICT InSkeleton, int32 BoneIndex,
                                     FVector3& OutT, FQuat& OutR, FVector3& OutS) const
    {
        AnimPose::GetBindLocalTRS(InSkeleton, BoneIndex, OutT, OutR, OutS);

        const FAnimationResource::FResolvedChannelSet* Resolved = AnimationResource->GetResolvedChannelSet(InSkeleton);
        const TVector<FAnimationChannel>& Channels = AnimationResource->Channels;

        for (SIZE_T c = 0; c < Channels.size(); ++c)
        {
            if (Resolved->BoneIndices[c] != BoneIndex)
            {
                continue;
            }

            const FAnimationChannel& Channel = Channels[c];
            switch (Channel.TargetPath)
            {
            case FAnimationChannel::ETargetPath::Translation:
                OutT = Detail::SampleVec3(Channel.Timestamps, Channel.Translations, Time);
                break;
            case FAnimationChannel::ETargetPath::Rotation:
                OutR = Detail::SampleQuat(Channel.Timestamps, Channel.Rotations, Time);
                break;
            case FAnimationChannel::ETargetPath::Scale:
                OutS = Detail::SampleVec3(Channel.Timestamps, Channel.Scales, Time);
                break;
            default:
                break;
            }
        }
    }
}
