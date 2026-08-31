#include "RuntimePCH.h"
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
        FQuat RagdollAxisAngle(const FVector3& Axis, float Angle)
        {
            const float Half = Angle * 0.5f;
            const FVector3 Scaled = Math::Normalize(Axis) * Math::Sin(Half);
            return FQuat(Math::Cos(Half), Scaled.x, Scaled.y, Scaled.z);
        }

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
                return RagdollAxisAngle(FVector3(0.0f, 0.0f, 1.0f), LE_PI_F);
            }
            return RagdollAxisAngle(Math::Normalize(Math::Cross(Y, Dir)), Math::Acos(D));
        }

        FVector3 RagdollPerpendicular(const FVector3& Axis)
        {
            const FVector3 Reference = Math::Abs(Axis.y) < 0.99f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
            return Math::Normalize(Math::Cross(Reference, Axis));
        }

        struct FRagdollBodyDef
        {
            int32           BoneIndex = INDEX_NONE;
            int32           ParentBodyIndex = -1;
            FVector3        WorldPos;
            FQuat           WorldRot;
            FPendingShape   Shape;
            bool            bOverrideMass = false;
            float           Mass = 1.0f;
            float           TwistDeg = 30.0f;
            float           Swing1Deg = 45.0f;
            float           Swing2Deg = 45.0f;
        };
    }

    TSharedPtr<FPhysicsRagdollHandle> FBox3DPhysicsScene::CreateRagdoll(const FRagdollDesc& Desc)
    {
        LUMINA_PROFILE_SCOPE();

        const FSkeletonResource* Skeleton = Desc.Skeleton;
        if (Skeleton == nullptr || Desc.ComponentBoneGlobals == nullptr)
        {
            return nullptr;
        }

        const TVector<FMatrix4>& Globals = *Desc.ComponentBoneGlobals;
        const int32 NumBones = Skeleton->GetNumBones();
        if ((int32)Globals.size() != NumBones || NumBones == 0)
        {
            return nullptr;
        }

        const FCollisionProfile Profile = Desc.Asset ? Desc.Asset->CollisionProfile : Desc.FallbackProfile;

        TVector<FRagdollBodyDef> Defs;
        THashMap<int32, int32> BoneToBody;

        auto DecomposeWorld = [&](int32 BoneIndex, FVector3& OutPos, FQuat& OutRot)
        {
            FVector3 Scale;
            AnimPose::DecomposeTRS(Desc.EntityToWorld * Globals[BoneIndex], OutPos, OutRot, Scale);
        };

        if (Desc.Asset && !Desc.Asset->Bodies.empty())
        {
            TVector<int32> Order;
            Order.reserve(Desc.Asset->Bodies.size());
            for (int32 i = 0; i < (int32)Desc.Asset->Bodies.size(); ++i)
            {
                if (Skeleton->FindBoneIndex(Desc.Asset->Bodies[i].BoneName) != INDEX_NONE)
                {
                    Order.push_back(i);
                }
            }

            Algo::Sort(Order, [&](int32 A, int32 B)
            {
                return Skeleton->FindBoneIndex(Desc.Asset->Bodies[A].BoneName) < Skeleton->FindBoneIndex(Desc.Asset->Bodies[B].BoneName);
            });

            for (int32 SetupIdx : Order)
            {
                const SPhysicsBodySetup& Setup = Desc.Asset->Bodies[SetupIdx];
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

                BoneToBody[BoneIndex] = (int32)Defs.size();
                Defs.push_back(Move(Def));
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

                if (!Children.empty())
                {
                    FVector3 ChildAvg(0.0f);
                    for (int32 C : Children)
                    {
                        ChildAvg += FVector3(Globals[C][3]);
                    }
                    ChildAvg /= (float)Children.size();

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

                BoneToBody[BoneIndex] = (int32)Defs.size();
                Defs.push_back(Move(Def));
            }
        }

        if (Defs.empty())
        {
            return nullptr;
        }

        for (FRagdollBodyDef& Def : Defs)
        {
            int32 Parent = Skeleton->GetBone(Def.BoneIndex).ParentIndex;
            while (Parent >= 0)
            {
                auto It = BoneToBody.find(Parent);
                if (It != BoneToBody.end())
                {
                    Def.ParentBodyIndex = It->second;
                    break;
                }
                Parent = Skeleton->GetBone(Parent).ParentIndex;
            }
        }

        TSharedPtr<FPhysicsRagdollHandle> Handle = MakeShared<FPhysicsRagdollHandle>();
        Handle->WorldId = WorldId;
        Handle->Bodies.reserve(Defs.size());
        Handle->BodyHandles.reserve(Defs.size());
        Handle->JointToBone.reserve(Defs.size());

        b3ShapeDef ShapeDef = b3DefaultShapeDef();
        ShapeDef.filter = Box3DUtils::MakeShapeFilter(Profile);
        ShapeDef.userData = Box3DUtils::PackProfileUserData(Profile);
        ShapeDef.enableCustomFiltering = Box3DUtils::UsesPermissiveCollisionFilter();
        ShapeDef.enableContactEvents = true;
        ShapeDef.updateBodyMass = false;

        for (const FRagdollBodyDef& Def : Defs)
        {
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

            Handle->Bodies.push_back(BodyId);
            Handle->BodyHandles.push_back(BodyHandle);
            Handle->JointToBone.push_back(Def.BoneIndex);
        }

        if (Handle->Bodies.empty())
        {
            return nullptr;
        }

        for (int32 i = 0; i < (int32)Defs.size(); ++i)
        {
            const FRagdollBodyDef& Def = Defs[i];
            if (Def.ParentBodyIndex < 0 || i >= (int32)Handle->Bodies.size() || Def.ParentBodyIndex >= (int32)Handle->Bodies.size())
            {
                continue;
            }

            const FRagdollBodyDef& ParentDef = Defs[Def.ParentBodyIndex];

            FVector3 TwistAxis = Def.WorldPos - ParentDef.WorldPos;
            TwistAxis = Math::Length(TwistAxis) > 1e-4f
                ? Math::Normalize(TwistAxis)
                : Math::Normalize(Def.WorldRot * FVector3(0.0f, 1.0f, 0.0f));

            float Twist = Def.TwistDeg;
            float Swing1 = Def.Swing1Deg;
            float Swing2 = Def.Swing2Deg;

            if (Desc.Asset)
            {
                const FName ChildBone = Skeleton->GetBone(Def.BoneIndex).Name;
                for (const SPhysicsConstraintSetup& C : Desc.Asset->Constraints)
                {
                    if (C.ChildBone == ChildBone)
                    {
                        Twist = C.TwistLimitDegrees;
                        Swing1 = C.Swing1LimitDegrees;
                        Swing2 = C.Swing2LimitDegrees;
                        break;
                    }
                }
            }

            const b3BodyId ChildBody = Handle->Bodies[i];
            const b3BodyId ParentBody = Handle->Bodies[Def.ParentBodyIndex];

            // Box3D drives the swing cone about the frame X axis, so the frame is aimed along the limb.
            const FVector3 Perp = RagdollPerpendicular(TwistAxis);
            const FVector3 Bitangent = Math::Cross(TwistAxis, Perp);
            const FMatrix4 Basis(FVector4(TwistAxis.x, TwistAxis.y, TwistAxis.z, 0.0f),
                                 FVector4(Perp.x, Perp.y, Perp.z, 0.0f),
                                 FVector4(Bitangent.x, Bitangent.y, Bitangent.z, 0.0f),
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
                Handle->Joints.push_back(JointId);
            }
        }

        // Limbs that are not directly jointed still must not collide, which is what a filter joint is for.
        for (int32 i = 0; i < (int32)Handle->Bodies.size(); ++i)
        {
            for (int32 j = i + 1; j < (int32)Handle->Bodies.size(); ++j)
            {
                b3FilterJointDef FilterDef = b3DefaultFilterJointDef();
                FilterDef.base.bodyIdA = Handle->Bodies[i];
                FilterDef.base.bodyIdB = Handle->Bodies[j];

                const b3JointId FilterId = b3CreateFilterJoint(WorldId, &FilterDef);
                if (b3Joint_IsValid(FilterId))
                {
                    Handle->Joints.push_back(FilterId);
                }
            }
        }

        Handle->bAddedToScene = true;
        return Handle;
    }

    void FBox3DPhysicsScene::ReadRagdollPose(const FPhysicsRagdollHandle& Handle, const FMatrix4& WorldToEntity,
                                             const FSkeletonResource* Skeleton, TVector<FMatrix4>& OutBoneTransforms)
    {
        LUMINA_PROFILE_SCOPE();

        if (Handle.Bodies.empty() || Skeleton == nullptr)
        {
            return;
        }

        const int32 NumBones = Skeleton->GetNumBones();
        OutBoneTransforms.resize(NumBones);

        TVector<FMatrix4> ComponentGlobals;
        ComponentGlobals.resize(NumBones);
        TVector<uint8> Mapped;
        Mapped.resize(NumBones, 0);

        for (int32 j = 0; j < (int32)Handle.JointToBone.size(); ++j)
        {
            const int32 BoneIndex = Handle.JointToBone[j];
            if (!Skeleton->IsBoneIndexValid(BoneIndex) || j >= (int32)Handle.Bodies.size())
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
            ComponentGlobals[BoneIndex] = WorldToEntity * AnimPose::ComposeTRS(Pos, Rot, FVector3(1.0f));
            Mapped[BoneIndex] = 1;
        }

        // Parents precede children in the bone array, so a single forward pass resolves them.
        for (int32 i = 0; i < NumBones; ++i)
        {
            if (!Mapped[i])
            {
                const int32 Parent = Skeleton->GetBone(i).ParentIndex;
                const FMatrix4& Local = Skeleton->GetBone(i).LocalTransform;
                ComponentGlobals[i] = Parent >= 0 ? ComponentGlobals[Parent] * Local : Local;
            }
            OutBoneTransforms[i] = ComponentGlobals[i] * Skeleton->GetBone(i).InvBindMatrix;
        }
    }

    void FBox3DPhysicsScene::DestroyRagdoll(const TSharedPtr<FPhysicsRagdollHandle>& Handle)
    {
        if (!Handle)
        {
            return;
        }

        for (uint32 BodyHandle : Handle->BodyHandles)
        {
            UnregisterBody(BodyHandle);
        }
        Handle->BodyHandles.clear();

        Handle->Release();
    }

    void FBox3DPhysicsScene::GetRagdollRootTransform(const FPhysicsRagdollHandle& Handle, FVector3& OutPosition, FQuat& OutRotation)
    {
        OutPosition = FVector3(0.0f);
        OutRotation = FQuat::Identity();

        if (Handle.Bodies.empty() || !b3Body_IsValid(Handle.Bodies[0]))
        {
            return;
        }

        // Body 0 is the ragdoll root, the lowest bodied bone with no bodied ancestor.
        OutPosition = Box3DUtils::FromB3Vec3(b3Body_GetPosition(Handle.Bodies[0]));
        OutRotation = Box3DUtils::FromB3Quat(b3Body_GetRotation(Handle.Bodies[0]));
    }

    void FBox3DPhysicsScene::ApplyBuoyancyImpulse(ECS::FEntity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
        float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float DeltaTime)
    {
        const b3BodyId BodyId = ResolveBody(GetEntityBodyID(Entity));
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

        const FVector3 Normal = Math::LengthSquared(SurfaceNormal) > LE_SMALL_NUMBER
            ? Math::Normalize(SurfaceNormal)
            : FVector3(0.0f, 1.0f, 0.0f);

        const FVector3 Center = (Min + Max) * 0.5f;
        const FVector3 Extent = (Max - Min) * 0.5f;

        // Projected half-height of the bounds along the surface normal.
        const float Reach = Math::Abs(Extent.x * Normal.x) + Math::Abs(Extent.y * Normal.y) + Math::Abs(Extent.z * Normal.z);
        if (Reach <= LE_SMALL_NUMBER)
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
