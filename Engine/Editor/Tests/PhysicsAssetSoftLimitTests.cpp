#include <gtest/gtest.h>

#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Renderer/SkeletonResource.h"
#include "UI/Tools/AssetEditors/PhysicsAsset/PhysicsAssetEditorTool.h"

using namespace Lumina;

namespace
{
    constexpr int32 TwistAxis = 0;
    constexpr int32 Swing1Axis = 1;
    constexpr int32 Swing2Axis = 2;

    float Pull(const SPhysicsConstraintSetup& Constraint, int32 Axis, float Degrees)
    {
        return FPhysicsAssetEditorTool::SampleSoftLimitPull(Constraint, Axis, Math::Radians(Degrees));
    }

    void AddBone(FSkeletonResource& Skeleton, const char* Name, int32 ParentIndex)
    {
        FSkeletonResource::FBoneInfo Bone;
        Bone.Name = FName(Name);
        Bone.ParentIndex = ParentIndex;
        Skeleton.BoneNameToIndex[Bone.Name] = Skeleton.Bones.Num();
        Skeleton.Bones.Add(Bone);
    }

    SPhysicsConstraintSetup& AddConstraint(CPhysicsAsset& Asset, const char* Parent, const char* Child)
    {
        SPhysicsConstraintSetup& Constraint = Asset.Constraints.EmplaceGetRef();
        Constraint.ParentBone = FName(Parent);
        Constraint.ChildBone = FName(Child);
        return Constraint;
    }
}

// The curve is flat inside the soft range and climbs past it to the ligament's full spring at the hard limit.
TEST(PhysicsAssetSoftLimitCurve, PullStartsAtTheSoftStartAndPeaksAtTheHardLimit)
{
    SPhysicsConstraintSetup Joint;
    Joint.Swing1LimitDegrees = 30.0f;
    Joint.Swing2LimitDegrees = 30.0f;
    Joint.SoftLimitStart = 0.7f;
    Joint.LimitStiffness = 3.0f;

    EXPECT_EQ(Pull(Joint, Swing1Axis, 20.0f), 0.0f) << "inside the 21 degree soft range";
    EXPECT_GT(Pull(Joint, Swing1Axis, 25.0f), 0.0f);
    EXPECT_GT(Pull(Joint, Swing1Axis, 28.0f), Pull(Joint, Swing1Axis, 25.0f));

    const float Omega = Math::TwoPi<float>() * Joint.LimitStiffness;
    EXPECT_NEAR(Pull(Joint, Swing1Axis, 30.0f), Omega * Omega * Math::Radians(9.0f), 0.5f) << "full spring rate, nine degrees past the soft start";

    Joint.bSoftLimits = false;
    EXPECT_EQ(Pull(Joint, Swing1Axis, 29.0f), 0.0f) << "without soft limits only the hard stop acts";
}

// Each axis of the curve reads its own limit, so an elliptical cone pulls sooner about its narrow swing.
TEST(PhysicsAssetSoftLimitCurve, EachAxisMeasuresAgainstItsOwnLimit)
{
    SPhysicsConstraintSetup Joint;
    Joint.TwistLimitDegrees = 20.0f;
    Joint.Swing1LimitDegrees = 30.0f;
    Joint.Swing2LimitDegrees = 60.0f;
    Joint.SoftLimitStart = 0.7f;

    EXPECT_GT(Pull(Joint, Swing1Axis, 40.0f), 0.0f) << "past the narrow swing";
    EXPECT_EQ(Pull(Joint, Swing2Axis, 40.0f), 0.0f) << "inside the wide swing's 42 degree soft range";
    EXPECT_GT(Pull(Joint, TwistAxis, 16.0f), 0.0f) << "past the 14 degree soft twist";
}

// Applying to the subtree reaches the joints at and below the source's child bone and nothing else, and only the soft limit fields.
TEST(PhysicsAssetSoftLimitCopy, SubtreeReachesOnlyJointsBelowTheSource)
{
    FSkeletonResource Skeleton;
    AddBone(Skeleton, "pelvis", -1);
    AddBone(Skeleton, "spine", 0);
    AddBone(Skeleton, "upperarm", 1);
    AddBone(Skeleton, "forearm", 2);
    AddBone(Skeleton, "thigh", 0);

    CPhysicsAsset* Asset = NewObject<CPhysicsAsset>();
    AddConstraint(*Asset, "pelvis", "spine");
    SPhysicsConstraintSetup& Source = AddConstraint(*Asset, "spine", "upperarm");
    AddConstraint(*Asset, "upperarm", "forearm");
    AddConstraint(*Asset, "pelvis", "thigh");

    Source.SoftLimitStart = 0.4f;
    Source.LimitStiffness = 9.0f;
    Source.LimitDampingRatio = 2.0f;
    Source.MaxLimitTorque = 50.0f;
    Source.Swing1LimitDegrees = 80.0f;

    const SPhysicsConstraintSetup Defaults;
    FPhysicsAssetEditorTool::CopySoftLimits(*Asset, 1, &Skeleton);

    const SPhysicsConstraintSetup& Spine = Asset->Constraints[0];
    const SPhysicsConstraintSetup& Forearm = Asset->Constraints[2];
    const SPhysicsConstraintSetup& Thigh = Asset->Constraints[3];
    EXPECT_EQ(Forearm.SoftLimitStart, 0.4f);
    EXPECT_EQ(Forearm.LimitStiffness, 9.0f);
    EXPECT_EQ(Forearm.LimitDampingRatio, 2.0f);
    EXPECT_EQ(Forearm.MaxLimitTorque, 50.0f);
    EXPECT_EQ(Forearm.Swing1LimitDegrees, Defaults.Swing1LimitDegrees) << "hard limits are not soft limit settings";
    EXPECT_EQ(Spine.SoftLimitStart, Defaults.SoftLimitStart) << "the parent is above the subtree";
    EXPECT_EQ(Thigh.SoftLimitStart, Defaults.SoftLimitStart) << "a sibling branch is outside the subtree";

    FPhysicsAssetEditorTool::CopySoftLimits(*Asset, 1, nullptr);
    EXPECT_EQ(Spine.SoftLimitStart, 0.4f);
    EXPECT_EQ(Thigh.MaxLimitTorque, 50.0f);
}
