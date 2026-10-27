#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Physics/API/Box3D/Box3DPhysicsScene.h"
#include "Physics/API/Box3D/Box3DRagdollHandle.h"
#include "Renderer/SkeletonResource.h"
#include "World/World.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/RagdollLocomotionComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"

using namespace Lumina;

namespace
{
    struct FMannequinBone
    {
        const char* Name;
        int32       Parent;
        FVector3    Position;
    };

    // A plain humanoid standing at the origin facing +Z, arms down, every bone unrotated.
    const FMannequinBone kMannequinBones[] =
    {
        { "pelvis",     -1, FVector3( 0.00f, 1.00f, 0.0f) },
        { "spine",       0, FVector3( 0.00f, 1.15f, 0.0f) },
        { "chest",       1, FVector3( 0.00f, 1.35f, 0.0f) },
        { "head",        2, FVector3( 0.00f, 1.62f, 0.0f) },
        { "upperarm_l",  2, FVector3( 0.22f, 1.48f, 0.0f) },
        { "forearm_l",   4, FVector3( 0.22f, 1.20f, 0.0f) },
        { "upperarm_r",  2, FVector3(-0.22f, 1.48f, 0.0f) },
        { "forearm_r",   6, FVector3(-0.22f, 1.20f, 0.0f) },
        { "thigh_l",     0, FVector3( 0.10f, 0.95f, 0.0f) },
        { "shin_l",      8, FVector3( 0.10f, 0.52f, 0.0f) },
        { "foot_l",      9, FVector3( 0.10f, 0.09f, 0.0f) },
        { "thigh_r",     0, FVector3(-0.10f, 0.95f, 0.0f) },
        { "shin_r",     11, FVector3(-0.10f, 0.52f, 0.0f) },
        { "foot_r",     12, FVector3(-0.10f, 0.09f, 0.0f) },
    };

    struct FMannequinBody
    {
        const char*       Bone;
        ERagdollBodyShape Shape;
        FVector3          Offset;
        float             Radius;
        float             HalfHeight;
        FVector3          HalfExtent;
        float             Mass;
    };

    const FMannequinBody kMannequinBodies[] =
    {
        { "pelvis",     ERagdollBodyShape::Box,     FVector3(0.0f,  0.02f,  0.00f), 0.0f,   0.0f,  FVector3(0.15f, 0.08f, 0.10f), 12.0f },
        { "spine",      ERagdollBodyShape::Box,     FVector3(0.0f,  0.10f,  0.00f), 0.0f,   0.0f,  FVector3(0.14f, 0.09f, 0.09f), 10.0f },
        { "chest",      ERagdollBodyShape::Box,     FVector3(0.0f,  0.12f,  0.00f), 0.0f,   0.0f,  FVector3(0.17f, 0.12f, 0.10f), 16.0f },
        { "head",       ERagdollBodyShape::Sphere,  FVector3(0.0f,  0.12f,  0.00f), 0.11f,  0.0f,  FVector3(0.0f),               5.0f },
        { "upperarm_l", ERagdollBodyShape::Capsule, FVector3(0.0f, -0.14f,  0.00f), 0.045f, 0.09f, FVector3(0.0f),               2.5f },
        { "forearm_l",  ERagdollBodyShape::Capsule, FVector3(0.0f, -0.15f,  0.00f), 0.04f,  0.10f, FVector3(0.0f),               2.0f },
        { "upperarm_r", ERagdollBodyShape::Capsule, FVector3(0.0f, -0.14f,  0.00f), 0.045f, 0.09f, FVector3(0.0f),               2.5f },
        { "forearm_r",  ERagdollBodyShape::Capsule, FVector3(0.0f, -0.15f,  0.00f), 0.04f,  0.10f, FVector3(0.0f),               2.0f },
        { "thigh_l",    ERagdollBodyShape::Capsule, FVector3(0.0f, -0.215f, 0.00f), 0.07f,  0.14f, FVector3(0.0f),               9.0f },
        { "shin_l",     ERagdollBodyShape::Capsule, FVector3(0.0f, -0.215f, 0.00f), 0.055f, 0.15f, FVector3(0.0f),               4.5f },
        { "foot_l",     ERagdollBodyShape::Box,     FVector3(0.0f, -0.05f,  0.05f), 0.0f,   0.0f,  FVector3(0.05f, 0.035f, 0.12f), 1.2f },
        { "thigh_r",    ERagdollBodyShape::Capsule, FVector3(0.0f, -0.215f, 0.00f), 0.07f,  0.14f, FVector3(0.0f),               9.0f },
        { "shin_r",     ERagdollBodyShape::Capsule, FVector3(0.0f, -0.215f, 0.00f), 0.055f, 0.15f, FVector3(0.0f),               4.5f },
        { "foot_r",     ERagdollBodyShape::Box,     FVector3(0.0f, -0.05f,  0.05f), 0.0f,   0.0f,  FVector3(0.05f, 0.035f, 0.12f), 1.2f },
    };

