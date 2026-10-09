#include <gtest/gtest.h>

#include <bit>

#include "Animation/AnimCompression.h"
#include "Animation/Pose.h"
#include "Animation/TaskSystem/AnimTask.h"
#include "Animation/TaskSystem/AnimTaskExecutor.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Renderer/SkeletonResource.h"

using namespace Lumina;

namespace
{
    void MakeExecutorChain(FSkeletonResource& Skeleton, int32 NumBones)
    {
        for (int32 i = 0; i < NumBones; ++i)
        {
            FSkeletonResource::FBoneInfo Bone;
            Bone.Name           = FName(("ExecBone" + std::to_string(i)).c_str());
            Bone.ParentIndex    = i - 1;
            Bone.InvBindMatrix  = FMatrix4::Identity();
            Bone.LocalTransform = AnimPose::ComposeTRS(FVector3(0.0f, 0.3f, 0.0f), FQuat::Identity(), FVector3(1.0f));
            Skeleton.BoneNameToIndex[Bone.Name] = i;
            Skeleton.Bones.push_back(Bone);
        }
        Skeleton.BuildBindPoseCache();
    }

    CAnimation* MakeExecutorClip(const FSkeletonResource& Skeleton, float SweepRadians)
    {
        CAnimation* Clip = NewObject<CAnimation>();
        FAnimationResource& Resource = *Clip->GetAnimationResource();
        Resource.Duration = 1.0f;

        for (const FSkeletonResource::FBoneInfo& Bone : Skeleton.Bones)
        {
            FAnimationChannel Channel;
            Channel.TargetBone = Bone.Name;
            Channel.TargetPath = FAnimationChannel::ETargetPath::Rotation;
            for (int32 k = 0; k <= 30; ++k)
            {
                const float T = (float)k / 30.0f;
                Channel.Timestamps.push_back(T);
                Channel.Rotations.push_back(FQuat(FVector3(SweepRadians * T, 0.5f * SweepRadians * T, 0.0f)));
            }
            Resource.Channels.push_back(Channel);
        }

        AnimCompression::Build(Resource);
        return Clip;
    }

    struct FExecutorRig
    {
        FSkeletonResource Skeleton;
        CAnimation*       ClipA = nullptr;
        CAnimation*       ClipB = nullptr;
        TVector<float>    Mask;

        FExecutorRig()
        {
            MakeExecutorChain(Skeleton, 6);
            ClipA = MakeExecutorClip(Skeleton, 0.8f);
            ClipB = MakeExecutorClip(Skeleton, -1.1f);
            Mask  = { 1.0f, 1.0f, 0.5f, 0.0f, 1.0f, 0.25f };
        }

        int16 Sample(FAnimTaskList& List, CAnimation* Clip, float Time) const
        {
            FAnimTask Task;
            Task.Type = EAnimTaskType::SampleClip;
            Task.Clip = Clip;
            Task.Time = Time;
            return List.Add(Task);
        }

        static int16 Blend(FAnimTaskList& List, int16 A, int16 B, float Alpha)
        {
            FAnimTask Task;
            Task.Type  = EAnimTaskType::Blend;
            Task.DepA  = A;
            Task.DepB  = B;
            Task.Alpha = Alpha;
            return List.Add(Task);
        }

        TVector<FMatrix4> Execute(FAnimTaskList& List)
        {
            List.Skeleton = &Skeleton;
            TVector<FMatrix4> Out;
            EXPECT_TRUE(Anim::ExecuteTaskList(List, Out, nullptr));
            return Out;
        }

        TVector<FMatrix4> Reference(const FPose& Pose)
        {
            TVector<FMatrix4> Out;
            AnimPose::ToSkinningMatrices(Pose, &Skeleton, Out);
            return Out;
        }

        FPose SamplePose(CAnimation* Clip, float Time)
        {
            FPose Pose;
            Clip->SampleLocalPose(Time, &Skeleton, Pose, -1);
            return Pose;
        }
    };

