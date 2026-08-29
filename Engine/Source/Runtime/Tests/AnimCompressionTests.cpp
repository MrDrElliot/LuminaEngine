#include <gtest/gtest.h>

#include "Animation/AnimCompression.h"
#include "Animation/BindPose.h"
#include "Animation/Pose.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Versioning/CoreVersion.h"
#include "Renderer/SkeletonResource.h"

using namespace Lumina;

namespace
{
    // A few times the measured worst case, so a regression in the quantizer trips these without flaking.
    constexpr float TranslationBudget     = 1e-6f;
    constexpr float ScaleBudget           = 5e-5f;
    constexpr float RotationBudgetDegrees = 0.01f;

    // FK amplifies a bone's rotation error by the chain's reach, so a composed matrix cannot hold the local budget.
    constexpr float SkinningMatrixBudget = 1e-3f;

    FAnimationChannel MakeChannel(const FName& Bone, FAnimationChannel::ETargetPath Path, float Duration, uint32 NumKeys)
    {
        FAnimationChannel Channel;
        Channel.TargetBone = Bone;
        Channel.TargetPath = Path;
        Channel.Timestamps.reserve(NumKeys);
        for (uint32 i = 0; i < NumKeys; ++i)
        {
            Channel.Timestamps.push_back(Duration * (float)i / (float)(NumKeys - 1));
        }
        return Channel;
    }

    void AddRotation(FAnimationResource& Resource, const FName& Bone, float Duration, uint32 NumKeys, float SweepRadians)
    {
        FAnimationChannel Channel = MakeChannel(Bone, FAnimationChannel::ETargetPath::Rotation, Duration, NumKeys);
        for (uint32 i = 0; i < NumKeys; ++i)
        {
            const float Angle = SweepRadians * (float)i / (float)(NumKeys - 1);
            Channel.Rotations.push_back(FQuat(FVector3(Angle, Angle * 0.5f, -Angle * 0.25f)));
        }
        Resource.Channels.push_back(Channel);
    }

    void AddTranslation(FAnimationResource& Resource, const FName& Bone, float Duration, uint32 NumKeys, const FVector3& Travel)
    {
        FAnimationChannel Channel = MakeChannel(Bone, FAnimationChannel::ETargetPath::Translation, Duration, NumKeys);
        for (uint32 i = 0; i < NumKeys; ++i)
        {
            Channel.Translations.push_back(Travel * ((float)i / (float)(NumKeys - 1)));
        }
        Resource.Channels.push_back(Channel);
    }

    void AddConstantTranslation(FAnimationResource& Resource, const FName& Bone, float Duration, uint32 NumKeys, const FVector3& Value)
    {
        FAnimationChannel Channel = MakeChannel(Bone, FAnimationChannel::ETargetPath::Translation, Duration, NumKeys);
        Channel.Translations.assign(NumKeys, Value);
        Resource.Channels.push_back(Channel);
    }

    void AddScale(FAnimationResource& Resource, const FName& Bone, float Duration, uint32 NumKeys, float From, float To)
    {
        FAnimationChannel Channel = MakeChannel(Bone, FAnimationChannel::ETargetPath::Scale, Duration, NumKeys);
        for (uint32 i = 0; i < NumKeys; ++i)
        {
            const float Value = Math::Mix(From, To, (float)i / (float)(NumKeys - 1));
            Channel.Scales.push_back(FVector3(Value, Value, Value));
        }
        Resource.Channels.push_back(Channel);
    }

    const FCompressedAnimBone* FindBone(const FCompressedAnimData& Data, const FName& Name)
    {
        for (const FCompressedAnimBone& Bone : Data.Bones)
        {
            if (Bone.BoneName == Name)
            {
                return &Bone;
            }
        }
        return nullptr;
    }
}

TEST(AnimCompression, InfersAuthoredSampleRate)
{
    FAnimationResource Resource;
    Resource.Duration = 2.0f;
    AddRotation(Resource, FName("Spine"), 2.0f, 61, Math::Radians(90.0f));

    EXPECT_FLOAT_EQ(AnimCompression::InferSampleRate(Resource), 30.0f);

    AnimCompression::Build(Resource);
    EXPECT_EQ(Resource.Compressed.NumFrames, 61u);
    EXPECT_FLOAT_EQ(Resource.Compressed.SampleRate, 30.0f);
}

