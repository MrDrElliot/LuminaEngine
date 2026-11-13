#include <gtest/gtest.h>

#include <cstdio>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Physics/API/Box3D/Box3DPhysicsScene.h"
#include "Renderer/SkeletonResource.h"
#include "World/World.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"

using namespace Lumina;

namespace
{
    constexpr float kLimbReach = 0.25f;

    // A torso block with one ball-on-a-stick limb pointing along Direction from a pivot on the block.
    struct FLimbRig
    {
        FVector3 TorsoHalfExtent;
        float    TorsoMass;
        FVector3 Pivot;
        FVector3 Direction;
    };

    class RagdollSoftLimit : public testing::Test
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
            SavedSettings = World->GetDefaultWorldSettings();
            Dt = 1.0f / Math::Max(10.0f, SavedSettings.PhysicsHz);

            const ECS::FEntity Ground = Registry->Create();
            Registry->Emplace<STransformComponent>(Ground).SetLocation(FVector3(0.0f, -0.5f, 0.0f));
            Registry->Emplace<SRigidBodyComponent>(Ground).BodyType = EBodyType::Static;
            Registry->Emplace<SBoxColliderComponent>(Ground).HalfExtent = FVector3(100.0f, 0.5f, 100.0f);
        }

        void TearDown() override
        {
            Scene->StopSimulate();
            Scene.reset();
            Assets.Reset();
            // A world without a settings entity hands out one shared default, which the next test would inherit.
            World->GetDefaultWorldSettings() = SavedSettings;
            WorldOwner.Reset();
        }

        ECS::FEntity Spawn(const FVector3& At, const FLimbRig& Rig, const SPhysicsConstraintSetup& Joint)
        {
            FSkeletonResource Skeleton;
            TVector<FMatrix4> Globals;
            const FVector3 Positions[] = { FVector3(0.0f), Rig.Pivot, Rig.Pivot + Rig.Direction * (2.0f * kLimbReach) };
            const char* Names[] = { "torso", "limb", "tip" };
            for (int32 Index = 0; Index < 3; ++Index)
            {
                FSkeletonResource::FBoneInfo Bone;
                Bone.Name           = FName(Names[Index]);
                Bone.ParentIndex    = Index - 1;
                Bone.LocalTransform = Math::Translate(FMatrix4(1.0f), Positions[Index] - (Index > 0 ? Positions[Index - 1] : FVector3(0.0f)));
                Bone.InvBindMatrix  = Math::Inverse(Math::Translate(FMatrix4(1.0f), Positions[Index]));
                Skeleton.BoneNameToIndex[Bone.Name] = Index;
                Skeleton.Bones.Add(Bone);
                Globals.Add(Math::Translate(FMatrix4(1.0f), Positions[Index]));
            }

            CPhysicsAsset* Asset = NewObject<CPhysicsAsset>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            Assets.Emplace(Asset);

            SPhysicsBodySetup& Torso = Asset->Bodies.EmplaceGetRef();
            Torso.BoneName      = FName("torso");
            Torso.Shape         = ERagdollBodyShape::Box;
            Torso.HalfExtent    = Rig.TorsoHalfExtent;
            Torso.Mass          = Rig.TorsoMass;
            Torso.bOverrideMass = true;

            SPhysicsBodySetup& Limb = Asset->Bodies.EmplaceGetRef();
            Limb.BoneName          = FName("limb");
            Limb.Shape             = ERagdollBodyShape::Sphere;
            Limb.Radius            = 0.06f;
            Limb.TranslationOffset = Rig.Direction * kLimbReach;
            Limb.Mass              = 3.0f;
            Limb.bOverrideMass     = true;

            SPhysicsConstraintSetup& Constraint = Asset->Constraints.AddGetRef(Joint);
            Constraint.ParentBone = FName("torso");
            Constraint.ChildBone  = FName("limb");

            const ECS::FEntity Owner = Registry->Create();
            Registry->Emplace<STransformComponent>(Owner).SetLocation(At);
            SRagdollComponent& Ragdoll = Registry->Emplace<SRagdollComponent>(Owner);
            Ragdoll.State = ERagdollState::Simulated;

            Physics::FRagdollDesc Desc;
            Desc.Entity               = Owner;
            Desc.Asset                = Asset;
            Desc.Skeleton             = &Skeleton;
            Desc.ComponentBoneGlobals = &Globals;
            Desc.EntityToWorld        = Math::Translate(FMatrix4(1.0f), At);
            Ragdoll.Ragdoll = Scene->CreateRagdoll(Desc);
            EXPECT_TRUE(Ragdoll.Ragdoll);
            Scene->Update(0.0);
            EXPECT_TRUE(Ragdoll.Ragdoll && Scene->IsRagdollReady(*Ragdoll.Ragdoll));
            return Owner;
        }

        Physics::FPhysicsBodyState Body(ECS::FEntity Owner, const char* Bone) const
        {
            Physics::FPhysicsBodyState State;
            EXPECT_TRUE(Scene->TryGetTargetState(Scene->GetRagdollBoneTarget(Owner, FName(Bone)), State)) << Bone;
            return State;
        }

        // Angle between where the limb points and where it pointed at rest, both seen from the torso.
        float SwingDegrees(ECS::FEntity Owner, const FVector3& Direction) const
        {
            const FVector3 Along = Math::Inverse(Body(Owner, "torso").Rotation) * (Body(Owner, "limb").Rotation * Direction);
            return Math::Degrees(Math::Acos(Math::Clamp(Math::Dot(Along, Direction), -1.0f, 1.0f)));
        }

        float RelativeSpin(ECS::FEntity Owner) const
        {
            return Math::Length(Body(Owner, "limb").AngularVelocity - Body(Owner, "torso").AngularVelocity);
        }

        CWorld*                                  World = nullptr;
        TStrongObjectPtr<CWorld>                 WorldOwner;
        ECS::FRegistry*                          Registry = nullptr;
        TUniquePtr<Physics::FBox3DPhysicsScene>  Scene;
        float                                    Dt = 0.0f;
        TVector<TStrongObjectPtr<CPhysicsAsset>> Assets;
        SDefaultWorldSettings                    SavedSettings;
    };

    struct FFlingWatch
    {
        float MaxSpinDrop = 0.0f;
        float PeakSwing = 0.0f;
        float Swing = 0.0f;
        float Spin = 0.0f;
        float Direction = 0.0f;
        int32 Reversals = 0;

        void Sample(float NewSpin, float NewSwing, float SoftStartDegrees)
        {
            MaxSpinDrop = Math::Max(MaxSpinDrop, Spin - NewSpin);
            PeakSwing = Math::Max(PeakSwing, NewSwing);

            // A swing that turns around more than once while out past the soft start is bouncing.
            const float NewDirection = NewSwing - Swing;
            if (Math::Abs(NewDirection) > 0.05f)
            {
                Reversals += Direction != 0.0f && NewDirection * Direction < 0.0f && NewSwing > SoftStartDegrees ? 1 : 0;
                Direction = NewDirection;
            }
            Spin = NewSpin;
            Swing = NewSwing;
        }
    };
}

