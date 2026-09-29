#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "Box3DPhysicsScene.h"

#include <box3d/collision.h>

#include "Box3DInternal.h"
#include "Box3DUtils.h"

#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "Log/Log.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

namespace Lumina::Physics
{
    namespace
    {
        // Builds the local frame a joint needs on one body from a world-space anchor and axis.
        b3Transform MakeLocalFrame(b3BodyId Body, const FVector3& WorldAnchor, const FVector3& WorldAxis)
        {
            FVector3 Axis = Math::LengthSquared(WorldAxis) > LE_SMALL_NUMBER
                ? Math::Normalize(WorldAxis)
                : FVector3(0.0f, 1.0f, 0.0f);

            // Box3D drives revolute and prismatic joints about the frame's X axis, so X is aimed at the axis.
            FVector3 Reference = Math::Abs(Axis.y) < 0.99f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
            const FVector3 Y = Math::Normalize(Math::Cross(Reference, Axis));
            const FVector3 Z = Math::Cross(Axis, Y);

            const FMatrix4 Basis(FVector4(Axis.x, Axis.y, Axis.z, 0.0f),
                                 FVector4(Y.x, Y.y, Y.z, 0.0f),
                                 FVector4(Z.x, Z.y, Z.z, 0.0f),
                                 FVector4(0.0f, 0.0f, 0.0f, 1.0f));
            const FQuat WorldRotation = Math::ToQuat(Basis);

            if (!b3Body_IsValid(Body))
            {
                return Box3DUtils::ToB3Transform(WorldAnchor, WorldRotation);
            }

            const b3Vec3 LocalPoint = b3Body_GetLocalPoint(Body, Box3DUtils::ToB3Vec3(WorldAnchor));
            const b3Quat BodyRotation = b3Body_GetRotation(Body);
            const FQuat LocalRotation = Math::Normalize(Math::Inverse(Box3DUtils::FromB3Quat(BodyRotation)) * WorldRotation);

            return b3Transform{ LocalPoint, Box3DUtils::ToB3Quat(LocalRotation) };
        }
    }

    uint32 FBox3DPhysicsScene::CreateConstraint(const FConstraintDesc& Desc)
    {
        auto RevisionOf = [&](ECS::FEntity Entity) -> uint64
        {
            if (auto It = RigidBodies.find(Entity); It != RigidBodies.end()) { return It->second.Revision; }
            if (auto It = CharacterBodies.find(Entity); It != CharacterBodies.end()) { return It->second.Revision; }
            return 0;
        };
        const uint64 RevisionA = RevisionOf(Desc.BodyA);
        const uint64 RevisionB = RevisionOf(Desc.BodyB);
        if (RevisionB == 0 || (Desc.BodyA != ECS::NullEntity && RevisionA == 0))
        {
            return 0;
        }
        FScopeLock Lock(ConstraintsMutex);
        const uint32 Handle = NextConstraintID++;
        FBox3DConstraint& Constraint = Constraints[Handle];
        Constraint.Description = Desc;
        Constraint.RevisionA = RevisionA;
        Constraint.RevisionB = RevisionB;
        Constraint.Type = Desc.Type;
        Constraint.bSettingsDirty = true;
        return Handle;
    }

