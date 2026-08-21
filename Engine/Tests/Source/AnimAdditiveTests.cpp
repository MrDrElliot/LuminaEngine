#include <gtest/gtest.h>

#include "Animation/AnimCompression.h"
#include "Animation/BindPose.h"
#include "Animation/Pose.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Renderer/SkeletonResource.h"

using namespace Lumina;

namespace
{
    constexpr float AdditivePositionTolerance = 1e-4f;
    constexpr float AdditiveScaleTolerance    = 1e-4f;
    constexpr float AdditiveAngleTolerance    = 0.05f;

    void MakeAdditiveTestChain(FSkeletonResource& Skeleton, std::initializer_list<const char*> BoneNames)
    {
        int32 Parent = INDEX_NONE;
        for (const char* Name : BoneNames)
        {
            FSkeletonResource::FBoneInfo Bone;
            Bone.Name           = FName(Name);
            Bone.ParentIndex    = Parent;
            Bone.InvBindMatrix  = FMatrix4::Identity();
            Bone.LocalTransform = AnimPose::ComposeTRS(FVector3(0.0f, 0.25f, 0.0f), FQuat::Identity(), FVector3(1.0f));

            Skeleton.BoneNameToIndex[Bone.Name] = (int32)Skeleton.Bones.size();
            Parent = (int32)Skeleton.Bones.size();
            Skeleton.Bones.push_back(Bone);
        }

        Skeleton.BuildBindPoseCache();
    }

    float AdditiveAngleDegrees(const FQuat& A, const FQuat& B)
    {
        const FQuat Delta = A * Math::Conjugate(B);
        return Math::Degrees(2.0f * std::atan2(Math::Length(FVector3(Delta.x, Delta.y, Delta.z)), Math::Abs(Delta.w)));
    }

    FQuat AdditiveComponentRotation(const FPose& Pose, const FSkeletonResource& Skeleton, int32 BoneIndex)
    {
        int32 Chain[16];
        int32 Length = 0;
        for (int32 Cursor = BoneIndex; Cursor >= 0; Cursor = Skeleton.GetBone(Cursor).ParentIndex)
        {
            Chain[Length++] = Cursor;
        }

        FQuat Result = FQuat::Identity();
        for (int32 i = Length - 1; i >= 0; --i)
        {
            Result = Result * Pose.Rotations[Chain[i]];
        }
        return Result;
    }

    void ExpectAdditivePosesMatch(const FPose& Actual, const FPose& Expected)
    {
        ASSERT_EQ(Actual.GetNumBones(), Expected.GetNumBones());
        for (int32 i = 0; i < Actual.GetNumBones(); ++i)
        {
            EXPECT_NEAR(Math::Length(Actual.Translations[i] - Expected.Translations[i]), 0.0f, AdditivePositionTolerance) << "bone " << i;
            EXPECT_NEAR(Math::Length(Actual.Scales[i] - Expected.Scales[i]), 0.0f, AdditiveScaleTolerance) << "bone " << i;
            EXPECT_NEAR(AdditiveAngleDegrees(Actual.Rotations[i], Expected.Rotations[i]), 0.0f, AdditiveAngleTolerance) << "bone " << i;
        }
    }

    // Differs from bind on every channel, so a round trip cannot pass by accident.
    FPose MakeAdditiveTestPose(const FSkeletonResource& Skeleton, float Seed)
    {
        FPose Pose;
        Pose.ResetToBindPose(&Skeleton);
        for (int32 i = 0; i < Pose.GetNumBones(); ++i)
        {
            const float Scaled = Seed * (float)(i + 1);
            Pose.Translations[i] += FVector3(0.11f * Scaled, -0.07f * Scaled, 0.19f * Scaled);
            Pose.Rotations[i]     = Math::Normalize(FQuat(FVector3(Math::Radians(17.0f * Scaled),
                                                                   Math::Radians(-23.0f * Scaled),
                                                                   Math::Radians(9.0f * Scaled))) * Pose.Rotations[i]);
            Pose.Scales[i]       *= FVector3(1.0f + 0.05f * Scaled, 1.0f - 0.03f * Scaled, 1.0f + 0.08f * Scaled);
        }
        return Pose;
    }