    void ExpectMatricesNear(const TVector<FMatrix4>& Actual, const TVector<FMatrix4>& Expected)
    {
        ASSERT_EQ(Actual.size(), Expected.size());
        for (SIZE_T b = 0; b < Actual.size(); ++b)
        {
            for (int32 c = 0; c < 4; ++c)
            {
                for (int32 r = 0; r < 4; ++r)
                {
                    EXPECT_NEAR(Actual[b][c][r], Expected[b][c][r], 1e-5f) << "bone " << b;
                }
            }
        }
    }
}

// A blend resolved to one side shows exactly that side, whichever input the executor skipped.
TEST(AnimTaskExecutor, ABlendAtZeroOrOneShowsOneSide)
{
    FExecutorRig Rig;

    FAnimTaskList AtZero;
    AtZero.OutputTask = FExecutorRig::Blend(AtZero, Rig.Sample(AtZero, Rig.ClipA, 0.4f), Rig.Sample(AtZero, Rig.ClipB, 0.4f), 0.0f);
    ExpectMatricesNear(Rig.Execute(AtZero), Rig.Reference(Rig.SamplePose(Rig.ClipA, 0.4f)));

    FAnimTaskList AtOne;
    AtOne.OutputTask = FExecutorRig::Blend(AtOne, Rig.Sample(AtOne, Rig.ClipA, 0.4f), Rig.Sample(AtOne, Rig.ClipB, 0.4f), 1.0f);
    ExpectMatricesNear(Rig.Execute(AtOne), Rig.Reference(Rig.SamplePose(Rig.ClipB, 0.4f)));
}

// Blending a pose with itself under a mask leaves it unchanged.
TEST(AnimTaskExecutor, AMaskedBlendOfOnePoseWithItselfIsThatPose)
{
    FExecutorRig Rig;

    FAnimTaskList List;
    const int16 Source = Rig.Sample(List, Rig.ClipA, 0.7f);
    FAnimTask Masked;
    Masked.Type        = EAnimTaskType::BlendMasked;
    Masked.DepA        = Source;
    Masked.DepB        = Source;
    Masked.Alpha       = 0.6f;
    Masked.MaskWeights = &Rig.Mask;
    List.OutputTask = List.Add(Masked);

    ExpectMatricesNear(Rig.Execute(List), Rig.Reference(Rig.SamplePose(Rig.ClipA, 0.7f)));
}

// An input one consumer skips can still feed another, so its buffer must outlive the skip.
TEST(AnimTaskExecutor, ASkippedInputStillFeedsItsOtherConsumer)
{
    FExecutorRig Rig;

    FAnimTaskList List;
    const int16 A = Rig.Sample(List, Rig.ClipA, 0.25f);
    const int16 B = Rig.Sample(List, Rig.ClipB, 0.25f);
    const int16 OnlyA = FExecutorRig::Blend(List, A, B, 0.0f);
    List.OutputTask = FExecutorRig::Blend(List, OnlyA, B, 0.5f);

    FPose Expected;
    AnimPose::Blend(Rig.SamplePose(Rig.ClipA, 0.25f), Rig.SamplePose(Rig.ClipB, 0.25f), 0.5f, Expected, -1);
    ExpectMatricesNear(Rig.Execute(List), Rig.Reference(Expected));
}

// An ordinary weight still blends both inputs.
TEST(AnimTaskExecutor, APartialBlendMixesBothInputs)
{
    FExecutorRig Rig;

    FAnimTaskList List;
    List.OutputTask = FExecutorRig::Blend(List, Rig.Sample(List, Rig.ClipA, 0.9f), Rig.Sample(List, Rig.ClipB, 0.9f), 0.35f);

    FPose Expected;
    AnimPose::Blend(Rig.SamplePose(Rig.ClipA, 0.9f), Rig.SamplePose(Rig.ClipB, 0.9f), 0.35f, Expected, -1);
    ExpectMatricesNear(Rig.Execute(List), Rig.Reference(Expected));
}

