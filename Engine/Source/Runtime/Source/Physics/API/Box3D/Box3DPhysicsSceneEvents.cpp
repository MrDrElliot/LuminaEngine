#include "RuntimePCH.h"
#include "Box3DPhysicsScene.h"

#include "Box3DCharacterHandle.h"
#include "Box3DInternal.h"
#include "Box3DUtils.h"

#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "Log/Log.h"
#include "Scripting/EntityScript.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Events/CollisionEvent.h"
#include "World/World.h"

namespace Lumina::Physics
{
    namespace
    {
        // Orient a contact record for one receiving side; POD fill, no per-event table built downstream.
        SCollisionEvent BuildCollisionEvent(entt::entity SelfEntity, entt::entity OtherEntity,
                                           uint32 SelfBodyID, uint32 OtherBodyID,
                                           const FContactRecord& Record, bool bFlipNormal)
        {
            SCollisionEvent Event;
            Event.Entity = SelfEntity;
            Event.Other = OtherEntity;
            Event.BodyID = SelfBodyID;
            Event.OtherBodyID = OtherBodyID;
            Event.Point = Record.Point;

            // The normal points outward from self, so a script can react with its negation to bounce away.
            Event.Normal = bFlipNormal ? -Record.Normal : Record.Normal;
            Event.Velocity = bFlipNormal ? Record.VelocityB : Record.VelocityA;
            Event.OtherVelocity = bFlipNormal ? Record.VelocityA : Record.VelocityB;
            Event.RelativeVelocity = Event.OtherVelocity - Event.Velocity;
            Event.ImpactSpeed = Record.ImpactSpeed;

            Event.bIsTrigger = bFlipNormal ? Record.bSensorA : Record.bSensorB;
            return Event;
        }
    }