    bool FBox3DPhysicsScene::CommitConstraint(uint32 Handle, const FConstraintDesc& Desc)
    {
        LUMINA_PROFILE_SCOPE();

        const b3BodyId BodyA = Desc.BodyA == ECS::NullEntity ? b3_nullBodyId : ResolveBody(FindEntityBody(Desc.BodyA));
        const b3BodyId BodyB = Desc.BodyB == ECS::NullEntity ? b3_nullBodyId : ResolveBody(FindEntityBody(Desc.BodyB));

        // Box3D has no world anchor body, so the constrained side must at least exist.
        if (!b3Body_IsValid(BodyB) || (Desc.BodyA != ECS::NullEntity && !b3Body_IsValid(BodyA)))
        {
            return 0;
        }

        b3JointDef Base{};
        Base.bodyIdA = b3Body_IsValid(BodyA) ? BodyA : BodyB;
        Base.bodyIdB = BodyB;
        Base.collideConnected = false;
        Base.forceThreshold = Desc.BreakForce > 0.0f ? Desc.BreakForce : FLT_MAX;
        Base.torqueThreshold = FLT_MAX;
        Base.localFrameA = MakeLocalFrame(Base.bodyIdA, Desc.Anchor, Desc.Axis);
        Base.localFrameB = MakeLocalFrame(BodyB, Desc.Anchor, Desc.Axis);

        if (Desc.LimitFrequency > 0.0f)
        {
            Base.constraintHertz = Desc.LimitFrequency;
            Base.constraintDampingRatio = Desc.LimitDamping;
        }

        b3JointId JointId = b3_nullJointId;

        switch (Desc.Type)
        {
            case EPhysicsConstraintType::Fixed:
            {
                b3WeldJointDef Def = b3DefaultWeldJointDef();
                Def.base = Base;
                JointId = b3CreateWeldJoint(WorldId, &Def);
                break;
            }
            case EPhysicsConstraintType::Point:
            {
                b3SphericalJointDef Def = b3DefaultSphericalJointDef();
                Def.base = Base;
                JointId = b3CreateSphericalJoint(WorldId, &Def);
                break;
            }
            case EPhysicsConstraintType::Cone:
            {
                b3SphericalJointDef Def = b3DefaultSphericalJointDef();
                Def.base = Base;
                Def.enableConeLimit = Desc.HalfConeAngle > 0.0f;
                Def.coneAngle = Desc.HalfConeAngle;
                JointId = b3CreateSphericalJoint(WorldId, &Def);
                break;
            }
            case EPhysicsConstraintType::Distance:
            {
                const float Length = Math::Length(Desc.AnchorB - Desc.Anchor);

                b3DistanceJointDef Def = b3DefaultDistanceJointDef();
                Def.base = Base;
                Def.base.localFrameB = MakeLocalFrame(BodyB, Desc.AnchorB, Desc.Axis);
                Def.length = Math::Max(Length, B3_LINEAR_SLOP);
                Def.enableLimit = Desc.bHasLimits;
                Def.minLength = Desc.MinLimit;
                Def.maxLength = Math::Max(Desc.MaxLimit, Desc.MinLimit);

                if (Desc.LimitFrequency > 0.0f)
                {
                    Def.enableSpring = true;
                    Def.hertz = Desc.LimitFrequency;
                    Def.dampingRatio = Desc.LimitDamping;
                }
                JointId = b3CreateDistanceJoint(WorldId, &Def);
                break;
            }
            case EPhysicsConstraintType::Hinge:
            {
                b3RevoluteJointDef Def = b3DefaultRevoluteJointDef();
                Def.base = Base;
                Def.enableLimit = Desc.bHasLimits;
                Def.lowerAngle = Desc.MinLimit;
                Def.upperAngle = Desc.MaxLimit;

                if (Desc.MaxFriction > 0.0f)
                {
                    // Box3D models joint friction as a zero-speed motor with a bounded torque.
                    Def.enableMotor = true;
                    Def.motorSpeed = 0.0f;
                    Def.maxMotorTorque = Desc.MaxFriction;
                }

                if (Desc.MotorFrequency > 0.0f)
                {
                    Def.enableSpring = true;
                    Def.hertz = Desc.MotorFrequency;
                    Def.dampingRatio = Desc.MotorDamping;
                }
                JointId = b3CreateRevoluteJoint(WorldId, &Def);
                break;
            }
            case EPhysicsConstraintType::Slider:
            {
                b3PrismaticJointDef Def = b3DefaultPrismaticJointDef();
                Def.base = Base;
                Def.enableLimit = Desc.bHasLimits;
                Def.lowerTranslation = Desc.MinLimit;
                Def.upperTranslation = Desc.MaxLimit;

                if (Desc.MaxFriction > 0.0f)
                {
                    Def.enableMotor = true;
                    Def.motorSpeed = 0.0f;
                    Def.maxMotorForce = Desc.MaxFriction;
                }

                if (Desc.MotorFrequency > 0.0f)
                {
                    Def.enableSpring = true;
                    Def.hertz = Desc.MotorFrequency;
                    Def.dampingRatio = Desc.MotorDamping;
                }
                JointId = b3CreatePrismaticJoint(WorldId, &Def);
                break;
            }
        }

        if (!b3Joint_IsValid(JointId))
        {
            return 0;
        }

        b3Joint_SetUserData(JointId, reinterpret_cast<void*>((uintptr_t)Handle));

        FBox3DConstraint& Constraint = Constraints[Handle];
        Constraint.JointId = JointId;
        Constraint.Type = Desc.Type;
        Constraint.BreakForce = Desc.BreakForce;
        Constraint.MotorForceLimit = Desc.MotorForceLimit;
        Constraint.MotorTorqueLimit = Desc.MotorTorqueLimit;

        return true;
    }

