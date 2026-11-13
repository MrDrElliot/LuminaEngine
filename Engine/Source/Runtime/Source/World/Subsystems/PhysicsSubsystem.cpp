#include "RuntimePCH.h"

#include "World/Subsystems/PhysicsSubsystem.h"

#include "Physics/PhysicsScene.h"
#include "World/World.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include <cfloat>

namespace Lumina
{
    namespace
    {
        // The caller sizes the result set; past this a query reports its true total and is called again.
        constexpr size_t DefaultQueryCapacity = 256;

        Physics::IPhysicsScene* SceneOf(CWorld* World)
        {
            return World != nullptr ? World->GetPhysicsScene() : nullptr;
        }

        void StageIgnore(ECS::FEntity Entity, TInlineVector<ECS::FEntity, MaxInlineIgnoreEntities>& Out)
        {
            if (Entity != ECS::NullEntity)
            {
                Out.Add(Entity);
            }
        }

        using FIgnoreList = TInlineVector<ECS::FEntity, MaxInlineIgnoreEntities>;

        FIgnoreList QueryIgnoreOf(ECS::FEntity Entity)
        {
            FIgnoreList Ignore;
            StageIgnore(Entity, Ignore);
            return Ignore;
        }

        SRayResult QueryMissOf(const FVector3& Start, const FVector3& End)
        {
            SRayResult Miss;
            Miss.Start = Start;
            Miss.End = End;
            return Miss;
        }

        // The scene writes into a span, so the result vector is sized first and trimmed to what it wrote.
        void Overlap(CWorld* World, const SQueryShape& Shape, const FVector3& Center, ECS::FEntity IgnoreEntity,
            ECollisionProfiles LayerMask, TVector<ECS::FEntity>& Out)
        {
            Physics::IPhysicsScene* Scene = SceneOf(World);
            if (Scene == nullptr)
            {
                return;
            }

            const int32 Start = Out.Num();
            Out.SetNum(Start + DefaultQueryCapacity);
            const int32 Written = Scene->OverlapShape(Shape, Center, LayerMask, QueryIgnoreOf(IgnoreEntity),
                TSpan<ECS::FEntity>(Out.GetData() + Start, DefaultQueryCapacity));
            Out.SetNum(Start + (size_t)(Written > 0 ? Written : 0));
        }

        bool IsFree(CWorld* World, const SQueryShape& Shape, const FVector3& Center, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
        {
            Physics::IPhysicsScene* Scene = SceneOf(World);
            ECS::FEntity First;
            return Scene == nullptr || Scene->OverlapShape(Shape, Center, LayerMask, QueryIgnoreOf(IgnoreEntity), TSpan<ECS::FEntity>(&First, 1)) == 0;
        }
    }

    SRayResult CPhysicsSubsystem::Raycast(FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        const SRayResult Miss = QueryMissOf(Start, End);
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        if (Scene == nullptr)
        {
            return Miss;
        }

        SRayCastSettings Settings;
        Settings.Start = Start;
        Settings.End = End;
        Settings.LayerMask = LayerMask;
        StageIgnore(IgnoreEntity, Settings.IgnoreEntities);

        TOptional<SRayResult> Result = Scene->CastRay(Settings);
        return Result.IsSet() ? Result.GetValue() : Miss;
    }

    void CPhysicsSubsystem::RaycastAll(FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity,
        ECollisionProfiles LayerMask, TVector<SRayResult>& Out)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        if (Scene == nullptr)
        {
            return;
        }

        SRayCastSettings Settings;
        Settings.Start = Start;
        Settings.End = End;
        Settings.LayerMask = LayerMask;
        StageIgnore(IgnoreEntity, Settings.IgnoreEntities);

        Scene->CastRayAll(Settings, Out);
    }

    SRayResult CPhysicsSubsystem::RaycastDirection(FVector3 Start, FVector3 Direction, float Distance,
        ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        CWorld* World = GetWorld();
        return Raycast(Start, Start + Math::Normalize(Direction) * Distance, IgnoreEntity, LayerMask);
    }

    bool CPhysicsSubsystem::HasLineOfSight(FVector3 From, FVector3 To, ECS::FEntity IgnoreEntity,
        ECS::FEntity TargetEntity, ECollisionProfiles LayerMask)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        if (Scene == nullptr)
        {
            return true;
        }