    void FBox3DPhysicsScene::DrainStepEvents()
    {
        LUMINA_PROFILE_SCOPE();

        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        auto& RigidStorage = Registry.storage<SRigidBodyComponent>();

        // Only an entity that can actually receive the event is worth resolving a manifold for.
        auto WantsContactEvents = [&](entt::entity Entity, bool bAdded, bool bOverlap)
        {
            if (Entity == entt::null || !RigidStorage.contains(Entity))
            {
                return false;
            }

            if (Registry.all_of<SEntityScriptComponent>(Entity))
            {
                return true;
            }

            const SRigidBodyComponent& Body = RigidStorage.get(Entity);
            const TScriptDelegate<SCollisionEvent>& Delegate = bOverlap
                ? (bAdded ? Body.OnOverlapBegin : Body.OnOverlapEnd)
                : (bAdded ? Body.OnContactBegin : Body.OnContactEnd);
            return Delegate.IsBound();
        };

        auto FillPair = [&](FContactRecord& Record, b3ShapeId ShapeA, b3ShapeId ShapeB)
        {
            const b3BodyId BodyA = b3Shape_GetBody(ShapeA);
            const b3BodyId BodyB = b3Shape_GetBody(ShapeB);

            void* UserA = b3Body_IsValid(BodyA) ? b3Body_GetUserData(BodyA) : nullptr;
            void* UserB = b3Body_IsValid(BodyB) ? b3Body_GetUserData(BodyB) : nullptr;

            Record.EntityA = UnpackEntity(UserA);
            Record.EntityB = UnpackEntity(UserB);
            Record.BodyIDA = UnpackHandle(UserA);
            Record.BodyIDB = UnpackHandle(UserB);
            Record.bSensorA = b3Shape_IsSensor(ShapeA);
            Record.bSensorB = b3Shape_IsSensor(ShapeB);
            Record.VelocityA = b3Body_IsValid(BodyA) ? Box3DUtils::FromB3Vec3(b3Body_GetLinearVelocity(BodyA)) : FVector3(0.0f);
            Record.VelocityB = b3Body_IsValid(BodyB) ? Box3DUtils::FromB3Vec3(b3Body_GetLinearVelocity(BodyB)) : FVector3(0.0f);
        };

        const b3ContactEvents Contacts = b3World_GetContactEvents(WorldId);

        for (int32 i = 0; i < Contacts.beginCount; ++i)
        {
            const b3ContactBeginTouchEvent& Event = Contacts.beginEvents[i];

            FContactRecord Record{};
            Record.Type = EContactEventType::Added;
            FillPair(Record, Event.shapeIdA, Event.shapeIdB);

            const bool bOverlap = Record.bSensorA || Record.bSensorB;
            if (!WantsContactEvents(Record.EntityA, true, bOverlap) && !WantsContactEvents(Record.EntityB, true, bOverlap))
            {
                continue;
            }

            // The manifold is only fetched once a listener is known to exist, since it is a world lookup.
            const b3ContactData Data = b3Contact_GetData(Event.contactId);
            if (Data.manifoldCount > 0 && Data.manifolds[0].pointCount > 0)
            {
                const b3Manifold& Manifold = Data.manifolds[0];
                Record.Normal = Box3DUtils::FromB3Vec3(Manifold.normal);

                b3Vec3 Average{ 0.0f, 0.0f, 0.0f };
                for (int32 p = 0; p < Manifold.pointCount; ++p)
                {
                    Average = b3Add(Average, Manifold.points[p].anchorA);
                }

                // Manifold anchors are relative to body A's center of mass, so that center lands it in world space.
                const b3BodyId AnchorBody = b3Shape_GetBody(Event.shapeIdA);
                const b3Vec3 AnchorOrigin = b3Body_IsValid(AnchorBody) ? b3Body_GetWorldCenter(AnchorBody) : b3Vec3{ 0.0f, 0.0f, 0.0f };
                Record.Point = Box3DUtils::FromB3Vec3(b3Add(AnchorOrigin, b3MulSV(1.0f / (float)Manifold.pointCount, Average)));
            }

            Record.ImpactSpeed = Math::Abs(Math::Dot(Record.VelocityB - Record.VelocityA, Record.Normal));
            ContactDrainScratch.push_back(Record);
        }

        // Hit events already carry world point, normal and approach speed, so they need no manifold lookup.
        for (int32 i = 0; i < Contacts.hitCount; ++i)
        {
            const b3ContactHitEvent& Event = Contacts.hitEvents[i];

            FContactRecord Record{};
            Record.Type = EContactEventType::Added;
            FillPair(Record, Event.shapeIdA, Event.shapeIdB);

            const bool bOverlap = Record.bSensorA || Record.bSensorB;
            if (!WantsContactEvents(Record.EntityA, true, bOverlap) && !WantsContactEvents(Record.EntityB, true, bOverlap))
            {
                continue;
            }

            Record.Point = Box3DUtils::FromB3Vec3(Event.point);
            Record.Normal = Box3DUtils::FromB3Vec3(Event.normal);
            Record.ImpactSpeed = Event.approachSpeed;
            ContactDrainScratch.push_back(Record);
        }

        for (int32 i = 0; i < Contacts.endCount; ++i)
        {
            const b3ContactEndTouchEvent& Event = Contacts.endEvents[i];

            FContactRecord Record{};
            Record.Type = EContactEventType::Removed;
            FillPair(Record, Event.shapeIdA, Event.shapeIdB);

            const bool bOverlap = Record.bSensorA || Record.bSensorB;
            if (!WantsContactEvents(Record.EntityA, false, bOverlap) && !WantsContactEvents(Record.EntityB, false, bOverlap))
            {
                continue;
            }

            ContactDrainScratch.push_back(Record);
        }

        const b3SensorEvents Sensors = b3World_GetSensorEvents(WorldId);

        for (int32 i = 0; i < Sensors.beginCount; ++i)
        {
            const b3SensorBeginTouchEvent& Event = Sensors.beginEvents[i];

            FContactRecord Record{};
            Record.Type = EContactEventType::Added;
            FillPair(Record, Event.sensorShapeId, Event.visitorShapeId);
            Record.bSensorA = true;

            if (WantsContactEvents(Record.EntityA, true, true) || WantsContactEvents(Record.EntityB, true, true))
            {
                ContactDrainScratch.push_back(Record);
            }
        }

        for (int32 i = 0; i < Sensors.endCount; ++i)
        {
            const b3SensorEndTouchEvent& Event = Sensors.endEvents[i];

            FContactRecord Record{};
            Record.Type = EContactEventType::Removed;
            FillPair(Record, Event.sensorShapeId, Event.visitorShapeId);
            Record.bSensorA = true;

            if (WantsContactEvents(Record.EntityA, false, true) || WantsContactEvents(Record.EntityB, false, true))
            {
                ContactDrainScratch.push_back(Record);
            }
        }
    }

