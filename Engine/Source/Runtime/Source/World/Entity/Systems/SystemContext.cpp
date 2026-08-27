#include "RuntimePCH.h"
#include "SystemContext.h"
#include "World/ECS/Registry.h"
#include "World/World.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/NameComponent.h"

namespace Lumina
{
    namespace
    {
        // Honest-access checks for the access-implying context helpers (no-op in Shipping / outside a system).
        void CheckTransformWrite()
        {
            ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<STransformComponent>()), true, "Write<STransformComponent>");
        }
        void CheckPhysics(bool bWrite)
        {
            ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<SystemResource::PhysicsQuery>()), bWrite,
                bWrite ? "Write<SystemResource::PhysicsQuery>" : "Read<SystemResource::PhysicsQuery>");
        }
        void CheckStructure()
        {
            ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<SystemResource::EntityStructure>()), true, "Write<SystemResource::EntityStructure>");
        }
    }

    FSystemContext::FSystemContext(CWorld* InWorld)
        : World(InWorld)
        , Registry(InWorld->EntityRegistry)
        , Dispatcher(InWorld->SingletonDispatcher)
    {}


    void FSystemContext::SetEntityLifetime(ECS::FEntity Entity, float Lifetime) const
    {
        Registry.GetOrEmplace<SLifetimeComponent>(Entity).Lifetime = Lifetime;
    }

    // Read, not write, since a declared Write satisfies it and only a total absence is caught.
    Physics::IPhysicsScene* FSystemContext::GetPhysicsScene() const
    {
        CheckPhysics(false);
        return World ? World->GetPhysicsScene() : nullptr;
    }

    void FSystemContext::ActivateBody(uint32 BodyID)
    {
        CheckPhysics(true);
        World->PhysicsScene->ActivateBody(BodyID);
    }

    void FSystemContext::DeactivateBody(uint32 BodyID)
    {
        CheckPhysics(true);
        World->PhysicsScene->DeactivateBody(BodyID);
    }

    void FSystemContext::ChangeBodyMotionType(uint32 BodyID, EBodyType NewType)
    {
        CheckPhysics(true);
        World->PhysicsScene->ChangeBodyMotionType(BodyID, NewType);
    }

    uint32 FSystemContext::GetEntityBodyID(ECS::FEntity Entity) const
    {
        CheckPhysics(false);
        return World->PhysicsScene ? World->PhysicsScene->GetEntityBodyID(Entity) : ~0u;
    }

    FVector3 FSystemContext::GetBodyPosition(ECS::FEntity Entity) const
    {
        CheckPhysics(false);
        return World->PhysicsScene ? World->PhysicsScene->GetBodyPosition(Entity) : FVector3(0.0f);
    }

    FQuat FSystemContext::GetBodyRotation(ECS::FEntity Entity) const
    {
        CheckPhysics(false);
        return World->PhysicsScene ? World->PhysicsScene->GetBodyRotation(Entity) : FQuat();
    }

    FVector3 FSystemContext::GetVelocityAtPoint(ECS::FEntity Entity, const FVector3& Point) const
    {
        CheckPhysics(false);
        return World->PhysicsScene ? World->PhysicsScene->GetVelocityAtPoint(Entity, Point) : FVector3(0.0f);
    }

    void FSystemContext::AddForceAtPosition(ECS::FEntity Entity, const FVector3& Force, const FVector3& Position) const
    {
        CheckPhysics(true);
        if (World->PhysicsScene)
        {
            World->PhysicsScene->AddForceAtPosition(Entity, Force, Position);
        }
    }

    void FSystemContext::ApplyBuoyancyImpulse(ECS::FEntity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
        float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float InDeltaTime) const
    {
        CheckPhysics(true);
        if (World->PhysicsScene)
        {
            World->PhysicsScene->ApplyBuoyancyImpulse(Entity, SurfacePosition, SurfaceNormal, Buoyancy, LinearDrag, AngularDrag, FluidVelocity, InDeltaTime);
        }
    }


    void FSystemContext::CastSphere(const SSphereCastSettings& Settings, TVector<SRayResult>& OutHits) const
    {
        CheckPhysics(false);
        World->CastSphere(Settings, OutHits);
    }

    TOptional<SRayResult> FSystemContext::CastSphereClosest(const SSphereCastSettings& Settings) const
    {
        CheckPhysics(false);
        return World->CastSphereClosest(Settings);
    }

    STransformComponent& FSystemContext::GetEntityTransform(ECS::FEntity Entity) const
    {
        CheckTransformWrite(); // returns a mutable ref -> caller may write
        return Get<STransformComponent>(Entity);
    }

    FVector3 FSystemContext::TranslateEntity(ECS::FEntity Entity, const FVector3& Translation)
    {
        CheckTransformWrite();
        FVector3 NewLocation = Registry.Get<STransformComponent>(Entity).Translate(Translation);
        return NewLocation;
    }

    void FSystemContext::SetEntityLocation(ECS::FEntity Entity, const FVector3& Location)
    {
        CheckTransformWrite();
        Registry.Get<STransformComponent>(Entity).SetLocation(Location);
    }

    void FSystemContext::SetEntityRotation(ECS::FEntity Entity, const FQuat& Rotation)
    {
        CheckTransformWrite();
        Registry.Get<STransformComponent>(Entity).SetRotation(Rotation);
    }

    void FSystemContext::SetEntityScale(ECS::FEntity Entity, const FVector3& Scale)
    {
        CheckTransformWrite();
        Registry.Get<STransformComponent>(Entity).SetScale(Scale);
    }

    void FSystemContext::DrawDebugLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness, float Duration) const
    {
        World->DrawLine(Start, End, Color, Thickness, true, Duration);
    }

    void FSystemContext::DrawDebugBox(const FVector3& Center, const FVector3& Extents, const FQuat& Rotation, const FVector4& Color, float Thickness, float Duration) const
    {
        World->DrawBox(Center, Extents, Rotation, Color, Thickness, true, Duration);
    }

    void FSystemContext::DrawDebugSphere(const FVector3& Center, float Radius, const FVector4& Color, uint8 Segments, float Thickness, float Duration) const
    {
        World->DrawSphere(Center, Radius, Color, Segments, Thickness, true, Duration);
    }

    void FSystemContext::DrawDebugCone(const FVector3& Apex, const FVector3& Direction, float AngleRadians, float Length, const FVector4& Color, uint8 Segments, uint8 Stacks, float Thickness, float Duration) const
    {
        World->DrawCone(Apex, Direction, AngleRadians, Length, Color, Segments, Stacks, Thickness, true, Duration);
    }

    void FSystemContext::DrawFrustum(const FMatrix4& Matrix, float zNear, float zFar, const FVector4& Color, float Thickness, float Duration) const
    {
        World->DrawFrustum(Matrix, zNear, zFar, Color, Thickness, true, Duration);
    }

    void FSystemContext::DrawDebugArrow(const FVector3& Start, const FVector3& Direction, float Length, const FVector4& Color, float Thickness, float Duration, float HeadSize) const
    {
        World->DrawArrow(Start, Direction, Length, Color, Thickness, true, Duration, HeadSize);
    }

    void FSystemContext::DrawDebugSolidTriangles(TVector<FSimpleElementVertex>&& Vertices, ESolidDrawMode Mode, float Duration) const
    {
        World->DrawSolidTriangles(std::move(Vertices), Mode, Duration);
    }

    ECS::FEntity FSystemContext::Create(const FTransform& Transform, FName EntityName) const
    {
        LUMINA_PROFILE_SCOPE();
        CheckStructure();

        ECS::FEntity EntityID = Registry.Create();
        Registry.Emplace<STransformComponent>(EntityID).SetWorldTransform(Transform);
        Registry.Emplace<SNameComponent>(EntityID, EntityName);
        Registry.EmplaceOrReplace<FNeedsTransformUpdate>(EntityID);
        return EntityID;
    }
    
    ECS::FEntity FSystemContext::Create(FVector3 Location, FName EntityName) const
    {
        LUMINA_PROFILE_SCOPE();
        CheckStructure();

        ECS::FEntity EntityID = Registry.Create();
        Registry.Emplace<STransformComponent>(EntityID).SetLocation(Location);
        Registry.Emplace<SNameComponent>(EntityID, EntityName);
        Registry.EmplaceOrReplace<FNeedsTransformUpdate>(EntityID);
        return EntityID;
    }
    
    ECS::FEntity FSystemContext::Create(FName EntityName) const
    {
        LUMINA_PROFILE_SCOPE();
        CheckStructure();

        ECS::FEntity EntityID = Registry.Create();
        Registry.Emplace<STransformComponent>(EntityID);
        Registry.Emplace<SNameComponent>(EntityID, EntityName);
        Registry.EmplaceOrReplace<FNeedsTransformUpdate>(EntityID);
        return EntityID;
    }

    size_t FSystemContext::GetNumEntities() const
    {
        return Registry.NumEntities();
    }

    bool FSystemContext::IsValidEntity(ECS::FEntity Entity) const
    {
        return Registry.IsValid(Entity);
    }

    EWorldType FSystemContext::GetWorldType() const
    {
        return World->GetWorldType();
    }
}