        SRayCastSettings Settings;
        Settings.Start = From;
        Settings.End = To;
        Settings.LayerMask = LayerMask;
        StageIgnore(IgnoreEntity, Settings.IgnoreEntities);
        StageIgnore(TargetEntity, Settings.IgnoreEntities);
        return !Scene->CastRayAny(Settings);
    }

    SRayResult CPhysicsSubsystem::RaycastEntity(ECS::FEntity Entity, FVector3 Start, FVector3 End)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        TOptional<SRayResult> Hit = Scene != nullptr ? Scene->CastRayAgainst(Entity, Start, End) : TOptional<SRayResult>();
        return Hit.IsSet() ? Hit.GetValue() : QueryMissOf(Start, End);
    }

    void CPhysicsSubsystem::SphereCast(FVector3 Start, FVector3 End, float Radius,
        ECS::FEntity IgnoreEntity, TVector<SRayResult>& Out, ECollisionProfiles LayerMask)
    {
        CWorld* World = GetWorld();
        ShapeCastAll(SQueryShape::Sphere(Radius), Start, End, IgnoreEntity, Out, LayerMask);
    }

    SRayResult CPhysicsSubsystem::SphereCastClosest(FVector3 Start, FVector3 End, float Radius,
        ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        CWorld* World = GetWorld();
        return ShapeCast(SQueryShape::Sphere(Radius), Start, End, IgnoreEntity, LayerMask);
    }

    SRayResult CPhysicsSubsystem::BoxCast(FVector3 Start, FVector3 End, FVector3 HalfExtents, FQuat Rotation,
        ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        CWorld* World = GetWorld();
        return ShapeCast(SQueryShape::Box(HalfExtents, Rotation), Start, End, IgnoreEntity, LayerMask);
    }

    void CPhysicsSubsystem::BoxCastAll(FVector3 Start, FVector3 End, FVector3 HalfExtents, FQuat Rotation,
        ECS::FEntity IgnoreEntity, TVector<SRayResult>& Out, ECollisionProfiles LayerMask)
    {
        CWorld* World = GetWorld();
        ShapeCastAll(SQueryShape::Box(HalfExtents, Rotation), Start, End, IgnoreEntity, Out, LayerMask);
    }

    SRayResult CPhysicsSubsystem::CapsuleCast(FVector3 Start, FVector3 End, float Radius, float HalfHeight, FQuat Rotation,
        ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        CWorld* World = GetWorld();
        return ShapeCast(SQueryShape::Capsule(Radius, HalfHeight, Rotation), Start, End, IgnoreEntity, LayerMask);
    }

    void CPhysicsSubsystem::CapsuleCastAll(FVector3 Start, FVector3 End, float Radius, float HalfHeight, FQuat Rotation,
        ECS::FEntity IgnoreEntity, TVector<SRayResult>& Out, ECollisionProfiles LayerMask)
    {
        CWorld* World = GetWorld();
        ShapeCastAll(SQueryShape::Capsule(Radius, HalfHeight, Rotation), Start, End, IgnoreEntity, Out, LayerMask);
    }

    SRayResult CPhysicsSubsystem::ShapeCast(SQueryShape Shape, FVector3 Start, FVector3 End,
        ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        TOptional<SRayResult> Hit = Scene != nullptr ? Scene->CastShapeClosest(Shape, Start, End, LayerMask, QueryIgnoreOf(IgnoreEntity)) : TOptional<SRayResult>();
        return Hit.IsSet() ? Hit.GetValue() : QueryMissOf(Start, End);
    }

    void CPhysicsSubsystem::ShapeCastAll(SQueryShape Shape, FVector3 Start, FVector3 End,
        ECS::FEntity IgnoreEntity, TVector<SRayResult>& Out, ECollisionProfiles LayerMask)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld()))
        {
            Scene->CastShapeAll(Shape, Start, End, LayerMask, QueryIgnoreOf(IgnoreEntity), Out);
        }
    }

    void CPhysicsSubsystem::OverlapSphere(FVector3 Center, float Radius,
        ECS::FEntity IgnoreEntity, TVector<ECS::FEntity>& Out, ECollisionProfiles LayerMask)
    {
        Overlap(GetWorld(), SQueryShape::Sphere(Radius), Center, IgnoreEntity, LayerMask, Out);
    }

    void CPhysicsSubsystem::OverlapBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation,
        ECS::FEntity IgnoreEntity, TVector<ECS::FEntity>& Out, ECollisionProfiles LayerMask)
    {
        Overlap(GetWorld(), SQueryShape::Box(HalfExtents, Rotation), Center, IgnoreEntity, LayerMask, Out);
    }

    void CPhysicsSubsystem::OverlapCapsule(FVector3 Center, float Radius, float HalfHeight, FQuat Rotation,
        ECS::FEntity IgnoreEntity, TVector<ECS::FEntity>& Out, ECollisionProfiles LayerMask)
    {
        Overlap(GetWorld(), SQueryShape::Capsule(Radius, HalfHeight, Rotation), Center, IgnoreEntity, LayerMask, Out);
    }

    void CPhysicsSubsystem::OverlapShape(SQueryShape Shape, FVector3 Center, ECS::FEntity IgnoreEntity,
        TVector<ECS::FEntity>& Out, ECollisionProfiles LayerMask)
    {
        Overlap(GetWorld(), Shape, Center, IgnoreEntity, LayerMask, Out);
    }

    void CPhysicsSubsystem::OverlapPoint(FVector3 Point, ECS::FEntity IgnoreEntity,
        TVector<ECS::FEntity>& Out, ECollisionProfiles LayerMask)
    {
        Overlap(GetWorld(), SQueryShape::Sphere(0.0f), Point, IgnoreEntity, LayerMask, Out);
    }

    bool CPhysicsSubsystem::IsSpaceFree(FVector3 Center, float Radius, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        return IsFree(GetWorld(), SQueryShape::Sphere(Radius), Center, IgnoreEntity, LayerMask);
    }

    bool CPhysicsSubsystem::IsBoxSpaceFree(FVector3 Center, FVector3 HalfExtents, FQuat Rotation,
        ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        return IsFree(GetWorld(), SQueryShape::Box(HalfExtents, Rotation), Center, IgnoreEntity, LayerMask);
    }

    bool CPhysicsSubsystem::IsCapsuleSpaceFree(FVector3 Center, float Radius, float HalfHeight, FQuat Rotation,
        ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        return IsFree(GetWorld(), SQueryShape::Capsule(Radius, HalfHeight, Rotation), Center, IgnoreEntity, LayerMask);
    }

    ECS::FEntity CPhysicsSubsystem::FindNearestEntity(FVector3 Center, float Radius, ECS::FEntity IgnoreEntity,
        ECollisionProfiles LayerMask)
    {
        CWorld* World = GetWorld();
        TVector<ECS::FEntity> Candidates;
        OverlapSphere(Center, Radius, IgnoreEntity, Candidates, LayerMask);

        ECS::FEntity Nearest = ECS::NullEntity;
        float NearestDistance = FLT_MAX;
        for (ECS::FEntity Candidate : Candidates)
        {
            const float Distance = GetDistanceToEntity(Candidate, Center);
            if (Distance >= 0.0f && Distance < NearestDistance)
            {
                NearestDistance = Distance;
                Nearest = Candidate;
            }
        }
        return Nearest;
    }

    FVector3 CPhysicsSubsystem::GetClosestPoint(ECS::FEntity Entity, FVector3 Point)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        FVector3 Closest = Point;
        if (Scene != nullptr)
        {
            Scene->GetClosestPointOnBody(Entity, Point, Closest);
        }
        return Closest;
    }

    float CPhysicsSubsystem::GetDistanceToEntity(ECS::FEntity Entity, FVector3 Point)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        FVector3 Closest;
        return Scene != nullptr && Scene->GetClosestPointOnBody(Entity, Point, Closest) ? Math::Length(Closest - Point) : -1.0f;
    }

    SAABB CPhysicsSubsystem::GetBodyBounds(ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        SAABB Bounds;
        if (Scene == nullptr || !Scene->GetBodyBounds(Entity, Bounds))
        {
            return SAABB();
        }
        return Bounds;
    }

    void CPhysicsSubsystem::RebuildPhysics(ECS::FEntity Entity)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || !World->IsValidEntity(Entity))
        {
            return;
        }

        // The patch is what physics listens for, so the scene re-reads every field on its next step.
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        if (Registry.HasAll<SRigidBodyComponent>(Entity))
        {
            Registry.Patch<SRigidBodyComponent>(Entity);
        }
        if (Registry.HasAll<SCharacterPhysicsComponent>(Entity))
        {
            Registry.Patch<SCharacterPhysicsComponent>(Entity);
        }
    }

    void CPhysicsSubsystem::SetCapsuleSize(ECS::FEntity Entity, float Radius, float HalfHeight)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || !World->IsValidEntity(Entity))
        {
            return;
        }

        ECS::FRegistry& Registry = World->GetEntityRegistry();
        if (SCharacterPhysicsComponent* Character = Registry.TryGet<ECS::Write<SCharacterPhysicsComponent>>(Entity))
        {
            Character->Radius     = Math::Max(Radius, 0.0f);
            Character->HalfHeight = Math::Max(HalfHeight, 0.0f);
        }
        else if (SCapsuleColliderComponent* Capsule = Registry.TryGet<ECS::Write<SCapsuleColliderComponent>>(Entity))
        {
            Capsule->Radius     = Math::Max(Radius, 0.0f);
            Capsule->HalfHeight = Math::Max(HalfHeight, 0.0f);
        }
        RebuildPhysics(Entity);
    }

    void CPhysicsSubsystem::SetBoxHalfExtent(ECS::FEntity Entity, FVector3 HalfExtent)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || !World->IsValidEntity(Entity))
        {
            return;
        }

        if (SBoxColliderComponent* Box = World->GetEntityRegistry().TryGet<ECS::Write<SBoxColliderComponent>>(Entity))
        {
            Box->HalfExtent = Math::Max(HalfExtent, FVector3(0.0f));
            RebuildPhysics(Entity);
        }
    }

    void CPhysicsSubsystem::SetSphereRadius(ECS::FEntity Entity, float Radius)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || !World->IsValidEntity(Entity))
        {
            return;
        }

        if (SSphereColliderComponent* Sphere = World->GetEntityRegistry().TryGet<ECS::Write<SSphereColliderComponent>>(Entity))
        {
            Sphere->Radius = Math::Max(Radius, 0.0f);
            RebuildPhysics(Entity);
        }
    }

    void CPhysicsSubsystem::SetCollisionProfile(ECS::FEntity Entity, SCollisionProfile Profile)
    {
        CWorld* World = GetWorld();
        if (World == nullptr || !World->IsValidEntity(Entity))
        {
            return;
        }

        ECS::FRegistry& Registry = World->GetEntityRegistry();
        if (SRigidBodyComponent* Body = Registry.TryGet<ECS::Write<SRigidBodyComponent>>(Entity))
        {
            Body->CollisionProfile = Profile;
        }
        if (SCharacterPhysicsComponent* Character = Registry.TryGet<ECS::Write<SCharacterPhysicsComponent>>(Entity))
        {
            Character->CollisionProfile = Profile;
        }
        RebuildPhysics(Entity);
    }

    float CPhysicsSubsystem::GetGroundHeight(FVector3 Point, float SearchDistance, ECS::FEntity IgnoreEntity, float Fallback,
        ECollisionProfiles LayerMask)
    {
        CWorld* World = GetWorld();
        const FVector3 Start(Point.x, Point.y + SearchDistance, Point.z);
        const FVector3 End(Point.x, Point.y - SearchDistance, Point.z);
        const SRayResult Hit = Raycast(Start, End, IgnoreEntity, LayerMask);
        return Hit.bHit ? Hit.Location.y : Fallback;
    }

    void CPhysicsSubsystem::AddForce(ECS::FEntity Entity, FVector3 Force)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->AddForce(Entity, Force); }
    }

    void CPhysicsSubsystem::AddImpulse(ECS::FEntity Entity, FVector3 Impulse)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->AddImpulse(Entity, Impulse); }
    }

    void CPhysicsSubsystem::AddTorque(ECS::FEntity Entity, FVector3 Torque)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->AddTorque(Entity, Torque); }
    }

    void CPhysicsSubsystem::AddAngularImpulse(ECS::FEntity Entity, FVector3 AngularImpulse)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->AddAngularImpulse(Entity, AngularImpulse); }
    }

    void CPhysicsSubsystem::AddForceAtPosition(ECS::FEntity Entity, FVector3 Force, FVector3 Position)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->AddForceAtPosition(Entity, Force, Position); }
    }

    void CPhysicsSubsystem::AddImpulseAtPosition(ECS::FEntity Entity, FVector3 Impulse, FVector3 Position)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->AddImpulseAtPosition(Entity, Impulse, Position); }
    }

    void CPhysicsSubsystem::SetLinearVelocity(ECS::FEntity Entity, FVector3 Velocity)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->SetLinearVelocity(Entity, Velocity); }
    }

    FVector3 CPhysicsSubsystem::GetLinearVelocity(ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene ? Scene->GetLinearVelocity(Entity) : FVector3(0.0f);
    }

    void CPhysicsSubsystem::SetAngularVelocity(ECS::FEntity Entity, FVector3 Velocity)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->SetAngularVelocity(Entity, Velocity); }
    }

    FVector3 CPhysicsSubsystem::GetAngularVelocity(ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene ? Scene->GetAngularVelocity(Entity) : FVector3(0.0f);
    }

    SPhysicsBodyTarget CPhysicsSubsystem::GetRagdollBody(ECS::FEntity Entity, FName Bone)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene ? Scene->GetRagdollBoneTarget(Entity, Bone) : SPhysicsBodyTarget();
    }

    void CPhysicsSubsystem::AddImpulseToBody(SPhysicsBodyTarget Body, FVector3 Impulse, FVector3 Position)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->AddImpulseAtTarget(Body, Impulse, Position); }
    }

    void CPhysicsSubsystem::SetBodyLinearVelocity(SPhysicsBodyTarget Body, FVector3 Velocity)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->SetTargetLinearVelocity(Body, Velocity); }
    }

    void CPhysicsSubsystem::SetBodyAngularVelocity(SPhysicsBodyTarget Body, FVector3 Velocity)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->SetTargetAngularVelocity(Body, Velocity); }
    }

    FVector3 CPhysicsSubsystem::GetBodyLinearVelocity(SPhysicsBodyTarget Body)
    {
        Physics::FPhysicsBodyState State;
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene && Scene->TryGetTargetState(Body, State) ? State.LinearVelocity : FVector3(0.0f);
    }

    FVector3 CPhysicsSubsystem::GetVelocityAtPoint(ECS::FEntity Entity, FVector3 Point)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene ? Scene->GetVelocityAtPoint(Entity, Point) : FVector3(0.0f);
    }

    FVector3 CPhysicsSubsystem::GetBodyPosition(ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene ? Scene->GetBodyPosition(Entity) : FVector3(0.0f);
    }

    FQuat CPhysicsSubsystem::GetBodyRotation(ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene ? Scene->GetBodyRotation(Entity) : FQuat();
    }

    FVector3 CPhysicsSubsystem::GetCenterOfMass(ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene ? Scene->GetCenterOfMass(Entity) : FVector3(0.0f);
    }

    void CPhysicsSubsystem::SetGravityFactor(ECS::FEntity Entity, float Factor)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->SetGravityFactor(Entity, Factor); }
    }

    void CPhysicsSubsystem::ActivateBody(ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        if (Scene == nullptr)
        {
            return;
        }
        Scene->ActivateBody(Entity);
    }

    void CPhysicsSubsystem::DeactivateBody(ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        if (Scene == nullptr)
        {
            return;
        }
        Scene->DeactivateBody(Entity);
    }

    bool CPhysicsSubsystem::IsAwake(ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        if (Scene == nullptr)
        {
            return false;
        }
        return Scene->IsBodyActive(Entity);
    }

    int32 CPhysicsSubsystem::GetAwakeBodyCount()
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene != nullptr ? static_cast<int32>(Scene->GetAwakeBodyCount()) : 0;
    }

    void CPhysicsSubsystem::SetSurfaceVelocity(ECS::FEntity Entity, FVector3 Linear, FVector3 Angular)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->SetSurfaceVelocity(Entity, Linear, Angular); }
    }

    uint32 CPhysicsSubsystem::CreateConstraint(Physics::SConstraintDesc Desc)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene ? Scene->CreateConstraint(Desc) : 0;
    }

    uint32 CPhysicsSubsystem::CreateFixedConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
        float BreakForce)
    {
        CWorld* World = GetWorld();
        Physics::SConstraintDesc Desc;
        Desc.Type = EPhysicsConstraintType::Fixed;
        Desc.BodyA = BodyA;
        Desc.BodyB = BodyB;
        Desc.BreakForce = BreakForce;
        return CreateConstraint(Desc);
    }

    uint32 CPhysicsSubsystem::CreatePointConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
        FVector3 Pivot, float BreakForce)
    {
        CWorld* World = GetWorld();
        Physics::SConstraintDesc Desc;
        Desc.Type = EPhysicsConstraintType::Point;
        Desc.BodyA = BodyA;
        Desc.BodyB = BodyB;
        Desc.Anchor = Pivot;
        Desc.BreakForce = BreakForce;
        return CreateConstraint(Desc);
    }

    uint32 CPhysicsSubsystem::CreateDistanceConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
        FVector3 PointA, FVector3 PointB, float MinDistance, float MaxDistance, float Frequency,
        float Damping, float BreakForce)
    {
        CWorld* World = GetWorld();
        Physics::SConstraintDesc Desc;
        Desc.Type = EPhysicsConstraintType::Distance;
        Desc.BodyA = BodyA;
        Desc.BodyB = BodyB;
        Desc.Anchor = PointA;
        Desc.AnchorB = PointB;
        Desc.MinLimit = MinDistance;
        Desc.MaxLimit = MaxDistance;
        Desc.bHasLimits = MinDistance >= 0.0f || MaxDistance >= 0.0f;
        Desc.LimitFrequency = Frequency;
        Desc.LimitDamping = Damping;
        Desc.BreakForce = BreakForce;
        return CreateConstraint(Desc);
    }

    uint32 CPhysicsSubsystem::CreateHingeConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
        FVector3 Pivot, FVector3 Axis, float MinAngle, float MaxAngle, bool bLimited, float MaxFrictionTorque,
        float MotorTorqueLimit, float MotorFrequency, float MotorDamping, float BreakForce)
    {
        CWorld* World = GetWorld();
        Physics::SConstraintDesc Desc;
        Desc.Type = EPhysicsConstraintType::Hinge;
        Desc.BodyA = BodyA;
        Desc.BodyB = BodyB;
        Desc.Anchor = Pivot;
        Desc.Axis = Axis;
        Desc.MinLimit = MinAngle;
        Desc.MaxLimit = MaxAngle;
        Desc.bHasLimits = bLimited;
        Desc.MaxFriction = MaxFrictionTorque;
        Desc.MotorTorqueLimit = MotorTorqueLimit;
        Desc.MotorFrequency = MotorFrequency;
        Desc.MotorDamping = MotorDamping;
        Desc.BreakForce = BreakForce;
        return CreateConstraint(Desc);
    }

    uint32 CPhysicsSubsystem::CreateSliderConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
        FVector3 Pivot, FVector3 Axis, float MinDistance, float MaxDistance, bool bLimited,
        float MaxFrictionForce, float MotorForceLimit, float MotorFrequency, float MotorDamping,
        float BreakForce)
    {
        CWorld* World = GetWorld();
        Physics::SConstraintDesc Desc;
        Desc.Type = EPhysicsConstraintType::Slider;
        Desc.BodyA = BodyA;
        Desc.BodyB = BodyB;
        Desc.Anchor = Pivot;
        Desc.Axis = Axis;
        Desc.MinLimit = MinDistance;
        Desc.MaxLimit = MaxDistance;
        Desc.bHasLimits = bLimited;
        Desc.MaxFriction = MaxFrictionForce;
        Desc.MotorForceLimit = MotorForceLimit;
        Desc.MotorFrequency = MotorFrequency;
        Desc.MotorDamping = MotorDamping;
        Desc.BreakForce = BreakForce;
        return CreateConstraint(Desc);
    }

    uint32 CPhysicsSubsystem::CreateConeConstraint(ECS::FEntity BodyA, ECS::FEntity BodyB,
        FVector3 Pivot, FVector3 TwistAxis, float HalfConeAngle, float BreakForce)
    {
        CWorld* World = GetWorld();
        Physics::SConstraintDesc Desc;
        Desc.Type = EPhysicsConstraintType::Cone;
        Desc.BodyA = BodyA;
        Desc.BodyB = BodyB;
        Desc.Anchor = Pivot;
        Desc.Axis = TwistAxis;
        Desc.HalfConeAngle = HalfConeAngle;
        Desc.BreakForce = BreakForce;
        return CreateConstraint(Desc);
    }

    void CPhysicsSubsystem::DestroyConstraint(uint32 ConstraintID)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->DestroyConstraint(ConstraintID); }
    }

    void CPhysicsSubsystem::SetConstraintEnabled(uint32 ConstraintID, bool bEnabled)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->SetConstraintEnabled(ConstraintID, bEnabled); }
    }

    void CPhysicsSubsystem::SetConstraintMotor(uint32 ConstraintID,
        Physics::EConstraintMotorMode Mode, float Target)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(GetWorld())) { Scene->SetConstraintMotor(ConstraintID, Mode, Target); }
    }

    bool CPhysicsSubsystem::IsConstraintBroken(uint32 ConstraintID)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene != nullptr && Scene->IsConstraintBroken(ConstraintID);
    }

    float CPhysicsSubsystem::GetConstraintValue(uint32 ConstraintID)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene ? Scene->GetConstraintValue(ConstraintID) : 0.0f;
    }

    float CPhysicsSubsystem::GetMass(ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(GetWorld());
        return Scene ? Scene->GetBodyMass(Entity) : 0.0f;
    }
}