// Rotations inside the soft range pass through, and one past it is pulled back onto the ellipse and twist range.
TEST(RagdollSoftLimitRange, ClampsOnlyPastTheSoftStart)
{
    const FVector3 Limits = Math::Radians(FVector3(20.0f, 30.0f, 60.0f));

    FQuat Inside = Math::AngleAxis(Math::Radians(40.0f), FVector3(0.0f, 1.0f, 0.0f));
    EXPECT_EQ(ClampToSoftRange(Inside, Limits, 0.7f), 0.0f) << "40 degrees about the wide axis is inside its 42 degree soft range";

    FQuat Narrow = Math::AngleAxis(Math::Radians(27.0f), FVector3(1.0f, 0.0f, 0.0f));
    EXPECT_NEAR(ClampToSoftRange(Narrow, Limits, 0.7f), 0.67f, 0.01f) << "27 of 30 degrees is two thirds through the soft zone that starts at 21";
    EXPECT_NEAR(Math::Degrees(2.0f * Math::Acos(Math::Abs(Narrow.w))), 21.0f, 0.05f);

    FQuat Twisted = Math::AngleAxis(Math::Radians(50.0f), FVector3(0.0f, 0.0f, 1.0f));
    EXPECT_EQ(ClampToSoftRange(Twisted, Limits, 0.7f), 1.0f) << "past the limit";
    EXPECT_NEAR(Math::Degrees(2.0f * Math::Atan2(Twisted.z, Twisted.w)), 14.0f, 0.05f);
}

