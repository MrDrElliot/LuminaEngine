#include <gtest/gtest.h>

#include "Animation/AnimCompression.h"
#include "Animation/Pose.h"
#include "Renderer/SkeletonResource.h"
#include <bit>
#include <random>
#include <string>
#include <vector>

using namespace Lumina;

TEST(PoseCompose, LocalMatricesMatchComposeTRS)
{
    // Not a multiple of eight, so the scalar tail runs after the wide steps.
    constexpr int32 kBones = 115;

    FPose Pose;
    Pose.SetNumBones(kBones);

    std::mt19937 Rng(42);
    std::uniform_real_distribution<float> Unit(-1.0f, 1.0f);
    std::uniform_real_distribution<float> Positive(0.1f, 3.0f);
    for (int32 i = 0; i < kBones; ++i)
    {
        const FQuat R = Math::Normalize(FQuat(Unit(Rng), Unit(Rng), Unit(Rng), Unit(Rng)));
        Pose.SetBone(i, FVector3(Unit(Rng), Unit(Rng), Unit(Rng)) * 50.0f, R, FVector3(Positive(Rng), Positive(Rng), Positive(Rng)));
    }

    std::vector<FMatrix4> Batched(kBones);
    AnimPose::ComposeLocalMatrices(Pose, Batched.data());

    for (int32 i = 0; i < kBones; ++i)
    {
        const FMatrix4 Expected = AnimPose::ComposeTRS(Pose.GetTranslation(i), Pose.GetRotation(i), Pose.GetScale(i));
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                ASSERT_EQ(std::bit_cast<uint32>(Batched[i][c][r]), std::bit_cast<uint32>(Expected[c][r]))
                    << "bone " << i << " column " << c << " row " << r;
            }
        }
    }
}

TEST(PoseCompose, SkinningMatricesMatchUncachedPath)
{
    constexpr int32 kBones = 61;

    std::mt19937 Rng(7);
    std::uniform_real_distribution<float> Unit(-1.0f, 1.0f);
    const auto RandomRotation = [&] { return Math::Normalize(FQuat(Unit(Rng), Unit(Rng), Unit(Rng), Unit(Rng))); };

    FSkeletonResource Skeleton;
    for (int32 i = 0; i < kBones; ++i)
    {
        FSkeletonResource::FBoneInfo Bone;
        Bone.Name           = FName(("PoseComposeBone" + std::to_string(i)).c_str());
        Bone.ParentIndex    = i == 0 ? Constants::kIndexNone : (int32)(Rng() % (uint32)i);
        Bone.LocalTransform = AnimPose::ComposeTRS(FVector3(Unit(Rng), Unit(Rng), Unit(Rng)), RandomRotation(), FVector3(1.0f));
        Bone.InvBindMatrix  = AnimPose::ComposeTRS(FVector3(Unit(Rng), Unit(Rng), Unit(Rng)), RandomRotation(), FVector3(1.0f));
        Skeleton.BoneNameToIndex[Bone.Name] = i;
        Skeleton.Bones.push_back(Bone);
    }
    Skeleton.BuildBindPoseCache();
    ASSERT_TRUE(Skeleton.HasFlatBoneCache());

    FPose Pose;
    Pose.SetNumBones(kBones);
    for (int32 i = 0; i < kBones; ++i)
    {
        Pose.SetBone(i, FVector3(Unit(Rng), Unit(Rng), Unit(Rng)), RandomRotation(), FVector3(1.0f + 0.2f * Unit(Rng)));
    }

    TVector<FMatrix4> Cached;
    AnimPose::ToSkinningMatrices(Pose, &Skeleton, Cached);

    Skeleton.BoneParents.clear();
    ASSERT_FALSE(Skeleton.HasFlatBoneCache());
    TVector<FMatrix4> Uncached;
    AnimPose::ToSkinningMatrices(Pose, &Skeleton, Uncached);

    ASSERT_EQ(Cached.size(), Uncached.size());
    for (int32 i = 0; i < kBones; ++i)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                ASSERT_EQ(std::bit_cast<uint32>(Cached[i][c][r]), std::bit_cast<uint32>(Uncached[i][c][r]))
                    << "bone " << i << " column " << c << " row " << r;
            }
        }
    }
}

TEST(PoseCompose, RotationBatchMatchesScalarDecode)
{
    constexpr uint32 kFrames = 9;
    constexpr int32  kTracks = 24;

    FCompressedAnimData Data;
    Data.NumFrames = kFrames;
    std::mt19937 Rng(99);
    Data.QuantizedData.resize((size_t)kTracks * kFrames * 4);
    for (uint16& Value : Data.QuantizedData)
    {
        Value = (uint16)Rng();
    }

    // Edge values, including an all-midpoint frame whose length is near zero.
    for (int32 i = 0; i < 4; ++i)
    {
        Data.QuantizedData[i] = 32767;
        Data.QuantizedData[4 + i] = 0;
        Data.QuantizedData[8 + i] = 65535;
    }

    for (const float Alpha : { 0.0f, 0.25f, 0.5f, 0.9999f, 1.0f })
    {
        for (uint32 Frame0 = 0; Frame0 + 1 < kFrames; ++Frame0)
        {
            for (int32 Base = 0; Base + FCompressedAnimData::RotationBatchSize <= kTracks; Base += FCompressedAnimData::RotationBatchSize)
            {
                uint32 Offsets[FCompressedAnimData::RotationBatchSize];
                for (int32 k = 0; k < FCompressedAnimData::RotationBatchSize; ++k)
                {
                    Offsets[k] = (uint32)((Base + k) * kFrames * 4);
                }

                float Batch[4][FCompressedAnimData::RotationBatchSize];
                Data.DecodeQuantizedRotationBatch(Offsets, Frame0, Frame0 + 1, Alpha, Batch);

                for (int32 k = 0; k < FCompressedAnimData::RotationBatchSize; ++k)
                {
                    FCompressedAnimTrack Track;
                    Track.Format     = EAnimTrackFormat::Quantized;
                    Track.DataOffset = Offsets[k];
                    const FQuat Expected = Data.DecodeRotation(Track, Frame0, Frame0 + 1, Alpha);
                    ASSERT_EQ(std::bit_cast<uint32>(Batch[0][k]), std::bit_cast<uint32>(Expected.x)) << "track " << Base + k;
                    ASSERT_EQ(std::bit_cast<uint32>(Batch[1][k]), std::bit_cast<uint32>(Expected.y)) << "track " << Base + k;
                    ASSERT_EQ(std::bit_cast<uint32>(Batch[2][k]), std::bit_cast<uint32>(Expected.z)) << "track " << Base + k;
                    ASSERT_EQ(std::bit_cast<uint32>(Batch[3][k]), std::bit_cast<uint32>(Expected.w)) << "track " << Base + k;
                }
            }
        }
    }
}
