#include "RuntimePCH.h"

#include "PhysicsLibrary.h"

#include "Physics/PhysicsScene.h"
#include "World/World.h"

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

        void StageIgnore(ECS::FEntity Entity, TFixedVector<ECS::FEntity, MaxInlineIgnoreEntities>& Out)
        {
            if (Entity != ECS::NullEntity)
            {
                Out.push_back(Entity);
            }
        }

        // The scene writes into a span, so the result vector is sized first and trimmed to what it wrote.
        template<typename TQuery>
        void RunQuery(TVector<ECS::FEntity>& Out, TQuery&& Query)
        {
            const size_t Start = Out.size();
            Out.resize(Start + DefaultQueryCapacity);
            const int32 Written = Query(TSpan<ECS::FEntity>(Out.data() + Start, DefaultQueryCapacity));
            Out.resize(Start + (size_t)(Written > 0 ? Written : 0));
        }
    }

    SRayResult CPhysicsLibrary::Raycast(CWorld* World, FVector3 Start, FVector3 End,
        ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        if (Scene == nullptr)
        {
            return SRayResult();
        }

        SRayCastSettings Settings;
        Settings.Start = Start;
        Settings.End = End;
        Settings.LayerMask = LayerMask;
        StageIgnore(IgnoreEntity, Settings.IgnoreEntities);

        TOptional<SRayResult> Result = Scene->CastRay(Settings);
        return Result.has_value() ? Result.value() : SRayResult();
    }

    void CPhysicsLibrary::RaycastAll(CWorld* World, FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity,
        ECollisionProfiles LayerMask, TVector<SRayResult>& Out)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
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

    void CPhysicsLibrary::SphereCast(CWorld* World, FVector3 Start, FVector3 End, float Radius,
        ECS::FEntity IgnoreEntity, TVector<SRayResult>& Out)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        if (Scene == nullptr)
        {
            return;
        }

        SSphereCastSettings Settings;
        Settings.Start = Start;
        Settings.End = End;
        Settings.Radius = Radius;
        StageIgnore(IgnoreEntity, Settings.IgnoreEntities);

        World->CastSphere(Settings, Out);
    }

    void CPhysicsLibrary::OverlapSphere(CWorld* World, FVector3 Center, float Radius,
        ECS::FEntity IgnoreEntity, TVector<ECS::FEntity>& Out)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        if (Scene == nullptr)
        {
            return;
        }

        TFixedVector<ECS::FEntity, Lumina::MaxInlineIgnoreEntities> Ignore;
        StageIgnore(IgnoreEntity, Ignore);
        RunQuery(Out, [&](TSpan<ECS::FEntity> Results)
        {
            return Scene->OverlapSphere(Center, Radius, Ignore, Results);
        });
    }

    void CPhysicsLibrary::OverlapBox(CWorld* World, FVector3 Center, FVector3 HalfExtents, FQuat Rotation,
        ECS::FEntity IgnoreEntity, TVector<ECS::FEntity>& Out)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        if (Scene == nullptr)
        {
            return;
        }

        TFixedVector<ECS::FEntity, Lumina::MaxInlineIgnoreEntities> Ignore;
        StageIgnore(IgnoreEntity, Ignore);
        RunQuery(Out, [&](TSpan<ECS::FEntity> Results)
        {
            return Scene->OverlapBox(Center, HalfExtents, Rotation, Ignore, Results);
        });
    }

    void CPhysicsLibrary::OverlapPoint(CWorld* World, FVector3 Point, ECS::FEntity IgnoreEntity,
        TVector<ECS::FEntity>& Out)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        if (Scene == nullptr)
        {
            return;
        }

        TFixedVector<ECS::FEntity, Lumina::MaxInlineIgnoreEntities> Ignore;
        StageIgnore(IgnoreEntity, Ignore);
        RunQuery(Out, [&](TSpan<ECS::FEntity> Results)
        {
            return Scene->CollidePoint(Point, Ignore, Results);
        });
    }

    float CPhysicsLibrary::GetGroundHeight(CWorld* World, FVector3 Point, float SearchDistance, ECS::FEntity IgnoreEntity, float Fallback)
    {
        const FVector3 Start(Point.x, Point.y + SearchDistance, Point.z);
        const FVector3 End(Point.x, Point.y - SearchDistance, Point.z);
        const SRayResult Hit = Raycast(World, Start, End, IgnoreEntity);
        return Hit.bHit ? Hit.Location.y : Fallback;
    }

    bool CPhysicsLibrary::IsSpaceFree(CWorld* World, FVector3 Center, float Radius, ECS::FEntity IgnoreEntity)
    {
        TVector<ECS::FEntity> Overlaps;
        OverlapSphere(World, Center, Radius, IgnoreEntity, Overlaps);
        return Overlaps.empty();
    }

    void CPhysicsLibrary::AddForce(CWorld* World, ECS::FEntity Entity, FVector3 Force)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->AddForce(Entity, Force); }
    }

    void CPhysicsLibrary::AddImpulse(CWorld* World, ECS::FEntity Entity, FVector3 Impulse)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->AddImpulse(Entity, Impulse); }
    }

    void CPhysicsLibrary::AddTorque(CWorld* World, ECS::FEntity Entity, FVector3 Torque)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->AddTorque(Entity, Torque); }
    }

    void CPhysicsLibrary::AddAngularImpulse(CWorld* World, ECS::FEntity Entity, FVector3 AngularImpulse)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->AddAngularImpulse(Entity, AngularImpulse); }
    }

    void CPhysicsLibrary::AddForceAtPosition(CWorld* World, ECS::FEntity Entity, FVector3 Force, FVector3 Position)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->AddForceAtPosition(Entity, Force, Position); }
    }

    void CPhysicsLibrary::AddImpulseAtPosition(CWorld* World, ECS::FEntity Entity, FVector3 Impulse, FVector3 Position)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->AddImpulseAtPosition(Entity, Impulse, Position); }
    }

    void CPhysicsLibrary::SetLinearVelocity(CWorld* World, ECS::FEntity Entity, FVector3 Velocity)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->SetLinearVelocity(Entity, Velocity); }
    }

    FVector3 CPhysicsLibrary::GetLinearVelocity(CWorld* World, ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        return Scene ? Scene->GetLinearVelocity(Entity) : FVector3(0.0f);
    }

    void CPhysicsLibrary::SetAngularVelocity(CWorld* World, ECS::FEntity Entity, FVector3 Velocity)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->SetAngularVelocity(Entity, Velocity); }
    }

    FVector3 CPhysicsLibrary::GetAngularVelocity(CWorld* World, ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        return Scene ? Scene->GetAngularVelocity(Entity) : FVector3(0.0f);
    }

    FVector3 CPhysicsLibrary::GetVelocityAtPoint(CWorld* World, ECS::FEntity Entity, FVector3 Point)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        return Scene ? Scene->GetVelocityAtPoint(Entity, Point) : FVector3(0.0f);
    }

    FVector3 CPhysicsLibrary::GetBodyPosition(CWorld* World, ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        return Scene ? Scene->GetBodyPosition(Entity) : FVector3(0.0f);
    }

    FQuat CPhysicsLibrary::GetBodyRotation(CWorld* World, ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        return Scene ? Scene->GetBodyRotation(Entity) : FQuat();
    }

    FVector3 CPhysicsLibrary::GetCenterOfMass(CWorld* World, ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        return Scene ? Scene->GetCenterOfMass(Entity) : FVector3(0.0f);
    }

    float CPhysicsLibrary::GetMass(CWorld* World, ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        return Scene ? Scene->GetBodyMass(Entity) : 0.0f;
    }

    void CPhysicsLibrary::SetGravityFactor(CWorld* World, ECS::FEntity Entity, float Factor)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->SetGravityFactor(Entity, Factor); }
    }

    void CPhysicsLibrary::ActivateBody(CWorld* World, ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        if (Scene == nullptr)
        {
            return;
        }
        Scene->ActivateBody(Entity);
    }

    void CPhysicsLibrary::DeactivateBody(CWorld* World, ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        if (Scene == nullptr)
        {
            return;
        }
        Scene->DeactivateBody(Entity);
    }

    bool CPhysicsLibrary::IsAwake(CWorld* World, ECS::FEntity Entity)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        if (Scene == nullptr)
        {
            return false;
        }
        return Scene->IsBodyActive(Entity);
    }

    void CPhysicsLibrary::SetSurfaceVelocity(CWorld* World, ECS::FEntity Entity, FVector3 Linear, FVector3 Angular)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->SetSurfaceVelocity(Entity, Linear, Angular); }
    }

    uint32 CPhysicsLibrary::CreateConstraint(CWorld* World, Physics::FConstraintDesc Desc)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        return Scene ? Scene->CreateConstraint(Desc) : 0;
    }

    uint32 CPhysicsLibrary::CreateFixedConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
        float BreakForce)
    {
        Physics::FConstraintDesc Desc;
        Desc.Type = EPhysicsConstraintType::Fixed;
        Desc.BodyA = BodyA;
        Desc.BodyB = BodyB;
        Desc.BreakForce = BreakForce;
        return CreateConstraint(World, Desc);
    }

    uint32 CPhysicsLibrary::CreatePointConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
        FVector3 Pivot, float BreakForce)
    {
        Physics::FConstraintDesc Desc;
        Desc.Type = EPhysicsConstraintType::Point;
        Desc.BodyA = BodyA;
        Desc.BodyB = BodyB;
        Desc.Anchor = Pivot;
        Desc.BreakForce = BreakForce;
        return CreateConstraint(World, Desc);
    }

    uint32 CPhysicsLibrary::CreateDistanceConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
        FVector3 PointA, FVector3 PointB, float MinDistance, float MaxDistance, float Frequency,
        float Damping, float BreakForce)
    {
        Physics::FConstraintDesc Desc;
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
        return CreateConstraint(World, Desc);
    }

    uint32 CPhysicsLibrary::CreateHingeConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
        FVector3 Pivot, FVector3 Axis, float MinAngle, float MaxAngle, bool bLimited, float MaxFrictionTorque,
        float MotorTorqueLimit, float MotorFrequency, float MotorDamping, float BreakForce)
    {
        Physics::FConstraintDesc Desc;
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
        return CreateConstraint(World, Desc);
    }

    uint32 CPhysicsLibrary::CreateSliderConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
        FVector3 Pivot, FVector3 Axis, float MinDistance, float MaxDistance, bool bLimited,
        float MaxFrictionForce, float MotorForceLimit, float MotorFrequency, float MotorDamping,
        float BreakForce)
    {
        Physics::FConstraintDesc Desc;
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
        return CreateConstraint(World, Desc);
    }

    uint32 CPhysicsLibrary::CreateConeConstraint(CWorld* World, ECS::FEntity BodyA, ECS::FEntity BodyB,
        FVector3 Pivot, FVector3 TwistAxis, float HalfConeAngle, float BreakForce)
    {
        Physics::FConstraintDesc Desc;
        Desc.Type = EPhysicsConstraintType::Cone;
        Desc.BodyA = BodyA;
        Desc.BodyB = BodyB;
        Desc.Anchor = Pivot;
        Desc.Axis = TwistAxis;
        Desc.HalfConeAngle = HalfConeAngle;
        Desc.BreakForce = BreakForce;
        return CreateConstraint(World, Desc);
    }

    void CPhysicsLibrary::DestroyConstraint(CWorld* World, uint32 ConstraintID)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->DestroyConstraint(ConstraintID); }
    }

    void CPhysicsLibrary::SetConstraintEnabled(CWorld* World, uint32 ConstraintID, bool bEnabled)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->SetConstraintEnabled(ConstraintID, bEnabled); }
    }

    void CPhysicsLibrary::SetConstraintMotor(CWorld* World, uint32 ConstraintID,
        Physics::EConstraintMotorMode Mode, float Target)
    {
        if (Physics::IPhysicsScene* Scene = SceneOf(World)) { Scene->SetConstraintMotor(ConstraintID, Mode, Target); }
    }

    bool CPhysicsLibrary::IsConstraintBroken(CWorld* World, uint32 ConstraintID)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        return Scene != nullptr && Scene->IsConstraintBroken(ConstraintID);
    }

    float CPhysicsLibrary::GetConstraintValue(CWorld* World, uint32 ConstraintID)
    {
        Physics::IPhysicsScene* Scene = SceneOf(World);
        return Scene ? Scene->GetConstraintValue(ConstraintID) : 0.0f;
    }
}
