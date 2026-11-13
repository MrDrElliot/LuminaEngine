#include "RuntimePCH.h"
#include "World/Entity/Systems/SystemAccess.h"
#include "World/ECS/Registry.h"
#include "Box3DPhysicsScene.h"

#include <box3d/collision.h>

#include "Box3DInternal.h"
#include "Box3DRagdollHandle.h"
#include "Box3DUtils.h"

#include "Animation/Pose.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/World.h"

namespace Lumina::Physics
{
    namespace
    {
        // Rotation taking local +Y onto Dir, which is the axis a capsule is built along.
        FQuat RagdollYToDir(const FVector3& Dir)
        {
            const FVector3 Y(0.0f, 1.0f, 0.0f);
            const float D = Math::Clamp(Math::Dot(Y, Dir), -1.0f, 1.0f);
            if (D > 0.9999f)
            {
                return FQuat::Identity();
            }
            if (D < -0.9999f)
            {
                return Math::AngleAxis(Math::Pi<float>(), FVector3(0.0f, 0.0f, 1.0f));
            }
            return Math::AngleAxis(Math::Acos(D), Math::Normalize(Math::Cross(Y, Dir)));
        }

        // The largest axis of the entity's scale, the same measure physics sizes spheres and capsules by.
        float ComputeRagdollShapeScale(const FMatrix4& EntityToWorld)
        {
            FVector3 Position;
            FQuat Rotation;
            FVector3 Scale;
            AnimPose::DecomposeTRS(EntityToWorld, Position, Rotation, Scale);
            const float Largest = Math::Max(Math::Abs(Scale.x), Math::Max(Math::Abs(Scale.y), Math::Abs(Scale.z)));
            return Largest > 1e-4f ? Largest : 1.0f;
        }

        // Sets a joint's hard cone and twist and its ligament's spring from Setup, adding the ligament the first time the joint needs one.
        void ApplyRagdollJointLimit(FPhysicsRagdollHandle& Ragdoll, FRagdollJointLimit& Limit, const SPhysicsConstraintSetup& Setup)
        {
            // Box3D's cone is round, so it backstops the wider swing and the ligament holds the narrower one.
            b3SphericalJoint_SetConeLimit(Limit.Cone, Math::Radians(Math::Max(Setup.Swing1LimitDegrees, Setup.Swing2LimitDegrees)));
            b3SphericalJoint_SetTwistLimits(Limit.Cone, -Math::Radians(Setup.TwistLimitDegrees), Math::Radians(Setup.TwistLimitDegrees));

            Limit.LimitsRadians = Setup.GetLimitsRadians();
            Limit.SoftStart = Setup.GetSoftLimitStart();
            Limit.LimitHertz = Setup.LimitStiffness * Limit.Retune;
            Limit.MaxTorque = Setup.MaxLimitTorque * Limit.TorqueShare;

            // With soft limits off an elliptical cone still needs the ligament, held at the limits themselves.
            if (!b3Joint_IsValid(Limit.Ligament) && (Setup.bSoftLimits || Setup.Swing1LimitDegrees != Setup.Swing2LimitDegrees))
            {
                Limit.Ligament = Ragdoll.AddSlackMotor(Ragdoll.Bodies[Limit.ParentBody], Ragdoll.Bodies[Limit.Body], b3Joint_GetLocalFrameA(Limit.Cone), b3Joint_GetLocalFrameB(Limit.Cone));
            }
            if (b3Joint_IsValid(Limit.Ligament))
            {
                b3MotorJoint_SetAngularDampingRatio(Limit.Ligament, Setup.LimitDampingRatio * Limit.Retune);
            }
            b3Joint_WakeBodies(Limit.Cone);
        }

        struct FRagdollBodyDef
        {
            int32           BoneIndex = Constants::kIndexNone;
            int32           ParentBodyIndex = -1;
            FVector3        WorldPos;
            FQuat           WorldRot;
            FPendingShape   Shape;
            bool            bOverrideMass = false;
            float           Mass = 1.0f;
        };

        // A joint's muscle gets a share of the strongest by the mass hanging off it, so a neck is weaker than a hip.
        constexpr float kMinMuscleShare = 0.2f;
        constexpr float kMuscleShareGain = 1.4f;
    }