// A limb flung into its limit eases to a stop instead of hitting one, stays near the limit, and comes back inside without bouncing.
TEST_F(RagdollSoftLimit, AFlungLimbEasesIntoItsLimit)
{
    constexpr float kFlingSpeed = 2.0f;
    const FLimbRig Hanging = { FVector3(0.15f), 1000.0f, FVector3(0.0f, -0.15f, 0.0f), FVector3(0.0f, -1.0f, 0.0f) };

    SPhysicsConstraintSetup Soft;
    Soft.TwistLimitDegrees  = 20.0f;
    Soft.Swing1LimitDegrees = 60.0f;
    Soft.Swing2LimitDegrees = 60.0f;
    SPhysicsConstraintSetup Hard = Soft;
    Hard.bSoftLimits = false;

    // Weightless and on a heavy torso, so only the joint acts on the swing, and one solver substep per step, so a step's change is a substep's.
    SDefaultWorldSettings& Settings = World->GetDefaultWorldSettings();
    Settings.GravityScale = 0.0f;
    Settings.PhysicsHz = 240.0f;
    Settings.SolverSubStepCount = 1;
    Dt = 1.0f / Settings.PhysicsHz;

    const ECS::FEntity Owners[] = { Spawn(FVector3(0.0f, 2.0f, 0.0f), Hanging, Hard), Spawn(FVector3(5.0f, 2.0f, 0.0f), Hanging, Soft) };
    FFlingWatch Watches[2];
    for (int32 Index = 0; Index < 2; ++Index)
    {
        const SPhysicsBodyTarget Limb = Scene->GetRagdollBoneTarget(Owners[Index], FName("limb"));
        const FVector3 Spin(0.0f, 0.0f, kFlingSpeed);
        Scene->SetTargetAngularVelocity(Limb, Spin);
        Scene->SetTargetLinearVelocity(Limb, Math::Cross(Spin, Hanging.Direction * kLimbReach));
    }

    const float SoftStartDegrees = Soft.Swing1LimitDegrees * Soft.SoftLimitStart;
    Scene->Update(Dt);
    for (int32 Index = 0; Index < 2; ++Index)
    {
        Watches[Index].Spin = RelativeSpin(Owners[Index]);
    }
    for (float Elapsed = Dt; Elapsed < 2.0f; Elapsed += Dt)
    {
        Scene->Update(Dt);
        for (int32 Index = 0; Index < 2; ++Index)
        {
            Watches[Index].Sample(RelativeSpin(Owners[Index]), SwingDegrees(Owners[Index], Hanging.Direction), SoftStartDegrees);
        }
    }

    const FFlingWatch& HardWatch = Watches[0];
    const FFlingWatch& SoftWatch = Watches[1];
    std::printf("hard limit: largest one-substep spin drop %.2f rad/s, peak swing %.1f deg\n", HardWatch.MaxSpinDrop, HardWatch.PeakSwing);
    std::printf("soft limit: largest one-substep spin drop %.2f rad/s, peak swing %.1f deg, final swing %.1f deg, final spin %.2f rad/s, reversals %d\n",
        SoftWatch.MaxSpinDrop, SoftWatch.PeakSwing, SoftWatch.Swing, SoftWatch.Spin, SoftWatch.Reversals);

    EXPECT_GT(HardWatch.PeakSwing, 55.0f) << "the hard limit was reached";
    EXPECT_GE(HardWatch.MaxSpinDrop, 3.0f * SoftWatch.MaxSpinDrop) << "the soft limit stops the limb at least three times more gently";
    EXPECT_GT(SoftWatch.PeakSwing, SoftStartDegrees) << "the limb did reach the soft zone";
    EXPECT_LT(SoftWatch.PeakSwing, Soft.Swing1LimitDegrees + 3.0f) << "never far past the hard limit";
    EXPECT_LT(SoftWatch.Swing, SoftStartDegrees + 1.0f) << "back inside the soft range";
    EXPECT_LE(SoftWatch.Reversals, 1) << "it turns back once and does not bounce";
}

