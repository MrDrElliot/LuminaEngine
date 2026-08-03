#include "RuntimePCH.h"
#include "SystemContext.h"
#include "World/World.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "world/entity/components/namecomponent.h"

namespace Lumina
{
    namespace
    {
        // Honest-access checks for the access-implying context helpers (no-op in Shipping / outside a system).
        void CheckTransformWrite()
        {
            ValidateSystemAccess(static_cast<uint32>(entt::type_hash<STransformComponent>::value()), true, "Write<STransformComponent>");
        }
        void CheckPhysics(bool bWrite)
        {
            ValidateSystemAccess(static_cast<uint32>(entt::type_hash<SystemResource::PhysicsQuery>::value()), bWrite,
                bWrite ? "Write<SystemResource::PhysicsQuery>" : "Read<SystemResource::PhysicsQuery>");
        }
        void CheckStructure()
        {
            ValidateSystemAccess(static_cast<uint32>(entt::type_hash<SystemResource::EntityStructure>::value()), true, "Write<SystemResource::EntityStructure>");
        }
    }

    FSystemContext::FSystemContext(CWorld* InWorld)
        : World(InWorld)
        , Registry(InWorld->EntityRegistry)
        , Dispatcher(InWorld->SingletonDispatcher)
    {}


    void FSystemContext::SetEntityLifetime(entt::entity Entity, float Lifetime) const
    {
        Registry.get_or_emplace<SLifetimeComponent>(Entity).Lifetime = Lifetime;
    }

    entt::runtime_view FSystemContext::CreateRuntimeView(const THashSet<entt::id_type>& Components) const
    {
        entt::runtime_view RuntimeView;
        
        for (entt::id_type Type : Components)
        {
            entt::meta_type Meta = entt::resolve(Type);
            if (!Meta)
            {
                if (entt::basic_sparse_set<>* Storage = Registry.storage(Type))
                {
                    RuntimeView.iterate(*Storage);
                }
            }
            else if (entt::basic_sparse_set<>* Storage = Registry.storage(Meta.info().hash()))
            {
                RuntimeView.iterate(*Storage);
            }
        }

        return RuntimeView;
    }

    Physics::IPhysicsScene* FSystemContext::GetPhysicsScene() const
    {
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

    uint32 FSystemContext::GetEntityBodyID(entt::entity Entity) const
    {
        CheckPhysics(false);
        return World->PhysicsScene ? World->PhysicsScene->GetEntityBodyID(Entity) : ~0u;
    }

    FVector3 FSystemContext::GetBodyPosition(entt::entity Entity) const
    {
        CheckPhysics(false);
        return World->PhysicsScene ? World->PhysicsScene->GetBodyPosition(Entity) : FVector3(0.0f);
    }

    FQuat FSystemContext::GetBodyRotation(entt::entity Entity) const
    {
        CheckPhysics(false);
        return World->PhysicsScene ? World->PhysicsScene->GetBodyRotation(Entity) : FQuat();
    }

    FVector3 FSystemContext::GetVelocityAtPoint(entt::entity Entity, const FVector3& Point) const
    {
        CheckPhysics(false);
        return World->PhysicsScene ? World->PhysicsScene->GetVelocityAtPoint(Entity, Point) : FVector3(0.0f);
    }

    void FSystemContext::AddForceAtPosition(entt::entity Entity, const FVector3& Force, const FVector3& Position) const
    {
        CheckPhysics(true);
        if (World->PhysicsScene)
        {
            World->PhysicsScene->AddForceAtPosition(Entity, Force, Position);
        }
    }

    void FSystemContext::ApplyBuoyancyImpulse(entt::entity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
        float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float InDeltaTime) const
    {
        CheckPhysics(true);
        if (World->PhysicsScene)
        {
            World->PhysicsScene->ApplyBuoyancyImpulse(Entity, SurfacePosition, SurfaceNormal, Buoyancy, LinearDrag, AngularDrag, FluidVelocity, InDeltaTime);
        }
    }


    TVector<SRayResult> FSystemContext::CastSphere(const SSphereCastSettings& Settings) const
    {
        CheckPhysics(false);
        return World->CastSphere(Settings);
    }

    STransformComponent& FSystemContext::GetEntityTransform(entt::entity Entity) const
    {
        CheckTransformWrite(); // returns a mutable ref -> caller may write
        return Get<STransformComponent>(Entity);
    }

    FVector3 FSystemContext::TranslateEntity(entt::entity Entity, const FVector3& Translation)
    {
        CheckTransformWrite();
        FVector3 NewLocation = Registry.get<STransformComponent>(Entity).Translate(Translation);
        return NewLocation;
    }

    void FSystemContext::SetEntityLocation(entt::entity Entity, const FVector3& Location)
    {
        CheckTransformWrite();
        Registry.get<STransformComponent>(Entity).SetLocation(Location);
    }

    void FSystemContext::SetEntityRotation(entt::entity Entity, const FQuat& Rotation)
    {
        CheckTransformWrite();
        Registry.get<STransformComponent>(Entity).SetRotation(Rotation);
    }

    void FSystemContext::SetEntityScale(entt::entity Entity, const FVector3& Scale)
    {
        CheckTransformWrite();
        Registry.get<STransformComponent>(Entity).SetScale(Scale);
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
    
    entt::entity FSystemContext::Create(FVector3 Location) const
    {
        LUMINA_PROFILE_SCOPE();
        CheckStructure();

        entt::entity EntityID = Registry.create();
        Registry.emplace<STransformComponent>(EntityID).SetLocation(Location);
        Registry.emplace<SNameComponent>(EntityID).Name = "Entity";
        Registry.emplace_or_replace<FNeedsTransformUpdate>(EntityID);
        return EntityID;
    }
    
    entt::entity FSystemContext::Create() const
    {
        LUMINA_PROFILE_SCOPE();
        CheckStructure();

        entt::entity EntityID = Registry.create();
        Registry.emplace<STransformComponent>(EntityID);
        Registry.emplace<SNameComponent>(EntityID).Name = "Entity";
        Registry.emplace_or_replace<FNeedsTransformUpdate>(EntityID);
        return EntityID;
    }

    size_t FSystemContext::GetNumEntities() const
    {
        return Registry.storage<entt::entity>().size();
    }

    bool FSystemContext::IsValidEntity(entt::entity Entity) const
    {
        return Registry.valid(Entity);
    }

    EWorldType FSystemContext::GetWorldType() const
    {
        return World->GetWorldType();
    }
}