    TSharedPtr<FPhysicsRagdollHandle> FBox3DPhysicsScene::CreateRagdoll(const FRagdollDesc& Desc)
    {
        ValidateNotInParallelSystem("CreateRagdoll");
        if (Desc.Skeleton == nullptr || Desc.ComponentBoneGlobals == nullptr
            || Desc.Skeleton->GetNumBones() == 0 || Desc.ComponentBoneGlobals->Num() != Desc.Skeleton->Bones.Num())
        {
            return nullptr;
        }
        FPendingRagdoll Request;
        Request.Description = Desc;
        Request.Description.Asset = nullptr;
        Request.Description.Skeleton = nullptr;
        Request.Description.ComponentBoneGlobals = nullptr;
        Request.Skeleton = MakeShared<FSkeletonResource>();
        Request.Skeleton->Bones = Desc.Skeleton->Bones;
        Request.Skeleton->BoneNameToIndex = Desc.Skeleton->BoneNameToIndex;
        Request.Globals = *Desc.ComponentBoneGlobals;
        if (Desc.Asset)
        {
            Request.Bodies = Desc.Asset->Bodies;
            Request.Constraints = Desc.Asset->Constraints;
            Request.Description.FallbackProfile = Desc.Asset->CollisionProfile;
        }
        Request.Handle = MakeShared<FPhysicsRagdollHandle>();
        auto Handle = Request.Handle;
        OwnedRagdolls.Emplace(Desc.Entity, Handle);
        PendingRagdolls.Add(Move(Request));
        return Handle;
    }