    FAnimationChannel MakeAdditiveTestChannel(const FName& Bone, FAnimationChannel::ETargetPath Path, float Duration, uint32 NumKeys)
    {
        FAnimationChannel Channel;
        Channel.TargetBone = Bone;
        Channel.TargetPath = Path;
        for (uint32 i = 0; i < NumKeys; ++i)
        {
            Channel.Timestamps.push_back(Duration * (float)i / (float)(NumKeys - 1));
        }
        return Channel;
    }

    void AddAdditiveRotationSweep(FAnimationResource& Resource, const FName& Bone, float Duration, uint32 NumKeys, float SweepDegrees)
    {
        FAnimationChannel Channel = MakeAdditiveTestChannel(Bone, FAnimationChannel::ETargetPath::Rotation, Duration, NumKeys);
        for (uint32 i = 0; i < NumKeys; ++i)
        {
            const float Angle = Math::Radians(SweepDegrees) * (float)i / (float)(NumKeys - 1);
            Channel.Rotations.push_back(FQuat(FVector3(Angle, Angle * 0.5f, -Angle * 0.25f)));
        }
        Resource.Channels.push_back(Channel);
    }

    void AddAdditiveTranslationSweep(FAnimationResource& Resource, const FName& Bone, float Duration, uint32 NumKeys, const FVector3& Travel)
    {
        FAnimationChannel Channel = MakeAdditiveTestChannel(Bone, FAnimationChannel::ETargetPath::Translation, Duration, NumKeys);
        for (uint32 i = 0; i < NumKeys; ++i)
        {
            Channel.Translations.push_back(Travel * ((float)i / (float)(NumKeys - 1)));
        }
        Resource.Channels.push_back(Channel);
    }

    CAnimation* MakeAdditiveTestClip(float Duration, float SweepDegrees, const FVector3& Travel)
    {
        CAnimation* Clip = NewObject<CAnimation>();
        FAnimationResource& Resource = *Clip->GetAnimationResource();
        Resource.Duration = Duration;

        AddAdditiveRotationSweep(Resource, FName("Root"), Duration, 31, SweepDegrees);
        AddAdditiveTranslationSweep(Resource, FName("Root"), Duration, 31, Travel);
        AddAdditiveRotationSweep(Resource, FName("Spine"), Duration, 31, -SweepDegrees * 0.6f);
        AddAdditiveRotationSweep(Resource, FName("Hand"), Duration, 31, SweepDegrees * 1.4f);

        AnimCompression::Build(Resource);
        return Clip;
    }
}

TEST(AnimAdditive, LocalSpaceRoundTripsAgainstBindPose)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    const FPose Source = MakeAdditiveTestPose(Skeleton, 1.0f);

    FPose Delta;
    AnimPose::MakeAdditive(Source, &Skeleton, Delta);
    EXPECT_EQ(Delta.AdditiveSpace, EPoseAdditiveSpace::LocalSpace);

    FPose Bind;
    Bind.ResetToBindPose(&Skeleton);

    FPose Result;
    AnimPose::ApplyAdditive(Bind, Delta, 1.0f, Result);
    ExpectAdditivePosesMatch(Result, Source);
    EXPECT_EQ(Result.AdditiveSpace, EPoseAdditiveSpace::None);
}

TEST(AnimAdditive, LocalSpaceRoundTripsAgainstAnArbitraryBase)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    const FPose Base   = MakeAdditiveTestPose(Skeleton, 0.4f);
    const FPose Source = MakeAdditiveTestPose(Skeleton, 1.3f);

    FPose Delta;
    AnimPose::MakeAdditiveFromBase(Source, Base, Delta);
    EXPECT_EQ(Delta.AdditiveSpace, EPoseAdditiveSpace::LocalSpace);

    FPose Result;
    AnimPose::ApplyAdditive(Base, Delta, 1.0f, Result);
    ExpectAdditivePosesMatch(Result, Source);
}

