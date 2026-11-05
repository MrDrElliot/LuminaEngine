#include <gtest/gtest.h>

#include "Animation/BoneLOD.h"
#include "Animation/AnimCompression.h"
#include "Animation/Pose.h"
#include "Animation/TaskSystem/AnimTask.h"
#include "Animation/TaskSystem/AnimTaskExecutor.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "Renderer/SkeletonResource.h"
#include <random>
#include <string>

#if WITH_EDITOR
#include "Tools/Import/ImportHelpers.h"
#endif

using namespace Lumina;

namespace
{
    struct FBoneSpec
    {
        const char* Name;
        int32       Parent;
        FVector3    Joint;
    };

    // Bind pose with no rotation, so each bone sits at Joint and its local offset is the step from its parent.
    void BuildSkeleton(FSkeletonResource& Skeleton, std::initializer_list<FBoneSpec> Specs)
    {
        TVector<FBoneSpec> Bones(Specs);
        for (const FBoneSpec& Spec : Bones)
        {
            FSkeletonResource::FBoneInfo Bone;
            Bone.Name           = FName(Spec.Name);
            Bone.ParentIndex    = Spec.Parent;
            const FVector3 ParentJoint = Spec.Parent >= 0 ? Bones[Spec.Parent].Joint : FVector3(0.0f);
            Bone.LocalTransform = AnimPose::ComposeTRS(Spec.Joint - ParentJoint, FQuat::Identity(), FVector3(1.0f));
            Bone.InvBindMatrix  = AnimPose::ComposeTRS(-Spec.Joint, FQuat::Identity(), FVector3(1.0f));
            Skeleton.BoneNameToIndex[Bone.Name] = Skeleton.Bones.Num();
            Skeleton.Bones.Add(Bone);
        }
        Skeleton.BuildBindPoseCache();
    }

    // Root at the floor, a body bone, a hand, a short finger, and an unweighted prop bone held off the hand.
    void BuildCharacter(FSkeletonResource& Skeleton)
    {
        BuildSkeleton(Skeleton, {
            { "Root",   -1, FVector3(0.0f, 0.0f, 0.0f) },
            { "Body",    0, FVector3(0.0f, 0.2f, 0.0f) },
            { "Hand",    1, FVector3(0.0f, 1.5f, 0.0f) },
            { "Finger",  2, FVector3(0.0f, 1.6f, 0.0f) },
            { "Prop",    2, FVector3(0.1f, 1.5f, 0.0f) },
        });
    }

    void AddVertex(FMeshResource& Mesh, const FVector3& Position, uint16 Bone)
    {
        Mesh.Positions.Add(Position);
        Mesh.JointIndices.Emplace(Bone, 0, 0, 0);
        Mesh.JointWeights.Emplace(255, 0, 0, 0);
    }

    float Joint(const FSkeletonResource& Skeleton, const char* Name, int32 Axis)
    {
        return Skeleton.BindGlobalMatrices[Skeleton.FindBoneIndex(FName(Name))][3][Axis];
    }
}

TEST(BoneLOD, ErrorGrowsWithReachAndParentsNeverCutBeforeChildren)
{
    FSkeletonResource Skeleton;
    BuildCharacter(Skeleton);

    FMeshResource Mesh;
    Mesh.bSkinnedMesh = true;
    AddVertex(Mesh, FVector3(0.0f, 1.2f, 0.0f), 1);
    AddVertex(Mesh, FVector3(0.0f, 1.55f, 0.0f), 2);
    AddVertex(Mesh, FVector3(0.0f, 1.62f, 0.0f), 3);

    const TVector<float> Errors = BoneLOD::ComputeBoneErrors(Mesh, Skeleton);
    ASSERT_EQ(Errors.Num(), 5);

    // A finger vertex 2 cm out swings by about 2 cm, far less than a body vertex a meter from its joint.
    EXPECT_NEAR(Errors[3], 0.02f * 2.0f * Math::Sin(0.5f), 1e-3f);
    EXPECT_GT(Errors[1], 0.9f);

    // The prop carries no vertex, yet its offset from the hand still counts.
    EXPECT_NEAR(Errors[4], 0.1f * 2.0f * Math::Sin(0.5f), 1e-3f);

    for (int32 Bone = 1; Bone < 5; ++Bone)
    {
        EXPECT_GE(Errors[Skeleton.GetBone(Bone).ParentIndex], Errors[Bone]) << "bone " << Bone;
    }
}