    TSharedPtr<FPhysicsRagdollHandle> FBox3DPhysicsScene::CommitRagdoll(const FPendingRagdoll& Request)
    {
        LUMINA_PROFILE_SCOPE();

        const FRagdollDesc& Desc = Request.Description;
        const FSkeletonResource* Skeleton = Request.Skeleton.get();
        if (Skeleton == nullptr)
        {
            return nullptr;
        }

        const TVector<FMatrix4>& Globals = Request.Globals;
        const int32 NumBones = Skeleton->GetNumBones();
        if (Globals.Num() != NumBones || NumBones == 0)
        {
            return nullptr;
        }

        const SCollisionProfile Profile = Desc.FallbackProfile;

        TVector<FRagdollBodyDef> Defs;
        THashMap<int32, int32> BoneToBody;

        auto DecomposeWorld = [&](int32 BoneIndex, FVector3& OutPos, FQuat& OutRot)
        {
            FVector3 Scale;
            AnimPose::DecomposeTRS(Desc.EntityToWorld * Globals[BoneIndex], OutPos, OutRot, Scale);
        };

        // Bodies are placed through the entity's scale, so shapes authored in skeleton units scale with it like other physics shapes.
        const float ShapeScale = ComputeRagdollShapeScale(Desc.EntityToWorld);

        if (!Request.Bodies.IsEmpty())
        {
            TVector<int32> Order;
            Order.Reserve(Request.Bodies.Num());
            for (int32 i = 0; i < Request.Bodies.Num(); ++i)
            {
                if (Skeleton->FindBoneIndex(Request.Bodies[i].BoneName) != Constants::kIndexNone)
                {
                    Order.Add(i);
                }
            }

            Algo::Sort(Order, [&](int32 A, int32 B)
            {
                return Skeleton->FindBoneIndex(Request.Bodies[A].BoneName) < Skeleton->FindBoneIndex(Request.Bodies[B].BoneName);
            });

            for (int32 SetupIdx : Order)
            {
                const SPhysicsBodySetup& Setup = Request.Bodies[SetupIdx];
                const int32 BoneIndex = Skeleton->FindBoneIndex(Setup.BoneName);

                FRagdollBodyDef Def;
                Def.BoneIndex = BoneIndex;
                Def.bOverrideMass = Setup.bOverrideMass;
                Def.Mass = Setup.Mass;
                DecomposeWorld(BoneIndex, Def.WorldPos, Def.WorldRot);

                const FQuat OffsetRot(Math::Radians(Setup.RotationOffset));

                switch (Setup.Shape)
                {
                    case ERagdollBodyShape::Box:
                    {
                        const b3HullData* Hull = GetOrCreateBoxHull(Setup.HalfExtent * ShapeScale);
                        if (Hull == nullptr)
                        {
                            continue;
                        }
                        Def.Shape = MakeHullShape(Hull, Setup.TranslationOffset * ShapeScale, OffsetRot);
                        break;
                    }
                    case ERagdollBodyShape::Sphere:
                    {
                        Def.Shape = MakeSphereShape(Setup.TranslationOffset * ShapeScale, Setup.Radius * ShapeScale);
                        break;
                    }
                    default:
                    {
                        Def.Shape = MakeCapsuleShape(Setup.TranslationOffset * ShapeScale, OffsetRot, Setup.Radius * ShapeScale, Setup.HalfHeight * ShapeScale);
                        break;
                    }
                }

                BoneToBody[BoneIndex] = Defs.Num();
                Defs.Add(Move(Def));
            }
        }
        else
        {
            for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
            {
                FRagdollBodyDef Def;
                Def.BoneIndex = BoneIndex;
                DecomposeWorld(BoneIndex, Def.WorldPos, Def.WorldRot);

                const FVector3 BonePos(Globals[BoneIndex][3]);
                FQuat BoneRotC;
                FVector3 BonePosC, BoneScaleC;
                AnimPose::DecomposeTRS(Globals[BoneIndex], BonePosC, BoneRotC, BoneScaleC);

                const TVector<int32> Children = Skeleton->GetChildBones(BoneIndex);
                bool bBuilt = false;

                if (!Children.IsEmpty())
                {
                    FVector3 ChildAvg(0.0f);
                    for (int32 C : Children)
                    {
                        ChildAvg += FVector3(Globals[C][3]);
                    }
                    ChildAvg /= (float)Children.Num();

                    const FVector3 DirC = ChildAvg - BonePos;
                    const float SkeletonLength = Math::Length(DirC);
                    if (SkeletonLength > 1e-4f)
                    {
                        const FVector3 DirLocal = Math::Normalize(Math::Inverse(BoneRotC) * DirC);
                        const float Length = SkeletonLength * ShapeScale;
                        const float Radius = Math::Clamp(SkeletonLength * 0.12f, 0.02f, 0.1f) * ShapeScale;
                        const float HalfHeight = Math::Max(Length * 0.5f - Radius, 0.01f * ShapeScale);

                        Def.Shape = MakeCapsuleShape(DirLocal * (Length * 0.5f), RagdollYToDir(DirLocal), Radius, HalfHeight);
                        bBuilt = true;
                    }
                }

                if (!bBuilt)
                {
                    Def.Shape = MakeSphereShape(FVector3(0.0f), 0.03f * ShapeScale);
                }

                BoneToBody[BoneIndex] = Defs.Num();
                Defs.Add(Move(Def));
            }
        }

        if (Defs.IsEmpty())
        {
            return nullptr;
        }

        for (FRagdollBodyDef& Def : Defs)
        {
            int32 Parent = Skeleton->GetBone(Def.BoneIndex).ParentIndex;
            while (Parent >= 0)
            {
                auto It = BoneToBody.Find(Parent);
                if (It != nullptr)
                {
                    Def.ParentBodyIndex = (*It);
                    break;
                }
                Parent = Skeleton->GetBone(Parent).ParentIndex;
            }
        }

        TSharedPtr<FPhysicsRagdollHandle> Handle = Request.Handle;
        if (!HasBodyCapacity(static_cast<uint32>(Defs.Num()))) { return nullptr; }
        Handle->WorldId = WorldId;
        Handle->Bodies.Reserve(Defs.Num());
        Handle->BodyHandles.Reserve(Defs.Num());
        Handle->JointToBone.Reserve(Defs.Num());

        b3ShapeDef ShapeDef = b3DefaultShapeDef();
        ShapeDef.filter = Box3DUtils::MakeShapeFilter(Profile);
        ShapeDef.userData = Box3DUtils::PackProfileUserData(Profile);
        ShapeDef.enableCustomFiltering = Box3DUtils::UsesPermissiveCollisionFilter();
        ShapeDef.enableContactEvents = true;
        ShapeDef.updateBodyMass = false;

        TVector<int32> DefIndexToBodyIndex;
        DefIndexToBodyIndex.SetNum(Defs.Num(), -1);

        for (int32 DefIndex = 0; DefIndex < Defs.Num(); ++DefIndex)
        {
            const FRagdollBodyDef& Def = Defs[DefIndex];

            b3BodyDef BodyDef = b3DefaultBodyDef();
            BodyDef.type = b3_dynamicBody;
            BodyDef.position = Box3DUtils::ToB3Vec3(Def.WorldPos);
            BodyDef.rotation = Box3DUtils::ToB3Quat(Math::Normalize(Def.WorldRot));

            const b3BodyId BodyId = b3CreateBody(WorldId, &BodyDef);
            if (!b3Body_IsValid(BodyId))
            {
                continue;
            }

            const uint32 BodyHandle = RegisterBody(BodyId);
            b3Body_SetUserData(BodyId, PackBodyUserData(Desc.Entity, BodyHandle));

            switch (Def.Shape.Type)
            {
                case b3_sphereShape:  b3CreateSphereShape(BodyId, &ShapeDef, &Def.Shape.Sphere); break;
                case b3_capsuleShape: b3CreateCapsuleShape(BodyId, &ShapeDef, &Def.Shape.Capsule); break;
                default:              b3CreateTransformedHullShape(BodyId, &ShapeDef, Def.Shape.Hull, Def.Shape.Transform, Def.Shape.Scale); break;
            }

            b3Body_ApplyMassFromShapes(BodyId);

            if (Def.bOverrideMass)
            {
                b3MassData MassData = b3Body_GetMassData(BodyId);
                const float Ratio = MassData.mass > 0.0f ? Def.Mass / MassData.mass : 1.0f;
                MassData.mass = Def.Mass;
                MassData.inertia.cx = b3MulSV(Ratio, MassData.inertia.cx);
                MassData.inertia.cy = b3MulSV(Ratio, MassData.inertia.cy);
                MassData.inertia.cz = b3MulSV(Ratio, MassData.inertia.cz);
                b3Body_SetMassData(BodyId, MassData);
            }

            DefIndexToBodyIndex[DefIndex] = Handle->Bodies.Num();
            Handle->Bodies.Add(BodyId);
            Handle->BodyHandles.Add(BodyHandle);
            Handle->JointToBone.Add(Def.BoneIndex);
        }

        if (Handle->Bodies.IsEmpty())
        {
            return nullptr;
        }

        const int32 NumBodies = Handle->Bodies.Num();
        Handle->ParentBody.SetNum(NumBodies, -1);
        TVector<float> SubtreeMass;
        SubtreeMass.SetNum(NumBodies, 0.0f);
        TVector<FVector3> SubtreeMoment;
        SubtreeMoment.SetNum(NumBodies, FVector3(0.0f));
        float TotalMass = 0.0f;
        for (int32 DefIndex = 0; DefIndex < Defs.Num(); ++DefIndex)
        {
            const int32 BodyIndex = DefIndexToBodyIndex[DefIndex];
            if (BodyIndex < 0)
            {
                continue;
            }

            const int32 ParentDefIndex = Defs[DefIndex].ParentBodyIndex;
            Handle->ParentBody[BodyIndex] = (ParentDefIndex >= 0 && DefIndexToBodyIndex[ParentDefIndex] >= 0)
                ? DefIndexToBodyIndex[ParentDefIndex] : -1;
            const b3BodyId Body = Handle->Bodies[BodyIndex];
            if (b3Body_IsValid(Body))
            {
                SubtreeMass[BodyIndex] = b3Body_GetMass(Body);
                SubtreeMoment[BodyIndex] = Box3DUtils::FromB3Vec3(b3Body_GetWorldCenter(Body)) * SubtreeMass[BodyIndex];
                TotalMass += SubtreeMass[BodyIndex];
            }
        }
        for (int32 i = NumBodies - 1; i >= 0; --i)
        {
            if (Handle->ParentBody[i] >= 0)
            {
                SubtreeMass[Handle->ParentBody[i]] += SubtreeMass[i];
                SubtreeMoment[Handle->ParentBody[i]] += SubtreeMoment[i];
            }
        }

        // Every jointed body gets its hard limits, a muscle left slack until the ragdoll is powered, and a ligament for its soft limits.
        for (int32 DefIndex = 0; DefIndex < Defs.Num(); ++DefIndex)
        {
            const int32 BodyIndex = DefIndexToBodyIndex[DefIndex];
            if (BodyIndex < 0 || Handle->ParentBody[BodyIndex] < 0)
            {
                continue;
            }

            const FRagdollBodyDef& Def = Defs[DefIndex];
            const FRagdollBodyDef& ParentDef = Defs[Def.ParentBodyIndex];
            const int32 Parent = Handle->ParentBody[BodyIndex];
            const b3BodyId ChildBody = Handle->Bodies[BodyIndex];
            const b3BodyId ParentBody = Handle->Bodies[Parent];

            // The limit cone is centered on the way the limb itself points at rest, toward its children, so a hanging arm rests inside it.
            FVector3 TowardChildren(0.0f);
            for (int32 ChildBone : Skeleton->GetChildBones(Def.BoneIndex))
            {
                FVector3 ChildPos;
                FQuat ChildRot;
                DecomposeWorld(ChildBone, ChildPos, ChildRot);
                TowardChildren += ChildPos - Def.WorldPos;
            }

            // A bone the asset has no constraint for, and every bone of a generated ragdoll, gets the defaults.
            const SPhysicsConstraintSetup Setup = FindConstraintSetup(Request.Constraints, Skeleton->GetBone(Def.BoneIndex).Name);

            // Box3D centers the swing cone and the twist on the frame's Z axis, so Z is aimed along the limb.
            const FQuat FrameRotation = MakeRagdollJointFrame(TowardChildren, Def.WorldPos - ParentDef.WorldPos, Def.WorldRot * FVector3(0.0f, 1.0f, 0.0f));

            auto MakeFrame = [&](b3BodyId Body)
            {
                const b3Vec3 LocalPoint = b3Body_GetLocalPoint(Body, Box3DUtils::ToB3Vec3(Def.WorldPos));
                const FQuat LocalRotation = Math::Normalize(Math::Inverse(Box3DUtils::FromB3Quat(b3Body_GetRotation(Body))) * FrameRotation);
                return b3Transform{ LocalPoint, Box3DUtils::ToB3Quat(LocalRotation) };
            };

            b3SphericalJointDef JointDef = b3DefaultSphericalJointDef();
            JointDef.base.bodyIdA = ParentBody;
            JointDef.base.bodyIdB = ChildBody;
            JointDef.base.localFrameA = MakeFrame(ParentBody);
            JointDef.base.localFrameB = MakeFrame(ChildBody);
            JointDef.base.collideConnected = false;
            JointDef.enableConeLimit = true;
            JointDef.enableTwistLimit = true;

            const b3JointId JointId = b3CreateSphericalJoint(WorldId, &JointDef);
            const float Share = Math::Clamp(kMuscleShareGain * Math::Sqrt(SubtreeMass[BodyIndex] / Math::Max(TotalMass, 1e-3f)), kMinMuscleShare, 1.0f);
            const FQuat RestRelative = Math::Normalize(Math::Inverse(ParentDef.WorldRot) * Def.WorldRot);
            const b3JointId MuscleId = Handle->AddSlackMotor(ParentBody, ChildBody,
                b3Transform{ b3Body_GetLocalPoint(ParentBody, Box3DUtils::ToB3Vec3(Def.WorldPos)), b3Quat_identity },
                b3Transform{ b3Vec3_zero, Box3DUtils::ToB3Quat(Math::Inverse(RestRelative)) });
            if (b3Joint_IsValid(MuscleId))
            {
                Handle->Muscles.Emplace(MuscleId, BodyIndex, Parent, Share * Setup.MotorStrength, Setup.MotorFrequency, Setup.MotorDamping);
            }

            if (!b3Joint_IsValid(JointId))
            {
                continue;
            }
            Handle->Joints.Add(JointId);

            // Box3D springs a joint against the two bodies' own inertia, so the ligament is retuned to the limb swinging about its pivot.
            const float ChildInertia = Math::Max(MeanRotationalInertia(ChildBody), 1e-6f);
            const float PairInertia = 1.0f / (1.0f / ChildInertia + 1.0f / Math::Max(MeanRotationalInertia(ParentBody), 1e-6f));
            const FVector3 SubtreeCenter = SubtreeMoment[BodyIndex] / Math::Max(SubtreeMass[BodyIndex], 1e-6f);
            const float SwungInertia = ChildInertia + SubtreeMass[BodyIndex] * Math::LengthSquared(SubtreeCenter - Def.WorldPos);

            FRagdollJointLimit& Limit = Handle->JointLimits.EmplaceGetRef();
            Limit.Cone        = JointId;
            Limit.Body        = BodyIndex;
            Limit.ParentBody  = Parent;
            Limit.ParentFrame = Box3DUtils::FromB3Quat(JointDef.base.localFrameA.q);
            Limit.ChildFrame  = Box3DUtils::FromB3Quat(JointDef.base.localFrameB.q);
            Limit.Retune      = Math::Sqrt(SwungInertia / PairInertia);
            Limit.TorqueShare = Share;
            ApplyRagdollJointLimit(*Handle, Limit, Setup);
        }

        // Limbs that are not directly jointed still must not collide, which is what a filter joint is for.
        for (int32 i = 0; i < Handle->Bodies.Num(); ++i)
        {
            for (int32 j = i + 1; j < Handle->Bodies.Num(); ++j)
            {
                b3FilterJointDef FilterDef = b3DefaultFilterJointDef();
                FilterDef.base.bodyIdA = Handle->Bodies[i];
                FilterDef.base.bodyIdB = Handle->Bodies[j];

                const b3JointId FilterId = b3CreateFilterJoint(WorldId, &FilterDef);
                if (b3Joint_IsValid(FilterId))
                {
                    Handle->Joints.Add(FilterId);
                }
            }
        }

        Handle->Skeleton = Request.Skeleton;
        Handle->bAddedToScene = true;
        SimulatingRagdolls.FindOrAdd(Desc.Entity) = Handle.get();
        return Handle;
    }

