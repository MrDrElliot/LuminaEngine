#include "RuntimePCH.h"
#include <algorithm>
#include "World/ECS/Registry.h"
#include "Box3DPhysicsScene.h"

#include "Box3DCharacterHandle.h"
#include "Box3DRagdollHandle.h"
#include "Box3DInternal.h"
#include "Box3DUtils.h"

#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "Log/Log.h"
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
        SCollisionEvent BuildCollisionEvent(ECS::FEntity SelfEntity, ECS::FEntity OtherEntity,
                                           const FContactRecord& Record, bool bFlipNormal)
        {
            SCollisionEvent Event;
            Event.Entity = SelfEntity;
            Event.Other = OtherEntity;
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

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        auto RigidStorage = Registry.GetStorage<SRigidBodyComponent>();

        // Only an entity that can actually receive the event is worth resolving a manifold for.
        auto WantsContactEvents = [&](ECS::FEntity Entity, bool bAdded, bool bOverlap)
        {
            if (Entity == ECS::NullEntity || !RigidStorage.Contains(Entity))
            {
                return false;
            }

            const SRigidBodyComponent& Body = RigidStorage.Get(Entity);
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

        auto CharacterStorage = Registry.GetStorage<SCharacterPhysicsComponent>();

        // A resting character runs no world queries, so a body that just touched its proxy has to nudge it awake.
        auto WakeRestingCharacter = [&](ECS::FEntity Entity)
        {
            if (Entity == ECS::NullEntity || !CharacterStorage.Contains(Entity))
            {
                return;
            }

            SCharacterPhysicsComponent& Component = CharacterStorage.Get(Entity);
            if (Component.Character)
            {
                Component.Character->bResting = false;
            }
        };

        const b3ContactEvents Contacts = b3World_GetContactEvents(WorldId);

        for (int32 i = 0; i < Contacts.beginCount; ++i)
        {
            const b3ContactBeginTouchEvent& Event = Contacts.beginEvents[i];

            FContactRecord Record{};
            Record.Type = EContactEventType::Added;
            FillPair(Record, Event.shapeIdA, Event.shapeIdB);

            WakeRestingCharacter(Record.EntityA);
            WakeRestingCharacter(Record.EntityB);

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

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        auto Deliver = [&](ECS::FEntity Self, ECS::FEntity Other, const FContactRecord& Record, bool bFlipNormal, bool bIsAdded, bool bIsOverlap)
        {
            if (Self == ECS::NullEntity || !Registry.IsValid(Self))
            {
                return;
            }

            SRigidBodyComponent* Body = Registry.TryGet<SRigidBodyComponent>(Self);
            if (Body == nullptr)
            {
                return;
            }

            TScriptDelegate<SCollisionEvent>& Delegate = bIsOverlap
                ? (bIsAdded ? Body->OnOverlapBegin : Body->OnOverlapEnd)
                : (bIsAdded ? Body->OnContactBegin : Body->OnContactEnd);

            if (!Delegate.IsBound())
            {
                return;
            }

            Delegate.Broadcast(BuildCollisionEvent(Self, Other, Record, bFlipNormal));
        };

        for (const FContactRecord& Record : ContactDrainScratch)
        {
            const bool bAdded = (Record.Type == EContactEventType::Added);
            const bool bOverlap = Record.bSensorA || Record.bSensorB;

            Deliver(Record.EntityA, Record.EntityB, Record, false, bAdded, bOverlap);
            Deliver(Record.EntityB, Record.EntityA, Record, true, bAdded, bOverlap);
        }

        ContactDrainScratch.clear();
    }

    void FBox3DPhysicsScene::DispatchActivationEvents()
    {
        if (ActivationDrainScratch.empty())
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        for (const FActivationRecord& Record : ActivationDrainScratch)
        {
            if (Record.Entity == ECS::NullEntity || !Registry.IsValid(Record.Entity))
            {
                continue;
            }

            SRigidBodyComponent* Body = Registry.TryGet<SRigidBodyComponent>(Record.Entity);
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

    void FBox3DPhysicsScene::OnRigidBodyComponentConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity))
        {
            Transform->SetHasPhysicsBody(true);
        }
        FBodyRecord& Record = RigidBodies.FindOrAdd(Entity);
        Record.Revision = NextBindingRevision++;
        PendingRigidBodies.push_back(Entity);
    }

    void FBox3DPhysicsScene::OnRigidBodyComponentUpdated(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (FBodyRecord* Record = RigidBodies.Find(Entity))
        {
            Record->bRebuild = true;
            PendingRigidBodies.push_back(Entity);
        }
    }

    void FBox3DPhysicsScene::OnRigidBodyComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (const FBodyRecord* Record = RigidBodies.Find(Entity))
        {
            if (Record->Handle != InvalidBodyHandle)
            {
                PendingBodyDestructions.push_back(Record->Handle);
            }
            RigidBodies.Remove(Entity);
        }
    }

    void FBox3DPhysicsScene::OnColliderComponentAdded(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        OnRigidBodyComponentUpdated(Registry, Entity);
    }

    void FBox3DPhysicsScene::OnColliderComponentRemoved(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        OnRigidBodyComponentUpdated(Registry, Entity);
    }

    void FBox3DPhysicsScene::RebuildStaleDynamicMeshBodies(ECS::FRegistry& Registry)
    {
        auto View = Registry.View<SDynamicMeshColliderComponent, SDynamicMeshComponent, SRigidBodyComponent>();
        for (auto [Entity, Collider, Mesh, Body] : View.Each())
        {
            const uint32 Version = Mesh.LoadRenderDataVersion();
            if (Version != Collider.GeometryVersion)
            {
                Collider.GeometryVersion = Version;
                OnRigidBodyComponentUpdated(Registry, Entity);
            }
        }
    }

    void FBox3DPhysicsScene::DestroyBodyHandle(uint32 Handle)
    {
        const b3BodyId Body = ResolveBody(Handle);
        if (!b3Body_IsValid(Body))
        {
            return;
        }
        b3DestroyBody(Body);
        UnregisterBody(Handle);
        ClearBodyMaterial(Handle);
        if (Handle < BodyAwake.size())
        {
            BodyAwake[Handle] = 0;
        }
    }

    void FBox3DPhysicsScene::SynchronizeBodyGroups()
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        for (const FPendingStaticGroup& Group : PendingStaticGroups)
        {
            if (Registry.IsValid(Group.Owner)) { CommitStaticBodyGroup(Group.GroupID, Group.Owner, Group.Builds); }
            else { StaticBodyGroups.erase(Group.GroupID); }
        }
        PendingStaticGroups.clear();
        for (const FPendingRagdoll& Request : PendingRagdolls)
        {
            if (Registry.IsValid(Request.Description.Entity) && !Request.Handle->bPendingDestroy)
            {
                CommitRagdoll(Request);
            }
        }
        PendingRagdolls.clear();
        size_t Count = 0;
        for (const FOwnedRagdoll& Ragdoll : OwnedRagdolls)
        {
            if (Ragdoll.Handle->bPendingDestroy || !Registry.IsValid(Ragdoll.Owner))
            {
                for (uint32 Handle : Ragdoll.Handle->BodyHandles) { DestroyBodyHandle(Handle); }
                Ragdoll.Handle->BodyHandles.clear();
                Ragdoll.Handle->Release();
            }
            else
            {
                OwnedRagdolls[Count++] = Ragdoll;
            }
        }
        OwnedRagdolls.resize(Count);
    }

    void FBox3DPhysicsScene::SynchronizeBodies()
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        for (uint32 Handle : PendingBodyDestructions)
        {
            DestroyBodyHandle(Handle);
        }
        PendingBodyDestructions.clear();

        PendingDrainScratch.clear();
        PendingDrainScratch.swap(PendingRigidBodies);
        std::sort(PendingDrainScratch.begin(), PendingDrainScratch.end());
        PendingDrainScratch.erase(std::unique(PendingDrainScratch.begin(), PendingDrainScratch.end()), PendingDrainScratch.end());
        size_t Count = 0;
        for (ECS::FEntity Entity : PendingDrainScratch)
        {
            FBodyRecord* Found = RigidBodies.Find(Entity);
            if (!Registry.IsValid(Entity) || Found == nullptr)
            {
                continue;
            }
            FBodyRecord& Record = *Found;
            if (Record.bRebuild)
            {
                DestroyBodyHandle(Record.Handle);
                Record.Handle = InvalidBodyHandle;
                Record.Status = EPhysicsBodyStatus::Pending;
                Record.bRebuild = false;
            }
            PendingDrainScratch[Count++] = Entity;
        }
        PendingDrainScratch.resize(Count);
        CreateRigidBodiesBatched(PendingDrainScratch);

        TVector<ECS::FEntity> Characters;
        Characters.swap(PendingCharacters);
        for (ECS::FEntity Entity : Characters)
        {
            if (Registry.IsValid(Entity) && CharacterBodies.Contains(Entity))
            {
                CreateCharacter(Registry, Entity);
                const FBodyRecord* Record = CharacterBodies.Find(Entity);
                if (Record != nullptr && Record->Status == EPhysicsBodyStatus::Pending)
                {
                    PendingCharacters.push_back(Entity);
                }
            }
        }
        SynchronizeBodyGroups();
        DrainPendingConstraints();
        SynchronizeConstraints();
        if (bStaticTreeDirty)
        {
            b3World_RebuildStaticTree(WorldId);
            bStaticTreeDirty = false;
        }
    }

    void FBox3DPhysicsScene::CreateRigidBodiesBatched(const TVector<ECS::FEntity>& Entities)
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 Count = (uint32)Entities.size();
        if (Count == 0)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

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
            const ECS::FEntity Entity = Entities[Index];

            switch (BatchStatusScratch[Index])
            {
                case EBodyBuildStatus::Success:
                {
                    FRigidBodyBuildResult& Build = BatchBuildScratch[Index];
                    const uint32 Handle = CommitRigidBody(Entity, Build);
                    if (Handle == InvalidBodyHandle)
                    {
                        RigidBodies.FindOrAdd(Entity).Status = EPhysicsBodyStatus::Failed;
                        break;
                    }

                    FBodyRecord& Body = RigidBodies.FindOrAdd(Entity);
                    Body.Handle = Handle;
                    Body.Status = EPhysicsBodyStatus::Ready;
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
                {
                    PendingRigidBodies.push_back(Entity);
                    break;
                }
                case EBodyBuildStatus::NoCollider:
                    break;
                case EBodyBuildStatus::Error:
                    RigidBodies.FindOrAdd(Entity).Status = EPhysicsBodyStatus::Failed;
                    break;
                default:
                    break;
            }
        }

        // Static shapes skipped their own contact scan on creation, so the tree is rebuilt once here.
        if (bCreatedStatic)
        {
            bStaticTreeDirty = true;
        }
    }

    void FBox3DPhysicsScene::BulkCreateRigidBodies(ECS::FRegistry& Registry)
    {
        Registry.View<SRigidBodyComponent>().ForEach([&](ECS::FEntity Entity, SRigidBodyComponent&)
        {
            OnRigidBodyComponentConstructed(Registry, Entity);
        });

        SynchronizeBodies();
    }

}
