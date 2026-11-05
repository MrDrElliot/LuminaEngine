#include <gtest/gtest.h>

#include "Animation/AnimCompression.h"
#include "Animation/BindPose.h"
#include "Animation/Pose.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/SkeletonResource.h"

using namespace Lumina;

namespace
{
    constexpr float RetargetPositionTolerance = 1e-4f;
    constexpr float RetargetAngleTolerance    = 0.05f;
    constexpr float RetargetDuration          = 1.0f;
    constexpr uint32 RetargetKeys             = 31;
    constexpr float SpineSweepDegrees         = 40.0f;

    float RetargetAngleDegrees(const FQuat& A, const FQuat& B)
    {
        const FQuat Delta = A * Math::Conjugate(B);
        return Math::Degrees(2.0f * std::atan2(Math::Length(FVector3(Delta.x, Delta.y, Delta.z)), Math::Abs(Delta.w)));
    }

    // Root then Spine, the shape every FBX rig has under the node that carries its axis and unit conversion.
    TUniquePtr<FSkeletonResource> MakeRig(const FMatrix4& RootLocal, const FVector3& SpineOffset)
    {
        TUniquePtr<FSkeletonResource> Rig = MakeUnique<FSkeletonResource>();

        FSkeletonResource::FBoneInfo Root;
        Root.Name           = FName("Root");
        Root.ParentIndex    = Constants::kIndexNone;
        Root.InvBindMatrix  = FMatrix4::Identity();
        Root.LocalTransform = RootLocal;

        FSkeletonResource::FBoneInfo Spine;
        Spine.Name           = FName("Spine");
        Spine.ParentIndex    = 0;
        Spine.InvBindMatrix  = FMatrix4::Identity();
        Spine.LocalTransform = AnimPose::ComposeTRS(SpineOffset, FQuat::Identity(), FVector3(1.0f));

        Rig->BoneNameToIndex[Root.Name]  = 0;
        Rig->BoneNameToIndex[Spine.Name] = 1;
        Rig->Bones.Add(Root);
        Rig->Bones.Add(Spine);
        Rig->BuildBindPoseCache();
        return Rig;
    }

    FQuat SpineRotationAt(float Time)
    {
        const float Angle = Math::Radians(SpineSweepDegrees) * (Time / RetargetDuration);
        return FQuat(FVector3(Angle, 0.0f, 0.0f));
    }

    // A clip exported with its rig, so its root keys repeat the file's conversion and its spine keys use the file's units.
    CAnimation* MakeClipOn(CSkeleton* Source)
    {
        const FSkeletonResource& Rig = *Source->GetSkeletonResource();

        FVector3 RootT, RootS, SpineT, SpineS;
        FQuat RootR, SpineR;
        AnimPose::GetBindLocalTRS(&Rig, 0, RootT, RootR, RootS);
        AnimPose::GetBindLocalTRS(&Rig, 1, SpineT, SpineR, SpineS);

        CAnimation* Clip = NewObject<CAnimation>();
        Clip->Skeleton = Source;
        FAnimationResource& Resource = *Clip->GetAnimationResource();
        Resource.Duration = RetargetDuration;

        auto MakeChannel = [](const char* Bone, FAnimationChannel::ETargetPath Path)
        {
            FAnimationChannel Channel;
            Channel.TargetBone = FName(Bone);
            Channel.TargetPath = Path;
            for (uint32 i = 0; i < RetargetKeys; ++i)
            {
                Channel.Timestamps.Add(RetargetDuration * (float)i / (float)(RetargetKeys - 1));
            }
            return Channel;
        };

        FAnimationChannel RootTranslation = MakeChannel("Root", FAnimationChannel::ETargetPath::Translation);
        FAnimationChannel RootRotation    = MakeChannel("Root", FAnimationChannel::ETargetPath::Rotation);
        FAnimationChannel RootScale       = MakeChannel("Root", FAnimationChannel::ETargetPath::Scale);
        FAnimationChannel SpineTranslation = MakeChannel("Spine", FAnimationChannel::ETargetPath::Translation);
        FAnimationChannel SpineRotation    = MakeChannel("Spine", FAnimationChannel::ETargetPath::Rotation);

        for (uint32 i = 0; i < RetargetKeys; ++i)
        {
            const float Time = RetargetDuration * (float)i / (float)(RetargetKeys - 1);
            RootTranslation.Translations.Add(RootT);
            RootRotation.Rotations.Add(RootR);
            RootScale.Scales.Add(RootS);
            SpineTranslation.Translations.Add(SpineT);
            SpineRotation.Rotations.Add(SpineRotationAt(Time));
        }

        Resource.Channels = { RootTranslation, RootRotation, RootScale, SpineTranslation, SpineRotation };
        AnimCompression::Build(Resource);
        return Clip;
    }

    struct FRetargetRigs
    {
        CSkeleton* Source = nullptr;
        CSkeleton* Target = nullptr;
    };

