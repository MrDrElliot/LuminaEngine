#include <gtest/gtest.h>

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