TEST(AnimCompression, UniformClipStaysWithinBudget)
{
    FAnimationResource Resource;
    Resource.Duration = 1.0f;
    AddRotation(Resource, FName("Spine"), 1.0f, 31, Math::Radians(120.0f));
    AddRotation(Resource, FName("Hand"), 1.0f, 31, Math::Radians(-45.0f));
    AddConstantTranslation(Resource, FName("Hand"), 1.0f, 31, FVector3(0.0f, 0.35f, 0.0f));
    AddScale(Resource, FName("Spine"), 1.0f, 31, 1.0f, 1.5f);

    AnimCompression::Build(Resource);

    const AnimCompression::FValidationReport Report = AnimCompression::Validate(Resource);
    ASSERT_TRUE(Report.bHasCompressedData);

    EXPECT_LT(Report.MaxTranslationError, TranslationBudget);
    EXPECT_LT(Report.MaxScaleError, ScaleBudget);
    EXPECT_LT(Math::Degrees(Report.MaxRotationRadians), RotationBudgetDegrees);
}

TEST(AnimCompression, CollapsesUnmovingTracks)
{
    FAnimationResource Resource;
    Resource.Duration = 1.0f;
    AddRotation(Resource, FName("Spine"), 1.0f, 31, Math::Radians(120.0f));
    AddConstantTranslation(Resource, FName("Spine"), 1.0f, 31, FVector3(0.0f, 0.35f, 0.0f));
    AddScale(Resource, FName("Spine"), 1.0f, 31, 1.0f, 1.0f);

    AnimCompression::Build(Resource);

    const FCompressedAnimBone* Bone = FindBone(Resource.Compressed, FName("Spine"));
    ASSERT_NE(Bone, nullptr);

    EXPECT_EQ(Bone->Translation.Format, EAnimTrackFormat::Constant);
    EXPECT_EQ(Bone->Scale.Format, EAnimTrackFormat::Constant);
    EXPECT_EQ(Bone->Rotation.Format, EAnimTrackFormat::Quantized);

    // Only the rotation should own frame data, 31 frames of four lanes.
    EXPECT_EQ(Resource.Compressed.QuantizedData.size(), 31u * 4u);
    EXPECT_TRUE(Resource.Compressed.RawData.empty());
}

TEST(AnimCompression, RootTranslationKeepsFullPrecision)
{
    FAnimationResource Resource;
    Resource.Duration = 1.0f;
    AddTranslation(Resource, FName("Root"), 1.0f, 31, FVector3(0.0f, 0.0f, 4.0f));

    AnimCompression::Build(Resource);

    const FCompressedAnimBone* Bone = FindBone(Resource.Compressed, FName("Root"));
    ASSERT_NE(Bone, nullptr);
    EXPECT_EQ(Bone->Translation.Format, EAnimTrackFormat::Raw);

    const AnimCompression::FValidationReport Report = AnimCompression::Validate(Resource);
    EXPECT_LT(Report.MaxTranslationError, 1e-6f);
}

TEST(AnimCompression, ShortensChannelsIntoFewerBytes)
{
    FAnimationResource Resource;
    Resource.Duration = 2.0f;
    for (uint32 i = 0; i < 40; ++i)
    {
        const FName Bone("Bone", i);
        AddRotation(Resource, Bone, 2.0f, 61, Math::Radians(30.0f));
        AddConstantTranslation(Resource, Bone, 2.0f, 61, FVector3(0.0f, 0.2f, 0.0f));
        AddScale(Resource, Bone, 2.0f, 61, 1.0f, 1.0f);
    }

    AnimCompression::Build(Resource);

    const AnimCompression::FValidationReport Report = AnimCompression::Validate(Resource);
    ASSERT_TRUE(Report.bHasCompressedData);
    EXPECT_LT(Report.CompressedBytes, Report.SourceBytes / 4);
}

TEST(AnimCompression, SingleKeyChannelsBecomeConstant)
{
    FAnimationResource Resource;
    Resource.Duration = 1.0f;

    FAnimationChannel Channel;
    Channel.TargetBone = FName("Spine");
    Channel.TargetPath = FAnimationChannel::ETargetPath::Rotation;
    Channel.Timestamps.push_back(0.0f);
    Channel.Rotations.push_back(FQuat(FVector3(Math::Radians(20.0f), 0.0f, 0.0f)));
    Resource.Channels.push_back(Channel);

    AnimCompression::Build(Resource);

    const FCompressedAnimBone* Bone = FindBone(Resource.Compressed, FName("Spine"));
    ASSERT_NE(Bone, nullptr);
    EXPECT_EQ(Bone->Rotation.Format, EAnimTrackFormat::Constant);
    EXPECT_TRUE(Resource.Compressed.QuantizedData.empty());
}

