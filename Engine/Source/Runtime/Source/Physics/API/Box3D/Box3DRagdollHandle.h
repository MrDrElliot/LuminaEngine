#pragma once

#include <box3d/box3d.h>
#include "Containers/Vector.h"
#include "Core/Math/Matrix/MatrixMath.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    struct FSkeletonResource;

    // One joint's muscle, a motor joint whose angular spring pulls the child body toward its target rotation under a torque cap.
    struct FRagdollMuscle
    {
        b3JointId Joint{};
        int32     Body = -1;
        int32     ParentBody = -1;
        // Share of the ragdoll's muscle torque, from the mass the joint carries and the asset's MotorStrength.
        float     TorqueScale = 1.0f;
        float     HertzOverride = 0.0f;
        float     DampingOverride = 0.0f;
    };

    // A soft joint from a fixed anchor that holds one torso body upright, solved implicitly so a light chest can carry a whole body's reflex.
    struct FRagdollBrace
    {
        b3JointId Joint = b3_nullJointId;
        b3BodyId  Body = b3_nullBodyId;
        float     InertiaShare = 0.0f;
    };

    struct FRagdollLeg
    {
        int32 Thigh = -1;
        int32 Shin = -1;
        int32 Foot = -1;
    };

    struct FRagdollArm
    {
        int32 Upper = -1;
        int32 Fore = -1;
    };

    // Which bodies are the pelvis, spine, legs and arms, and the character's axes in skeleton space, found once from bone names.
    struct FRagdollRig
    {
        bool        bBuilt = false;
        bool        bValid = false;
        int32       Pelvis = -1;
        int32       Chest = -1;
        TVector<int32> Spine;
        FRagdollLeg Legs[2];
        FRagdollArm Arms[2];
        FVector3    Up = FVector3(0.0f, 1.0f, 0.0f);
        FVector3    Forward = FVector3(0.0f, 0.0f, 1.0f);
        // Positive rotation about the swing axis carries a hanging leg forward, and about the abduct axis toward Legs[0]'s side.
        FVector3    SwingAxis = FVector3(-1.0f, 0.0f, 0.0f);
        FVector3    AbductAxis = FVector3(0.0f, 0.0f, 1.0f);
        FVector3    Lateral = FVector3(1.0f, 0.0f, 0.0f);
        // Hip to ankle at rest, and how far each hip sits out from the middle.
        float       LegLength = 0.9f;
        float       HipHalfWidth = 0.1f;
        // Height of the pelvis body's origin above the ground when standing.
        float       StandHeight = 1.0f;
        // Turns a rest pose with the arms held out into one with them hanging at the sides.
        FQuat       ArmRest[2] = { FQuat::Identity(), FQuat::Identity() };
    };

    // What a locomotion controller remembers between steps.
    struct FRagdollGait
    {
        // Leg in the air, or -1 with both feet planted, and how far through its step it is.
        int32 SwingLeg = -1;
        float StepProgress = 0.0f;
        // Each leg's target swing forward and out to the side, and knee bend, in radians.
        float LegForward[2] = { 0.0f, 0.0f };
        float LegLateral[2] = { 0.0f, 0.0f };
        float LegKnee[2] = { 0.0f, 0.0f };
        float Lean = 0.0f;
        // Turns a swing leg target set against an upright body into one the tilted pelvis can hold, in skeleton space.
        FQuat TiltFix = FQuat::Identity();
        float AirTime = 0.0f;
        float FallenTime = 0.0f;
        float Recovery = 0.0f;
        float RecoverStartHeight = 0.0f;
        float StrengthScale = 1.0f;
        bool  bFallen = false;
        bool  bGrounded = false;
    };

    // Box3D has no ragdoll object, so a ragdoll is just the bodies and joints the scene built for it.
    struct FPhysicsRagdollHandle
    {
        b3WorldId           WorldId{};
        TVector<b3BodyId>   Bodies;
        TVector<uint32>     BodyHandles;
        TVector<b3JointId>  Joints;

        // Parent body of each body, -1 for the root, parents always before their children.
        TVector<int32>          ParentBody;
        TVector<FRagdollMuscle> Muscles;
        b3BodyId                BraceAnchor = b3_nullBodyId;
        TVector<FRagdollBrace>  Braces;
        // Sum of the braced bodies' inertia, which is how hard the torso is to turn on its own.
        float                   TorsoInertia = 0.0f;
        FRagdollRig             Rig;
        FRagdollGait            Gait;
        bool                    bMusclesLive = false;

        // Per body scratch the drive rewrites each step, kept here so a step allocates nothing.
        TVector<FQuat>          RestRotation;
        TVector<FVector3>       RestPosition;
        TVector<FQuat>          Delta;
        TVector<uint8>          bExplicitDelta;

        // Bind-pose body transform per part, used to rebuild the skinning matrix from the simulated pose.
        TVector<FMatrix4>   PartBindInverse;
        TVector<int32>      JointToBone;

        // Bone names and parents, so a limb can be found by bone name.
        TSharedPtr<FSkeletonResource> Skeleton;

        bool bAddedToScene = false;
        bool bPendingDestroy = false;

        FPhysicsRagdollHandle() = default;
        FPhysicsRagdollHandle(const FPhysicsRagdollHandle&) = delete;
        FPhysicsRagdollHandle& operator=(const FPhysicsRagdollHandle&) = delete;
        FPhysicsRagdollHandle(FPhysicsRagdollHandle&&) = delete;
        FPhysicsRagdollHandle& operator=(FPhysicsRagdollHandle&&) = delete;

        ~FPhysicsRagdollHandle()
        {
            Release();
        }

        void Release()
        {
            if (!bAddedToScene || !b3World_IsValid(WorldId))
            {
                bAddedToScene = false;
                return;
            }

            for (b3JointId Joint : Joints)
            {
                if (b3Joint_IsValid(Joint))
                {
                    b3DestroyJoint(Joint, false);
                }
            }

            for (b3BodyId Body : Bodies)
            {
                if (b3Body_IsValid(Body))
                {
                    b3DestroyBody(Body);
                }
            }
            if (b3Body_IsValid(BraceAnchor))
            {
                b3DestroyBody(BraceAnchor);
            }

            Joints.clear();
            Bodies.clear();
            Braces.clear();
            BraceAnchor = b3_nullBodyId;
            bAddedToScene = false;
        }
    };
}
