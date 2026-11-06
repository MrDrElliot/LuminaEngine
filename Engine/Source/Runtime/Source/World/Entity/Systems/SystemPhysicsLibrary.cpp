#include "RuntimePCH.h"
#include "SystemPhysicsLibrary.h"

#include "Physics/PhysicsScene.h"

namespace Lumina
{
    namespace SystemPhysicsLibraryInternal
    {
        constexpr int32 MaxOverlapResults = 16384;

        TSpan<const ECS::FEntity> IgnoreOf(const ECS::FEntity& Entity)
        {
            return TSpan<const ECS::FEntity>(&Entity, Entity.IsNull() ? 0 : 1);
        }

        SRayResult MissOf(const FVector3& Start, const FVector3& End)
        {
            SRayResult Miss;
            Miss.Start = Start;
            Miss.End = End;
            return Miss;
        }

        SRayCastSettings RayOf(const FVector3& Start, const FVector3& End, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
        {
            SRayCastSettings Settings;
            Settings.Start = Start;
            Settings.End = End;
            Settings.LayerMask = LayerMask;
            if (!IgnoreEntity.IsNull())
            {
                Settings.IgnoreEntities.Add(IgnoreEntity);
            }
            return Settings;
        }
    }

    using namespace SystemPhysicsLibraryInternal;

    SRayResult CSystemPhysicsLibrary::Raycast(const SSystemContext& Context, FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        TOptional<SRayResult> Hit = Scene != nullptr ? Scene->CastRay(RayOf(Start, End, IgnoreEntity, LayerMask)) : TOptional<SRayResult>();
        return Hit.IsSet() ? Hit.GetValue() : MissOf(Start, End);
    }