TEST(BoneLOD, SortIsParentsFirstEvenFromAChildFirstSource)
{
    // glTF lists joints in skin order, which may put a child ahead of its parent.
    FSkeletonResource Skeleton;
    BuildSkeleton(Skeleton, {
        { "Finger", 2, FVector3(0.0f, 1.6f, 0.0f) },
        { "Root",  -1, FVector3(0.0f, 0.0f, 0.0f) },
        { "Hand",   3, FVector3(0.0f, 1.5f, 0.0f) },
        { "Body",   1, FVector3(0.0f, 0.2f, 0.0f) },
    });
    ASSERT_FALSE(Skeleton.HasConsistentHierarchy());

    const TVector<float> Errors = { 0.01f, 5.0f, 0.3f, 2.0f };
    const TVector<int32> NewToOld = BoneLOD::SortBonesByError(Skeleton, Errors);
    BoneLOD::ReorderSkeleton(Skeleton, NewToOld);

    ASSERT_TRUE(Skeleton.HasConsistentHierarchy());
    EXPECT_EQ(Skeleton.GetBone(0).Name, FName("Root"));
    EXPECT_EQ(Skeleton.GetBone(1).Name, FName("Body"));
    EXPECT_EQ(Skeleton.GetBone(2).Name, FName("Hand"));
    EXPECT_EQ(Skeleton.GetBone(3).Name, FName("Finger"));
    EXPECT_NEAR(Joint(Skeleton, "Finger", 1), 1.6f, 1e-5f);
}

TEST(BoneLOD, SortPutsLowErrorBranchesLast)
{
    FSkeletonResource Skeleton;
    BuildCharacter(Skeleton);

    // Prop matters more than Finger here, so it moves ahead of it while both stay under Hand.
    const TVector<float> Errors = { 9.0f, 3.0f, 1.0f, 0.01f, 0.2f };
    const TVector<int32> OldToNew = BoneLOD::ReorderSkeleton(Skeleton, BoneLOD::SortBonesByError(Skeleton, Errors));

    EXPECT_EQ(OldToNew[3], 4);
    EXPECT_EQ(OldToNew[4], 3);
    EXPECT_EQ(Skeleton.FindBoneIndex(FName("Prop")), 3);
    EXPECT_EQ(Skeleton.GetBone(4).ParentIndex, Skeleton.FindBoneIndex(FName("Hand")));

    const TVector<float> Remapped = BoneLOD::RemapBoneErrors(Errors, OldToNew);
    EXPECT_FLOAT_EQ(Remapped[3], 0.2f);
    EXPECT_FLOAT_EQ(Remapped[4], 0.01f);
}

TEST(BoneLOD, CountKeepsEveryBoneWhoseErrorWouldShow)
{
    const TVector<float> Errors = { 9.0f, 3.0f, 1.0f, 0.2f, 0.01f };

    EXPECT_EQ(BoneLOD::CountActiveBones(Errors, 5, 0.0f, 1), 5);
    EXPECT_EQ(BoneLOD::CountActiveBones(Errors, 5, 0.005f, 1), 5);
    EXPECT_EQ(BoneLOD::CountActiveBones(Errors, 5, 0.05f, 1), 4);
    EXPECT_EQ(BoneLOD::CountActiveBones(Errors, 5, 1.5f, 1), 2);
    EXPECT_EQ(BoneLOD::CountActiveBones(Errors, 5, 100.0f, 1), 1);

    // A socket on the last bone holds the whole skeleton.
    EXPECT_EQ(BoneLOD::CountActiveBones(Errors, 5, 100.0f, 5), 5);

    // An unsorted skeleton still cuts only past the last bone that matters.
    const TVector<float> Unsorted = { 9.0f, 0.01f, 3.0f, 0.01f, 0.01f };
    EXPECT_EQ(BoneLOD::CountActiveBones(Unsorted, 5, 0.05f, 1), 3);

    // Errors measured against another layout are ignored.
    EXPECT_EQ(BoneLOD::CountActiveBones(Errors, 6, 100.0f, 1), 6);
}

