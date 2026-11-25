#include "RuntimePCH.h"
#include "AnimGpuClip.h"

#include "Animation/BindPose.h"
#include "Animation/TaskSystem/AnimTaskRecord.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    namespace
    {
        // Scale is not stored, so a clip whose bones leave unit scale by more than this stays on the CPU.
        constexpr float kUnitScaleTolerance = 1e-4f;

        uint32 PackSnorm16Pair(float A, float B)
        {
            const auto Quantize = [](float Value) -> uint32
            {
                const int32 Scaled = (int32)Math::Round(Math::Clamp(Value, -1.0f, 1.0f) * 32767.0f);
                return (uint32)(uint16)(int16)Scaled;
            };
            return Quantize(A) | (Quantize(B) << 16);
        }
    }

    namespace Anim
    {
        bool BakeGpuClip(const CAnimation& Clip, FSkeletonResource& Skeleton, FAnimGpuClip& Out)
        {
            Out = FAnimGpuClip();
            const FAnimationResource* Resource = Clip.GetAnimationResource();
            if (Resource == nullptr || Clip.IsAdditive() || Skeleton.GetNumBones() == 0)
            {
                return false;
            }

            Out.NumFrames = Math::Max(Resource->Compressed.NumFrames, 1u);
            Out.NumBones  = (uint32)Skeleton.GetNumBones();
            Out.Duration  = Resource->Duration;
            Out.Rotations.Reserve((int32)(Out.NumFrames * Out.NumBones));
            Out.Translations.Reserve((int32)(Out.NumFrames * Out.NumBones));

            // Sampled through the CPU path at each keyframe, so retargeting, constant overlays and the bind fallback all match it there.
            FPose Pose;
            for (uint32 Frame = 0; Frame < Out.NumFrames; ++Frame)
            {
                const float Time = Out.NumFrames > 1 ? Out.Duration * (float)Frame / (float)(Out.NumFrames - 1) : 0.0f;
                Clip.SampleLocalPose(Time, &Skeleton, Pose);
                for (uint32 Bone = 0; Bone < Out.NumBones; ++Bone)
                {
                    const FVector3 Scale = Pose.GetScale((int32)Bone);
                    if (Math::Abs(Scale.x - 1.0f) > kUnitScaleTolerance || Math::Abs(Scale.y - 1.0f) > kUnitScaleTolerance || Math::Abs(Scale.z - 1.0f) > kUnitScaleTolerance)
                    {
                        Out = FAnimGpuClip();
                        return false;
                    }
                    const FQuat Rotation = Math::Normalize(Pose.GetRotation((int32)Bone));
                    Out.Rotations.Emplace(PackSnorm16Pair(Rotation.x, Rotation.y), PackSnorm16Pair(Rotation.z, Rotation.w));
                    Out.Translations.Emplace(Pose.GetTranslation((int32)Bone), 0.0f);
                }
            }
            return true;
        }

        FMatrix4 EvaluateWeightedBoneGlobal(FSkeletonResource& Skeleton, const TVector<FAnimWeightedSample>& Samples, int32 RootLockBone, int32 Bone)
        {
            int32 Chain[kMaxGpuPoseBones];
            int32 Depth = 0;
            for (int32 b = Bone; Skeleton.IsBoneIndexValid(b) && Depth < kMaxGpuPoseBones; b = Skeleton.Bones[b].ParentIndex)
            {
                Chain[Depth++] = b;
            }

            // Summed as AnimEvaluatePose.slang sums them, each sample sign-aligned to the first and a locked root held at bind.
            FMatrix4 Global(1.0f);
            for (int32 i = Depth - 1; i >= 0; --i)
            {
                const int32 b = Chain[i];
                FVector3 BindT, BindS;
                FQuat BindR;
                AnimPose::GetBindLocalTRS(&Skeleton, b, BindT, BindR, BindS);

                FQuat    Rotation    = Samples.IsEmpty() ? BindR : FQuat(0.0f, 0.0f, 0.0f, 0.0f);
                FVector3 Translation = Samples.IsEmpty() ? BindT : FVector3(0.0f);
                FQuat    Reference   = BindR;
                for (int32 s = 0; s < Samples.Num(); ++s)
                {
                    const FAnimWeightedSample& Sample = Samples[s];
                    FVector3 T = BindT, S = BindS;
                    FQuat R = BindR;
                    if (Sample.Clip != nullptr && b != RootLockBone)
                    {
                        Sample.Clip->SampleBoneLocal(Sample.Time, &Skeleton, b, T, R, S);
                    }
                    Reference = s == 0 ? R : Reference;
                    R = Math::Dot(Reference, R) < 0.0f ? -R : R;
                    Rotation    = Rotation + R * Sample.Weight;
                    Translation = Translation + T * Sample.Weight;
                }
                Global = Global * AnimPose::ComposeTRS(Translation, Math::Normalize(Rotation), FVector3(1.0f));
            }
            return Global;
        }
    }
}