    struct FMannequinJoint
    {
        const char* Child;
        float       Twist;
        float       Swing;
    };

    const FMannequinJoint kMannequinJoints[] =
    {
        { "spine", 15.0f, 25.0f }, { "chest", 15.0f, 25.0f }, { "head", 40.0f, 40.0f },
        { "upperarm_l", 50.0f, 85.0f }, { "forearm_l", 10.0f, 80.0f }, { "upperarm_r", 50.0f, 85.0f }, { "forearm_r", 10.0f, 80.0f },
        { "thigh_l", 20.0f, 70.0f }, { "shin_l", 5.0f, 80.0f }, { "foot_l", 10.0f, 35.0f },
        { "thigh_r", 20.0f, 70.0f }, { "shin_r", 5.0f, 80.0f }, { "foot_r", 10.0f, 35.0f },
    };

    class PhysicalAnimation : public testing::Test
    {
    protected:

        void SetUp() override
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            WorldOwner = World;
            Registry = &World->GetEntityRegistry();
            Scene = MakeUnique<Physics::FBox3DPhysicsScene>(World);
            Scene->Simulate();
            Dt = 1.0f / Math::Max(10.0f, World->GetDefaultWorldSettings().PhysicsHz);

            const ECS::FEntity Ground = Registry->Create();
            Registry->Emplace<STransformComponent>(Ground).SetLocation(FVector3(0.0f, -0.5f, 0.0f));
            Registry->Emplace<SRigidBodyComponent>(Ground).BodyType = EBodyType::Static;
            Registry->Emplace<SBoxColliderComponent>(Ground).HalfExtent = FVector3(100.0f, 0.5f, 100.0f);

            for (int32 Index = 0; Index < (int32)std::size(kMannequinBones); ++Index)
            {
                const FMannequinBone& Source = kMannequinBones[Index];
                const FVector3 ParentPosition = Source.Parent >= 0 ? kMannequinBones[Source.Parent].Position : FVector3(0.0f);
                const FMatrix4 Global = Math::Translate(FMatrix4(1.0f), Source.Position);

                FSkeletonResource::FBoneInfo Bone;
                Bone.Name           = FName(Source.Name);
                Bone.ParentIndex    = Source.Parent;
                Bone.LocalTransform = Math::Translate(FMatrix4(1.0f), Source.Position - ParentPosition);
                Bone.InvBindMatrix  = Math::Inverse(Global);
                Skeleton.Bones.push_back(Bone);
                Skeleton.BoneNameToIndex[Bone.Name] = Index;
                BindGlobals.push_back(Global);
            }

            Asset = NewObject<CPhysicsAsset>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            AssetOwner = Asset;
            for (const FMannequinBody& Source : kMannequinBodies)
            {
                SPhysicsBodySetup& Body = Asset->Bodies.emplace_back();
                Body.BoneName          = FName(Source.Bone);
                Body.Shape             = Source.Shape;
                Body.TranslationOffset = Source.Offset;
                Body.Radius            = Source.Radius;
                Body.HalfHeight        = Source.HalfHeight;
                Body.HalfExtent        = Source.HalfExtent;
                Body.Mass              = Source.Mass;
                Body.bOverrideMass     = true;
            }
            for (const FMannequinJoint& Source : kMannequinJoints)
            {
                SPhysicsConstraintSetup& Joint = Asset->Constraints.emplace_back();
                Joint.ChildBone          = FName(Source.Child);
                Joint.ParentBone         = Skeleton.GetBone(Skeleton.GetBone(Skeleton.FindBoneIndex(Joint.ChildBone)).ParentIndex).Name;
                Joint.TwistLimitDegrees  = Source.Twist;
                Joint.Swing1LimitDegrees = Source.Swing;
                Joint.Swing2LimitDegrees = Source.Swing;
            }
        }

