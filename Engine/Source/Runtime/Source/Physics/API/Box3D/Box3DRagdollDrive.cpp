#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "Box3DPhysicsScene.h"

#include "Box3DRagdollHandle.h"
#include "Box3DUtils.h"

#include "Animation/Pose.h"
#include "Renderer/SkeletonResource.h"
#include "World/World.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/RagdollLocomotionComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina::Physics
{
    namespace
    {
        // The ground probe reaches this far below a standing pelvis, and anything nearer than the grounded reach holds it up.
        constexpr float kGroundProbeReach  = 1.6f;
        constexpr float kGroundedReach     = 1.3f;
        constexpr float kSupportHertz      = 2.5f;
        constexpr float kMaxSupportWeight  = 2.5f;
        constexpr float kUprightHertz      = 2.2f;
        constexpr float kYawHertz          = 1.2f;
        constexpr float kYawStiffnessShare = 0.25f * (kYawHertz / kUprightHertz) * (kYawHertz / kUprightHertz);
        constexpr float kMaxYawTorqueArm   = 0.3f;
        constexpr float kDriveResponse     = 6.0f;
        // A standing body unlocks its knees a little, which leaves the legs room to absorb a step.
        constexpr float kStandCrouch       = 0.97f;
        constexpr float kStandKneeBend     = 0.08f;
        constexpr float kMaxAirTime        = 0.75f;
        constexpr float kRestSpeed         = 1.0f;
        constexpr float kStandUpTilt       = 0.45f;
        constexpr float kStrengthBlendRate = 6.0f;
        // Stepping starts once the body moves faster than this, or its capture point drifts this far from the feet.
        constexpr float kStepSpeed         = 0.15f;
        constexpr float kStepTrigger       = 0.12f;
        constexpr float kSlowStepTime      = 0.5f;
        constexpr float kFastStepTime      = 0.3f;
        // Sines of how far a leg may reach forward or out to the side as a share of its length.
        constexpr float kMaxLegReach       = 0.8f;
        constexpr float kMaxLegSpread      = 0.5f;
        constexpr float kMaxHipRate        = 7.0f;
        // A capture point past this many leg lengths from the feet is beyond any step.
        constexpr float kUncatchableReach  = 1.5f;
        constexpr float kMaxLean           = 0.15f;
        // Weight this far past the feet, as a share of standing height, starts to win over the reflexes, and this far beats them.
        constexpr float kStableOverhang    = 0.35f;
        constexpr float kLostOverhang      = 0.65f;
        constexpr float kElbowBend         = 0.3f;
        constexpr float kRootFollowHertz   = 3.0f;
        constexpr float kTwoPi             = 6.2831853f;

        enum class ELimb : uint8
        {
            None,
            Pelvis,
            Spine,
            Head,
            Thigh,
            Shin,
            Foot,
            Toe,
            UpperArm,
            ForeArm,
        };

        bool Has(const FString& Lower, const char* Word)
        {
            return Lower.contains(Word);
        }

        // Bone names across the common rigs, with helper bones rejected so a twist or end bone never stands in for a limb.
        ELimb ClassifyBone(const FName& Name)
        {
            FString Lower = Name.ToString();
            Lower.ToLower();
            for (const char* Reject : { "twist", "end", "ik", "roll", "pole", "target", "nub", "finger", "thumb", "index", "middle", "ring", "pinky" })
            {
                if (Has(Lower, Reject))
                {
                    return ELimb::None;
                }
            }
            if (Has(Lower, "pelvis") || Has(Lower, "hips"))           { return ELimb::Pelvis; }
            if (Has(Lower, "spine") || Has(Lower, "chest"))           { return ELimb::Spine; }
            if (Has(Lower, "head"))                                   { return ELimb::Head; }
            if (Has(Lower, "toe") || Has(Lower, "ball"))              { return ELimb::Toe; }
            if (Has(Lower, "foot") || Has(Lower, "ankle"))            { return ELimb::Foot; }
            if (Has(Lower, "thigh") || Has(Lower, "upleg") || Has(Lower, "upperleg")) { return ELimb::Thigh; }
            if (Has(Lower, "calf") || Has(Lower, "shin") || Has(Lower, "lowerleg") || Has(Lower, "knee")
                || (Has(Lower, "leg") && !Has(Lower, "up")))          { return ELimb::Shin; }
            if (Has(Lower, "forearm") || Has(Lower, "lowerarm"))      { return ELimb::ForeArm; }
            if (Has(Lower, "upperarm") || (Has(Lower, "arm") && !Has(Lower, "fore") && !Has(Lower, "lower"))) { return ELimb::UpperArm; }
            return ELimb::None;
        }

        FVector3 Horizontal(const FVector3& V, const FVector3& Up)
        {
            return V - Up * Math::Dot(V, Up);
        }

        FVector3 ClampLength(const FVector3& V, float Max)
        {
            const float Length = Math::Length(V);
            return Length > Max && Length > 0.0f ? V * (Max / Length) : V;
        }

        float SmoothStep01(float X)
        {
            const float T = Math::Clamp(X, 0.0f, 1.0f);
            return T * T * (3.0f - 2.0f * T);
        }

        // The rotation's axis scaled by its angle, the shortest way round.
        FVector3 RotationVector(const FQuat& Rotation)
        {
            FQuat Q = Math::Normalize(Rotation);
            if (Q.w < 0.0f)
            {
                Q = FQuat(-Q.w, -Q.x, -Q.y, -Q.z);
            }
            const FVector3 Axis(Q.x, Q.y, Q.z);
            const float SinHalf = Math::Length(Axis);
            if (SinHalf < 1e-6f)
            {
                return Axis * 2.0f;
            }
            return Axis * (2.0f * Math::Atan2(SinHalf, Q.w) / SinHalf);
        }

        float MeanRotationalInertia(b3BodyId Body)
        {
            const b3Matrix3 Inertia = b3Body_GetLocalRotationalInertia(Body);
            return (Inertia.cx.x + Inertia.cy.y + Inertia.cz.z) / 3.0f;
        }

        int32 NearestAncestor(const FPhysicsRagdollHandle& Ragdoll, const TVector<ELimb>& Limbs, int32 Body, ELimb Wanted)
        {
            for (int32 Parent = Ragdoll.ParentBody[Body]; Parent >= 0; Parent = Ragdoll.ParentBody[Parent])
            {
                if (Limbs[Parent] == Wanted)
                {
                    return Parent;
                }
            }
            return -1;
        }

        bool IsAncestor(const FPhysicsRagdollHandle& Ragdoll, int32 Ancestor, int32 Body)
        {
            for (int32 Parent = Ragdoll.ParentBody[Body]; Parent >= 0; Parent = Ragdoll.ParentBody[Parent])
            {
                if (Parent == Ancestor)
                {
                    return true;
                }
            }
            return false;
        }
    }

    static void FillRagdollRest(FPhysicsRagdollHandle& Ragdoll, const SRagdollComponent& Component)
    {
        const FSkeletonResource& Skeleton = *Ragdoll.Skeleton;
        const int32 NumBodies = (int32)Ragdoll.Bodies.size();
        const bool  bTargets  = (int32)Component.TargetGlobals.size() == Skeleton.GetNumBones();
        Ragdoll.RestRotation.resize(NumBodies);
        Ragdoll.RestPosition.resize(NumBodies);
        for (int32 Body = 0; Body < NumBodies; ++Body)
        {
            const int32 Bone = Ragdoll.JointToBone[Body];
            const FMatrix4 Global = bTargets ? Component.TargetGlobals[Bone] : Math::Inverse(Skeleton.GetBone(Bone).InvBindMatrix);
            FVector3 Scale;
            AnimPose::DecomposeTRS(Global, Ragdoll.RestPosition[Body], Ragdoll.RestRotation[Body], Scale);
            Ragdoll.RestRotation[Body] = Math::Normalize(Ragdoll.RestRotation[Body]);
        }
    }

    static void BuildRagdollRig(FPhysicsRagdollHandle& Ragdoll, const SRagdollLocomotionComponent* Locomotion)
    {
        FRagdollRig& Rig = Ragdoll.Rig;
        Rig = FRagdollRig();
        Rig.bBuilt = true;

        const FSkeletonResource& Skeleton = *Ragdoll.Skeleton;
        const int32 NumBodies = (int32)Ragdoll.Bodies.size();
        const TVector<FVector3>& Rest = Ragdoll.RestPosition;

        TVector<ELimb> Limbs;
        Limbs.resize(NumBodies, ELimb::None);
        for (int32 Body = 0; Body < NumBodies; ++Body)
        {
            Limbs[Body] = ClassifyBone(Skeleton.GetBone(Ragdoll.JointToBone[Body]).Name);
        }

        TFixedVector<int32, 4> Feet;
        TFixedVector<int32, 4> UpperArms;
        for (int32 Body = 0; Body < NumBodies; ++Body)
        {
            // A foot that carries another foot below it, as some rigs chain them, is not the end of the leg.
            if (Limbs[Body] == ELimb::Foot && NearestAncestor(Ragdoll, Limbs, Body, ELimb::Foot) < 0 && Feet.size() < 4)
            {
                Feet.push_back(Body);
            }
            if (Limbs[Body] == ELimb::UpperArm && NearestAncestor(Ragdoll, Limbs, Body, ELimb::UpperArm) < 0 && UpperArms.size() < 4)
            {
                UpperArms.push_back(Body);
            }
        }
        if (Feet.size() != 2)
        {
            return;
        }

        for (int32 Side = 0; Side < 2; ++Side)
        {
            FRagdollLeg& Leg = Rig.Legs[Side];
            Leg.Foot  = Feet[Side];
            Leg.Shin  = NearestAncestor(Ragdoll, Limbs, Leg.Foot, ELimb::Shin);
            Leg.Shin  = Leg.Shin >= 0 ? Leg.Shin : Ragdoll.ParentBody[Leg.Foot];
            Leg.Thigh = Leg.Shin >= 0 ? NearestAncestor(Ragdoll, Limbs, Leg.Shin, ELimb::Thigh) : -1;
            Leg.Thigh = Leg.Thigh >= 0 || Leg.Shin < 0 ? Leg.Thigh : Ragdoll.ParentBody[Leg.Shin];
            if (Leg.Shin < 0 || Leg.Thigh < 0)
            {
                return;
            }
        }

        for (int32 Body = 0; Body < NumBodies && Rig.Pelvis < 0; ++Body)
        {
            if (Limbs[Body] == ELimb::Pelvis && IsAncestor(Ragdoll, Body, Rig.Legs[0].Thigh) && IsAncestor(Ragdoll, Body, Rig.Legs[1].Thigh))
            {
                Rig.Pelvis = Body;
            }
        }
        if (Rig.Pelvis < 0)
        {
            Rig.Pelvis = Ragdoll.ParentBody[Rig.Legs[0].Thigh];
        }
        if (Rig.Pelvis < 0)
        {
            return;
        }

        for (int32 Body = 0; Body < NumBodies; ++Body)
        {
            if (Limbs[Body] == ELimb::Spine && IsAncestor(Ragdoll, Rig.Pelvis, Body))
            {
                Rig.Spine.push_back(Body);
            }
        }

        const FVector3 FeetCenter = (Rest[Rig.Legs[0].Foot] + Rest[Rig.Legs[1].Foot]) * 0.5f;
        Rig.Up = Math::Normalize(Rest[Rig.Pelvis] - FeetCenter);

        // Toes point the way the character faces, which beats any guess about how the rig was authored.
        FVector3 Forward = Locomotion != nullptr ? Locomotion->ForwardAxis : FVector3(0.0f, 0.0f, 1.0f);
        for (int32 Body = 0; Body < NumBodies; ++Body)
        {
            if (Limbs[Body] == ELimb::Toe && Ragdoll.ParentBody[Body] == Rig.Legs[0].Foot)
            {
                Forward = Rest[Body] - Rest[Rig.Legs[0].Foot];
                break;
            }
        }
        Forward = Horizontal(Forward, Rig.Up);
        if (Math::Length(Forward) < 1e-4f)
        {
            return;
        }
        Rig.Forward   = Math::Normalize(Forward);
        Rig.SwingAxis = Math::Normalize(Math::Cross(-Rig.Up, Rig.Forward));

        const FVector3 Lateral = Math::Cross(Rig.Up, Rig.Forward);
        if (Math::Dot(Rest[Rig.Legs[0].Thigh] - Rest[Rig.Pelvis], Lateral) < Math::Dot(Rest[Rig.Legs[1].Thigh] - Rest[Rig.Pelvis], Lateral))
        {
            const FRagdollLeg First = Rig.Legs[0];
            Rig.Legs[0] = Rig.Legs[1];
            Rig.Legs[1] = First;
        }

        for (int32 Upper : UpperArms)
        {
            const int32 Side = Math::Dot(Rest[Upper] - Rest[Rig.Pelvis], Lateral) >= 0.0f ? 0 : 1;
            FRagdollArm& Arm = Rig.Arms[Side];
            Arm.Upper = Upper;
            for (int32 Body = 0; Body < NumBodies; ++Body)
            {
                if (Limbs[Body] == ELimb::ForeArm && NearestAncestor(Ragdoll, Limbs, Body, ELimb::UpperArm) == Upper)
                {
                    Arm.Fore = Body;
                    break;
                }
            }

            // Arms held out from the side are brought down to hang, or the walk would swing a scarecrow.
            if (Arm.Fore >= 0)
            {
                const FVector3 Along = Math::Normalize(Rest[Arm.Fore] - Rest[Upper]);
                const FVector3 Hang  = Math::Normalize(-Rig.Up + Lateral * (Side == 0 ? 0.15f : -0.15f));
                if (Math::Dot(Along, -Rig.Up) < 0.75f)
                {
                    Rig.ArmRest[Side] = Math::RotationBetween(Along, Hang);
                }
            }
        }

        for (int32 Body : Rig.Spine)
        {
            if (Rig.Chest < 0 || Math::Dot(Rest[Body], Rig.Up) > Math::Dot(Rest[Rig.Chest], Rig.Up))
            {
                Rig.Chest = Body;
            }
        }
        if (Rig.Chest < 0 && Rig.Arms[0].Upper >= 0)
        {
            Rig.Chest = Ragdoll.ParentBody[Rig.Arms[0].Upper];
        }

        // The sole sits below the ankle by however far the foot body reaches down right now, which is its rest pose.
        float SoleDrop = 0.0f;
        const b3BodyId FootBody = Ragdoll.Bodies[Rig.Legs[0].Foot];
        if (b3Body_IsValid(FootBody))
        {
            const b3AABB Bounds = b3Body_ComputeAABB(FootBody);
            SoleDrop = Math::Max(Box3DUtils::FromB3Vec3(b3Body_GetPosition(FootBody)).y - Box3DUtils::FromB3Vec3(Bounds.lowerBound).y, 0.0f);
        }
        Rig.StandHeight  = Math::Max(Math::Dot(Rest[Rig.Pelvis] - FeetCenter, Rig.Up) + SoleDrop, 0.1f);
        Rig.Lateral      = Lateral;
        Rig.AbductAxis   = Math::Normalize(Math::Cross(-Rig.Up, Lateral));
        Rig.LegLength    = Math::Max(Math::Length(Rest[Rig.Legs[0].Thigh] - Rest[Rig.Legs[0].Foot]), 0.1f);
        Rig.HipHalfWidth = Math::Abs(Math::Dot(Rest[Rig.Legs[0].Thigh] - Rest[Rig.Legs[1].Thigh], Lateral)) * 0.5f;
        Rig.bValid = true;
    }

    static void SlackenRagdollMuscles(FPhysicsRagdollHandle& Ragdoll)
    {
        for (const FRagdollMuscle& Muscle : Ragdoll.Muscles)
        {
            if (b3Joint_IsValid(Muscle.Joint))
            {
                b3MotorJoint_SetMaxSpringTorque(Muscle.Joint, 0.0f);
            }
        }
        for (const FRagdollBrace& Brace : Ragdoll.Braces)
        {
            if (b3Joint_IsValid(Brace.Joint))
            {
                b3MotorJoint_SetMaxSpringTorque(Brace.Joint, 0.0f);
            }
        }
        Ragdoll.bMusclesLive = false;
        Ragdoll.Gait = FRagdollGait();
    }

    // A shapeless static anchor with one slack joint to each torso body, made the first time the ragdoll balances.
    static void CreateRagdollBraces(FPhysicsRagdollHandle& Ragdoll)
    {
        const FRagdollRig& Rig = Ragdoll.Rig;

        b3BodyDef AnchorDef = b3DefaultBodyDef();
        AnchorDef.type     = b3_staticBody;
        AnchorDef.position = b3Body_GetPosition(Ragdoll.Bodies[Rig.Pelvis]);
        Ragdoll.BraceAnchor = b3CreateBody(Ragdoll.WorldId, &AnchorDef);
        if (!b3Body_IsValid(Ragdoll.BraceAnchor))
        {
            return;
        }

        const auto Brace = [&Ragdoll](int32 Body)
        {
            b3MotorJointDef Def = b3DefaultMotorJointDef();
            Def.base.bodyIdA = Ragdoll.BraceAnchor;
            Def.base.bodyIdB = Ragdoll.Bodies[Body];
            Def.base.localFrameA = b3Transform{ b3Vec3_zero, b3Quat_identity };
            Def.base.localFrameB = b3Transform{ b3Vec3_zero, b3Quat_identity };
            Def.base.collideConnected = false;
            Def.linearVelocity = b3Vec3_zero;
            Def.maxVelocityForce = 0.0f;
            Def.angularVelocity = b3Vec3_zero;
            Def.maxVelocityTorque = 0.0f;
            Def.linearHertz = 0.0f;
            Def.linearDampingRatio = 0.0f;
            Def.maxSpringForce = 0.0f;
            Def.angularHertz = 0.0f;
            Def.angularDampingRatio = 1.0f;
            Def.maxSpringTorque = 0.0f;

            const b3JointId Joint = b3CreateMotorJoint(Ragdoll.WorldId, &Def);
            if (b3Joint_IsValid(Joint))
            {
                Ragdoll.Joints.push_back(Joint);
                Ragdoll.Braces.push_back(FRagdollBrace{ Joint, Ragdoll.Bodies[Body], MeanRotationalInertia(Ragdoll.Bodies[Body]) });
                Ragdoll.TorsoInertia += MeanRotationalInertia(Ragdoll.Bodies[Body]);
            }
        };

        Ragdoll.TorsoInertia = 0.0f;
        Brace(Rig.Pelvis);
        for (int32 Body : Rig.Spine)
        {
            Brace(Body);
        }
        for (FRagdollBrace& Each : Ragdoll.Braces)
        {
            Each.InertiaShare /= Math::Max(Ragdoll.TorsoInertia, 1e-6f);
        }
    }

    // Poses the legs where the stepping put them and swings the arms against them, as rotations of the rest pose about the character's own axes.
    static void PoseRagdollGait(FPhysicsRagdollHandle& Ragdoll, const SRagdollLocomotionComponent& Locomotion)
    {
        const FRagdollRig&  Rig  = Ragdoll.Rig;
        const FRagdollGait& Gait = Ragdoll.Gait;
        const FQuat ChestLean = Math::AngleAxis(-Gait.Lean, Rig.SwingAxis);

        for (int32 k = 0; k < (int32)Rig.Spine.size(); ++k)
        {
            const float Share = (float)(k + 1) / (float)Rig.Spine.size();
            Ragdoll.Delta[Rig.Spine[k]] = Math::AngleAxis(-Gait.Lean * Share, Rig.SwingAxis);
            Ragdoll.bExplicitDelta[Rig.Spine[k]] = 1;
        }

        for (int32 Side = 0; Side < 2; ++Side)
        {
            // A swing leg is placed in the world, while a planted leg's hip pushes any pelvis lean back upright.
            const FRagdollLeg& Leg = Rig.Legs[Side];
            const FQuat Frame = Gait.SwingLeg == Side ? Gait.TiltFix : FQuat::Identity();
            const FQuat Out = Math::AngleAxis(Gait.LegLateral[Side], Rig.AbductAxis);
            Ragdoll.Delta[Leg.Thigh] = Frame * Math::AngleAxis(Gait.LegForward[Side], Rig.SwingAxis) * Out;
            Ragdoll.Delta[Leg.Shin]  = Frame * Math::AngleAxis(Gait.LegForward[Side] - Gait.LegKnee[Side], Rig.SwingAxis) * Out;
            Ragdoll.Delta[Leg.Foot]  = Frame * Math::AngleAxis(0.3f * (Gait.LegKnee[Side] - kStandKneeBend), Rig.SwingAxis);
            Ragdoll.bExplicitDelta[Leg.Thigh] = 1;
            Ragdoll.bExplicitDelta[Leg.Shin]  = 1;
            Ragdoll.bExplicitDelta[Leg.Foot]  = 1;

            // Each arm swings against the leg on its own side.
            const FRagdollArm& Arm = Rig.Arms[Side];
            const float ArmAngle = -Locomotion.ArmSwing * Math::Clamp(Gait.LegForward[Side] / Math::Max(Locomotion.HipSwing, 0.1f), -1.0f, 1.0f);
            if (Arm.Upper >= 0)
            {
                Ragdoll.Delta[Arm.Upper] = Math::AngleAxis(ArmAngle, Rig.SwingAxis) * ChestLean * Rig.ArmRest[Side];
                Ragdoll.bExplicitDelta[Arm.Upper] = 1;
            }
            if (Arm.Fore >= 0)
            {
                Ragdoll.Delta[Arm.Fore] = Math::AngleAxis(ArmAngle + kElbowBend + Gait.Lean, Rig.SwingAxis) * ChestLean * Rig.ArmRest[Side];
                Ragdoll.bExplicitDelta[Arm.Fore] = 1;
            }
        }
    }

    // Reflexes that stand in for the balance a real body keeps through its feet, plus the push that walks it.
    void FBox3DPhysicsScene::BalanceRagdoll(ECS::FEntity Owner, FPhysicsRagdollHandle& Ragdoll, SRagdollLocomotionComponent& Locomotion, float Dt)
    {
        FRagdollRig&  Rig  = Ragdoll.Rig;
        FRagdollGait& Gait = Ragdoll.Gait;

        const b3BodyId Pelvis = Ragdoll.Bodies[Rig.Pelvis];
        const b3BodyId Chest  = Rig.Chest >= 0 ? Ragdoll.Bodies[Rig.Chest] : Pelvis;
        if (!b3Body_IsValid(Pelvis) || !b3Body_IsValid(Chest))
        {
            return;
        }

        const FVector3 Gravity = Box3DUtils::FromB3Vec3(b3World_GetGravity(WorldId));
        const float    G       = Math::Max(Math::Length(Gravity), 1e-3f);
        const FVector3 WorldUp = Math::Length(Gravity) > 1e-3f ? -Gravity / G : FVector3(0.0f, 1.0f, 0.0f);

        const FRagdollMotion Motion = MeasureRagdoll(Ragdoll);
        const float M = Math::Max(Motion.Mass, 1e-3f);

        const FVector3 PelvisPos = Box3DUtils::FromB3Vec3(b3Body_GetPosition(Pelvis));
        const FQuat    Character = Math::Normalize(Box3DUtils::FromB3Quat(b3Body_GetRotation(Pelvis)) * Math::Inverse(Ragdoll.RestRotation[Rig.Pelvis]));
        const FVector3 UpNow     = Math::Normalize(Character * Rig.Up);
        const FVector3 FacingNow = Character * Rig.Forward;

        SRayCastSettings Probe;
        Probe.Start = PelvisPos;
        Probe.End   = PelvisPos - WorldUp * (Rig.StandHeight * kGroundProbeReach);
        Probe.AddIgnoredEntity(Owner);
        const TOptional<SRayResult> Ground = CastRay(Probe);
        const float Height = Ground.has_value() && Ground->bHit ? Math::Dot(PelvisPos - Ground->Location, WorldUp) : 1e30f;

        Gait.bGrounded = Height < Rig.StandHeight * kGroundedReach;
        Gait.AirTime   = Gait.bGrounded ? 0.0f : Gait.AirTime + Dt;

        const FVector3 FeetCenter = (Box3DUtils::FromB3Vec3(b3Body_GetPosition(Ragdoll.Bodies[Rig.Legs[0].Foot]))
                                   + Box3DUtils::FromB3Vec3(b3Body_GetPosition(Ragdoll.Bodies[Rig.Legs[1].Foot]))) * 0.5f;

        // Moving so fast that no step could reach where the body is heading trips it, however upright it still is.
        const FVector3 Capture   = Horizontal(Motion.CenterOfMass + Motion.LinearVelocity * Math::Sqrt(Rig.StandHeight / G), WorldUp);
        const bool     bUncaught = Gait.bGrounded && Math::Length(Capture - Horizontal(FeetCenter, WorldUp)) > kUncatchableReach * Rig.LegLength;

        const float Tilt = Math::Acos(Math::Clamp(Math::Dot(UpNow, WorldUp), -1.0f, 1.0f));
        if (!Gait.bFallen && (Tilt > Math::Radians(Locomotion.FallAngle) || Gait.AirTime > kMaxAirTime || bUncaught))
        {
            Gait.bFallen    = true;
            Gait.FallenTime = 0.0f;
            Gait.Recovery   = 0.0f;
        }

        float BalanceAlpha   = 1.0f;
        float TargetStrength = 1.0f;
        if (Gait.bFallen)
        {
            Gait.FallenTime += Dt;
            const bool bAtRest = Math::Length(Motion.LinearVelocity) < kRestSpeed;
            if (Locomotion.bAutoRecover && (Gait.Recovery > 0.0f || (Gait.FallenTime > Locomotion.RecoverDelay && bAtRest && Gait.bGrounded)))
            {
                if (Gait.Recovery <= 0.0f)
                {
                    Gait.RecoverStartHeight = Math::Min(Height, Rig.StandHeight);
                }
                Gait.Recovery = Math::Min(Gait.Recovery + Dt / Math::Max(Locomotion.RecoverDuration, 0.1f), 1.0f);
            }
            BalanceAlpha   = SmoothStep01(Gait.Recovery);
            TargetStrength = Math::Lerp(Locomotion.FallenStrength, 1.0f, BalanceAlpha);
            if (Gait.Recovery >= 1.0f && Tilt < kStandUpTilt)
            {
                Gait.bFallen  = false;
                Gait.Recovery = 0.0f;
            }
        }
        Gait.StrengthScale = Math::MoveTowards(Gait.StrengthScale, TargetStrength, kStrengthBlendRate * Dt);

        const FVector3 HorizontalVelocity = Horizontal(Motion.LinearVelocity, WorldUp);

        // Reflexes only work while the weight is over the feet, so a shove that carries it past them topples the body.
        const float Overhang  = Math::Length(Horizontal(Motion.CenterOfMass - FeetCenter, WorldUp)) / Rig.StandHeight;
        const float Stability = Gait.bFallen ? 1.0f : 1.0f - SmoothStep01((Overhang - kStableOverhang) / (kLostOverhang - kStableOverhang));
        const float Strength  = Locomotion.BalanceStrength * BalanceAlpha * Stability;
        if (Strength > 0.0f)
        {
            const FVector3 PelvisVelocity = Box3DUtils::FromB3Vec3(b3Body_GetLinearVelocity(Pelvis));
            if (Gait.bGrounded)
            {
                // A spring on the pelvis height holds the body up as straightened legs would, never pulling it down.
                float TargetHeight = Rig.StandHeight * kStandCrouch;
                if (Gait.bFallen)
                {
                    TargetHeight = Math::Lerp(Gait.RecoverStartHeight, TargetHeight, BalanceAlpha);
                }
                const float Omega = kTwoPi * kSupportHertz;
                const float Accel = Omega * Omega * (TargetHeight - Height) - 2.0f * Omega * Math::Dot(PelvisVelocity, WorldUp);
                const float Lift  = Math::Clamp(M * (G * Locomotion.SupportFraction + Accel), 0.0f, M * G * kMaxSupportWeight) * Strength;
                b3Body_ApplyForceToCenter(Pelvis, Box3DUtils::ToB3Vec3(WorldUp * Lift), true);

                const FVector3 Wanted = ClampLength(Horizontal(Locomotion.MoveInput, WorldUp), 1.0f) * Locomotion.MaxSpeed;
                const FVector3 Push   = ClampLength((Wanted - HorizontalVelocity) * kDriveResponse, Locomotion.MaxAcceleration) * (M * BalanceAlpha);
                // Mostly at the hips, since a push high on the chest would tip the body over its own feet.
                b3Body_ApplyForceToCenter(Pelvis, Box3DUtils::ToB3Vec3(Push * 0.75f), true);
                b3Body_ApplyForceToCenter(Chest, Box3DUtils::ToB3Vec3(Push * 0.25f), true);
            }
        }

        // Upright and facing reflexes act on the whole body's inertia, approximated as its mass at a third of its height.
        const float Lever   = 0.35f * Rig.StandHeight;
        const float Inertia = M * Lever * Lever;
        const float UpOmega = kTwoPi * kUprightHertz;

        FVector3 Facing = Horizontal(Locomotion.FacingDirection, WorldUp);
        if (Math::Length(Facing) < 0.1f)
        {
            Facing = Horizontal(Locomotion.MoveInput, WorldUp);
        }
        const FVector3 FacingNowFlat = Horizontal(FacingNow, WorldUp);
        float YawError = 0.0f;
        if (Math::Length(Facing) > 0.1f && Math::Length(FacingNowFlat) > 1e-3f)
        {
            const FVector3 From = Math::Normalize(FacingNowFlat);
            const FVector3 To   = Math::Normalize(Facing);
            YawError = Math::Atan2(Math::Dot(Math::Cross(From, To), WorldUp), Math::Dot(From, To));
        }

        // The braces share one stiffness, so turning is asked for as a smaller angle that gives the gentler, capped yaw torque.
        const float Stiffness = Inertia * UpOmega * UpOmega;
        const float YawTurn   = Math::Clamp(YawError * kYawStiffnessShare, -M * G * kMaxYawTorqueArm / Stiffness, M * G * kMaxYawTorqueArm / Stiffness);
        const FQuat Correction = Math::AngleAxis(YawTurn, WorldUp) * Math::RotationBetween(UpNow, WorldUp);

        // Each brace is stiff against its own body's inertia, so together they move the whole body like one critically damped spring.
        const float BraceRatio = Math::Sqrt(Inertia / Math::Max(Ragdoll.TorsoInertia, 1e-6f));
        for (const FRagdollBrace& Brace : Ragdoll.Braces)
        {
            if (!b3Joint_IsValid(Brace.Joint))
            {
                continue;
            }
            const FQuat Target = Math::Normalize(Correction * Box3DUtils::FromB3Quat(b3Body_GetRotation(Brace.Body)));
            b3Joint_SetLocalFrameA(Brace.Joint, b3Transform{ b3Vec3_zero, Box3DUtils::ToB3Quat(Target) });
            b3MotorJoint_SetAngularHertz(Brace.Joint, kUprightHertz * BraceRatio);
            b3MotorJoint_SetAngularDampingRatio(Brace.Joint, BraceRatio);
            b3MotorJoint_SetMaxSpringTorque(Brace.Joint, M * G * Rig.StandHeight * Strength * Brace.InertiaShare);
        }

        const float Speed = Math::Length(HorizontalVelocity);
        StepRagdoll(Ragdoll, Locomotion, Character, Motion.CenterOfMass, HorizontalVelocity, FeetCenter, WorldUp, G, Dt);

        Locomotion.bFallen   = Gait.bFallen;
        Locomotion.bGrounded = Gait.bGrounded;
        Locomotion.Speed     = Speed;
        Locomotion.Recovery  = Gait.Recovery;
    }

    // Places each swing foot where the body is heading, its capture point, so steps keep the feet under a walking or shoved body.
    void FBox3DPhysicsScene::StepRagdoll(FPhysicsRagdollHandle& Ragdoll, const SRagdollLocomotionComponent& Locomotion, const FQuat& Character,
                                         const FVector3& CenterOfMass, const FVector3& Velocity, const FVector3& FeetCenter,
                                         const FVector3& WorldUp, float Gravity, float Dt)
    {
        const FRagdollRig& Rig  = Ragdoll.Rig;
        FRagdollGait&      Gait = Ragdoll.Gait;

        const float Speed = Math::Length(Velocity);
        Gait.Lean = Gait.bFallen ? 0.0f : kMaxLean * Math::Clamp(Speed / Math::Max(Locomotion.MaxSpeed, 0.1f), 0.0f, 1.0f);

        if (Gait.bFallen || !Gait.bGrounded)
        {
            Gait.SwingLeg = -1;
            Gait.TiltFix  = FQuat::Identity();
            for (int32 Side = 0; Side < 2; ++Side)
            {
                Gait.LegForward[Side] = 0.0f;
                Gait.LegLateral[Side] = 0.0f;
                Gait.LegKnee[Side]    = kStandKneeBend;
            }
            return;
        }

        const FVector3 Capture  = Horizontal(CenterOfMass, WorldUp) + Velocity * Math::Sqrt(Rig.StandHeight / Gravity);
        const FVector3 Lateral  = Math::Normalize(Horizontal(Character * Rig.Lateral, WorldUp));
        const FVector3 FeetFlat = Horizontal(FeetCenter, WorldUp);
        const bool bWantsToMove = Math::Length(Horizontal(Locomotion.MoveInput, WorldUp)) > 0.05f;
        const bool bNeedsStep   = bWantsToMove || Speed > kStepSpeed || Math::Length(Capture - FeetFlat) > kStepTrigger;

        const float StepDuration = Math::Lerp(kSlowStepTime, kFastStepTime, Math::Clamp(Speed / Math::Max(Locomotion.MaxSpeed, 0.1f), 0.0f, 1.0f));
        if (Gait.SwingLeg >= 0)
        {
            Gait.StepProgress += Dt / StepDuration;
            if (Gait.StepProgress >= 1.0f)
            {
                Gait.SwingLeg     = bNeedsStep ? 1 - Gait.SwingLeg : -1;
                Gait.StepProgress = 0.0f;
            }
        }
        else if (bNeedsStep)
        {
            // The foot farther from where the body is heading is the one that has to move.
            const FVector3 Foot0 = Box3DUtils::FromB3Vec3(b3Body_GetPosition(Ragdoll.Bodies[Rig.Legs[0].Foot]));
            const FVector3 Foot1 = Box3DUtils::FromB3Vec3(b3Body_GetPosition(Ragdoll.Bodies[Rig.Legs[1].Foot]));
            Gait.SwingLeg     = Math::Length(Horizontal(Foot0, WorldUp) - Capture) > Math::Length(Horizontal(Foot1, WorldUp) - Capture) ? 0 : 1;
            Gait.StepProgress = 0.0f;
        }

        // Leg targets assume an upright body facing where the pelvis faces, so a stance leg carries the torso.
        const FQuat    Level   = Math::RotationBetween(Math::Normalize(Character * Rig.Up), WorldUp);
        const FQuat    Upright = Math::Normalize(Level * Character);
        const FQuat    ToCharacter = Math::Inverse(Upright);
        Gait.TiltFix = Math::Normalize(Math::Inverse(Character) * Level * Character);
        for (int32 Side = 0; Side < 2; ++Side)
        {
            const FRagdollLeg& Leg = Rig.Legs[Side];
            const FVector3 Hip  = Box3DUtils::FromB3Vec3(b3Body_GetPosition(Ragdoll.Bodies[Leg.Thigh]));
            const FVector3 Foot = Box3DUtils::FromB3Vec3(b3Body_GetPosition(Ragdoll.Bodies[Leg.Foot]));
            const bool bSwing = Gait.SwingLeg == Side;

            // A swing foot reaches for the capture point at its own hip's width, a planted one holds where it stands.
            const FVector3 Target = bSwing ? Capture + Lateral * (Side == 0 ? Rig.HipHalfWidth : -Rig.HipHalfWidth) : Horizontal(Foot, WorldUp);
            const FVector3 Reach  = ToCharacter * Horizontal(Target - Hip, WorldUp);

            // Rate limited, since a hip snapped straight to its landing angle kicks the pelvis over with its reaction.
            const float Forward = Math::Asin(Math::Clamp(Math::Dot(Reach, Rig.Forward) / Rig.LegLength, -kMaxLegReach, kMaxLegReach));
            const float Out     = Math::Asin(Math::Clamp(Math::Dot(Reach, Rig.Lateral) / Rig.LegLength, -kMaxLegSpread, kMaxLegSpread));
            Gait.LegForward[Side] = Math::MoveTowards(Gait.LegForward[Side], Forward, kMaxHipRate * Dt);
            Gait.LegLateral[Side] = Math::MoveTowards(Gait.LegLateral[Side], Out, kMaxHipRate * Dt);
            Gait.LegKnee[Side]    = kStandKneeBend + (bSwing ? Locomotion.KneeLift * Math::Sin(Math::Pi<float>() * Math::Min(Gait.StepProgress, 1.0f)) : 0.0f);
        }
    }

    // Holds the root on the animated root under the entity, firmly enough to stand but loosely enough to be knocked about.
    void FBox3DPhysicsScene::FollowRagdollRoot(ECS::FEntity Owner, FPhysicsRagdollHandle& Ragdoll, const SRagdollComponent& Component)
    {
        const STransformComponent* Transform = World->GetEntityRegistry().TryGet<STransformComponent>(Owner);
        const b3BodyId Root = Ragdoll.Bodies[0];
        if (Transform == nullptr || !b3Body_IsValid(Root))
        {
            return;
        }

        FVector3 TargetPosition;
        FQuat    TargetRotation;
        FVector3 Scale;
        const int32 RootBone = Ragdoll.JointToBone[0];
        const FMatrix4 Global = (int32)Component.TargetGlobals.size() == Ragdoll.Skeleton->GetNumBones()
            ? Component.TargetGlobals[RootBone] : Math::Inverse(Ragdoll.Skeleton->GetBone(RootBone).InvBindMatrix);
        AnimPose::DecomposeTRS(Transform->GetWorldMatrix() * Global, TargetPosition, TargetRotation, Scale);

        const FVector3 Gravity = Box3DUtils::FromB3Vec3(b3World_GetGravity(WorldId));
        const float    M       = Math::Max(MeasureRagdoll(Ragdoll).Mass, 1e-3f);
        const float    Limit   = M * Math::Max(Math::Length(Gravity), 1.0f) * Component.RootFollowStrength;
        const float    Omega   = kTwoPi * kRootFollowHertz;

        const FVector3 Position = Box3DUtils::FromB3Vec3(b3Body_GetPosition(Root));
        const FVector3 Velocity = Box3DUtils::FromB3Vec3(b3Body_GetLinearVelocity(Root));
        const FVector3 Pull = ClampLength((TargetPosition - Position) * (Omega * Omega) - Velocity * (2.0f * Omega) - Gravity, Limit / M) * M;
        b3Body_ApplyForceToCenter(Root, Box3DUtils::ToB3Vec3(Pull), true);

        const FQuat    Rotation = Box3DUtils::FromB3Quat(b3Body_GetRotation(Root));
        const FVector3 Spin     = Box3DUtils::FromB3Vec3(b3Body_GetAngularVelocity(Root));
        const FVector3 Error    = RotationVector(Math::Normalize(TargetRotation) * Math::Inverse(Rotation));
        const float    Inertia  = M * 0.1f;
        const FVector3 Twist    = ClampLength((Error * (Omega * Omega) - Spin * (2.0f * Omega)) * Inertia, Limit * 0.5f);
        b3Body_ApplyTorque(Root, Box3DUtils::ToB3Vec3(Twist), true);
    }

    void FBox3DPhysicsScene::DrivePoweredRagdoll(ECS::FEntity Owner, FPhysicsRagdollHandle& Ragdoll, const SRagdollComponent& Component,
                                                 SRagdollLocomotionComponent* Locomotion, float Dt)
    {
        const int32 NumBodies = (int32)Ragdoll.Bodies.size();
        if (Ragdoll.Skeleton == nullptr || (int32)Ragdoll.ParentBody.size() != NumBodies || (int32)Ragdoll.JointToBone.size() != NumBodies)
        {
            return;
        }

        FillRagdollRest(Ragdoll, Component);
        if (!Ragdoll.Rig.bBuilt)
        {
            BuildRagdollRig(Ragdoll, Locomotion);
        }

        Ragdoll.Delta.resize(NumBodies);
        Ragdoll.bExplicitDelta.assign(NumBodies, 0);

        const bool bWalks = Locomotion != nullptr && Ragdoll.Rig.bValid;
        if (bWalks)
        {
            if (!b3Body_IsValid(Ragdoll.BraceAnchor))
            {
                CreateRagdollBraces(Ragdoll);
            }
            BalanceRagdoll(Owner, Ragdoll, *Locomotion, Dt);
            if (Locomotion->bProceduralGait)
            {
                PoseRagdollGait(Ragdoll, *Locomotion);
            }
        }
        else if (Component.RootDrive == ERagdollRootDrive::FollowEntity)
        {
            FollowRagdollRoot(Owner, Ragdoll, Component);
        }

        // A body without its own pose change carries its parent's, so a hand follows its swinging arm.
        for (int32 Body = 0; Body < NumBodies; ++Body)
        {
            if (!Ragdoll.bExplicitDelta[Body])
            {
                const int32 Parent = Ragdoll.ParentBody[Body];
                Ragdoll.Delta[Body] = Parent >= 0 ? Ragdoll.Delta[Parent] : FQuat::Identity();
            }
        }

        const float Strength = Component.MuscleStrength * (bWalks ? Ragdoll.Gait.StrengthScale : 1.0f);
        for (const FRagdollMuscle& Muscle : Ragdoll.Muscles)
        {
            if (!b3Joint_IsValid(Muscle.Joint))
            {
                continue;
            }

            const FQuat Child  = Ragdoll.Delta[Muscle.Body] * Ragdoll.RestRotation[Muscle.Body];
            const FQuat Parent = Ragdoll.Delta[Muscle.ParentBody] * Ragdoll.RestRotation[Muscle.ParentBody];
            const FQuat Relative = Math::Normalize(Math::Inverse(Parent) * Child);

            b3Joint_SetLocalFrameB(Muscle.Joint, b3Transform{ b3Vec3_zero, Box3DUtils::ToB3Quat(Math::Normalize(Math::Inverse(Relative))) });
            b3MotorJoint_SetAngularHertz(Muscle.Joint, Muscle.HertzOverride > 0.0f ? Muscle.HertzOverride : Component.MuscleStiffness);
            b3MotorJoint_SetAngularDampingRatio(Muscle.Joint, Muscle.DampingOverride > 0.0f ? Muscle.DampingOverride : Component.MuscleDamping);
            b3MotorJoint_SetMaxSpringTorque(Muscle.Joint, Component.MaxMuscleTorque * Strength * Muscle.TorqueScale);
        }
        Ragdoll.bMusclesLive = true;
    }

    void FBox3DPhysicsScene::UpdatePoweredRagdolls(float FixedDt)
    {
        if (OwnedRagdolls.empty())
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = World->GetEntityRegistry();
        for (const FOwnedRagdoll& Owned : OwnedRagdolls)
        {
            FPhysicsRagdollHandle& Ragdoll = *Owned.Handle;
            if (!IsRagdollReady(Ragdoll) || Ragdoll.Bodies.empty() || !Registry.IsValid(Owned.Owner))
            {
                continue;
            }

            const SRagdollComponent* Component = Registry.TryGet<SRagdollComponent>(Owned.Owner);
            if (Component == nullptr || Component->State != ERagdollState::Powered)
            {
                if (Ragdoll.bMusclesLive)
                {
                    SlackenRagdollMuscles(Ragdoll);
                }
                continue;
            }

            DrivePoweredRagdoll(Owned.Owner, Ragdoll, *Component, Registry.TryGet<SRagdollLocomotionComponent>(Owned.Owner), FixedDt);
        }
    }
}
