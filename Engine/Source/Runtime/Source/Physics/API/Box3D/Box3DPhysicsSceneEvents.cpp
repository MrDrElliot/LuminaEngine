#include "RuntimePCH.h"
#include "Containers/Algorithm.h"
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

        ECS::FRegistry& Registry = World->GetEntityRegistry();
        auto RigidStorage = Registry.GetStorage<SRigidBodyComponent>();

        // A system reading collisions wants them all, otherwise only a body listening for one is worth resolving a manifold for.
        const bool bCollisionsRead = World->GetEvents().HasInterest(ECS::GetComponentTypeID<SCollisionEvent>());
        auto WantsContactEvents = [&](ECS::FEntity Entity, bool bAdded, bool bOverlap)
        {
            if (Entity == ECS::NullEntity)
            {
                return false;
            }
            if (bCollisionsRead)
            {
                return true;
            }
            if (!RigidStorage.Contains(Entity))
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
            // An end event can name a shape destroyed this step, so that side resolves to no body instead of a recycled slot.
            const bool bValidA = b3Shape_IsValid(ShapeA);
            const bool bValidB = b3Shape_IsValid(ShapeB);
            const b3BodyId BodyA = bValidA ? b3Shape_GetBody(ShapeA) : b3_nullBodyId;
            const b3BodyId BodyB = bValidB ? b3Shape_GetBody(ShapeB) : b3_nullBodyId;

            Record.EntityA = EntityOfBody(BodyA);
            Record.EntityB = EntityOfBody(BodyB);
            Record.BodyIDA = HandleOfBody(BodyA);
            Record.BodyIDB = HandleOfBody(BodyB);
            Record.bSensorA = bValidA && b3Shape_IsSensor(ShapeA);
            Record.bSensorB = bValidB && b3Shape_IsSensor(ShapeB);
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
            ContactDrainScratch.Add(Record);
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
            ContactDrainScratch.Add(Record);
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

            ContactDrainScratch.Add(Record);
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
                ContactDrainScratch.Add(Record);
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
                ContactDrainScratch.Add(Record);
            }
        }
    }

    void FBox3DPhysicsScene::DispatchContactEvents()
    {
        if (ContactDrainScratch.IsEmpty())
        {
            return;
        }

        ECS::FEventQueues& Events = World->GetEvents();
        auto Send = [&](ECS::FEntity Self, ECS::FEntity Other, const FContactRecord& Record, bool bFlipNormal, ECollisionEventKind Kind)
        {
            if (Self != ECS::NullEntity)
            {
                SCollisionEvent Event = BuildCollisionEvent(Self, Other, Record, bFlipNormal);
                Event.Kind = Kind;
                ECS::SendEvent(Events, Event);
            }
        };

        for (const FContactRecord& Record : ContactDrainScratch)
        {
            const bool bAdded = (Record.Type == EContactEventType::Added);
            const bool bOverlap = Record.bSensorA || Record.bSensorB;
            const ECollisionEventKind Kind = bOverlap
                ? (bAdded ? ECollisionEventKind::OverlapBegin : ECollisionEventKind::OverlapEnd)
                : (bAdded ? ECollisionEventKind::ContactBegin : ECollisionEventKind::ContactEnd);

            Send(Record.EntityA, Record.EntityB, Record, false, Kind);
            Send(Record.EntityB, Record.EntityA, Record, true, Kind);
        }

        ContactDrainScratch.Reset();
    }

    void FBox3DPhysicsScene::DispatchActivationEvents()
    {
        ECS::FEventQueues& Events = World->GetEvents();
        for (const FActivationRecord& Record : ActivationDrainScratch)
        {
            // A body also wakes when it is first created, which is when OnWake first fires.
            if (Record.Entity != ECS::NullEntity)
            {
                SBodyActivationEvent Event;
                Event.Entity = Record.Entity;
                Event.bAwake = Record.bActivated;
                ECS::SendEvent(Events, Event);
            }
        }

        ActivationDrainScratch.Reset();
    }

    void FBox3DPhysicsScene::OnRigidBodyComponentConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity))
        {
            Transform->SetHasPhysicsBody(true);
        }
        FBodyRecord& Record = RigidBodies.FindOrAdd(Entity);
        Record.Revision = NextBindingRevision++;
        PendingRigidBodies.Add(Entity);
    }

    void FBox3DPhysicsScene::OnRigidBodyComponentUpdated(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (FBodyRecord* Record = RigidBodies.Find(Entity))
        {
            Record->bRebuild = true;
            PendingRigidBodies.Add(Entity);
        }
    }

    void FBox3DPhysicsScene::OnRigidBodyComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (const FBodyRecord* Record = RigidBodies.Find(Entity))
        {
            if (Record->Handle != InvalidBodyHandle)
            {
                PendingBodyDestructions.Add(Record->Handle);
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
        DynamicMeshCooks.Remove(Entity);
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
        if (Handle < static_cast<uint32>(BodyAwake.Num()))
        {
            BodyAwake[Handle] = 0;
        }
    }

    void FBox3DPhysicsScene::SynchronizeBodyGroups()
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        for (const FPendingStaticGroup& Group : PendingStaticGroups)
        {
            if (Registry.IsValid(Group.Owner)) { CommitStaticBodyGroup(Group.GroupID, Group.Owner, Group.Builds); }
            else { StaticBodyGroups.Remove(Group.GroupID); }
        }
        PendingStaticGroups.Reset();
        for (const FPendingRagdoll& Request : PendingRagdolls)
        {
            if (Registry.IsValid(Request.Description.Entity) && !Request.Handle->bPendingDestroy)
            {
                CommitRagdoll(Request);
            }
        }
        PendingRagdolls.Reset();
        int32 Count = 0;
        for (const FOwnedRagdoll& Ragdoll : OwnedRagdolls)
        {
            if (Ragdoll.Handle->bPendingDestroy || !Registry.IsValid(Ragdoll.Owner))
            {
                if (FPhysicsRagdollHandle** Live = SimulatingRagdolls.Find(Ragdoll.Owner); Live != nullptr && *Live == Ragdoll.Handle.get())
                {
                    SimulatingRagdolls.Remove(Ragdoll.Owner);
                }
                for (uint32 Handle : Ragdoll.Handle->BodyHandles) { DestroyBodyHandle(Handle); }
                Ragdoll.Handle->BodyHandles.Reset();
                Ragdoll.Handle->Release();
            }
            else
            {
                OwnedRagdolls[Count++] = Ragdoll;
            }
        }
        OwnedRagdolls.SetNum(Count);
    }

    void FBox3DPhysicsScene::SynchronizeBodies()
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        for (uint32 Handle : PendingBodyDestructions)
        {
            DestroyBodyHandle(Handle);
        }
        PendingBodyDestructions.Reset();

        PendingDrainScratch.Reset();
        swap(PendingDrainScratch, PendingRigidBodies);
        Algo::Sort(PendingDrainScratch);
        PendingDrainScratch.SetNum(static_cast<int32>(Algo::Unique(PendingDrainScratch) - PendingDrainScratch.begin()));
        int32 Count = 0;
        for (ECS::FEntity Entity : PendingDrainScratch)
        {
            if (Registry.IsValid(Entity) && RigidBodies.Contains(Entity))
            {
                PendingDrainScratch[Count++] = Entity;
            }
        }
        PendingDrainScratch.SetNum(Count);

        StartDynamicMeshCooks(Registry, PendingDrainScratch);

        Count = 0;
        for (ECS::FEntity Entity : PendingDrainScratch)
        {
            FBodyRecord& Record = RigidBodies.FindChecked(Entity);
            if (Record.bRebuild)
            {
                // The old body keeps colliding until its replacement is cooked, so a rebuilt chunk never drops what stands on it.
                if (IsDynamicMeshCookPending(Entity))
                {
                    PendingRigidBodies.Add(Entity);
                    continue;
                }

                const b3BodyId Old = ResolveBody(Record.Handle);
                if (b3Body_IsValid(Old) && b3Body_GetType(Old) == b3_dynamicBody)
                {
                    CarriedMotion.Emplace(Entity, b3Body_GetLinearVelocity(Old), b3Body_GetAngularVelocity(Old), b3Body_IsAwake(Old));
                }

                DestroyBodyHandle(Record.Handle);
                Record.Handle = InvalidBodyHandle;
                Record.Status = EPhysicsBodyStatus::Pending;
                Record.bRebuild = false;
            }
            PendingDrainScratch[Count++] = Entity;
        }
        PendingDrainScratch.SetNum(Count);
        CreateRigidBodiesBatched(PendingDrainScratch);

        for (const FCarriedMotion& Motion : CarriedMotion)
        {
            const FBodyRecord* Rebuilt = RigidBodies.Find(Motion.Entity);
            const b3BodyId Body = Rebuilt != nullptr ? ResolveBody(Rebuilt->Handle) : b3_nullBodyId;
            if (b3Body_IsValid(Body) && b3Body_GetType(Body) == b3_dynamicBody)
            {
                b3Body_SetLinearVelocity(Body, Motion.Linear);
                b3Body_SetAngularVelocity(Body, Motion.Angular);
                b3Body_SetAwake(Body, Motion.bAwake);
            }
        }
        CarriedMotion.Reset();

        TVector<ECS::FEntity> Characters;
        swap(Characters, PendingCharacters);
        for (ECS::FEntity Entity : Characters)
        {
            if (Registry.IsValid(Entity) && CharacterBodies.Contains(Entity))
            {
                CreateCharacter(Registry, Entity);
                const FBodyRecord* Record = CharacterBodies.Find(Entity);
                if (Record != nullptr && Record->Status == EPhysicsBodyStatus::Pending)
                {
                    PendingCharacters.Add(Entity);
                }
            }
        }
        ApplyPendingCharacterUpdates(Registry);
        SynchronizeBodyGroups();
        DrainPendingConstraints();
        SynchronizeConstraints();
    }

    void FBox3DPhysicsScene::CreateRigidBodiesBatched(const TVector<ECS::FEntity>& Entities)
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 Count = (uint32)Entities.Num();
        if (Count == 0)
        {
            return;
        }

        ECS::FRegistry& Registry = World->GetEntityRegistry();

        if (static_cast<uint32>(BatchBuildScratch.Num()) < Count)
        {
            BatchBuildScratch.SetNum(Count);
            BatchStatusScratch.SetNum(Count);
        }

        // Shape assembly only reads the registry and loaded assets, so it parallelizes; the commit is serial.
        Jobs::ParallelFor(Count, [&](uint32 Index)
        {
            BatchStatusScratch[Index] = TryBuildRigidBody(Registry, Entities[Index], BatchBuildScratch[Index]);
        }, 16);


        for (uint32 Index = 0; Index < Count; ++Index)
        {
            const ECS::FEntity Entity = Entities[Index];

            if (BatchStatusScratch[Index] != EBodyBuildStatus::Defer)
            {
                DynamicMeshCooks.Remove(Entity);
            }

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

                    if (Handle >= static_cast<uint32>(BodyAwake.Num()))
                    {
                        BodyAwake.SetNum(Handle + 1, 0);
                    }
                    break;
                }
                case EBodyBuildStatus::Defer:
                {
                    PendingRigidBodies.Add(Entity);
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
    }

    void FBox3DPhysicsScene::BulkCreateRigidBodies(ECS::FRegistry& Registry)
    {
        Registry.View<SRigidBodyComponent>().ForEach([&](ECS::FEntity Entity, SRigidBodyComponent&)
        {
            OnRigidBodyComponentConstructed(Registry, Entity);
        });

        bCookDynamicMeshesInline = true;
        SynchronizeBodies();
        bCookDynamicMeshesInline = false;
    }

}