// Switching out of Powered mid swing must not leave the muscle's last spring impulse pushing a slack limb in a weightless world.
TEST_F(RagdollSoftLimit, SlackenedMusclesStopPushingAfterPoweredEnds)
{
    constexpr float kFlingSpeed = 3.0f;
    const FLimbRig Hanging = { FVector3(0.15f), 1000.0f, FVector3(0.0f, -0.15f, 0.0f), FVector3(0.0f, -1.0f, 0.0f) };

    SPhysicsConstraintSetup Joint;
    Joint.TwistLimitDegrees  = 45.0f;
    Joint.Swing1LimitDegrees = 80.0f;
    Joint.Swing2LimitDegrees = 80.0f;

    SDefaultWorldSettings& Settings = World->GetDefaultWorldSettings();
    Settings.GravityScale = 0.0f;
    Settings.PhysicsHz = 240.0f;
    Settings.SolverSubStepCount = 1;
    Dt = 1.0f / Settings.PhysicsHz;

    const ECS::FEntity Owner = Spawn(FVector3(0.0f, 2.0f, 0.0f), Hanging, Joint);
    Registry->Get<SRagdollComponent>(Owner).State = ERagdollState::Powered;

    const SPhysicsBodyTarget Limb = Scene->GetRagdollBoneTarget(Owner, FName("limb"));
    const FVector3 Spin(0.0f, 0.0f, kFlingSpeed);
    Scene->SetTargetAngularVelocity(Limb, Spin);
    Scene->SetTargetLinearVelocity(Limb, Math::Cross(Spin, Hanging.Direction * kLimbReach));

    // The swing's peak is where the spin bottoms out near zero while the muscle still holds the limb away from rest.
    float PreviousSpin = 0.0f;
    float SpinAtPeak = 0.0f;
    float SwingAtPeak = 0.0f;
    bool bFoundPeak = false;
    for (float Elapsed = 0.0f; Elapsed < 1.0f && !bFoundPeak; Elapsed += Dt)
    {
        Scene->Update(Dt);
        const float CurrentSpin = RelativeSpin(Owner);
        const float Swing = SwingDegrees(Owner, Hanging.Direction);
        if (Elapsed > Dt && CurrentSpin > PreviousSpin && PreviousSpin < 0.1f && Swing > 5.0f)
        {
            SpinAtPeak = PreviousSpin;
            SwingAtPeak = Swing;
            bFoundPeak = true;
        }
        PreviousSpin = CurrentSpin;
    }
    ASSERT_TRUE(bFoundPeak) << "the fling did not peak with a measurable swing before it settled";

    Registry->Get<SRagdollComponent>(Owner).State = ERagdollState::Simulated;

    float MaxSpinAfterSlackening = 0.0f;
    for (float Elapsed = 0.0f; Elapsed < 1.0f; Elapsed += Dt)
    {
        Scene->Update(Dt);
        MaxSpinAfterSlackening = Math::Max(MaxSpinAfterSlackening, RelativeSpin(Owner));
    }
    std::printf("slackened muscle: swing at peak %.1f deg, spin at peak %.3f rad/s, largest spin afterward %.3f rad/s\n",
        SwingAtPeak, SpinAtPeak, MaxSpinAfterSlackening);

    EXPECT_LT(MaxSpinAfterSlackening, 0.2f) << "a phantom muscle torque would keep accelerating the limb after it goes slack";
}