    const FPhysicsRagdollHandle* FBox3DPhysicsScene::FindSimulatingRagdoll(ECS::FEntity Entity) const
    {
        FPhysicsRagdollHandle* const* Ragdoll = SimulatingRagdolls.Find(Entity);
        return Ragdoll != nullptr && IsRagdollReady(**Ragdoll) ? *Ragdoll : nullptr;
    }

    void FBox3DPhysicsScene::RetuneRagdollLimits(ECS::FEntity Entity, const CPhysicsAsset& Asset)
    {
        ValidateNotInParallelSystem("RetuneRagdollLimits");
        FPhysicsRagdollHandle* Ragdoll = const_cast<FPhysicsRagdollHandle*>(FindSimulatingRagdoll(Entity));
        if (Ragdoll == nullptr || Ragdoll->Skeleton == nullptr)
        {
            return;
        }

        for (FRagdollJointLimit& Limit : Ragdoll->JointLimits)
        {
            const FName ChildBone = Ragdoll->Skeleton->GetBone(Ragdoll->JointToBone[Limit.Body]).Name;
            ApplyRagdollJointLimit(*Ragdoll, Limit, FindConstraintSetup(Asset.Constraints, ChildBone));
        }
    }

    FBox3DPhysicsScene::FRagdollMotion FBox3DPhysicsScene::MeasureRagdoll(const FPhysicsRagdollHandle& Ragdoll) const
    {
        FRagdollMotion Motion;
        for (b3BodyId Body : Ragdoll.Bodies)
        {
            if (!b3Body_IsValid(Body))
            {
                continue;
            }
            const float Mass = b3Body_GetMass(Body);
            Motion.Mass += Mass;
            Motion.CenterOfMass += Box3DUtils::FromB3Vec3(b3Body_GetWorldCenter(Body)) * Mass;
            Motion.LinearVelocity += Box3DUtils::FromB3Vec3(b3Body_GetLinearVelocity(Body)) * Mass;
            Motion.AngularVelocity += Box3DUtils::FromB3Vec3(b3Body_GetAngularVelocity(Body)) * Mass;
        }
        if (Motion.Mass > 0.0f)
        {
            Motion.CenterOfMass /= Motion.Mass;
            Motion.LinearVelocity /= Motion.Mass;
            Motion.AngularVelocity /= Motion.Mass;
        }
        return Motion;
    }