    void FBox3DPhysicsScene::DispatchContactEvents()
    {
        if (ContactDrainScratch.empty())
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        auto Deliver = [&](entt::entity Self, entt::entity Other, uint32 SelfBody, uint32 OtherBody,
                           const FContactRecord& Record, bool bFlipNormal, bool bIsAdded, bool bIsOverlap)
        {
            if (Self == entt::null || !Registry.valid(Self))
            {
                return;
            }

            SRigidBodyComponent* Body = Registry.try_get<SRigidBodyComponent>(Self);
            if (Body == nullptr)
            {
                return;
            }

            TScriptDelegate<SCollisionEvent>& Delegate = bIsOverlap
                ? (bIsAdded ? Body->OnOverlapBegin : Body->OnOverlapEnd)
                : (bIsAdded ? Body->OnContactBegin : Body->OnContactEnd);

            const bool bHasScripts = Registry.all_of<SEntityScriptComponent>(Self);
            if (!Delegate.IsBound() && !bHasScripts)
            {
                return;
            }

            const SCollisionEvent Event = BuildCollisionEvent(Self, Other, SelfBody, OtherBody, Record, bFlipNormal);

            if (Delegate.IsBound())
            {
                Delegate.Broadcast(Event);
            }

            // Re-checked, since a handler above is free to have destroyed the entity it fired for.
            if (bHasScripts && Registry.valid(Self))
            {
                using ECallback = EntityScripts::ECollisionCallback;
                const ECallback Callback = bIsOverlap
                    ? (bIsAdded ? ECallback::OverlapBegin : ECallback::OverlapEnd)
                    : (bIsAdded ? ECallback::ContactBegin : ECallback::ContactEnd);
                EntityScripts::DispatchCollision(Registry, Self, Callback, Event);
            }
        };

        for (const FContactRecord& Record : ContactDrainScratch)
        {
            const bool bAdded = (Record.Type == EContactEventType::Added);
            const bool bOverlap = Record.bSensorA || Record.bSensorB;

            Deliver(Record.EntityA, Record.EntityB, Record.BodyIDA, Record.BodyIDB, Record, false, bAdded, bOverlap);
            Deliver(Record.EntityB, Record.EntityA, Record.BodyIDB, Record.BodyIDA, Record, true, bAdded, bOverlap);
        }

        ContactDrainScratch.clear();
    }

    void FBox3DPhysicsScene::DispatchActivationEvents()
    {
        if (ActivationDrainScratch.empty())
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);
        for (const FActivationRecord& Record : ActivationDrainScratch)
        {
            if (Record.Entity == entt::null || !Registry.valid(Record.Entity))
            {
                continue;
            }

            SRigidBodyComponent* Body = Registry.try_get<SRigidBodyComponent>(Record.Entity);
            if (Body == nullptr)
            {
                continue;
            }

            // OnWake also fires on spawn, when the body first becomes active.
            FScriptDelegate& Delegate = Record.bActivated ? Body->OnWake : Body->OnSleep;
            Delegate.Broadcast();
        }