// Sparse authoring resamples to a rate matching its own key spacing, so the keys still land on frames.
TEST(AnimCompression, SparseClipStillReproducesItsKeys)
{
    FAnimationResource Resource;
    Resource.Duration = 4.0f;
    AddRotation(Resource, FName("Spine"), 4.0f, 5, Math::Radians(90.0f));

    AnimCompression::Build(Resource);
    EXPECT_EQ(Resource.Compressed.NumFrames, 5u);

    const AnimCompression::FValidationReport Report = AnimCompression::Validate(Resource);
    EXPECT_LT(Math::Degrees(Report.MaxRotationRadians), RotationBudgetDegrees);
}

TEST(AnimCompression, ZeroDurationClipIsAllConstant)
{
    FAnimationResource Resource;
    Resource.Duration = 0.0f;
    AddRotation(Resource, FName("Spine"), 0.0f, 2, Math::Radians(90.0f));

    AnimCompression::Build(Resource);

    EXPECT_EQ(Resource.Compressed.NumFrames, 1u);
    const FCompressedAnimBone* Bone = FindBone(Resource.Compressed, FName("Spine"));
    ASSERT_NE(Bone, nullptr);
    EXPECT_EQ(Bone->Rotation.Format, EAnimTrackFormat::Constant);
}

namespace
{
    void MakeSkeleton(FSkeletonResource& Skeleton, std::initializer_list<const char*> BoneNames)
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

    CAnimation* MakeAnimatedClip()
    {
        CAnimation* Clip = NewObject<CAnimation>();
        FAnimationResource& Resource = *Clip->GetAnimationResource();
        Resource.Duration = 1.0f;

        AddRotation(Resource, FName("Root"), 1.0f, 31, Math::Radians(35.0f));
        AddTranslation(Resource, FName("Root"), 1.0f, 31, FVector3(0.0f, 0.0f, 3.0f));
        AddRotation(Resource, FName("Spine"), 1.0f, 31, Math::Radians(-70.0f));
        AddScale(Resource, FName("Spine"), 1.0f, 31, 1.0f, 1.4f);
        AddRotation(Resource, FName("Hand"), 1.0f, 31, Math::Radians(110.0f));
        AddConstantTranslation(Resource, FName("Hand"), 1.0f, 31, FVector3(0.1f, 0.2f, 0.3f));

        AnimCompression::Build(Resource);
        return Clip;
    }
}

namespace
{
    const FAnimationChannel* FindChannel(const FAnimationResource& Resource, const FName& Bone,
                                         FAnimationChannel::ETargetPath Path)
    {
        for (const FAnimationChannel& Channel : Resource.Channels)
        {
            if (Channel.TargetBone == Bone && Channel.TargetPath == Path)
            {
                return &Channel;
            }
        }
        return nullptr;
    }

    float AngleBetweenDegrees(const FQuat& A, const FQuat& B)
    {
        const FQuat Delta = A * Math::Conjugate(B);
        return Math::Degrees(2.0f * std::atan2(Math::Length(FVector3(Delta.x, Delta.y, Delta.z)), Math::Abs(Delta.w)));
    }
}