    // Z-up centimeters against Y-up meters, the mismatch two separately exported FBX files produce.
    FRetargetRigs MakeMismatchedRigs()
    {
        const FMatrix4 SourceRoot = AnimPose::ComposeTRS(FVector3(0.0f), FQuat(FVector3(Math::Radians(-90.0f), 0.0f, 0.0f)), FVector3(0.01f));

        FRetargetRigs Rigs;
        Rigs.Source = NewObject<CSkeleton>();
        Rigs.Source->SetSkeletonResource(MakeRig(SourceRoot, FVector3(0.0f, 25.0f, 0.0f)));
        Rigs.Target = NewObject<CSkeleton>();
        Rigs.Target->SetSkeletonResource(MakeRig(FMatrix4::Identity(), FVector3(0.0f, 0.25f, 0.0f)));
        return Rigs;
    }
}

TEST(AnimRetarget, SameSkeletonNeedsNoFixes)
{
    const FRetargetRigs Rigs = MakeMismatchedRigs();
    CAnimation* Clip = MakeClipOn(Rigs.Source);

    const FAnimationResource::FResolvedSkeleton* Resolved = Clip->GetAnimationResource()->GetResolvedSkeleton(
        Rigs.Source->GetSkeletonResource(), Clip->GetSourceSkeletonResource());
    EXPECT_TRUE(Resolved->RetargetFixes.IsEmpty());
}

TEST(AnimRetarget, LocalPoseLandsInTargetSpace)
{
    const FRetargetRigs Rigs = MakeMismatchedRigs();
    CAnimation* Clip = MakeClipOn(Rigs.Source);
    FSkeletonResource* Target = Rigs.Target->GetSkeletonResource();

    for (float Time : { 0.0f, 0.5f, 1.0f })
    {
        FPose Pose;
        Clip->SampleLocalPose(Time, Target, Pose);

        EXPECT_NEAR(Math::Length(Pose.GetTranslation(0)), 0.0f, RetargetPositionTolerance) << "t=" << Time;
        EXPECT_NEAR(Math::Length(Pose.GetScale(0) - FVector3(1.0f)), 0.0f, RetargetPositionTolerance) << "t=" << Time;
        EXPECT_NEAR(RetargetAngleDegrees(Pose.GetRotation(0), FQuat::Identity()), 0.0f, RetargetAngleTolerance) << "t=" << Time;

        EXPECT_NEAR(Math::Length(Pose.GetTranslation(1) - FVector3(0.0f, 0.25f, 0.0f)), 0.0f, RetargetPositionTolerance) << "t=" << Time;
        EXPECT_NEAR(RetargetAngleDegrees(Pose.GetRotation(1), SpineRotationAt(Time)), 0.0f, RetargetAngleTolerance) << "t=" << Time;
    }
}

TEST(AnimRetarget, MatrixAndSingleBonePathsAgreeWithLocalPose)
{
    const FRetargetRigs Rigs = MakeMismatchedRigs();
    CAnimation* Clip = MakeClipOn(Rigs.Source);
    FSkeletonResource* Target = Rigs.Target->GetSkeletonResource();

    constexpr float Time = 0.5f;

    TVector<FMatrix4> Matrices;
    Clip->SamplePose(Time, Target, Matrices);
    ASSERT_EQ(Matrices.Num(), 2);
    EXPECT_NEAR(Math::Length(FVector3(Matrices[0][3])), 0.0f, RetargetPositionTolerance);
    EXPECT_NEAR(Math::Length(FVector3(Matrices[1][3]) - FVector3(0.0f, 0.25f, 0.0f)), 0.0f, RetargetPositionTolerance);

    FVector3 RootT, RootS, SpineT, SpineS;
    FQuat RootR, SpineR;
    Clip->SampleBoneLocal(Time, Target, 0, RootT, RootR, RootS);
    Clip->SampleBoneLocal(Time, Target, 1, SpineT, SpineR, SpineS);
    EXPECT_NEAR(Math::Length(RootT), 0.0f, RetargetPositionTolerance);
    EXPECT_NEAR(Math::Length(RootS - FVector3(1.0f)), 0.0f, RetargetPositionTolerance);
    EXPECT_NEAR(Math::Length(SpineT - FVector3(0.0f, 0.25f, 0.0f)), 0.0f, RetargetPositionTolerance);
    EXPECT_NEAR(RetargetAngleDegrees(SpineR, SpineRotationAt(Time)), 0.0f, RetargetAngleTolerance);
}

TEST(AnimRetarget, SourceSkeletonStillSamplesUnchanged)
{
    const FRetargetRigs Rigs = MakeMismatchedRigs();
    CAnimation* Clip = MakeClipOn(Rigs.Source);
    FSkeletonResource* Source = Rigs.Source->GetSkeletonResource();

    FPose Pose;
    Clip->SampleLocalPose(0.0f, Source, Pose);

    FVector3 BindT, BindS;
    FQuat BindR;
    AnimPose::GetBindLocalTRS(Source, 0, BindT, BindR, BindS);
    EXPECT_NEAR(Math::Length(Pose.GetScale(0) - BindS), 0.0f, RetargetPositionTolerance);
    EXPECT_NEAR(RetargetAngleDegrees(Pose.GetRotation(0), BindR), 0.0f, RetargetAngleTolerance);
    EXPECT_NEAR(Math::Length(Pose.GetTranslation(1) - FVector3(0.0f, 25.0f, 0.0f)), 0.0f, 1e-2f);
}