TEST(AnimAdditive, MeshSpaceRoundTripsAgainstItsOwnBase)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    const FPose Base   = MakeAdditiveTestPose(Skeleton, 0.4f);
    const FPose Source = MakeAdditiveTestPose(Skeleton, 1.3f);

    FPose Delta;
    AnimPose::MakeAdditiveMeshSpace(Source, Base, &Skeleton, Delta);
    EXPECT_EQ(Delta.AdditiveSpace, EPoseAdditiveSpace::MeshSpace);

    FPose Result;
    AnimPose::ApplyAdditiveMeshSpace(Base, Delta, 1.0f, &Skeleton, Result);
    ExpectAdditivePosesMatch(Result, Source);
}

// The local-space half proves the two modes really differ, not just that mesh space is self-consistent.
TEST(AnimAdditive, MeshSpaceDeltaIsIndependentOfTheBaseParentChain)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });
    const int32 Hand = Skeleton.FindBoneIndex(FName("Hand"));

    FPose BaseA;
    BaseA.ResetToBindPose(&Skeleton);

    FPose BaseB = BaseA;
    BaseB.Rotations[0] = FQuat(FVector3(0.0f, 0.0f, Math::Radians(50.0f))) * BaseB.Rotations[0];

    FPose Aimed = BaseA;
    Aimed.Rotations[Hand] = FQuat(FVector3(Math::Radians(30.0f), 0.0f, 0.0f)) * Aimed.Rotations[Hand];

    FPose MeshDelta;
    AnimPose::MakeAdditiveMeshSpace(Aimed, BaseA, &Skeleton, MeshDelta);

    FPose MeshOnA, MeshOnB;
    AnimPose::ApplyAdditiveMeshSpace(BaseA, MeshDelta, 1.0f, &Skeleton, MeshOnA);
    AnimPose::ApplyAdditiveMeshSpace(BaseB, MeshDelta, 1.0f, &Skeleton, MeshOnB);

    const FQuat MeshTurnOnA = AdditiveComponentRotation(MeshOnA, Skeleton, Hand) * Math::Conjugate(AdditiveComponentRotation(BaseA, Skeleton, Hand));
    const FQuat MeshTurnOnB = AdditiveComponentRotation(MeshOnB, Skeleton, Hand) * Math::Conjugate(AdditiveComponentRotation(BaseB, Skeleton, Hand));
    EXPECT_NEAR(AdditiveAngleDegrees(MeshTurnOnA, MeshTurnOnB), 0.0f, AdditiveAngleTolerance);

    FPose LocalDelta;
    AnimPose::MakeAdditiveFromBase(Aimed, BaseA, LocalDelta);

    FPose LocalOnA, LocalOnB;
    AnimPose::ApplyAdditive(BaseA, LocalDelta, 1.0f, LocalOnA);
    AnimPose::ApplyAdditive(BaseB, LocalDelta, 1.0f, LocalOnB);

    const FQuat LocalTurnOnA = AdditiveComponentRotation(LocalOnA, Skeleton, Hand) * Math::Conjugate(AdditiveComponentRotation(BaseA, Skeleton, Hand));
    const FQuat LocalTurnOnB = AdditiveComponentRotation(LocalOnB, Skeleton, Hand) * Math::Conjugate(AdditiveComponentRotation(BaseB, Skeleton, Hand));
    EXPECT_GT(AdditiveAngleDegrees(LocalTurnOnA, LocalTurnOnB), 1.0f);
}

TEST(AnimAdditive, ZeroAlphaLeavesTheBaseAlone)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    const FPose Base   = MakeAdditiveTestPose(Skeleton, 0.4f);
    const FPose Source = MakeAdditiveTestPose(Skeleton, 1.3f);

    FPose LocalDelta, MeshDelta;
    AnimPose::MakeAdditiveFromBase(Source, Base, LocalDelta);
    AnimPose::MakeAdditiveMeshSpace(Source, Base, &Skeleton, MeshDelta);

    FPose Result;
    AnimPose::ApplyAdditive(Base, LocalDelta, 0.0f, Result);
    ExpectAdditivePosesMatch(Result, Base);

    AnimPose::ApplyAdditiveMeshSpace(Base, MeshDelta, 0.0f, &Skeleton, Result);
    ExpectAdditivePosesMatch(Result, Base);
}