    SPhysicsBodyTarget FBox3DPhysicsScene::GetRagdollBoneTarget(ECS::FEntity Entity, FName Bone) const
    {
        const FPhysicsRagdollHandle* Ragdoll = FindSimulatingRagdoll(Entity);
        if (Ragdoll == nullptr || Ragdoll->Skeleton == nullptr)
        {
            return {};
        }

        // A bone too small for a body, such as a finger, moves with the nearest ancestor that has one.
        for (int32 BoneIndex = Ragdoll->Skeleton->FindBoneIndex(Bone); Ragdoll->Skeleton->IsBoneIndexValid(BoneIndex);
             BoneIndex = Ragdoll->Skeleton->GetBone(BoneIndex).ParentIndex)
        {
            for (int32 Part = 0; Part < Ragdoll->JointToBone.Num() && Part < Ragdoll->BodyHandles.Num(); ++Part)
            {
                if (Ragdoll->JointToBone[Part] == BoneIndex)
                {
                    return MakeBodyTarget(Ragdoll->BodyHandles[Part]);
                }
            }
        }
        return {};
    }

    bool FBox3DPhysicsScene::IsRagdollReady(const FPhysicsRagdollHandle& Handle) const
    {
        return Handle.bAddedToScene && !Handle.bPendingDestroy;
    }