namespace
{
    // Deterministic, so a mismatch reproduces from the seed alone.
    struct FCaptureRandom
    {
        uint32 State;
        float Next(float Min, float Max)
        {
            State = State * 1664525u + 1013904223u;
            return Min + (Max - Min) * ((float)(State >> 8) / 16777216.0f);
        }
    };

    void FillCapturePose(FPose& Pose, int32 NumBones, FCaptureRandom& Random)
    {
        Pose.SetNumBones(NumBones);
        for (int32 i = 0; i < NumBones; ++i)
        {
            FQuat R(Random.Next(-1.0f, 1.0f), Random.Next(-1.0f, 1.0f), Random.Next(-1.0f, 1.0f), Random.Next(-1.0f, 1.0f));
            if (i % 7 != 3)
            {
                R = Math::Normalize(R);
            }
            Pose.SetBone(i, FVector3(Random.Next(-2.0f, 2.0f), Random.Next(-2.0f, 2.0f), Random.Next(-2.0f, 2.0f)), R,
                         FVector3(Random.Next(0.5f, 1.5f), Random.Next(0.5f, 1.5f), Random.Next(0.5f, 1.5f)));
        }
    }

    void ExpectChannelsBitEqual(const FInertChannelSet& A, const FInertChannelSet& B, int32 NumBones, const char* What)
    {
        for (int32 Stream = 0; Stream < FInertChannelSet::NumStreams; ++Stream)
        {
            for (int32 i = 0; i < NumBones; ++i)
            {
                EXPECT_EQ(std::bit_cast<uint32>(A.Stream(Stream)[i]), std::bit_cast<uint32>(B.Stream(Stream)[i]))
                    << What << " stream " << Stream << " bone " << i;
            }
        }
    }
}

TEST(AnimTaskExecutor, InertializationCaptureVectorPathMatchesScalarBitForBit)
{
    constexpr int32 NumBones = 115;
    FCaptureRandom Random{ 0x5eedu };

    for (int32 Case = 0; Case < 24; ++Case)
    {
        FPose Source, Prev, Target;
        FillCapturePose(Source, NumBones, Random);
        FillCapturePose(Prev, NumBones, Random);
        FillCapturePose(Target, NumBones, Random);

        // Bones that sit exactly on the target, a zero quaternion, and offsets straddling the cutoffs.
        for (int32 i = 0; i < NumBones; i += 5)
        {
            Source.SetRotation(i, Target.GetRotation(i));
            Source.SetTranslation(i, Target.GetTranslation(i));
            Prev.SetScale(i, Target.GetScale(i));
        }
        Target.SetRotation(9, FQuat(0.0f, 0.0f, 0.0f, 0.0f));
        Source.SetTranslation(17, Target.GetTranslation(17) + FVector3(4e-7f, 0.0f, 0.0f));
        Source.SetTranslation(18, Target.GetTranslation(18) + FVector3(2e-6f, 0.0f, 0.0f));

        const bool  bHasVel = (Case % 2) == 0;
        const int32 Active  = (Case % 3) == 0 ? 50 : -1;
        const float Dt      = (Case % 4) == 1 ? 0.0f : 0.016f;

        FPose Mismatched;
        FillCapturePose(Mismatched, NumBones - 3, Random);
        const FPose& CaptureSource = (Case % 5) == 4 ? Mismatched : Source;

        FAnimInertializer Vector;
        FAnimInertializer Scalar;
        Anim::InertCaptureForTest(Vector, CaptureSource, Prev, Target, Dt, bHasVel, Active, false);
        Anim::InertCaptureForTest(Scalar, CaptureSource, Prev, Target, Dt, bHasVel, Active, true);

        const int32 N = Active >= 0 ? Active : NumBones;
        ExpectChannelsBitEqual(Vector.Rot, Scalar.Rot, N, "rotation");
        ExpectChannelsBitEqual(Vector.Trans, Scalar.Trans, N, "translation");
        ExpectChannelsBitEqual(Vector.Scale, Scalar.Scale, N, "scale");
    }
}