        void TearDown() override
        {
            Scene->StopSimulate();
            Scene.reset();
            AssetOwner.Reset();
            WorldOwner.Reset();
        }

        void SpawnMannequin(const FVector3& At, bool bWalks)
        {
            Owner = Registry->Create();
            Registry->Emplace<STransformComponent>(Owner).SetLocation(At);

            SRagdollComponent& Ragdoll = Registry->Emplace<SRagdollComponent>(Owner);
            Ragdoll.State         = ERagdollState::Powered;
            Ragdoll.TargetGlobals = BindGlobals;
            if (bWalks)
            {
                Registry->Emplace<SRagdollLocomotionComponent>(Owner);
            }
            else
            {
                Ragdoll.RootDrive = ERagdollRootDrive::FollowEntity;
            }

            Physics::FRagdollDesc Desc;
            Desc.Entity               = Owner;
            Desc.Asset                = Asset;
            Desc.Skeleton             = &Skeleton;
            Desc.ComponentBoneGlobals = &BindGlobals;
            Desc.EntityToWorld        = Math::Translate(FMatrix4(1.0f), At);
            Ragdoll.Ragdoll = Scene->CreateRagdoll(Desc);
            ASSERT_TRUE(Ragdoll.Ragdoll);
            Scene->Update(0.0);
            ASSERT_TRUE(Scene->IsRagdollReady(*Ragdoll.Ragdoll));
        }

        void Simulate(float Seconds, const TFunction<bool()>& Until = {})
        {
            for (float Elapsed = 0.0f; Elapsed < Seconds; Elapsed += Dt)
            {
                Scene->Update(Dt);
                if (Until && Until())
                {
                    return;
                }
            }
        }

        Physics::FPhysicsBodyState Limb(const char* Bone) const
        {
            Physics::FPhysicsBodyState State;
            EXPECT_TRUE(Scene->TryGetTargetState(Scene->GetRagdollBoneTarget(Owner, FName(Bone)), State)) << Bone;
            return State;
        }

        float PelvisTiltDegrees() const
        {
            const FVector3 Up = Limb("pelvis").Rotation * FVector3(0.0f, 1.0f, 0.0f);
            return Math::Degrees(Math::Acos(Math::Clamp(Up.y, -1.0f, 1.0f)));
        }

        SRagdollComponent& Ragdoll() const { return Registry->Get<SRagdollComponent>(Owner); }
        SRagdollLocomotionComponent& Locomotion() const { return Registry->Get<SRagdollLocomotionComponent>(Owner); }

        CWorld*                         World = nullptr;
        TStrongObjectPtr<CWorld>        WorldOwner;
        ECS::FRegistry*                 Registry = nullptr;
        TUniquePtr<Physics::FBox3DPhysicsScene> Scene;
        float                           Dt = 0.0f;
        FSkeletonResource               Skeleton;
        TVector<FMatrix4>               BindGlobals;
        CPhysicsAsset*                  Asset = nullptr;
        TStrongObjectPtr<CPhysicsAsset> AssetOwner;
        ECS::FEntity                    Owner;
    };
}

// Muscles hold a target pose against gravity, and with no strength the same limb just hangs.
TEST_F(PhysicalAnimation, MusclesLiftAnArmOnlyWhileTheyHaveStrength)
{
    SpawnMannequin(FVector3(0.0f, 2.0f, 0.0f), false);

    // The left arm reaches straight forward, rotated about the shoulder.
    const FQuat Raise = Math::AngleAxis(-Math::HalfPi<float>(), FVector3(1.0f, 0.0f, 0.0f));
    const FVector3 Shoulder = kMannequinBones[4].Position;
    const FVector3 Elbow = Shoulder + Raise * (kMannequinBones[5].Position - Shoulder);
    Ragdoll().TargetGlobals[4] = Math::Translate(FMatrix4(1.0f), Shoulder) * Math::ToMatrix4(Raise);
    Ragdoll().TargetGlobals[5] = Math::Translate(FMatrix4(1.0f), Elbow) * Math::ToMatrix4(Raise);

    Simulate(2.0f);
    EXPECT_NEAR(Limb("pelvis").Position.y, 3.0f, 0.15f) << "the root holds where the entity puts it";

    // Measured in the chest's own frame, since the arm is held relative to the body it hangs from.
    const FVector3 Raised = Math::Inverse(Limb("chest").Rotation) * (Limb("forearm_l").Position - Limb("upperarm_l").Position);
    EXPECT_GT(Raised.z, 0.22f) << "the elbow is out in front of the shoulder";
    EXPECT_GT(Raised.y, -0.08f);

}