TEST(AnimAdditive, ApplyAdditivePoseDispatchesOnTheDeltaSpace)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    const FPose Base   = MakeAdditiveTestPose(Skeleton, 0.4f);
    const FPose Source = MakeAdditiveTestPose(Skeleton, 1.3f);

    FPose MeshDelta;
    AnimPose::MakeAdditiveMeshSpace(Source, Base, &Skeleton, MeshDelta);

    FPose Dispatched, Explicit;
    AnimPose::ApplyAdditivePose(Base, MeshDelta, 0.6f, &Skeleton, Dispatched);
    AnimPose::ApplyAdditiveMeshSpace(Base, MeshDelta, 0.6f, &Skeleton, Explicit);
    ExpectAdditivePosesMatch(Dispatched, Explicit);

    FPose LocalDelta;
    AnimPose::MakeAdditiveFromBase(Source, Base, LocalDelta);
    AnimPose::ApplyAdditivePose(Base, LocalDelta, 0.6f, &Skeleton, Dispatched);
    AnimPose::ApplyAdditive(Base, LocalDelta, 0.6f, Explicit);
    ExpectAdditivePosesMatch(Dispatched, Explicit);
}

// A non-identity delta past the cut would drag those bones off the base pose.
TEST(AnimAdditive, BonesPastTheLODCutCarryAnIdentityDelta)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    const FPose Base   = MakeAdditiveTestPose(Skeleton, 0.4f);
    const FPose Source = MakeAdditiveTestPose(Skeleton, 1.3f);

    FPose Delta;
    AnimPose::MakeAdditiveFromBase(Source, Base, Delta, 2);

    EXPECT_NEAR(Math::Length(Delta.Translations[2]), 0.0f, AdditivePositionTolerance);
    EXPECT_NEAR(AdditiveAngleDegrees(Delta.Rotations[2], FQuat::Identity()), 0.0f, AdditiveAngleTolerance);
    EXPECT_NEAR(Math::Length(Delta.Scales[2] - FVector3(1.0f)), 0.0f, AdditiveScaleTolerance);

    FPose Result;
    AnimPose::ApplyAdditive(Base, Delta, 1.0f, Result);
    EXPECT_NEAR(AdditiveAngleDegrees(Result.Rotations[2], Base.Rotations[2]), 0.0f, AdditiveAngleTolerance);
}

TEST(AnimAdditiveSampling, RefPoseBaseSamplesTheDeltaAgainstBind)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    CAnimation* Clip = MakeAdditiveTestClip(1.0f, 40.0f, FVector3(0.0f, 0.0f, 2.0f));

    FPose Raw;
    Clip->SampleLocalPose(0.5f, &Skeleton, Raw);
    EXPECT_EQ(Raw.AdditiveSpace, EPoseAdditiveSpace::None);

    Clip->AdditiveAnimType = EAdditiveAnimType::LocalSpace;

    FPose Delta;
    Clip->SampleLocalPose(0.5f, &Skeleton, Delta);
    EXPECT_EQ(Delta.AdditiveSpace, EPoseAdditiveSpace::LocalSpace);

    FPose Bind;
    Bind.ResetToBindPose(&Skeleton);

    FPose Result;
    AnimPose::ApplyAdditivePose(Bind, Delta, 1.0f, &Skeleton, Result);
    ExpectAdditivePosesMatch(Result, Raw);
}