    void FBox3DPhysicsScene::ReadRagdollPose(const FPhysicsRagdollHandle& Handle, const FMatrix4& WorldToEntity,
                                             const FSkeletonResource* Skeleton, TVector<FMatrix4>& OutBoneTransforms)
    {
        LUMINA_PROFILE_SCOPE();

        if (Handle.Bodies.IsEmpty() || Skeleton == nullptr)
        {
            return;
        }

        const int32 NumBones = Skeleton->GetNumBones();
        OutBoneTransforms.SetNum(NumBones);

        // OutBoneTransforms holds the component globals until the last pass applies the inverse bind.
        TVector<uint8>& Mapped = Handle.PoseBoneMapped;
        Mapped.Init(0, NumBones);

        for (int32 j = 0; j < Handle.JointToBone.Num(); ++j)
        {
            const int32 BoneIndex = Handle.JointToBone[j];
            if (!Skeleton->IsBoneIndexValid(BoneIndex) || j >= Handle.Bodies.Num())
            {
                continue;
            }

            const b3BodyId BodyId = Handle.Bodies[j];
            if (!b3Body_IsValid(BodyId))
            {
                continue;
            }

            const FVector3 Pos = Box3DUtils::FromB3Vec3(b3Body_GetPosition(BodyId));
            const FQuat Rot = Box3DUtils::FromB3Quat(b3Body_GetRotation(BodyId));
            OutBoneTransforms[BoneIndex] = WorldToEntity * AnimPose::ComposeTRS(Pos, Rot, FVector3(1.0f));
            Mapped[BoneIndex] = 1;
        }

        // Parents precede children in the bone array, so a single forward pass resolves them.
        for (int32 i = 0; i < NumBones; ++i)
        {
            if (!Mapped[i])
            {
                const int32 Parent = Skeleton->GetBone(i).ParentIndex;
                const FMatrix4& Local = Skeleton->GetBone(i).LocalTransform;
                OutBoneTransforms[i] = Parent >= 0 ? OutBoneTransforms[Parent] * Local : Local;
            }
        }
        for (int32 i = 0; i < NumBones; ++i)
        {
            OutBoneTransforms[i] = OutBoneTransforms[i] * Skeleton->GetBone(i).InvBindMatrix;
        }
    }