    void CSystemPhysicsLibrary::RaycastAll(const SSystemContext& Context, FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask, TVector<SRayResult>& Out)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics())
        {
            Scene->CastRayAll(RayOf(Start, End, IgnoreEntity, LayerMask), Out);
        }
    }

    SRayResult CSystemPhysicsLibrary::RaycastEntity(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Start, FVector3 End)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        TOptional<SRayResult> Hit = Scene != nullptr ? Scene->CastRayAgainst(Entity, Start, End) : TOptional<SRayResult>();
        return Hit.IsSet() ? Hit.GetValue() : MissOf(Start, End);
    }

    SRayResult CSystemPhysicsLibrary::ShapeCast(const SSystemContext& Context, SQueryShape Shape, FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        TOptional<SRayResult> Hit = Scene != nullptr ? Scene->CastShapeClosest(Shape, Start, End, LayerMask, IgnoreOf(IgnoreEntity)) : TOptional<SRayResult>();
        return Hit.IsSet() ? Hit.GetValue() : MissOf(Start, End);
    }

    void CSystemPhysicsLibrary::ShapeCastAll(const SSystemContext& Context, SQueryShape Shape, FVector3 Start, FVector3 End, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask, TVector<SRayResult>& Out)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics())
        {
            Scene->CastShapeAll(Shape, Start, End, LayerMask, IgnoreOf(IgnoreEntity), Out);
        }
    }

    void CSystemPhysicsLibrary::Overlap(const SSystemContext& Context, SQueryShape Shape, FVector3 Center, ECS::FEntity IgnoreEntity, ECollisionProfiles LayerMask, TVector<ECS::FEntity>& Out)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        if (Scene == nullptr)
        {
            return;
        }

        int32 Capacity = 64;
        int32 Written = 0;
        for (;;)
        {
            Out.SetNum(Capacity);
            Written = Math::Max(Scene->OverlapShape(Shape, Center, LayerMask, IgnoreOf(IgnoreEntity), TSpan<ECS::FEntity>(Out.GetData(), (size_t)Capacity)), 0);
            if (Written < Capacity || Capacity >= MaxOverlapResults)
            {
                break;
            }
            Capacity *= 2;
        }
        Out.SetNum(Written);
    }

    bool CSystemPhysicsLibrary::IsAwake(const SSystemContext& Context, ECS::FEntity Entity)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        return Scene != nullptr && Scene->IsBodyActive(Entity);
    }

    int32 CSystemPhysicsLibrary::GetAwakeBodyCount(const SSystemContext& Context)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        return Scene != nullptr ? static_cast<int32>(Scene->GetAwakeBodyCount()) : 0;
    }

    FVector3 CSystemPhysicsLibrary::GetBodyPosition(const SSystemContext& Context, ECS::FEntity Entity)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        return Scene != nullptr ? Scene->GetBodyPosition(Entity) : FVector3(0.0f);
    }

    FQuat CSystemPhysicsLibrary::GetBodyRotation(const SSystemContext& Context, ECS::FEntity Entity)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        return Scene != nullptr ? Scene->GetBodyRotation(Entity) : FQuat::Identity();
    }

    FVector3 CSystemPhysicsLibrary::GetLinearVelocity(const SSystemContext& Context, ECS::FEntity Entity)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        return Scene != nullptr ? Scene->GetLinearVelocity(Entity) : FVector3(0.0f);
    }

    FVector3 CSystemPhysicsLibrary::GetAngularVelocity(const SSystemContext& Context, ECS::FEntity Entity)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        return Scene != nullptr ? Scene->GetAngularVelocity(Entity) : FVector3(0.0f);
    }

    FVector3 CSystemPhysicsLibrary::GetVelocityAtPoint(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Point)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        return Scene != nullptr ? Scene->GetVelocityAtPoint(Entity, Point) : FVector3(0.0f);
    }

    FVector3 CSystemPhysicsLibrary::GetCenterOfMass(const SSystemContext& Context, ECS::FEntity Entity)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        return Scene != nullptr ? Scene->GetCenterOfMass(Entity) : FVector3(0.0f);
    }

    float CSystemPhysicsLibrary::GetMass(const SSystemContext& Context, ECS::FEntity Entity)
    {
        Physics::IPhysicsQueries* Scene = Context.GetPhysics();
        return Scene != nullptr ? Scene->GetBodyMass(Entity) : 0.0f;
    }

    SAABB CSystemPhysicsLibrary::GetBodyBounds(const SSystemContext& Context, ECS::FEntity Entity)
    {
        SAABB Bounds;
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics())
        {
            Scene->GetBodyBounds(Entity, Bounds);
        }
        return Bounds;
    }

    void CSystemPhysicsLibrary::AddForce(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Force)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics()) { Scene->AddForce(Entity, Force); }
    }

    void CSystemPhysicsLibrary::AddImpulse(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Impulse)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics()) { Scene->AddImpulse(Entity, Impulse); }
    }

    void CSystemPhysicsLibrary::AddTorque(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Torque)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics()) { Scene->AddTorque(Entity, Torque); }
    }

    void CSystemPhysicsLibrary::AddAngularImpulse(const SSystemContext& Context, ECS::FEntity Entity, FVector3 AngularImpulse)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics()) { Scene->AddAngularImpulse(Entity, AngularImpulse); }
    }

    void CSystemPhysicsLibrary::AddForceAtPosition(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Force, FVector3 Position)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics()) { Scene->AddForceAtPosition(Entity, Force, Position); }
    }

    void CSystemPhysicsLibrary::AddImpulseAtPosition(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Impulse, FVector3 Position)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics()) { Scene->AddImpulseAtPosition(Entity, Impulse, Position); }
    }

    void CSystemPhysicsLibrary::SetLinearVelocity(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Velocity)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics()) { Scene->SetLinearVelocity(Entity, Velocity); }
    }

    void CSystemPhysicsLibrary::SetAngularVelocity(const SSystemContext& Context, ECS::FEntity Entity, FVector3 Velocity)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics()) { Scene->SetAngularVelocity(Entity, Velocity); }
    }

    void CSystemPhysicsLibrary::SetGravityFactor(const SSystemContext& Context, ECS::FEntity Entity, float Factor)
    {
        if (Physics::IPhysicsQueries* Scene = Context.GetPhysics()) { Scene->SetGravityFactor(Entity, Factor); }
    }
}