    void FBox3DPhysicsScene::DestroyConstraint(uint32 ConstraintID)
    {
        FScopeLock Lock(ConstraintsMutex);

        auto It = Constraints.find(ConstraintID);
        if (It == Constraints.end())
        {
            return;
        }

        It->second.bDestroy = true;
    }

    void FBox3DPhysicsScene::SetConstraintEnabled(uint32 ConstraintID, bool bEnabled)
    {
        FScopeLock Lock(ConstraintsMutex);
        if (auto It = Constraints.find(ConstraintID); It != Constraints.end())
        {
            It->second.bEnabled = bEnabled;
            It->second.bSettingsDirty = true;
        }
    }

    void FBox3DPhysicsScene::SetConstraintMotor(uint32 ConstraintID, EConstraintMotorMode Mode, float Target)
    {
        FScopeLock Lock(ConstraintsMutex);
        if (auto It = Constraints.find(ConstraintID); It != Constraints.end())
        {
            It->second.bMotorSet = true;
            It->second.MotorMode = Mode;
            It->second.MotorTarget = Target;
            It->second.bSettingsDirty = true;
        }
    }

    void FBox3DPhysicsScene::ApplyConstraintEnabled(uint32 ConstraintID, bool bEnabled)
    {
        FScopeLock Lock(ConstraintsMutex);

        auto It = Constraints.find(ConstraintID);
        if (It == Constraints.end() || !b3Joint_IsValid(It->second.JointId))
        {
            return;
        }

        // Box3D has no joint enable flag, so a disabled joint is one with no force budget left.
        FBox3DConstraint& Constraint = It->second;
        Constraint.bEnabled = bEnabled;
        b3Joint_SetConstraintTuning(Constraint.JointId, bEnabled ? 60.0f : 0.0f, bEnabled ? 2.0f : 0.0f);
    }

    void FBox3DPhysicsScene::ApplyConstraintMotor(uint32 ConstraintID, EConstraintMotorMode Mode, float Target)
    {
        FScopeLock Lock(ConstraintsMutex);

        auto It = Constraints.find(ConstraintID);
        if (It == Constraints.end() || !b3Joint_IsValid(It->second.JointId))
        {
            return;
        }

        const FBox3DConstraint& Constraint = It->second;
        const b3JointId JointId = Constraint.JointId;

        if (Constraint.Type == EPhysicsConstraintType::Hinge)
        {
            switch (Mode)
            {
                case EConstraintMotorMode::Off:
                    b3RevoluteJoint_EnableMotor(JointId, false);
                    b3RevoluteJoint_EnableSpring(JointId, false);
                    break;
                case EConstraintMotorMode::Velocity:
                    b3RevoluteJoint_EnableSpring(JointId, false);
                    b3RevoluteJoint_EnableMotor(JointId, true);
                    b3RevoluteJoint_SetMotorSpeed(JointId, Target);
                    b3RevoluteJoint_SetMaxMotorTorque(JointId, Constraint.MotorTorqueLimit > 0.0f ? Constraint.MotorTorqueLimit : FLT_MAX);
                    break;
                case EConstraintMotorMode::Position:
                    b3RevoluteJoint_EnableMotor(JointId, false);
                    b3RevoluteJoint_EnableSpring(JointId, true);
                    b3RevoluteJoint_SetTargetAngle(JointId, Target);
                    break;
            }
            return;
        }

        if (Constraint.Type == EPhysicsConstraintType::Slider)
        {
            switch (Mode)
            {
                case EConstraintMotorMode::Off:
                    b3PrismaticJoint_EnableMotor(JointId, false);
                    b3PrismaticJoint_EnableSpring(JointId, false);
                    break;
                case EConstraintMotorMode::Velocity:
                    b3PrismaticJoint_EnableSpring(JointId, false);
                    b3PrismaticJoint_EnableMotor(JointId, true);
                    b3PrismaticJoint_SetMotorSpeed(JointId, Target);
                    b3PrismaticJoint_SetMaxMotorForce(JointId, Constraint.MotorForceLimit > 0.0f ? Constraint.MotorForceLimit : FLT_MAX);
                    break;
                case EConstraintMotorMode::Position:
                    b3PrismaticJoint_EnableMotor(JointId, false);
                    b3PrismaticJoint_EnableSpring(JointId, true);
                    b3PrismaticJoint_SetTargetTranslation(JointId, Target);
                    break;
            }
        }
    }