TEST(AnimAdditiveSampling, AnimFrameBaseSubtractsOneFrameOfTheBaseClip)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    CAnimation* BaseClip = MakeAdditiveTestClip(1.0f, 25.0f, FVector3(0.0f, 0.0f, 1.0f));
    CAnimation* Clip     = MakeAdditiveTestClip(1.0f, 40.0f, FVector3(0.0f, 0.0f, 2.0f));

    Clip->AdditiveAnimType      = EAdditiveAnimType::LocalSpace;
    Clip->AdditiveBasePoseType  = EAdditiveBasePoseType::AnimFrame;
    Clip->AdditiveBaseAnimation = BaseClip;
    Clip->AdditiveBaseFrameTime = 0.25f;

    FPose Raw;
    Clip->SampleRawLocalPose(0.7f, &Skeleton, Raw);

    FPose BaseFrame;
    BaseClip->SampleRawLocalPose(0.25f, &Skeleton, BaseFrame);

    FPose Delta;
    Clip->SampleLocalPose(0.7f, &Skeleton, Delta);

    FPose Result;
    AnimPose::ApplyAdditivePose(BaseFrame, Delta, 1.0f, &Skeleton, Result);
    ExpectAdditivePosesMatch(Result, Raw);
}

TEST(AnimAdditiveSampling, AnimScaledBaseFollowsTheBaseClipOverTime)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    CAnimation* BaseClip = MakeAdditiveTestClip(2.0f, 25.0f, FVector3(0.0f, 0.0f, 1.0f));
    CAnimation* Clip     = MakeAdditiveTestClip(1.0f, 40.0f, FVector3(0.0f, 0.0f, 2.0f));

    Clip->AdditiveAnimType      = EAdditiveAnimType::LocalSpace;
    Clip->AdditiveBasePoseType  = EAdditiveBasePoseType::AnimScaled;
    Clip->AdditiveBaseAnimation = BaseClip;

    // Half way through the 1s clip maps to half way through the 2s base clip.
    FPose Base;
    BaseClip->SampleRawLocalPose(1.0f, &Skeleton, Base);

    FPose Raw;
    Clip->SampleRawLocalPose(0.5f, &Skeleton, Raw);

    FPose Delta;
    Clip->SampleLocalPose(0.5f, &Skeleton, Delta);

    FPose Result;
    AnimPose::ApplyAdditivePose(Base, Delta, 1.0f, &Skeleton, Result);
    ExpectAdditivePosesMatch(Result, Raw);
}

TEST(AnimAdditiveSampling, MeshSpaceClipStampsMeshSpaceOnItsDelta)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    CAnimation* Clip = MakeAdditiveTestClip(1.0f, 40.0f, FVector3(0.0f, 0.0f, 2.0f));
    Clip->AdditiveAnimType = EAdditiveAnimType::MeshSpace;

    FPose Raw;
    Clip->SampleRawLocalPose(0.5f, &Skeleton, Raw);

    FPose Delta;
    Clip->SampleLocalPose(0.5f, &Skeleton, Delta);
    EXPECT_EQ(Delta.AdditiveSpace, EPoseAdditiveSpace::MeshSpace);

    FPose Bind;
    Bind.ResetToBindPose(&Skeleton);

    FPose Result;
    AnimPose::ApplyAdditivePose(Bind, Delta, 1.0f, &Skeleton, Result);
    ExpectAdditivePosesMatch(Result, Raw);
}

// A base clip pointing back at the additive clip would recurse forever.
TEST(AnimAdditiveSampling, SelfReferencingBaseFallsBackToTheRefPose)
{
    FSkeletonResource Skeleton;
    MakeAdditiveTestChain(Skeleton, { "Root", "Spine", "Hand" });

    CAnimation* Clip = MakeAdditiveTestClip(1.0f, 40.0f, FVector3(0.0f, 0.0f, 2.0f));
    Clip->AdditiveAnimType      = EAdditiveAnimType::LocalSpace;
    Clip->AdditiveBasePoseType  = EAdditiveBasePoseType::AnimFrame;
    Clip->AdditiveBaseAnimation = Clip;

    EXPECT_EQ(Clip->GetAdditiveBaseAnimation(), nullptr);

    FPose Delta;
    Clip->SampleLocalPose(0.5f, &Skeleton, Delta);

    FPose Bind;
    Bind.ResetToBindPose(&Skeleton);

    FPose Raw;
    Clip->SampleRawLocalPose(0.5f, &Skeleton, Raw);

    FPose Result;
    AnimPose::ApplyAdditivePose(Bind, Delta, 1.0f, &Skeleton, Result);
    ExpectAdditivePosesMatch(Result, Raw);
}