TEST(BoneLOD, CutSkinningMatchesFullFKWithTheTailAtBind)
{
    constexpr int32 kBones = 61;
    constexpr int32 kActive = 23;

    std::mt19937 Rng(11);
    std::uniform_real_distribution<float> Unit(-1.0f, 1.0f);
    const auto RandomRotation = [&] { return Math::Normalize(FQuat(Unit(Rng), Unit(Rng), Unit(Rng), Unit(Rng))); };

    FSkeletonResource Skeleton;
    TVector<FMatrix4> BindGlobals(kBones);
    for (int32 i = 0; i < kBones; ++i)
    {
        FSkeletonResource::FBoneInfo Bone;
        Bone.Name           = FName(("BoneLODCut" + std::to_string(i)).c_str());
        Bone.ParentIndex    = i == 0 ? Constants::kIndexNone : (int32)(Rng() % (uint32)i);
        Bone.LocalTransform = AnimPose::ComposeTRS(FVector3(Unit(Rng), Unit(Rng), Unit(Rng)), RandomRotation(), FVector3(1.0f));
        BindGlobals[i]      = Bone.ParentIndex >= 0 ? BindGlobals[Bone.ParentIndex] * Bone.LocalTransform : Bone.LocalTransform;
        Bone.InvBindMatrix  = Math::Inverse(BindGlobals[i]);
        Skeleton.BoneNameToIndex[Bone.Name] = i;
        Skeleton.Bones.Add(Bone);
    }
    Skeleton.BuildBindPoseCache();

    FPose Pose;
    Pose.ResetToBindPose(&Skeleton);
    for (int32 i = 0; i < kActive; ++i)
    {
        Pose.SetBone(i, FVector3(Unit(Rng), Unit(Rng), Unit(Rng)), RandomRotation(), FVector3(1.0f));
    }

    TVector<FMatrix4> Full;
    TVector<FMatrix4> Cut;
    AnimPose::ToSkinningMatrices(Pose, &Skeleton, Full);
    AnimPose::ToSkinningMatrices(Pose, &Skeleton, Cut, kActive);

    ASSERT_EQ(Full.Num(), Cut.Num());
    for (int32 i = 0; i < kBones; ++i)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                ASSERT_NEAR(Cut[i][c][r], Full[i][c][r], 2e-4f) << "bone " << i << " column " << c << " row " << r;
            }
        }
    }
}

namespace
{
    CAnimation* MakeSwingClip(const FSkeletonResource& Skeleton, float SweepRadians)
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
                Channel.Timestamps.Add(T);
                Channel.Rotations.Emplace(FVector3(SweepRadians * T, 0.5f * SweepRadians * T, 0.0f));
            }
            Resource.Channels.Add(Channel);
        }
        AnimCompression::Build(Resource);
        return Clip;
    }

    // Two clips under a per-bone mask, then an offset on one asset bone, which is every kind of asset index a recipe carries.
    void RecordMaskedRecipe(FAnimTaskList& List, CAnimation* ClipA, CAnimation* ClipB, const TVector<float>& Mask, uint16 OffsetBone)
    {
        FAnimTask A;
        A.Type = EAnimTaskType::SampleClip;
        A.Clip = ClipA;
        A.Time = 0.4f;
        FAnimTask B = A;
        B.Clip = ClipB;
        const int16 IndexA = List.Add(A);
        const int16 IndexB = List.Add(B);

        FAnimTask Masked;
        Masked.Type        = EAnimTaskType::BlendMasked;
        Masked.DepA        = IndexA;
        Masked.DepB        = IndexB;
        Masked.Alpha       = 1.0f;
        Masked.MaskWeights = &Mask;
        const int16 IndexMasked = List.Add(Masked);

        FAnimTask Offset;
        Offset.Type  = EAnimTaskType::BoneTransform;
        Offset.DepA  = IndexMasked;
        Offset.BoneA = OffsetBone;
        Offset.T     = FVector3(0.0f, 0.0f, 0.2f);
        Offset.R     = FQuat(FVector3(0.0f, 0.7f, 0.0f));
        Offset.Alpha = 1.0f;
        Offset.Space = (uint8)AnimPose::EBoneSpace::LocalBone;
        Offset.Mode  = (uint8)AnimPose::EBoneApplyMode::Add;
        List.OutputTask = List.Add(Offset);
    }
}