// End to end, the pose the sampler hands back has to match the keys the clip came from.
TEST(AnimCompressionSampler, LocalPoseMatchesSourceKeys)
{
    FSkeletonResource Skeleton;
    MakeSkeleton(Skeleton, { "Root", "Spine", "Hand" });

    CAnimation* Clip = MakeAnimatedClip();
    const FAnimationResource& Resource = *Clip->GetAnimationResource();
    ASSERT_TRUE(Resource.Compressed.IsValid());
    ASSERT_FALSE(Resource.Channels.empty());

    for (uint32 Step = 0; Step <= 120; ++Step)
    {
        const float Time = (float)Step / 120.0f;

        FPose Pose;
        Clip->SampleLocalPose(Time, &Skeleton, Pose);

        for (int32 b = 0; b < Skeleton.GetNumBones(); ++b)
        {
            const FName& Bone = Skeleton.GetBone(b).Name;

            if (const FAnimationChannel* Channel = FindChannel(Resource, Bone, FAnimationChannel::ETargetPath::Translation))
            {
                const FVector3 Reference = AnimCompression::SampleKeysVec3(Channel->Timestamps, Channel->Translations, Time);
                EXPECT_LT(Math::Length(Pose.GetTranslation(b) - Reference), TranslationBudget) << "bone " << b;
            }

            if (const FAnimationChannel* Channel = FindChannel(Resource, Bone, FAnimationChannel::ETargetPath::Scale))
            {
                const FVector3 Reference = AnimCompression::SampleKeysVec3(Channel->Timestamps, Channel->Scales, Time);
                EXPECT_LT(Math::Length(Pose.GetScale(b) - Reference), ScaleBudget) << "bone " << b;
            }

            if (const FAnimationChannel* Channel = FindChannel(Resource, Bone, FAnimationChannel::ETargetPath::Rotation))
            {
                const FQuat Reference = AnimCompression::SampleKeysQuat(Channel->Timestamps, Channel->Rotations, Time);
                EXPECT_LT(AngleBetweenDegrees(Pose.GetRotation(b), Reference), RotationBudgetDegrees) << "bone " << b;
            }
        }
    }
}

TEST(AnimCompressionSampler, BoneLocalMatchesFullPose)
{
    FSkeletonResource Skeleton;
    MakeSkeleton(Skeleton, { "Root", "Spine", "Hand" });

    CAnimation* Clip = MakeAnimatedClip();

    for (uint32 Step = 0; Step <= 60; ++Step)
    {
        const float Time = (float)Step / 60.0f;

        FPose Reference;
        Clip->SampleLocalPose(Time, &Skeleton, Reference);

        for (int32 b = 0; b < Skeleton.GetNumBones(); ++b)
        {
            FVector3 T, S;
            FQuat R;
            Clip->SampleBoneLocal(Time, &Skeleton, b, T, R, S);

            EXPECT_LT(Math::Length(T - Reference.GetTranslation(b)), 1e-6f) << "bone " << b;
            EXPECT_LT(Math::Length(S - Reference.GetScale(b)), 1e-6f) << "bone " << b;
            EXPECT_GT(Math::Abs(Math::Dot(R, Reference.GetRotation(b))), 1.0f - 1e-6f) << "bone " << b;
        }
    }
}

TEST(AnimCompressionSampler, BonesWithoutTracksKeepBindPose)
{
    FSkeletonResource Skeleton;
    MakeSkeleton(Skeleton, { "Root", "Spine", "Hand", "Prop" });

    CAnimation* Clip = MakeAnimatedClip();

    FVector3 BindT, BindS;
    FQuat BindR;
    AnimPose::GetBindLocalTRS(&Skeleton, 3, BindT, BindR, BindS);

    FPose Pose;
    Clip->SampleLocalPose(0.5f, &Skeleton, Pose);

    EXPECT_LT(Math::Length(Pose.GetTranslation(3) - BindT), 1e-6f);
    EXPECT_LT(Math::Length(Pose.GetScale(3) - BindS), 1e-6f);
    EXPECT_GT(Math::Abs(Math::Dot(Pose.GetRotation(3), BindR)), 1.0f - 1e-6f);
}

TEST(AnimCompressionSampler, LodCutLeavesTrailingBonesAtBindPose)
{
    FSkeletonResource Skeleton;
    MakeSkeleton(Skeleton, { "Root", "Spine", "Hand" });

    CAnimation* Clip = MakeAnimatedClip();

    FVector3 BindT, BindS;
    FQuat BindR;
    AnimPose::GetBindLocalTRS(&Skeleton, 2, BindT, BindR, BindS);

    FPose Pose;
    Clip->SampleLocalPose(0.5f, &Skeleton, Pose, 2);

    EXPECT_GT(Math::Abs(Math::Dot(Pose.GetRotation(2), BindR)), 1.0f - 1e-6f);
    EXPECT_LT(Math::Length(Pose.GetTranslation(2) - BindT), 1e-6f);
}