        ActivationDrainScratch.clear();
    }

    void FBox3DPhysicsScene::OnRigidBodyComponentConstructed(entt::registry& Registry, entt::entity Entity)
    {
        LUMINA_PROFILE_SCOPE();

        if (STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity))
        {
            Transform->SetHasPhysicsBody(true);
        }

        if (BodyBatchDepth > 0)
        {
            BatchedBodyCreations.push_back(Entity);
            return;
        }

        if (bStepInProgress.load(std::memory_order_acquire))
        {
            FScopeLock Lock(PendingRigidBodyMutex);
            PendingRigidBodyCreations.push(Entity);
            return;
        }

        CreateRigidBodyImmediate(Registry, Entity);
    }

    void FBox3DPhysicsScene::OnRigidBodyComponentUpdated(entt::registry& Registry, entt::entity Entity)
    {
    }

    void FBox3DPhysicsScene::OnRigidBodyComponentDestroyed(entt::registry& Registry, entt::entity Entity)
    {
        SRigidBodyComponent* Body = Registry.try_get<SRigidBodyComponent>(Entity);
        if (Body == nullptr || Body->BodyID == InvalidBodyHandle)
        {
            return;
        }

        const uint32 Handle = Body->BodyID;
        const b3BodyId BodyId = ResolveBody(Handle);
        if (b3Body_IsValid(BodyId))
        {
            b3DestroyBody(BodyId);
        }

        UnregisterBody(Handle);
        ClearBodyMaterial(Handle);

        if (Handle < BodyAwake.size())
        {
            BodyAwake[Handle] = 0;
        }

        Body->BodyID = InvalidBodyHandle;
    }

    void FBox3DPhysicsScene::OnColliderComponentAdded(entt::registry& Registry, entt::entity Entity)
    {
    }

    void FBox3DPhysicsScene::OnColliderComponentRemoved(entt::registry& Registry, entt::entity Entity)
    {
    }

    void FBox3DPhysicsScene::CreateRigidBodyImmediate(entt::registry& Registry, entt::entity Entity)
    {
        FRigidBodyBuildResult Build;
        const EBodyBuildStatus Status = TryBuildRigidBody(Registry, Entity, Build);

        switch (Status)
        {
            case EBodyBuildStatus::Success:
            {
                const uint32 Handle = CommitRigidBody(Entity, Build);
                if (Handle == InvalidBodyHandle)
                {
                    return;
                }

                SRigidBodyComponent& Body = Registry.get<SRigidBodyComponent>(Entity);
                Body.BodyID = Handle;
                Body.LastBodyPosition = Build.LastBodyPosition;
                Body.LastBodyRotation = Build.LastBodyRotation;

                if (Handle >= BodyAwake.size())
                {
                    BodyAwake.resize(Handle + 1, 0);
                }
                break;
            }
            case EBodyBuildStatus::Defer:
            case EBodyBuildStatus::NoCollider:
            {
                FScopeLock Lock(PendingRigidBodyMutex);
                PendingRigidBodyCreations.push(Entity);
                break;
            }
            default:
                break;
        }
    }

    void FBox3DPhysicsScene::RebuildStaleDynamicMeshBodies(entt::registry& Registry)
    {
        LUMINA_PROFILE_SCOPE();

        auto View = Registry.view<SDynamicMeshColliderComponent, SDynamicMeshComponent, SRigidBodyComponent>();
        for (auto [Entity, Collider, Mesh, Body] : View.each())
        {
            const uint32 Version = Mesh.LoadRenderDataVersion();
            if (Version == Collider.GeometryVersion)
            {
                continue;
            }

            Collider.GeometryVersion = Version;

            if (Body.BodyID != InvalidBodyHandle)
            {
                OnRigidBodyComponentDestroyed(Registry, Entity);
            }

            FScopeLock Lock(PendingRigidBodyMutex);
            PendingRigidBodyCreations.push(Entity);
        }
    }

    void FBox3DPhysicsScene::CreateRigidBodiesBatched(const TVector<entt::entity>& Entities)
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 Count = (uint32)Entities.size();
        if (Count == 0)
        {
            return;
        }

        entt::registry& Registry = ECS::GetWorldRegistry(*World);

        if (BatchBuildScratch.size() < Count)
        {
            BatchBuildScratch.resize(Count);
            BatchStatusScratch.resize(Count);
        }

        // Shape assembly only reads the registry and loaded assets, so it parallelizes; the commit is serial.
        Task::ParallelFor(Count, [&](uint32 Index)
        {
            BatchStatusScratch[Index] = TryBuildRigidBody(Registry, Entities[Index], BatchBuildScratch[Index]);
        }, 16);

        bool bCreatedStatic = false;

        for (uint32 Index = 0; Index < Count; ++Index)
        {
            const entt::entity Entity = Entities[Index];

            switch (BatchStatusScratch[Index])
            {
                case EBodyBuildStatus::Success:
                {
                    FRigidBodyBuildResult& Build = BatchBuildScratch[Index];
                    const uint32 Handle = CommitRigidBody(Entity, Build);
                    if (Handle == InvalidBodyHandle)
                    {
                        break;
                    }

                    SRigidBodyComponent& Body = Registry.get<SRigidBodyComponent>(Entity);
                    Body.BodyID = Handle;
                    Body.LastBodyPosition = Build.LastBodyPosition;
                    Body.LastBodyRotation = Build.LastBodyRotation;

                    if (Handle >= BodyAwake.size())
                    {
                        BodyAwake.resize(Handle + 1, 0);
                    }

                    bCreatedStatic |= Build.BodyDef.type == b3_staticBody;
                    break;
                }
                case EBodyBuildStatus::Defer:
                case EBodyBuildStatus::NoCollider:
                {
                    FScopeLock Lock(PendingRigidBodyMutex);
                    PendingRigidBodyCreations.push(Entity);
                    break;
                }
                default:
                    break;
            }
        }

        // Static shapes skipped their own contact scan on creation, so the tree is rebuilt once here.
        if (bCreatedStatic)
        {
            b3World_RebuildStaticTree(WorldId);
        }
    }

    void FBox3DPhysicsScene::BulkCreateRigidBodies(entt::registry& Registry)
    {
        TVector<entt::entity> Candidates;
        Registry.view<SRigidBodyComponent>().each([&](entt::entity Entity, SRigidBodyComponent&)
        {
            Candidates.push_back(Entity);
        });

        CreateRigidBodiesBatched(Candidates);
    }

    void FBox3DPhysicsScene::BeginBodyBatch()
    {
        ++BodyBatchDepth;
    }

    void FBox3DPhysicsScene::EndBodyBatch()
    {
        if (BodyBatchDepth == 0)
        {
            return;
        }

        // An inner pair folds into the outermost batch, which is what commits.
        if (--BodyBatchDepth > 0)
        {
            return;
        }

        if (!BatchedBodyCreations.empty())
        {
            CreateRigidBodiesBatched(BatchedBodyCreations);
            BatchedBodyCreations.clear();
        }

        if (!BatchedCharacterCreations.empty())
        {
            entt::registry& Registry = ECS::GetWorldRegistry(*World);
            for (entt::entity Entity : BatchedCharacterCreations)
            {
                if (Registry.valid(Entity))
                {
                    OnCharacterComponentConstructed(Registry, Entity);
                }
            }
            BatchedCharacterCreations.clear();
        }
    }
}