    bool FBox3DPhysicsScene::IsConstraintBroken(uint32 ConstraintID)
    {
        FScopeLock Lock(ConstraintsMutex);

        auto It = Constraints.find(ConstraintID);
        return It != Constraints.end() && It->second.bBroken;
    }

    float FBox3DPhysicsScene::GetConstraintValue(uint32 ConstraintID)
    {
        FScopeLock Lock(ConstraintsMutex);

        auto It = Constraints.find(ConstraintID);
        if (It == Constraints.end() || !b3Joint_IsValid(It->second.JointId))
        {
            return 0.0f;
        }

        switch (It->second.Type)
        {
            case EPhysicsConstraintType::Hinge:  return b3RevoluteJoint_GetAngle(It->second.JointId);
            case EPhysicsConstraintType::Slider: return b3PrismaticJoint_GetTranslation(It->second.JointId);
            default:                            return 0.0f;
        }
    }

    void FBox3DPhysicsScene::MonitorBreakableConstraints(float Dt)
    {
        // Box3D raises a joint event once the force threshold is crossed, so nothing is polled per joint.
        const b3JointEvents Events = b3World_GetJointEvents(WorldId);
        if (Events.count == 0)
        {
            return;
        }

        FScopeLock Lock(ConstraintsMutex);

        for (int32 i = 0; i < Events.count; ++i)
        {
            const b3JointEvent& Event = Events.jointEvents[i];
            const uint32 Handle = (uint32)(uintptr_t)Event.userData;

            auto It = Constraints.find(Handle);
            if (It == Constraints.end() || It->second.BreakForce <= 0.0f || It->second.bBroken)
            {
                continue;
            }

            It->second.bBroken = true;
            if (b3Joint_IsValid(Event.jointId))
            {
                b3DestroyJoint(Event.jointId, true);
            }
            It->second.JointId = b3_nullJointId;
        }
    }

    void FBox3DPhysicsScene::DestroyAllConstraints()
    {
        FScopeLock Lock(ConstraintsMutex);

        for (auto& [Handle, Constraint] : Constraints)
        {
            if (b3Joint_IsValid(Constraint.JointId))
            {
                b3DestroyJoint(Constraint.JointId, false);
            }
        }

        Constraints.clear();
    }

