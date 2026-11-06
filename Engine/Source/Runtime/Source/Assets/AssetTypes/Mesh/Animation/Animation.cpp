#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "Animation.h"

#include "Animation/BindPose.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Animation/Pose.h"
#include "Core/Math/SIMD/SIMDConfig.h"
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

        static bool IsConsecutive(const int32 (&Bones)[FCompressedAnimData::RotationBatchSize])
        {
            for (int32 k = 1; k < FCompressedAnimData::RotationBatchSize; ++k)
            {
                if (Bones[k] != Bones[0] + k)
                {
                    return false;
                }
            }
            return true;
        }

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

    namespace Detail
    {
        constexpr float kRetargetTolerance = 1e-4f;

        bool IsNearlyIdentity(const FVector3& T, const FQuat& R, const FVector3& S)
        {
            return Math::Length(T) < kRetargetTolerance
                && Math::Abs(Math::Abs(R.w) - 1.0f) < kRetargetTolerance
                && Math::Length(S - FVector3(1.0f)) < kRetargetTolerance;
        }

        void ApplyRootFix(const FAnimationResource::FRetargetFix& Fix, uint8 Touched, FVector3& T, FQuat& R, FVector3& S)
        {
            if (!(Touched & TouchedT)) T = Fix.SourceBindT;
            if (!(Touched & TouchedR)) R = Fix.SourceBindR;
            if (!(Touched & TouchedS)) S = Fix.SourceBindS;
            AnimPose::DecomposeTRS(Fix.RootCorrection * AnimPose::ComposeTRS(T, R, S), T, R, S);
        }

        // The target's own offset plus the clip's motion away from the source rest, so joints keep the target's anatomy.
        FORCEINLINE FVector3 RetargetTranslation(const FAnimationResource::FRetargetFix& Fix, const FVector3& ClipT)
        {
            return Fix.TargetBindT + (ClipT - Fix.SourceBindT) * Fix.TranslationScale;
        }

        const FAnimationResource::FRetargetFix* FindRetargetFix(const FAnimationResource::FResolvedSkeleton& Resolved, int32 Bone)
        {
            for (const FAnimationResource::FRetargetFix& Fix : Resolved.RetargetFixes)
            {
                if (Fix.Bone == Bone)
                {
                    return &Fix;
                }
            }
            return nullptr;
        }

        void BuildRetargetFixes(const FCompressedAnimData& Compressed, FAnimationResource::FResolvedSkeleton& Resolved,
                                const FSkeletonResource& Target, const FSkeletonResource& Source)
        {
            for (int32 i = 0; i < Compressed.Bones.Num(); ++i)
            {
                const int32 Bone = Resolved.CompressedBones[i];
                const int32 SourceBone = Source.FindBoneIndex(Compressed.Bones[i].BoneName);
                if (Bone < 0 || Bone >= Target.GetNumBones() || SourceBone < 0)
                {
                    continue;
                }

                FAnimationResource::FRetargetFix Fix;
                Fix.Bone            = Bone;
                Fix.CompressedIndex = i;
                AnimPose::GetBindLocalTRS(&Source, SourceBone, Fix.SourceBindT, Fix.SourceBindR, Fix.SourceBindS);

                const bool bRootInBoth = Target.GetBone(Bone).ParentIndex == Constants::kIndexNone
                                      && Source.GetBone(SourceBone).ParentIndex == Constants::kIndexNone;
                if (bRootInBoth)
                {
                    Fix.bRoot = true;
                    Fix.RootCorrection = Target.GetBone(Bone).LocalTransform * Math::Inverse(Source.GetBone(SourceBone).LocalTransform);

                    FVector3 T, S;
                    FQuat R;
                    AnimPose::DecomposeTRS(Fix.RootCorrection, T, R, S);
                    if (!IsNearlyIdentity(T, R, S))
                    {
                        Resolved.RetargetFixes.Add(Fix);
                    }
                    continue;
                }

                FVector3 TargetS;
                FQuat TargetR;
                AnimPose::GetBindLocalTRS(&Target, Bone, Fix.TargetBindT, TargetR, TargetS);

                const float SourceLength = Math::Length(Fix.SourceBindT);
                Fix.TranslationScale = SourceLength < kRetargetTolerance ? 1.0f : Math::Length(Fix.TargetBindT) / SourceLength;
                if (Math::Length(Fix.TargetBindT - Fix.SourceBindT) > kRetargetTolerance)
                {
                    Resolved.RetargetFixes.Add(Fix);
                }
            }
        }

        uint64 MakeRetargetKey(const FSkeletonResource* Target, const FSkeletonResource* Source)
        {
            if (Source == nullptr || Source == Target)
            {
                return 0;
            }

            uint64 Key = (uint64)(uintptr_t)Source;
            Key ^= (uint64)Source->BindPoseGeneration * 0x9E3779B97F4A7C15ull;
            Key ^= ((uint64)Target->BindPoseGeneration << 32) | (Key >> 32);
            return Key == 0 ? 1 : Key;
        }

        // Pure selects, so every constant lands as the exact bits its track stores.
        void ApplyConstantOverlay(const FAnimationResource::FResolvedSkeleton& Resolved, FPose& Pose, int32 ActiveBones)
        {
            const int32 Stride = Resolved.OverlayStride;
            const int32 Whole  = ActiveBones & ~7;
            for (int32 Stream = 0; Stream < FPose::NumStreams; ++Stream)
            {
                if ((Resolved.OverlayStreams & (1u << Stream)) == 0u)
                {
                    continue;
                }

                float* RESTRICT        Out    = Pose.Stream(Stream);
                const float* RESTRICT  Values = Resolved.OverlayValues.GetData() + (SIZE_T)Stream * Stride;
                const uint32* RESTRICT Mask   = Resolved.OverlayMask.GetData() + (SIZE_T)Stream * Stride;

                int32 i = 0;
                for (; i < Whole; i += 8)
                {
                    const __m256 Select = _mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(Mask + i)));
                    _mm256_storeu_ps(Out + i, _mm256_blendv_ps(_mm256_loadu_ps(Out + i), _mm256_loadu_ps(Values + i), Select));
                }
                for (; i < ActiveBones; ++i)
                {
                    if (Mask[i] != 0u)
                    {
                        Out[i] = Values[i];
                    }
                }
            }
        }
    }

    void FAnimationResource::BuildConstantOverlay(FResolvedSkeleton& Resolved, int32 NumBones) const
    {
        const int32 Stride = FPose::StrideFor(NumBones);
        TVector<float>  Values((SIZE_T)Stride * FPose::NumStreams, 0.0f);
        TVector<uint32> Mask((SIZE_T)Stride * FPose::NumStreams, 0u);
        TVector<uint8>  Seen(NumBones, 0u);
        uint16 Streams = 0;

        const auto SetConstant = [&](int32 Stream, int32 Bone, float Value)
        {
            Values[Stream * Stride + Bone] = Value;
            Mask[Stream * Stride + Bone]   = ~0u;
            Streams |= (uint16)(1u << Stream);
        };

        // Mirrors the decoders, which hand back Track.Constant for every format they do not interpolate.
        for (int32 i = 0; i < Compressed.Bones.Num(); ++i)
        {
            const int32 Bone = Resolved.CompressedBones[i];
            if (Bone < 0 || Bone >= NumBones)
            {
                continue;
            }
            if (Seen[Bone] != 0u)
            {
                Resolved.AnimatedBones.Reset();
                return;
            }
            Seen[Bone] = 1u;

            const FCompressedAnimBone& Track = Compressed.Bones[i];
            const bool bAnimatedT = Track.Translation.Format == EAnimTrackFormat::Quantized || Track.Translation.Format == EAnimTrackFormat::Raw;
            const bool bAnimatedR = Track.Rotation.Format == EAnimTrackFormat::Quantized;
            const bool bAnimatedS = Track.Scale.Format == EAnimTrackFormat::Quantized;

            if (Track.Translation.Format == EAnimTrackFormat::Constant)
            {
                SetConstant(FPose::StreamTx, Bone, Track.Translation.Constant.x);
                SetConstant(FPose::StreamTy, Bone, Track.Translation.Constant.y);
                SetConstant(FPose::StreamTz, Bone, Track.Translation.Constant.z);
            }
            if (Track.Scale.Format != EAnimTrackFormat::None && !bAnimatedS)
            {
                SetConstant(FPose::StreamSx, Bone, Track.Scale.Constant.x);
                SetConstant(FPose::StreamSy, Bone, Track.Scale.Constant.y);
                SetConstant(FPose::StreamSz, Bone, Track.Scale.Constant.z);
            }
            if (Track.Rotation.Format != EAnimTrackFormat::None && !bAnimatedR)
            {
                SetConstant(FPose::StreamRx, Bone, Track.Rotation.Constant.x);
                SetConstant(FPose::StreamRy, Bone, Track.Rotation.Constant.y);
                SetConstant(FPose::StreamRz, Bone, Track.Rotation.Constant.z);
                SetConstant(FPose::StreamRw, Bone, Track.Rotation.Constant.w);
            }

            if (bAnimatedT || bAnimatedR || bAnimatedS)
            {
                Resolved.AnimatedBones.Add(i);
            }
        }

        // In pose order, so rotation batches land on consecutive bones and a bone LOD cut ends the walk.
        Algo::Sort(Resolved.AnimatedBones, [&Resolved](int32 A, int32 B)
        {
            return Resolved.CompressedBones[A] < Resolved.CompressedBones[B];
        });

        Resolved.OverlayStride  = Stride;
        Resolved.OverlayValues  = Move(Values);
        Resolved.OverlayMask    = Move(Mask);
        Resolved.OverlayStreams = Streams;
    }

    const FAnimationResource::FResolvedSkeleton* FAnimationResource::GetResolvedSkeleton(const FSkeletonResource* Skeleton, const FSkeletonResource* Source)
    {
        // A skeleton built without its bind cache hashes here, which only costs that rare caller.
        const uint64 Key = Skeleton->BoneLayoutHash != 0 ? Skeleton->BoneLayoutHash : Skeleton->ComputeBoneLayoutHash();
        const uint64 SourceKey = Detail::MakeRetargetKey(Skeleton, Source);

        const FResolvedSkeleton* Active = ActiveResolvedSkeleton.load(std::memory_order_acquire);
        if (Active && Active->LayoutKey == Key && Active->SourceKey == SourceKey)
        {
            return Active;
        }

        // Every character can carry its own merged skeleton, so a miss here must not take the lock.
        if (const FResolvedTable* Table = PublishedResolved.load(std::memory_order_acquire))
        {
            for (const FResolvedSkeleton* Resolved : *Table)
            {
                if (Resolved->LayoutKey == Key && Resolved->SourceKey == SourceKey)
                {
                    return Resolved;
                }
            }
        }

        FScopeLock Lock(ResolveMutex);

        for (const TUniquePtr<FResolvedSkeleton>& Resolved : ResolvedSkeletons)
        {
            if (Resolved->LayoutKey == Key && Resolved->SourceKey == SourceKey)
            {
                return Resolved.get();
            }
        }

        TUniquePtr<FResolvedSkeleton> NewSet = MakeUnique<FResolvedSkeleton>();
        NewSet->LayoutKey = Key;
        NewSet->SourceKey = SourceKey;

        const int32 NumBones = Skeleton->GetNumBones();
        NewSet->SkeletonToCompressed.Init(Constants::kIndexNone, NumBones);
        NewSet->CompressedBones.Reserve(Compressed.Bones.Num());

        int32 NumUnmatched = 0;
        for (int32 i = 0; i < Compressed.Bones.Num(); ++i)
        {
            const int32 BoneIndex = Skeleton->FindBoneIndex(Compressed.Bones[i].BoneName);
            NumUnmatched += BoneIndex < 0 ? 1 : 0;
            NewSet->CompressedBones.Add(BoneIndex);

            if (BoneIndex >= 0 && BoneIndex < NumBones)
            {
                NewSet->SkeletonToCompressed[BoneIndex] = i;
            }
        }

        BuildConstantOverlay(*NewSet, NumBones);

        if (SourceKey != 0)
        {
            Detail::BuildRetargetFixes(Compressed, *NewSet, *Skeleton, *Source);
        }

        // Unmatched bones silently freeze at bind pose, the telltale of the wrong skeleton.
        if (NumUnmatched > 0)
        {
            LOG_WARN("Animation '{}': {}/{} bones are missing from the skeleton (name mismatch or wrong skeleton)",
                     *Name, NumUnmatched, Compressed.Bones.Num());
        }

        const FResolvedSkeleton* Result = NewSet.get();
        ResolvedSkeletons.Add(Move(NewSet));

        // Old tables stay alive, since a reader may still be scanning one.
        TUniquePtr<FResolvedTable> Table = MakeUnique<FResolvedTable>();
        for (const TUniquePtr<FResolvedSkeleton>& Resolved : ResolvedSkeletons)
        {
            Table->Add(Resolved.get());
        }
        PublishedResolved.store(Table.get(), std::memory_order_release);
        ResolvedTables.Add(Move(Table));

        ActiveResolvedSkeleton.store(Result, std::memory_order_release);
        return Result;
    }

    void FAnimationResource::RebuildSyncTrack()
    {
        TVector<FSyncTrack::FMarker> Markers;

        if (!SyncTrackName.IsNone() && Duration > 0.0f)
        {
            for (const FAnimationNotify& Notify : Notifies)
            {
                if (Notify.NotifyTrack == SyncTrackName)
                {
                    Markers.Emplace(Notify.NotifyName, Math::Saturate(Notify.Time / Duration));
                }
            }

            Algo::Sort(Markers, [](const FSyncTrack::FMarker& A, const FSyncTrack::FMarker& B)
            {
                return A.StartTime < B.StartTime;
            });

            // Two markers on the same frame would leave a zero-length span, which no conversion survives.
            Markers.SetNum(static_cast<int32>(Algo::Unique(Markers, [](const FSyncTrack::FMarker& A, const FSyncTrack::FMarker& B)
            {
                return Math::Abs(A.StartTime - B.StartTime) < 1e-4f;
            }) - Markers.begin()));
        }

        SyncTrack.BuildFromMarkers(Markers);
    }

    void FAnimationResource::InvalidateResolvedSkeletons()
    {
        FScopeLock Lock(ResolveMutex);
        ActiveResolvedSkeleton.store(nullptr, std::memory_order_release);
        PublishedResolved.store(nullptr, std::memory_order_release);
        ResolvedSkeletons.Reset();
        ResolvedTables.Reset();
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
            if (!AnimationResource->Compressed.IsValid() && !AnimationResource->Channels.IsEmpty())
            {
                AnimCompression::Build(*AnimationResource);
            }

            AnimationResource->Channels.Reset();
            AnimationResource->Channels.Shrink();
            AnimationResource->InvalidateResolvedSkeletons();
        }
    }

    int32 CAnimation::FindCurveIndex(const FName& CurveName) const
    {
        const TVector<FAnimationCurve>& Curves = AnimationResource->Curves;
        for (int32 i = 0; i < Curves.Num(); ++i)
        {
            if (Curves[i].Name == CurveName)
            {
                return i;
            }
        }
        return Constants::kIndexNone;
    }

    float CAnimation::EvaluateCurve(const FName& CurveName, float Time, float Default) const
    {
        const int32 Index = FindCurveIndex(CurveName);
        return Index != Constants::kIndexNone ? AnimationResource->Curves[Index].Curve.Evaluate(Time) : Default;
    }

    void CAnimation::SamplePose(float Time, FSkeletonResource* RESTRICT InSkeleton, TVector<FMatrix4>& RESTRICT OutBoneTransforms) const
    {
        const int32 NumBones = InSkeleton->GetNumBones();
        OutBoneTransforms.SetNum(NumBones);

        if (NumBones == 0)
        {
            return;
        }

        // Per-thread scratch reused across frames; thread_local required since SamplePose runs in ParallelFor.
        thread_local TVector<FVector3> ScratchT;
        thread_local TVector<FQuat> ScratchR;
        thread_local TVector<FVector3> ScratchS;
        thread_local TVector<uint8>     ScratchTouched;

        if (ScratchT.Num() < NumBones)
        {
            ScratchT.SetNum(NumBones);
            ScratchR.SetNum(NumBones);
            ScratchS.SetNum(NumBones);
            ScratchTouched.SetNum(NumBones);
        }

        Memory::Memset(ScratchTouched.GetData(), 0, (size_t)NumBones * sizeof(uint8));

        // Pass 1 gathers per-bone TRS overrides with bone indices pre-resolved.
        const FAnimationResource::FResolvedSkeleton* Resolved = AnimationResource->GetResolvedSkeleton(InSkeleton, GetSourceSkeletonResource());
        const FCompressedAnimData& Compressed = AnimationResource->Compressed;

        uint32 Frame0, Frame1;
        float Alpha;
        Compressed.GetFrameBlend(Time, AnimationResource->Duration, Frame0, Frame1, Alpha);

        for (int32 b = 0; b < Compressed.Bones.Num(); ++b)
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

        for (const FAnimationResource::FRetargetFix& Fix : Resolved->RetargetFixes)
        {
            const int32 i = Fix.Bone;
            const uint8 Touched = ScratchTouched[i];
            if (Touched == 0)
            {
                continue;
            }

            if (Fix.bRoot)
            {
                Detail::ApplyRootFix(Fix, Touched, ScratchT[i], ScratchR[i], ScratchS[i]);
                ScratchTouched[i] = Detail::TouchedAll;
            }
            else if (Touched & Detail::TouchedT)
            {
                ScratchT[i] = Detail::RetargetTranslation(Fix, ScratchT[i]);
            }
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

            OutBoneTransforms[i] = Bone.ParentIndex != Constants::kIndexNone ? OutBoneTransforms[Bone.ParentIndex] * Local : Local;
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

        const FAnimationResource::FResolvedSkeleton* Resolved = AnimationResource->GetResolvedSkeleton(InSkeleton, GetSourceSkeletonResource());
        const FCompressedAnimData& Compressed = AnimationResource->Compressed;

        uint32 Frame0, Frame1;
        float Alpha;
        Compressed.GetFrameBlend(Time, AnimationResource->Duration, Frame0, Frame1, Alpha);

        constexpr int32 kBatch = FCompressedAnimData::RotationBatchSize;
        int32  BatchBones[kBatch];
        uint32 BatchOffsets[kBatch];
        int32  BatchCount = 0;

        float* RESTRICT Rx = OutPose.Rx(); float* RESTRICT Ry = OutPose.Ry();
        float* RESTRICT Rz = OutPose.Rz(); float* RESTRICT Rw = OutPose.Rw();

        const auto FlushRotations = [&]
        {
            float Decoded[4][kBatch];
            Compressed.DecodeQuantizedRotationBatch(BatchOffsets, Frame0, Frame1, Alpha, Decoded);

            // Clips usually list bones in skeleton order, which turns the scatter into four contiguous stores.
            const int32 FirstBone = BatchBones[0];
            if (Detail::IsConsecutive(BatchBones))
            {
                _mm256_storeu_ps(Rx + FirstBone, _mm256_loadu_ps(Decoded[0]));
                _mm256_storeu_ps(Ry + FirstBone, _mm256_loadu_ps(Decoded[1]));
                _mm256_storeu_ps(Rz + FirstBone, _mm256_loadu_ps(Decoded[2]));
                _mm256_storeu_ps(Rw + FirstBone, _mm256_loadu_ps(Decoded[3]));
            }
            else
            {
                for (int32 k = 0; k < kBatch; ++k)
                {
                    const int32 Bone = BatchBones[k];
                    Rx[Bone] = Decoded[0][k]; Ry[Bone] = Decoded[1][k]; Rz[Bone] = Decoded[2][k]; Rw[Bone] = Decoded[3][k];
                }
            }
            BatchCount = 0;
        };

        // With the overlay in place only animated tracks remain, since a constant decode would write the overlay's value again.
        const bool bOverlay = Resolved->OverlayStride == FPose::StrideFor(NumBones);
        const auto DecodeBone = [&](int32 b)
        {
            const int32 BoneIdx = Resolved->CompressedBones[b];
            if (BoneIdx < 0 || BoneIdx >= ActiveBones)
            {
                return;
            }

            const FCompressedAnimBone& Bone = Compressed.Bones[b];
            if (Bone.Translation.Format != EAnimTrackFormat::None && (!bOverlay || Bone.Translation.IsAnimated()))
            {
                OutPose.SetTranslation(BoneIdx, Compressed.DecodeTranslation(Bone.Translation, Frame0, Frame1, Alpha));
            }
            if (Bone.Scale.Format != EAnimTrackFormat::None && (!bOverlay || Bone.Scale.Format == EAnimTrackFormat::Quantized))
            {
                OutPose.SetScale(BoneIdx, Compressed.DecodeScale(Bone.Scale, Frame0, Frame1, Alpha));
            }

            if (Bone.Rotation.Format == EAnimTrackFormat::Quantized)
            {
                BatchBones[BatchCount]   = BoneIdx;
                BatchOffsets[BatchCount] = Bone.Rotation.DataOffset;
                if (++BatchCount == kBatch)
                {
                    FlushRotations();
                }
            }
            else if (Bone.Rotation.Format != EAnimTrackFormat::None && !bOverlay)
            {
                OutPose.SetRotation(BoneIdx, Compressed.DecodeRotation(Bone.Rotation, Frame0, Frame1, Alpha));
            }
        };

        if (bOverlay)
        {
            Detail::ApplyConstantOverlay(*Resolved, OutPose, ActiveBones);
            for (const int32 b : Resolved->AnimatedBones)
            {
                if (Resolved->CompressedBones[b] >= ActiveBones)
                {
                    break;
                }
                DecodeBone(b);
            }
        }
        else
        {
            for (int32 b = 0; b < Compressed.Bones.Num(); ++b)
            {
                DecodeBone(b);
            }
        }

        // Fewer than a batch remain, so these take the scalar decoder, which gives the same bits.
        for (int32 k = 0; k < BatchCount; ++k)
        {
            FCompressedAnimTrack Track;
            Track.Format     = EAnimTrackFormat::Quantized;
            Track.DataOffset = BatchOffsets[k];
            OutPose.SetRotation(BatchBones[k], Compressed.DecodeRotation(Track, Frame0, Frame1, Alpha));
        }

        for (const FAnimationResource::FRetargetFix& Fix : Resolved->RetargetFixes)
        {
            if (Fix.Bone >= ActiveBones)
            {
                continue;
            }

            const FCompressedAnimBone& Track = Compressed.Bones[Fix.CompressedIndex];
            const uint8 Touched = (Track.Translation.Format != EAnimTrackFormat::None ? Detail::TouchedT : 0u)
                                | (Track.Rotation.Format != EAnimTrackFormat::None ? Detail::TouchedR : 0u)
                                | (Track.Scale.Format != EAnimTrackFormat::None ? Detail::TouchedS : 0u);

            if (Fix.bRoot)
            {
                if (Touched == 0)
                {
                    continue;
                }

                FVector3 T = OutPose.GetTranslation(Fix.Bone);
                FQuat    R = OutPose.GetRotation(Fix.Bone);
                FVector3 S = OutPose.GetScale(Fix.Bone);
                Detail::ApplyRootFix(Fix, Touched, T, R, S);
                OutPose.SetTranslation(Fix.Bone, T);
                OutPose.SetRotation(Fix.Bone, R);
                OutPose.SetScale(Fix.Bone, S);
            }
            else if (Touched & Detail::TouchedT)
            {
                OutPose.SetTranslation(Fix.Bone, Detail::RetargetTranslation(Fix, OutPose.GetTranslation(Fix.Bone)));
            }
        }
    }

    void CAnimation::SampleBoneLocal(float Time, FSkeletonResource* RESTRICT InSkeleton, int32 BoneIndex,
                                     FVector3& OutT, FQuat& OutR, FVector3& OutS) const
    {
        AnimPose::GetBindLocalTRS(InSkeleton, BoneIndex, OutT, OutR, OutS);

        const FAnimationResource::FResolvedSkeleton* Resolved = AnimationResource->GetResolvedSkeleton(InSkeleton, GetSourceSkeletonResource());
        const FCompressedAnimData& Compressed = AnimationResource->Compressed;

        if (BoneIndex < 0 || BoneIndex >= Resolved->SkeletonToCompressed.Num())
        {
            return;
        }

        const int32 CompressedIndex = Resolved->SkeletonToCompressed[BoneIndex];
        if (CompressedIndex == Constants::kIndexNone)
        {
            return;
        }

        uint32 Frame0, Frame1;
        float Alpha;
        Compressed.GetFrameBlend(Time, AnimationResource->Duration, Frame0, Frame1, Alpha);

        Detail::FDecodedBone Decoded = Detail::DecodeBone(Compressed, Compressed.Bones[CompressedIndex], Frame0, Frame1, Alpha);
        const FAnimationResource::FRetargetFix* Fix = Detail::FindRetargetFix(*Resolved, BoneIndex);
        if (Fix != nullptr && Fix->bRoot && Decoded.Touched != 0)
        {
            Detail::ApplyRootFix(*Fix, Decoded.Touched, Decoded.T, Decoded.R, Decoded.S);
            OutT = Decoded.T;
            OutR = Decoded.R;
            OutS = Decoded.S;
            return;
        }

        if (Decoded.Touched & Detail::TouchedT) OutT = (Fix != nullptr) ? Detail::RetargetTranslation(*Fix, Decoded.T) : Decoded.T;
        if (Decoded.Touched & Detail::TouchedR) OutR = Decoded.R;
        if (Decoded.Touched & Detail::TouchedS) OutS = Decoded.S;
    }

    const FSkeletonResource* CAnimation::GetSourceSkeletonResource() const
    {
        return Skeleton.IsValid() ? Skeleton->GetSkeletonResource() : nullptr;
    }
}