    void FBox3DPhysicsScene::DestroyRagdoll(const TSharedPtr<FPhysicsRagdollHandle>& Handle)
    {
        ValidateNotInParallelSystem("DestroyRagdoll");
        if (!Handle)
        {
            return;
        }

        Handle->bPendingDestroy = true;
    }

    void FBox3DPhysicsScene::GetRagdollRootTransform(const FPhysicsRagdollHandle& Handle, FVector3& OutPosition, FQuat& OutRotation)
    {
        OutPosition = FVector3(0.0f);
        OutRotation = FQuat::Identity();

        if (Handle.Bodies.IsEmpty() || !b3Body_IsValid(Handle.Bodies[0]))
        {
            return;
        }

        // Body 0 is the ragdoll root, the lowest bodied bone with no bodied ancestor.
        OutPosition = Box3DUtils::FromB3Vec3(b3Body_GetPosition(Handle.Bodies[0]));
        OutRotation = Box3DUtils::FromB3Quat(b3Body_GetRotation(Handle.Bodies[0]));
    }

    void FBox3DPhysicsScene::ApplyBuoyancy(b3BodyId BodyId, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
        float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float DeltaTime)
    {
        if (!b3Body_IsValid(BodyId) || b3Body_GetType(BodyId) != b3_dynamicBody)
        {
            return;
        }

        const float Mass = b3Body_GetMass(BodyId);
        if (Mass <= 0.0f)
        {
            return;
        }

        // Box3D has no submerged-volume solver, so the fraction comes from the body bounds against the plane.
        const b3AABB Bounds = b3Body_ComputeAABB(BodyId);
        const FVector3 Min = Box3DUtils::FromB3Vec3(Bounds.lowerBound);
        const FVector3 Max = Box3DUtils::FromB3Vec3(Bounds.upperBound);

        const FVector3 Normal = Math::LengthSquared(SurfaceNormal) > Math::kSmallNumber
            ? Math::Normalize(SurfaceNormal)
            : FVector3(0.0f, 1.0f, 0.0f);

        const FVector3 Center = (Min + Max) * 0.5f;
        const FVector3 Extent = (Max - Min) * 0.5f;

        // Projected half-height of the bounds along the surface normal.
        const float Reach = Math::Abs(Extent.x * Normal.x) + Math::Abs(Extent.y * Normal.y) + Math::Abs(Extent.z * Normal.z);
        if (Reach <= Math::kSmallNumber)
        {
            return;
        }

        const float CenterDepth = Math::Dot(SurfacePosition - Center, Normal);
        const float Submerged = Math::Clamp((CenterDepth + Reach) / (2.0f * Reach), 0.0f, 1.0f);
        if (Submerged <= 0.0f)
        {
            return;
        }

        const b3Vec3 Gravity = b3World_GetGravity(WorldId);
        const FVector3 GravityVec = Box3DUtils::FromB3Vec3(Gravity);

        // Buoyancy is the fluid to body density ratio, so 1 is neutral and above 1 floats.
        const FVector3 Lift = -GravityVec * (Mass * Submerged * Buoyancy);
        b3Body_ApplyForceToCenter(BodyId, Box3DUtils::ToB3Vec3(Lift), true);

        const FVector3 LinearVelocity = Box3DUtils::FromB3Vec3(b3Body_GetLinearVelocity(BodyId));
        const FVector3 RelativeVelocity = LinearVelocity - FluidVelocity;
        const FVector3 DragImpulse = -RelativeVelocity * (Mass * Submerged * Math::Clamp(LinearDrag * DeltaTime, 0.0f, 1.0f));
        b3Body_ApplyLinearImpulseToCenter(BodyId, Box3DUtils::ToB3Vec3(DragImpulse), true);

        const FVector3 AngularVelocity = Box3DUtils::FromB3Vec3(b3Body_GetAngularVelocity(BodyId));
        const float AngularScale = Math::Clamp(AngularDrag * Submerged * DeltaTime, 0.0f, 1.0f);
        b3Body_SetAngularVelocity(BodyId, Box3DUtils::ToB3Vec3(AngularVelocity * (1.0f - AngularScale)));
    }
}