// A shoulder authored too weak to hold the arm up lets it hang however the pose asks for it.
TEST_F(PhysicalAnimation, AWeakShoulderCannotLiftTheArm)
{
    for (SPhysicsConstraintSetup& Joint : Asset->Constraints)
    {
        if (Joint.ChildBone == FName("upperarm_l"))
        {
            Joint.MotorStrength = 0.0f;
        }
    }
    SpawnMannequin(FVector3(0.0f, 2.0f, 0.0f), false);

    const FQuat Raise = Math::AngleAxis(-Math::HalfPi<float>(), FVector3(1.0f, 0.0f, 0.0f));
    const FVector3 Shoulder = kMannequinBones[4].Position;
    Ragdoll().TargetGlobals[4] = Math::Translate(FMatrix4(1.0f), Shoulder) * Math::ToMatrix4(Raise);
    Ragdoll().TargetGlobals[5] = Math::Translate(FMatrix4(1.0f), Shoulder + Raise * (kMannequinBones[5].Position - Shoulder)) * Math::ToMatrix4(Raise);

    Simulate(2.0f);
    const FVector3 Hanging = Limb("forearm_l").Position - Limb("upperarm_l").Position;
    EXPECT_LT(Hanging.y, -0.2f) << "the arm hangs from the shoulder";
}

// Balance alone keeps the mannequin on its feet with no input at all.
TEST_F(PhysicalAnimation, BalancedMannequinStandsOnItsOwn)
{
    SpawnMannequin(FVector3(0.0f), true);
    Simulate(4.0f);

    EXPECT_FALSE(Locomotion().bFallen);
    EXPECT_TRUE(Locomotion().bGrounded);
    EXPECT_LT(PelvisTiltDegrees(), 15.0f);
    EXPECT_GT(Limb("pelvis").Position.y, 0.85f);
    EXPECT_LT(Math::Length(FVector3(Limb("pelvis").Position.x, 0.0f, Limb("pelvis").Position.z)), 0.3f) << "it stays where it stood";
}

// It walks the way it is pointed, and turns to walk a new way.
TEST_F(PhysicalAnimation, MannequinWalksWhereItIsTold)
{
    SpawnMannequin(FVector3(0.0f), true);
    Simulate(1.0f);

    bool bEverFell = false;
    const auto Watch = [&] { bEverFell |= Locomotion().bFallen; return false; };

    Locomotion().MoveInput = FVector3(0.0f, 0.0f, 1.0f);
    Simulate(5.0f, Watch);
    const FVector3 AfterForward = Limb("pelvis").Position;
    EXPECT_FALSE(bEverFell) << "it walked without falling";
    EXPECT_GT(AfterForward.z, 3.0f);
    EXPECT_LT(Math::Abs(AfterForward.x), 1.0f);
    EXPECT_GT(Locomotion().Speed, 0.8f);

    Locomotion().MoveInput = FVector3(1.0f, 0.0f, 0.0f);
    Simulate(4.0f, Watch);
    const FVector3 AfterTurn = Limb("pelvis").Position;
    EXPECT_FALSE(bEverFell) << "it turned without falling";
    EXPECT_GT(AfterTurn.x - AfterForward.x, 2.0f);
    const FVector3 Facing = Limb("pelvis").Rotation * FVector3(0.0f, 0.0f, 1.0f);
    EXPECT_GT(Facing.x, 0.7f) << "it turned to face where it walks";
}

// A nudge moves the capture point off the feet, and a step or two catches it without falling.
TEST_F(PhysicalAnimation, ANudgeIsCaughtWithAStep)
{
    SpawnMannequin(FVector3(0.0f), true);
    Simulate(2.0f);
    Scene->SetLinearVelocity(Owner, FVector3(0.0f, 0.0f, 1.2f));

    bool bStepped = false;
    Simulate(3.0f, [&] { bStepped |= Limb("foot_l").Position.z > 0.15f || Limb("foot_r").Position.z > 0.15f; return Locomotion().bFallen; });
    EXPECT_FALSE(Locomotion().bFallen);
    EXPECT_TRUE(bStepped) << "a foot moved to catch the body";
    EXPECT_LT(PelvisTiltDegrees(), 20.0f);
}

