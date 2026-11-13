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

        FVector3 RagdollPerpendicular(const FVector3& Axis)
        {
            const FVector3 Reference = Math::Abs(Axis.y) < 0.99f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
            return Math::Normalize(Math::Cross(Reference, Axis));
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
            float           TwistDeg = 30.0f;
            float           Swing1Deg = 45.0f;
            float           Swing2Deg = 45.0f;
            float           MotorFrequency = 0.0f;
            float           MotorDamping = 0.0f;
            float           MotorStrength = 1.0f;
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
                        const b3HullData* Hull = GetOrCreateBoxHull(Setup.HalfExtent);
                        if (Hull == nullptr)
                        {
                            continue;
                        }
                        Def.Shape = MakeHullShape(Hull, Setup.TranslationOffset, OffsetRot);
                        break;
                    }
                    case ERagdollBodyShape::Sphere:
                    {
                        Def.Shape = MakeSphereShape(Setup.TranslationOffset, Setup.Radius);
                        break;
                    }
                    default:
                    {
                        Def.Shape = MakeCapsuleShape(Setup.TranslationOffset, OffsetRot, Setup.Radius, Setup.HalfHeight);
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
                    const float Length = Math::Length(DirC);
                    if (Length > 1e-4f)
                    {
                        const FVector3 DirLocal = Math::Normalize(Math::Inverse(BoneRotC) * DirC);
                        const float Radius = Math::Clamp(Length * 0.12f, 0.02f, 0.1f);
                        const float HalfHeight = Math::Max(Length * 0.5f - Radius, 0.01f);

                        Def.Shape = MakeCapsuleShape(DirLocal * (Length * 0.5f), RagdollYToDir(DirLocal), Radius, HalfHeight);
                        bBuilt = true;
                    }
                }

                if (!bBuilt)
                {
                    Def.Shape = MakeSphereShape(FVector3(0.0f), 0.03f);
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

        for (int32 DefIndex = 0; DefIndex < Defs.Num(); ++DefIndex)
        {
            const FRagdollBodyDef& Def = Defs[DefIndex];
            if (Def.ParentBodyIndex < 0 || DefIndexToBodyIndex[DefIndex] < 0 || DefIndexToBodyIndex[Def.ParentBodyIndex] < 0)
            {
                continue;
            }

            const FRagdollBodyDef& ParentDef = Defs[Def.ParentBodyIndex];

            // The limit cone is centered on the way the limb itself points at rest, toward its children, so a hanging arm rests inside it.
            FVector3 TwistAxis(0.0f);
            for (int32 ChildBone : Skeleton->GetChildBones(Def.BoneIndex))
            {
                FVector3 ChildPos;
                FQuat ChildRot;
                DecomposeWorld(ChildBone, ChildPos, ChildRot);
                TwistAxis += ChildPos - Def.WorldPos;
            }
            if (Math::Length(TwistAxis) <= 1e-4f)
            {
                TwistAxis = Def.WorldPos - ParentDef.WorldPos;
            }
            TwistAxis = Math::Length(TwistAxis) > 1e-4f
                ? Math::Normalize(TwistAxis)
                : Math::Normalize(Def.WorldRot * FVector3(0.0f, 1.0f, 0.0f));

            float Twist = Def.TwistDeg;
            float Swing1 = Def.Swing1Deg;
            float Swing2 = Def.Swing2Deg;

            if (!Request.Constraints.IsEmpty())
            {
                const FName ChildBone = Skeleton->GetBone(Def.BoneIndex).Name;
                for (const SPhysicsConstraintSetup& C : Request.Constraints)
                {
                    if (C.ChildBone == ChildBone)
                    {
                        Twist = C.TwistLimitDegrees;
                        Swing1 = C.Swing1LimitDegrees;
                        Swing2 = C.Swing2LimitDegrees;
                        Defs[DefIndex].MotorFrequency = C.MotorFrequency;
                        Defs[DefIndex].MotorDamping = C.MotorDamping;
                        Defs[DefIndex].MotorStrength = C.MotorStrength;
                        break;
                    }
                }
            }

            const b3BodyId ChildBody = Handle->Bodies[DefIndexToBodyIndex[DefIndex]];
            const b3BodyId ParentBody = Handle->Bodies[DefIndexToBodyIndex[Def.ParentBodyIndex]];

            // Box3D centers the swing cone and the twist on the frame's Z axis, so Z is aimed along the limb.
            const FVector3 Perp = RagdollPerpendicular(TwistAxis);
            const FVector3 Bitangent = Math::Cross(TwistAxis, Perp);
            const FMatrix4 Basis(FVector4(Perp.x, Perp.y, Perp.z, 0.0f),
                                 FVector4(Bitangent.x, Bitangent.y, Bitangent.z, 0.0f),
                                 FVector4(TwistAxis.x, TwistAxis.y, TwistAxis.z, 0.0f),
                                 FVector4(0.0f, 0.0f, 0.0f, 1.0f));
            const FQuat FrameRotation = Math::ToQuat(Basis);

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
            JointDef.coneAngle = Math::Radians(Math::Max(Swing1, Swing2));
            JointDef.enableTwistLimit = true;
            JointDef.lowerTwistAngle = -Math::Radians(Twist);
            JointDef.upperTwistAngle = Math::Radians(Twist);

            const b3JointId JointId = b3CreateSphericalJoint(WorldId, &JointDef);
            if (b3Joint_IsValid(JointId))
            {
                Handle->Joints.Add(JointId);
            }
        }

        const int32 NumBodies = Handle->Bodies.Num();
        Handle->ParentBody.SetNum(NumBodies, -1);
        TVector<float> SubtreeMass;
        SubtreeMass.SetNum(NumBodies, 0.0f);
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
            SubtreeMass[BodyIndex] = b3Body_IsValid(Handle->Bodies[BodyIndex]) ? b3Body_GetMass(Handle->Bodies[BodyIndex]) : 0.0f;
            TotalMass += SubtreeMass[BodyIndex];
        }
        for (int32 i = NumBodies - 1; i >= 0; --i)
        {
            if (Handle->ParentBody[i] >= 0)
            {
                SubtreeMass[Handle->ParentBody[i]] += SubtreeMass[i];
            }
        }

        // Every jointed body gets a muscle, left slack until the ragdoll is powered so a limp ragdoll behaves as it always did.
        for (int32 DefIndex = 0; DefIndex < Defs.Num(); ++DefIndex)
        {
            const int32 BodyIndex = DefIndexToBodyIndex[DefIndex];
            if (BodyIndex < 0)
            {
                continue;
            }

            const int32 Parent = Handle->ParentBody[BodyIndex];
            if (Parent < 0)
            {
                continue;
            }

            const int32 ParentDefIndex = Defs[DefIndex].ParentBodyIndex;
            const b3BodyId ChildBody = Handle->Bodies[BodyIndex];
            const b3BodyId ParentBody = Handle->Bodies[Parent];
            const FQuat RestRelative = Math::Normalize(Math::Inverse(Defs[ParentDefIndex].WorldRot) * Defs[DefIndex].WorldRot);

            b3MotorJointDef MuscleDef = b3DefaultMotorJointDef();
            MuscleDef.base.bodyIdA = ParentBody;
            MuscleDef.base.bodyIdB = ChildBody;
            MuscleDef.base.localFrameA = b3Transform{ b3Body_GetLocalPoint(ParentBody, Box3DUtils::ToB3Vec3(Defs[DefIndex].WorldPos)), b3Quat_identity };
            MuscleDef.base.localFrameB = b3Transform{ b3Vec3_zero, Box3DUtils::ToB3Quat(Math::Inverse(RestRelative)) };
            MuscleDef.base.collideConnected = false;
            MuscleDef.linearVelocity = b3Vec3_zero;
            MuscleDef.maxVelocityForce = 0.0f;
            MuscleDef.angularVelocity = b3Vec3_zero;
            MuscleDef.maxVelocityTorque = 0.0f;
            MuscleDef.linearHertz = 0.0f;
            MuscleDef.linearDampingRatio = 0.0f;
            MuscleDef.maxSpringForce = 0.0f;
            MuscleDef.angularHertz = 0.0f;
            MuscleDef.angularDampingRatio = 1.0f;
            MuscleDef.maxSpringTorque = 0.0f;

            const b3JointId MuscleId = b3CreateMotorJoint(WorldId, &MuscleDef);
            if (!b3Joint_IsValid(MuscleId))
            {
                continue;
            }
            Handle->Joints.Add(MuscleId);

            FRagdollMuscle& Muscle = Handle->Muscles.EmplaceGetRef();
            Muscle.Joint = MuscleId;
            Muscle.Body = BodyIndex;
            Muscle.ParentBody = Parent;
            Muscle.TorqueScale = Math::Clamp(kMuscleShareGain * Math::Sqrt(SubtreeMass[BodyIndex] / Math::Max(TotalMass, 1e-3f)), kMinMuscleShare, 1.0f) * Defs[DefIndex].MotorStrength;
            Muscle.HertzOverride = Defs[DefIndex].MotorFrequency;
            Muscle.DampingOverride = Defs[DefIndex].MotorDamping;
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