TEST(BoneLOD, PoseLayoutEvaluatesToTheSameAssetOrderMatrices)
{
    FSkeletonResource Asset;
    BuildCharacter(Asset);

    CAnimation* ClipA = MakeSwingClip(Asset, 0.8f);
    CAnimation* ClipB = MakeSwingClip(Asset, -1.1f);
    const TVector<float> Mask = { 1.0f, 0.5f, 0.0f, 0.25f, 1.0f };
    const uint16 Finger = (uint16)Asset.FindBoneIndex(FName("Finger"));

    FAnimTaskList Plain;
    Plain.Skeleton = &Asset;
    RecordMaskedRecipe(Plain, ClipA, ClipB, Mask, Finger);
    TVector<FMatrix4> Expected;
    ASSERT_TRUE(Anim::ExecuteTaskList(Plain, Expected, nullptr));

    // Prop ahead of Finger, so asset and pose indices disagree for both.
    FSkeletonResource PoseSkeleton;
    PoseSkeleton.Bones = Asset.Bones;
    PoseSkeleton.BoneNameToIndex = Asset.BoneNameToIndex;
    const TVector<int32> PoseToAsset = { 0, 1, 2, 4, 3 };
    const TVector<int32> AssetToPose = BoneLOD::ReorderSkeleton(PoseSkeleton, PoseToAsset);

    FAnimTaskList Laid;
    Laid.Skeleton    = &PoseSkeleton;
    Laid.PoseToAsset = PoseToAsset.GetData();
    Laid.AssetToPose = AssetToPose.GetData();
    RecordMaskedRecipe(Laid, ClipA, ClipB, Mask, Finger);
    TVector<FMatrix4> Actual;
    ASSERT_TRUE(Anim::ExecuteTaskList(Laid, Actual, nullptr));

    ASSERT_EQ(Expected.Num(), Actual.Num());
    for (int32 Bone = 0; Bone < Expected.Num(); ++Bone)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                ASSERT_NEAR(Actual[Bone][c][r], Expected[Bone][c][r], 1e-5f) << "bone " << Bone << " column " << c << " row " << r;
            }
        }
    }
}

#if WITH_EDITOR
TEST(BoneLOD, MeshletSkinMeasuresLikeTheSourceVertices)
{
    FSkeletonResource Skeleton;
    BuildCharacter(Skeleton);

    // A thin column of quads up the body, each row skinned to the bone just below it.
    FMeshResource Source;
    Source.bSkinnedMesh = true;
    constexpr uint32 kRows = 34;
    for (uint32 Row = 0; Row < kRows; ++Row)
    {
        const float Y = 1.65f * (float)Row / (float)(kRows - 1);
        const uint16 Bone = Y < 0.2f ? 0 : (Y < 1.5f ? 1 : (Y < 1.6f ? 2 : 3));
        for (float X : { -0.05f, 0.05f })
        {
            FSourceSkinnedVertex Vertex = {};
            Vertex.Position     = FVector3(X, Y, 0.0f);
            Vertex.Normal       = PackNormal(FVector3(0.0f, 0.0f, 1.0f));
            Vertex.Color        = 0xFFFFFFFFu;
            Vertex.JointIndices = FU16Vector4(Bone, 0, 0, 0);
            Vertex.JointWeights = FU8Vector4(255, 0, 0, 0);
            Source.AppendVertex(Vertex);
        }
    }
    for (uint32 Row = 0; Row + 1 < kRows; ++Row)
    {
        const uint32 b = Row * 2u;
        Source.Indices.Append({ b, b + 1u, b + 3u, b, b + 3u, b + 2u });
    }
    FGeometrySurface Surface;
    Surface.IndexCount = (uint32)Source.Indices.Num();
    Source.GeometrySurfaces.Add(Surface);

    const TVector<float> FromSource = BoneLOD::ComputeBoneErrors(Source, Skeleton);

    Import::Mesh::GenerateMeshlets(Source);
    ASSERT_FALSE(Source.MeshletData.MeshletBoneIndices.IsEmpty());

    // A loaded mesh has only its meshlets, so measure from them alone.
    Source.ClearVertices();

    const TVector<float> FromMeshlets = BoneLOD::ComputeBoneErrors(Source, Skeleton);
    ASSERT_EQ(FromSource.Num(), FromMeshlets.Num());
    for (int32 Bone = 0; Bone < FromSource.Num(); ++Bone)
    {
        EXPECT_NEAR(FromMeshlets[Bone], FromSource[Bone], 0.01f + FromSource[Bone] * 0.02f) << "bone " << Bone;
    }
}
#endif