// An elliptical cone holds a limb drooping under its own weight inside the narrow swing, where a round cone of the wide swing would let it fall.
TEST_F(RagdollSoftLimit, AnEllipticalConeHoldsTheNarrowSwing)
{
    SPhysicsConstraintSetup Joint;
    Joint.TwistLimitDegrees  = 20.0f;
    Joint.Swing1LimitDegrees = 30.0f;
    Joint.Swing2LimitDegrees = 75.0f;

    // A heavy block standing on the ground holds the limb straight out to the side, so it droops about the narrow swing axis.
    const FLimbRig Sideways = { FVector3(0.2f, 0.6f, 0.2f), 100.0f, FVector3(0.2f, 0.4f, 0.0f), FVector3(1.0f, 0.0f, 0.0f) };
    const ECS::FEntity Owner = Spawn(FVector3(0.0f, 0.61f, 0.0f), Sideways, Joint);

    float LateMaxDroop = 0.0f;
    for (float Elapsed = 0.0f; Elapsed < 3.0f; Elapsed += Dt)
    {
        Scene->Update(Dt);
        LateMaxDroop = Elapsed > 2.0f ? Math::Max(LateMaxDroop, SwingDegrees(Owner, Sideways.Direction)) : LateMaxDroop;
    }
    std::printf("elliptical cone: limb rests at %.1f deg droop, narrow swing limit %.0f\n", LateMaxDroop, Joint.Swing1LimitDegrees);

    EXPECT_LT(LateMaxDroop, Joint.Swing1LimitDegrees) << "rests inside the narrow swing";
    EXPECT_GT(LateMaxDroop, 5.0f) << "it does droop under its weight";
}

// Narrowing a running ragdoll's swing reaches it without a rebuild, adding the ligament a round cone was built without.
TEST_F(RagdollSoftLimit, RetuningARunningRagdollNarrowsItsSwing)
{
    SPhysicsConstraintSetup Wide;
    Wide.TwistLimitDegrees  = 20.0f;
    Wide.Swing1LimitDegrees = 75.0f;
    Wide.Swing2LimitDegrees = 75.0f;
    Wide.bSoftLimits        = false;

    const FLimbRig Sideways = { FVector3(0.2f, 0.6f, 0.2f), 100.0f, FVector3(0.2f, 0.4f, 0.0f), FVector3(1.0f, 0.0f, 0.0f) };
    const ECS::FEntity Owner = Spawn(FVector3(0.0f, 0.61f, 0.0f), Sideways, Wide);
    CPhysicsAsset* Asset = Assets.Last().Get();

    for (float Elapsed = 0.0f; Elapsed < 2.0f; Elapsed += Dt)
    {
        Scene->Update(Dt);
    }
    const float WideDroop = SwingDegrees(Owner, Sideways.Direction);

    SPhysicsConstraintSetup& Joint = Asset->Constraints[0];
    Joint.Swing1LimitDegrees = 30.0f;
    Joint.bSoftLimits        = true;
    Scene->RetuneRagdollLimits(Owner, *Asset);

    float LateMaxDroop = 0.0f;
    for (float Elapsed = 0.0f; Elapsed < 3.0f; Elapsed += Dt)
    {
        Scene->Update(Dt);
        LateMaxDroop = Elapsed > 2.0f ? Math::Max(LateMaxDroop, SwingDegrees(Owner, Sideways.Direction)) : LateMaxDroop;
    }
    std::printf("retuned ragdoll: droop %.1f deg in the wide cone, %.1f deg after narrowing to %.0f\n", WideDroop, LateMaxDroop, Joint.Swing1LimitDegrees);

    EXPECT_GT(WideDroop, Joint.Swing1LimitDegrees + 10.0f) << "the wide cone let the limb fall past the narrow limit";
    EXPECT_LT(LateMaxDroop, Joint.Swing1LimitDegrees) << "the retuned limit pulled it back inside";
}
