#include <gtest/gtest.h>

#include "Renderer/SkeletonResource.h"
#include "UI/Tools/AssetEditors/MeshEditor/SkeletonEditorTool.h"

using namespace Lumina;

namespace
{
    FSkeletonResource::FBoneInfo MakeBone(const char* Name, int32 ParentIndex)
    {
        FSkeletonResource::FBoneInfo Bone;
        Bone.Name = FName(Name);
        Bone.ParentIndex = ParentIndex;
        return Bone;
    }
}

// Bones[0] and Bones[1] parent each other, violating the ParentIndex < index invariant HasConsistentHierarchy normally enforces.
TEST(SkeletonEditorTool, CyclicParentChainTerminatesInsteadOfHanging)
{
    FSkeletonResource Skeleton;
    Skeleton.Bones.Add(MakeBone("A", 1));
    Skeleton.Bones.Add(MakeBone("B", 0));
    Skeleton.Bones.Add(MakeBone("C", 1));

    FSkeletonEditorTool::FBoneDepthStatistics Stats = FSkeletonEditorTool::ComputeBoneDepthStatistics(Skeleton);

    EXPECT_FALSE(Stats.bConsistentHierarchy);
    EXPECT_EQ(Stats.NumRootBones, 0);
    EXPECT_LE(Stats.MaxDepth, Skeleton.GetNumBones());
}

TEST(SkeletonEditorTool, SelfParentedBoneTerminatesInsteadOfHanging)
{
    FSkeletonResource Skeleton;
    Skeleton.Bones.Add(MakeBone("Root", -1));
    Skeleton.Bones.Add(MakeBone("Self", 1));

    FSkeletonEditorTool::FBoneDepthStatistics Stats = FSkeletonEditorTool::ComputeBoneDepthStatistics(Skeleton);

    EXPECT_FALSE(Stats.bConsistentHierarchy);
    EXPECT_EQ(Stats.NumRootBones, 1);
    EXPECT_LE(Stats.MaxDepth, Skeleton.GetNumBones());
}

TEST(SkeletonEditorTool, OutOfRangeParentIsReportedInsteadOfIndexingPastTheArray)
{
    FSkeletonResource Skeleton;
    Skeleton.Bones.Add(MakeBone("Root", -1));
    Skeleton.Bones.Add(MakeBone("Orphan", 5));

    FSkeletonEditorTool::FBoneDepthStatistics Stats = FSkeletonEditorTool::ComputeBoneDepthStatistics(Skeleton);

    EXPECT_FALSE(Stats.bConsistentHierarchy);
}

TEST(SkeletonEditorTool, AcyclicSkeletonReportsExpectedDepthStatistics)
{
    FSkeletonResource Skeleton;
    Skeleton.Bones.Add(MakeBone("Root", -1));
    Skeleton.Bones.Add(MakeBone("Spine", 0));
    Skeleton.Bones.Add(MakeBone("Head", 1));
    Skeleton.Bones.Add(MakeBone("OtherRoot", -1));

    FSkeletonEditorTool::FBoneDepthStatistics Stats = FSkeletonEditorTool::ComputeBoneDepthStatistics(Skeleton);

    EXPECT_TRUE(Stats.bConsistentHierarchy);
    EXPECT_EQ(Stats.NumRootBones, 2);
    EXPECT_EQ(Stats.MaxDepth, 2);
    EXPECT_FLOAT_EQ(Stats.AverageDepth, (0.0f + 1.0f + 2.0f + 0.0f) / 4.0f);
}