// SamplePose fuses gather, FK and InvBind; it has to agree with SampleLocalPose run through the same chain.
TEST(AnimCompressionSampler, SkinningMatricesMatchPoseChain)
{
    FSkeletonResource Skeleton;
    MakeSkeleton(Skeleton, { "Root", "Spine", "Hand" });

    CAnimation* Clip = MakeAnimatedClip();

    for (uint32 Step = 0; Step <= 30; ++Step)
    {
        const float Time = (float)Step / 30.0f;

        TVector<FMatrix4> Skinning;
        Clip->SamplePose(Time, &Skeleton, Skinning);

        FPose Pose;
        Clip->SampleLocalPose(Time, &Skeleton, Pose);

        TVector<FMatrix4> Expected(Skeleton.GetNumBones());
        for (int32 b = 0; b < Skeleton.GetNumBones(); ++b)
        {
            const FMatrix4 Local = AnimPose::ComposeTRS(Pose.GetTranslation(b), Pose.GetRotation(b), Pose.GetScale(b));
            const int32 Parent = Skeleton.GetBone(b).ParentIndex;
            Expected[b] = Parent != INDEX_NONE ? Expected[Parent] * Local : Local;
        }

        ASSERT_EQ(Skinning.size(), Expected.size());
        for (int32 b = 0; b < Skeleton.GetNumBones(); ++b)
        {
            const FMatrix4 Reference = Expected[b] * Skeleton.GetBone(b).InvBindMatrix;
            for (int32 Col = 0; Col < 4; ++Col)
            {
                EXPECT_LT(Math::Length(FVector3(Skinning[b][Col] - Reference[Col])), SkinningMatrixBudget)
                    << "bone " << b << " column " << Col;
            }
        }
    }
}

namespace
{
    void WriteResource(FAnimationResource& Resource, TVector<uint8>& OutBytes, ELuminaEngineVersion Version)
    {
        FMemoryWriter Writer(OutBytes);
        Writer.SetFileVersion((int32)Version);
        Writer << Resource;
    }

    void ReadResource(const TVector<uint8>& Bytes, FAnimationResource& OutResource, ELuminaEngineVersion Version)
    {
        FMemoryReader Reader(Bytes);
        Reader.SetFileVersion((int32)Version);
        Reader << OutResource;
    }
}

// The cutover is only real if a saved clip stops carrying its channels.
TEST(AnimCompressionSerialization, CurrentVersionDropsChannels)
{
    FAnimationResource Source;
    Source.Duration = 1.0f;
    AddRotation(Source, FName("Spine"), 1.0f, 31, Math::Radians(60.0f));
    AddTranslation(Source, FName("Root"), 1.0f, 31, FVector3(0.0f, 0.0f, 2.0f));
    AnimCompression::Build(Source);

    TVector<uint8> Bytes;
    WriteResource(Source, Bytes, ELuminaEngineVersion::AUTOMATIC_VERSION);

    FAnimationResource Loaded;
    ReadResource(Bytes, Loaded, ELuminaEngineVersion::AUTOMATIC_VERSION);

    EXPECT_TRUE(Loaded.Channels.empty());
    ASSERT_TRUE(Loaded.Compressed.IsValid());
    EXPECT_EQ(Loaded.Compressed.NumFrames, Source.Compressed.NumFrames);
    EXPECT_EQ(Loaded.Compressed.Bones.size(), Source.Compressed.Bones.size());
    EXPECT_EQ(Loaded.Compressed.QuantizedData, Source.Compressed.QuantizedData);
    EXPECT_EQ(Loaded.Compressed.RawData, Source.Compressed.RawData);
}

// Clips written before the cutover still have to load, and carry enough to compress on the spot.
TEST(AnimCompressionSerialization, PreCutoverFileStillReadsAndUpgrades)
{
    FAnimationResource Source;
    Source.Duration = 1.0f;
    AddRotation(Source, FName("Spine"), 1.0f, 31, Math::Radians(60.0f));

    TVector<uint8> Bytes;
    WriteResource(Source, Bytes, ELuminaEngineVersion::ANIM_CURVES);

    FAnimationResource Loaded;
    ReadResource(Bytes, Loaded, ELuminaEngineVersion::ANIM_CURVES);

    ASSERT_EQ(Loaded.Channels.size(), Source.Channels.size());
    EXPECT_FALSE(Loaded.Compressed.IsValid());

    AnimCompression::Build(Loaded);

    ASSERT_TRUE(Loaded.Compressed.IsValid());
    EXPECT_EQ(Loaded.Compressed.NumFrames, 31u);

    const AnimCompression::FValidationReport Report = AnimCompression::Validate(Loaded);
    ASSERT_TRUE(Report.bHasCompressedData);
    EXPECT_LT(Math::Degrees(Report.MaxRotationRadians), RotationBudgetDegrees);
}