// A hard shove topples it, it lies limp for a moment, then gets back on its feet.
TEST_F(PhysicalAnimation, AShoveKnocksItDownAndItGetsBackUp)
{
    SpawnMannequin(FVector3(0.0f), true);
    Simulate(2.0f);
    ASSERT_FALSE(Locomotion().bFallen);

    // Sideways and far faster than any step could catch.
    Scene->SetLinearVelocity(Owner, FVector3(7.0f, 0.0f, 0.0f));
    Simulate(2.0f, [&] { return Locomotion().bFallen; });
    EXPECT_TRUE(Locomotion().bFallen);

    Simulate(10.0f, [&] { return !Locomotion().bFallen; });
    EXPECT_FALSE(Locomotion().bFallen);
    Simulate(1.0f);
    EXPECT_LT(PelvisTiltDegrees(), 30.0f);
    EXPECT_GT(Limb("pelvis").Position.y, 0.75f);
}

// Walking forward, across, back and on a diagonal it stays on its feet and holds its body nearly upright.
TEST_F(PhysicalAnimation, WalksEveryWayWithoutFallingOrLurching)
{
    SpawnMannequin(FVector3(0.0f), true);
    Simulate(1.0f);

    int32 Falls = 0;
    bool  bWasFallen = false;
    float MaxTilt = 0.0f;
    float TiltSum = 0.0f;
    int32 Samples = 0;
    for (const FVector3& Heading : { FVector3(0.0f, 0.0f, 1.0f), FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f, 0.0f, -1.0f), FVector3(-0.7f, 0.0f, 0.7f) })
    {
        Locomotion().MoveInput = Heading;
        Simulate(4.0f, [&]
        {
            const bool bFallen = Locomotion().bFallen;
            Falls += bFallen && !bWasFallen ? 1 : 0;
            bWasFallen = bFallen;
            const float Tilt = PelvisTiltDegrees();
            MaxTilt = Math::Max(MaxTilt, Tilt);
            TiltSum += Tilt;
            ++Samples;
            return false;
        });
    }

    EXPECT_EQ(Falls, 0);
    EXPECT_LT(TiltSum / (float)Math::Max(Samples, 1), 8.0f) << "mean lean while walking";
    EXPECT_LT(MaxTilt, 25.0f);
}

// Standing and walking, the torso stays steady instead of buzzing, and a body left alone stands still instead of fidgeting.
TEST_F(PhysicalAnimation, StandsAndWalksWithoutShaking)
{
    SpawnMannequin(FVector3(0.0f), true);
    Simulate(2.0f);

    const auto ChestSpinRms = [&](float Seconds, float& OutTravel)
    {
        float SpinSquared = 0.0f;
        int32 Samples = 0;
        FVector3 Last = Limb("pelvis").Position;
        OutTravel = 0.0f;
        Simulate(Seconds, [&]
        {
            const Physics::FPhysicsBodyState Chest = Limb("chest");
            SpinSquared += Math::Dot(Chest.AngularVelocity, Chest.AngularVelocity);
            const FVector3 Pelvis = Limb("pelvis").Position;
            OutTravel += Math::Length(FVector3(Pelvis.x - Last.x, 0.0f, Pelvis.z - Last.z));
            Last = Pelvis;
            ++Samples;
            return false;
        });
        return Math::Sqrt(SpinSquared / (float)Math::Max(Samples, 1));
    };

    float Travel = 0.0f;
    EXPECT_LT(ChestSpinRms(4.0f, Travel), 0.5f) << "standing chest spin, in radians a second";
    EXPECT_LT(Travel, 0.3f) << "a standing body barely moves";

    Locomotion().MoveInput = FVector3(0.0f, 0.0f, 1.0f);
    Simulate(1.0f);
    EXPECT_LT(ChestSpinRms(4.0f, Travel), 0.8f) << "walking chest spin, in radians a second";
    EXPECT_GT(Travel, 4.0f);
}