    void FBox3DPhysicsScene::OnConstraintComponentConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        FScopeLock Lock(PendingConstraintMutex);
        PendingConstraintCreations.push_back(Entity);
    }

    void FBox3DPhysicsScene::OnConstraintComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (SPhysicsConstraintComponent* Component = Registry.TryGet<SPhysicsConstraintComponent>(Entity))
        {
            if (Component->ConstraintID != 0)
            {
                DestroyConstraint(Component->ConstraintID);
                Component->ConstraintID = 0;
            }
        }
    }

    bool FBox3DPhysicsScene::TryCreateComponentConstraint(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        SPhysicsConstraintComponent* Component = Registry.TryGet<SPhysicsConstraintComponent>(Entity);
        if (Component == nullptr)
        {
            return true;
        }

        if (Component->ConstraintID != 0)
        {
            return true;
        }

        // Both bodies have to exist before the joint can be anchored, so an early frame just retries.
        const uint32 SelfBody = FindEntityBody(Entity);
        if (!b3Body_IsValid(ResolveBody(SelfBody)))
        {
            return false;
        }

        const STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return false;
        }

        const ECS::FEntity TargetEntity = Component->TargetEntity;
        if (TargetEntity != ECS::NullEntity && FindEntityBody(TargetEntity) == InvalidBodyHandle)
        {
            return false;
        }

        FConstraintDesc Desc;
        Desc.Type = Component->Type;
        Desc.BodyA = TargetEntity;
        Desc.BodyB = Entity;
        Desc.Anchor = Transform->GetLocation() + Component->PivotOffset;
        Desc.Axis = Math::Rotate(Transform->GetRotation(), Component->Axis);
        Desc.AnchorB = Desc.Anchor;
        Desc.MinLimit = Component->LowerLimit;
        Desc.MaxLimit = Component->UpperLimit;
        Desc.HalfConeAngle = Math::Radians(Component->ConeHalfAngle);
        Desc.bHasLimits = Component->bLimited;
        Desc.MaxFriction = Component->Friction;
        Desc.BreakForce = Component->BreakForce;

        Component->ConstraintID = CreateConstraint(Desc);
        return Component->ConstraintID != 0;
    }

    void FBox3DPhysicsScene::SynchronizeConstraints()
    {
        auto Matches = [&](ECS::FEntity Entity, uint64 Revision)
        {
            if (Entity == ECS::NullEntity) { return true; }
            if (auto It = RigidBodies.find(Entity); It != RigidBodies.end()) { return It->second.Revision == Revision; }
            if (auto It = CharacterBodies.find(Entity); It != CharacterBodies.end()) { return It->second.Revision == Revision; }
            return false;
        };
        TVector<uint32> Removed;
        for (auto& [Handle, Constraint] : Constraints)
        {
            if (Constraint.bDestroy || !Matches(Constraint.Description.BodyA, Constraint.RevisionA)
                || !Matches(Constraint.Description.BodyB, Constraint.RevisionB))
            {
                if (b3Joint_IsValid(Constraint.JointId)) { b3DestroyJoint(Constraint.JointId, true); }
                Removed.push_back(Handle);
                continue;
            }
            if (!Constraint.bBroken && !b3Joint_IsValid(Constraint.JointId))
            {
                if (!CommitConstraint(Handle, Constraint.Description)) { continue; }
                Constraint.bSettingsDirty = true;
            }
            if (Constraint.bSettingsDirty)
            {
                ApplyConstraintEnabled(Handle, Constraint.bEnabled);
                if (Constraint.bMotorSet) { ApplyConstraintMotor(Handle, Constraint.MotorMode, Constraint.MotorTarget); }
                Constraint.bSettingsDirty = false;
            }
        }
        for (uint32 Handle : Removed) { Constraints.erase(Handle); }
    }

    void FBox3DPhysicsScene::DrainPendingConstraints()
    {
        FScopeLock Lock(PendingConstraintMutex);

        if (PendingConstraintCreations.empty())
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        // Compacted in place, so an entity whose bodies are not ready is retried next frame without a copy.
        size_t Write = 0;
        for (size_t Read = 0; Read < PendingConstraintCreations.size(); ++Read)
        {
            const ECS::FEntity Entity = PendingConstraintCreations[Read];
            if (!Registry.IsValid(Entity))
            {
                continue;
            }

            if (!TryCreateComponentConstraint(Registry, Entity))
            {
                PendingConstraintCreations[Write++] = Entity;
            }
        }

        PendingConstraintCreations.resize(Write);
    }

    uint32 FBox3DPhysicsScene::CreateStaticBodyGroup(ECS::FEntity Owner, TSpan<const FStaticInstanceDesc> Instances)
    {
        if (Instances.empty()) { return 0; }
        FPendingStaticGroup Request;
        Request.GroupID = NextStaticBodyGroupID++;
        Request.Owner = Owner;
        FCollisionProfile Profile;
        Profile.Layer = ECollisionProfiles::Static;
        for (const FStaticInstanceDesc& Instance : Instances)
        {
            FRigidBodyBuildResult Build;
            Build.BodyDef = b3DefaultBodyDef();
            Build.BodyDef.type = b3_staticBody;
            Build.BodyDef.position = Box3DUtils::ToB3Vec3(Instance.Position);
            Build.BodyDef.rotation = Box3DUtils::ToB3Quat(Instance.Rotation);
            Build.ShapeDef = b3DefaultShapeDef();
            Build.ShapeDef.updateBodyMass = false;
            Build.ShapeDef.filter = Box3DUtils::MakeShapeFilter(Profile);
            Build.ShapeDef.userData = Box3DUtils::PackProfileUserData(Profile);
            Build.ShapeDef.enableCustomFiltering = Box3DUtils::UsesPermissiveCollisionFilter();
            Build.ShapeDef.invokeContactCreation = false;
            if (Instance.Material)
            {
                Build.ShapeDef.baseMaterial.friction = Instance.Material->Friction;
                Build.ShapeDef.baseMaterial.restitution = Instance.Material->Restitution;
            }
            if (Instance.Shape)
            {
                BuildCollisionShapeAsset(*Instance.Shape, Instance.Scale, Build.Shapes);
            }
            else if (Instance.Mesh && Instance.bConvex)
            {
                if (const b3HullData* Hull = GetOrCreateMeshHull(Instance.Mesh))
                {
                    FPendingShape Shape = MakeHullShape(Hull, FVector3(0.0f), FQuat::Identity());
                    Shape.Scale = Box3DUtils::ToB3Vec3(Instance.Scale);
                    Build.Shapes.push_back(Shape);
                }
            }
            else if (Instance.Mesh)
            {
                if (const b3MeshData* Mesh = GetOrCreateTriangleMesh(Instance.Mesh))
                {
                    FPendingShape Shape;
                    Shape.Type = b3_meshShape;
                    Shape.Mesh = Mesh;
                    Shape.Scale = Box3DUtils::ToB3Vec3(Instance.Scale);
                    Build.Shapes.push_back(Shape);
                }
            }
            if (!Build.Shapes.empty()) { Request.Builds.push_back(Move(Build)); }
        }
        if (Request.Builds.empty()) { return 0; }
        const uint32 GroupID = Request.GroupID;
        StaticBodyGroups[GroupID];
        PendingStaticGroups.push_back(Move(Request));
        return GroupID;
    }

    void FBox3DPhysicsScene::CommitStaticBodyGroup(uint32 GroupID, ECS::FEntity Owner, TSpan<const FRigidBodyBuildResult> Builds)
    {
        auto It = StaticBodyGroups.find(GroupID);
        if (It == StaticBodyGroups.end()) { return; }
        auto& Handles = It->second;
        for (const FRigidBodyBuildResult& Build : Builds)
        {
            if (BodyHandles.size() - FreeBodyHandles.size() >= MaxBodies) { break; }
            const b3BodyId Body = b3CreateBody(WorldId, &Build.BodyDef);
            if (!b3Body_IsValid(Body)) { continue; }
            const uint32 Handle = RegisterBody(Body);
            b3Body_SetUserData(Body, PackBodyUserData(Owner, Handle));
            for (const FPendingShape& Shape : Build.Shapes)
            {
                switch (Shape.Type)
                {
                    case b3_sphereShape: b3CreateSphereShape(Body, &Build.ShapeDef, &Shape.Sphere); break;
                    case b3_capsuleShape: b3CreateCapsuleShape(Body, &Build.ShapeDef, &Shape.Capsule); break;
                    case b3_hullShape: b3CreateTransformedHullShape(Body, &Build.ShapeDef, Shape.Hull, Shape.Transform, Shape.Scale); break;
                    case b3_meshShape: b3CreateMeshShape(Body, &Build.ShapeDef, Shape.Mesh, Shape.Scale); break;
                    default: break;
                }
            }
            Handles.push_back(Handle);
        }
        bStaticTreeDirty = true;
    }

    void FBox3DPhysicsScene::DestroyStaticBodyGroup(uint32 GroupID)
    {
        auto It = StaticBodyGroups.find(GroupID);
        if (It == StaticBodyGroups.end())
        {
            return;
        }

        for (uint32 Handle : It->second)
        {
            PendingBodyDestructions.push_back(Handle);
        }
        StaticBodyGroups.erase(It);
    }

    void FBox3DPhysicsScene::DestroyAllStaticBodyGroups()
    {
        for (auto& [GroupID, Handles] : StaticBodyGroups)
        {
            for (uint32 Handle : Handles)
            {
                const b3BodyId BodyId = ResolveBody(Handle);
                if (b3Body_IsValid(BodyId))
                {
                    b3DestroyBody(BodyId);
                }
                UnregisterBody(Handle);
            }
        }

        StaticBodyGroups.clear();
    }
}
