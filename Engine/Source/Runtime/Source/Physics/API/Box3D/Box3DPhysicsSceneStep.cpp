#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "Box3DPhysicsScene.h"

#include "Box3DCharacterHandle.h"
#include "Box3DRagdollHandle.h"
#include "Box3DInternal.h"
#include "Box3DPhysics.h"
#include "Box3DUtils.h"

#include "Core/Console/ConsoleVariable.h"
#include "Core/Math/SIMD/SIMD.h"
#include "Log/Log.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Events/CollisionEvent.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"

namespace Lumina::Physics
{
    // Matches the transform resolve's own fan-out point, below which a job costs more than the work.
    static constexpr uint32 InterpParallelThreshold = 1000;

    void FBox3DPhysicsScene::Simulate()
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        BulkCreateRigidBodies(Registry);

        Registry.View<SCharacterPhysicsComponent>().ForEach([&](ECS::FEntity EntityID, SCharacterPhysicsComponent&)
        {
            OnCharacterComponentConstructed(Registry, EntityID);
        });

        Registry.View<SRigidBodyComponent, STransformComponent>().ForEach([](SRigidBodyComponent&, STransformComponent& T) { T.SetHasPhysicsBody(true); });

        SynchronizeBodies();

        // One rebuild after the bulk spawn beats the incremental inserts each static shape would have done.
        b3World_RebuildStaticTree(WorldId);

        Registry.GetSignals<SCharacterPhysicsComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnCharacterComponentConstructed>(this);
        Registry.GetSignals<SCharacterPhysicsComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnCharacterComponentDestroyed>(this);

        Registry.GetSignals<SSphereColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SSphereColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SSphereColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SBoxColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SBoxColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SBoxColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SCapsuleColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCapsuleColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCapsuleColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SCylinderColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCylinderColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCylinderColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<STaperedCapsuleColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STaperedCapsuleColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STaperedCapsuleColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<STaperedCylinderColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STaperedCylinderColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STaperedCylinderColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SPlaneColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SPlaneColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SPlaneColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SCollisionShapeComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCollisionShapeComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCollisionShapeComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SCompoundColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCompoundColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCompoundColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SMeshColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SMeshColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SMeshColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<STerrainColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STerrainColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STerrainColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SDynamicMeshColliderComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SDynamicMeshColliderComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SDynamicMeshColliderComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);

        Registry.GetSignals<SRigidBodyComponent>().OnUpdate.Connect<&FBox3DPhysicsScene::OnRigidBodyComponentUpdated>(this);
        Registry.GetSignals<SRigidBodyComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnRigidBodyComponentConstructed>(this);
        Registry.GetSignals<SRigidBodyComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnRigidBodyComponentDestroyed>(this);

        Registry.GetSignals<SPhysicsConstraintComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnConstraintComponentConstructed>(this);
        Registry.GetSignals<SPhysicsConstraintComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnConstraintComponentDestroyed>(this);

        Registry.View<SPhysicsConstraintComponent>().ForEach([&](ECS::FEntity E, SPhysicsConstraintComponent& C)
        {
            C.ConstraintID = 0;
            PendingConstraintCreations.push_back(E);
        });
    }

    void FBox3DPhysicsScene::StopSimulate()
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        Registry.GetSignals<SCharacterPhysicsComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnCharacterComponentConstructed>(this);
        Registry.GetSignals<SCharacterPhysicsComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnCharacterComponentDestroyed>(this);

        Registry.GetSignals<SSphereColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SSphereColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SSphereColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SBoxColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SBoxColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SBoxColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SCapsuleColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCapsuleColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCapsuleColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SCylinderColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCylinderColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCylinderColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<STaperedCapsuleColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STaperedCapsuleColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STaperedCapsuleColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<STaperedCylinderColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STaperedCylinderColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STaperedCylinderColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SPlaneColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SPlaneColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SPlaneColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SCollisionShapeComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCollisionShapeComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCollisionShapeComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SCompoundColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCompoundColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SCompoundColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SMeshColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SMeshColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SMeshColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<STerrainColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STerrainColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<STerrainColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);
        Registry.GetSignals<SDynamicMeshColliderComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SDynamicMeshColliderComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnColliderComponentAdded>(this);
        Registry.GetSignals<SDynamicMeshColliderComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnColliderComponentRemoved>(this);

        Registry.GetSignals<SRigidBodyComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnRigidBodyComponentUpdated>(this);
        Registry.GetSignals<SRigidBodyComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnRigidBodyComponentConstructed>(this);
        Registry.GetSignals<SRigidBodyComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnRigidBodyComponentDestroyed>(this);

        Registry.GetSignals<SPhysicsConstraintComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnConstraintComponentConstructed>(this);
        Registry.GetSignals<SPhysicsConstraintComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnConstraintComponentDestroyed>(this);

        Registry.View<SRigidBodyComponent>().ForEach([&](ECS::FEntity EntityID, SRigidBodyComponent&)
        {
            OnRigidBodyComponentDestroyed(Registry, EntityID);
        });

        // The hook is already disconnected above, so the proxies have to be released by hand like the bodies.
        Registry.View<SCharacterPhysicsComponent>().ForEach([&](ECS::FEntity EntityID, SCharacterPhysicsComponent&)
        {
            OnCharacterComponentDestroyed(Registry, EntityID);
        });

        PendingStaticGroups.clear();
        PendingRagdolls.clear();
        for (const FOwnedRagdoll& Ragdoll : OwnedRagdolls) { Ragdoll.Handle->bPendingDestroy = true; }
        DestroyAllConstraints();
        SynchronizeBodies();
        for (TVector<FBodyCommand>& Commands : ThreadBodyCommands)
        {
            Commands.clear();
        }
        OverflowBodyCommands.clear();
        WaitingBodyCommands.clear();
        BodyCommandScratch.clear();
        PendingRigidBodies.clear();
        PendingCharacters.clear();
        PendingConstraintCreations.clear();
        DestroyAllStaticBodyGroups();
    }

    void FBox3DPhysicsScene::ApplyDirtyTransforms(float FixedDt, uint32 RemainingSteps, bool bFirstStep)
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        ECS::Utils::FlushDirtyPhysicsBodies(Registry);

        auto BodySyncView = Registry.View<SRigidBodyComponent, FNeedsPhysicsBodyUpdate>();

        if (bFirstStep)
        {
        for (uint32 Handle : AuthoredKinematicHandles)
        {
            BodyAuthoredKinematic[Handle] = 0;
        }
        PreviousAuthoredKinematicHandles.swap(AuthoredKinematicHandles);
        AuthoredKinematicHandles.clear();
        }

        // Spread over every step this update runs, so a second step does not carry the body past its target.
        const float KinematicSpan = FixedDt * (float)Math::Max(RemainingSteps, 1u);

        // Box3D body writes touch shared world arrays, so this stays serial rather than a ParallelFor.
        for (auto [Entity, BodyComponent, Update] : BodySyncView.Each())
        {
            const b3BodyId BodyId = ResolveBody(FindEntityBody(Entity));
            if (!b3Body_IsValid(BodyId))
            {
                continue;
            }

            const STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
            const FVector3 TargetLocation = Transform.GetLocation();
            const FQuat TargetRotation = Transform.GetRotation();
            const b3Vec3 Position = Box3DUtils::ToB3Vec3(TargetLocation);
            const b3Quat Rotation = Box3DUtils::ToB3Quat(TargetRotation);

            // A teleport has no previous pose to blend from, so the interpolator must not span the jump.
            auto AdoptTeleportedPose = [&]
            {
                if (FBodyRecord* Record = RigidBodies.Find(Entity))
                {
                    Record->LastBodyPosition = TargetLocation;
                    Record->LastBodyRotation = TargetRotation;
                }
            };

            switch (b3Body_GetType(BodyId))
            {
                case b3_staticBody:
                {
                    b3Body_SetTransform(BodyId, Position, Rotation);
                    AdoptTeleportedPose();
                    break;
                }
                case b3_kinematicBody:
                {
                    // Target-transform drive keeps the swept motion the contact solver needs.
                    b3Body_SetTargetTransform(BodyId, b3WorldTransform{ Position, Rotation }, KinematicSpan, Update.bActivate);
                    MarkAuthoredKinematic(FindEntityBody(Entity));
                    break;
                }
                case b3_bodyTypeCount:
                {
                    break;
                }
                case b3_dynamicBody:
                {
                    switch (Update.MoveMode)
                    {
                        case EMoveMode::Teleport:
                        {
                            b3Body_SetTransform(BodyId, Position, Rotation);
                            AdoptTeleportedPose();
                            if (Update.bActivate)
                            {
                                b3Body_SetAwake(BodyId, true);
                            }
                            break;
                        }
                        case EMoveMode::MoveKinematic:
                        {
                            b3Body_SetTargetTransform(BodyId, b3WorldTransform{ Position, Rotation }, FixedDt, Update.bActivate);
                            break;
                        }
                        case EMoveMode::ActivateOnly:
                        {
                            b3Body_SetAwake(BodyId, true);
                            break;
                        }
                    }
                    break;
                }
            }
        }

        // The drive velocity outlives its target, so a body nobody placed again would keep drifting.
        if (bFirstStep)
        {
        for (uint32 Handle : PreviousAuthoredKinematicHandles)
        {
            const b3BodyId BodyId = ResolveBody(Handle);
            if (BodyAuthoredKinematic[Handle] == 0 && b3Body_IsValid(BodyId) && b3Body_GetType(BodyId) == b3_kinematicBody)
            {
                b3Body_SetLinearVelocity(BodyId, b3Vec3{ 0.0f, 0.0f, 0.0f });
                b3Body_SetAngularVelocity(BodyId, b3Vec3{ 0.0f, 0.0f, 0.0f });
            }
        }

        }

        // Carried forward with the payload intact, since a blanket clear lost a spawn-then-SetLocation.
        RetryBodyUpdates.clear();
        for (auto [Entity, BodyComponent, Update] : BodySyncView.Each())
        {
            if (FindEntityBody(Entity) == InvalidBodyHandle)
            {
                RetryBodyUpdates.push_back({ Entity, Update });
            }
        }

        Registry.ClearComponent<FNeedsPhysicsBodyUpdate>();

        for (const FDeferredBodyUpdate& Retry : RetryBodyUpdates)
        {
            if (Registry.IsValid(Retry.Entity))
            {
                Registry.Emplace<FNeedsPhysicsBodyUpdate>(Retry.Entity, Retry.Update);
            }
        }
    }

    void FBox3DPhysicsScene::MarkAuthoredKinematic(uint32 Handle)
    {
        if (Handle >= BodyAuthoredKinematic.size())
        {
            BodyAuthoredKinematic.resize(Handle + 1, 0);
        }

        if (BodyAuthoredKinematic[Handle] == 0)
        {
            BodyAuthoredKinematic[Handle] = 1;
            AuthoredKinematicHandles.push_back(Handle);
        }
    }

    uint32 FBox3DPhysicsScene::StageInterpSlot(uint32 BodyHandle, const FVector3& Position, const FQuat& Rotation)
    {
        if (BodyHandle >= BodyStagingSlot.size())
        {
            BodyStagingSlot.resize(BodyHandle + 1, InvalidBodyHandle);
        }

        uint32 Slot = BodyStagingSlot[BodyHandle];
        if (Slot != InvalidBodyHandle)
        {
            return Slot;
        }

        Slot = (uint32)InterpStaging.Entities.size();
        BodyStagingSlot[BodyHandle] = Slot;
        StagedBodyHandles.push_back(BodyHandle);

        InterpStaging.PushBack();
        InterpStaging.Flags[Slot] = EInterpFlag::Interpolate;
        InterpStaging.PrevPos[Slot] = Position;
        InterpStaging.CurrPos[Slot] = Position;
        InterpStaging.PrevQx[Slot] = InterpStaging.CurrQx[Slot] = Rotation.x;
        InterpStaging.PrevQy[Slot] = InterpStaging.CurrQy[Slot] = Rotation.y;
        InterpStaging.PrevQz[Slot] = InterpStaging.CurrQz[Slot] = Rotation.z;
        InterpStaging.PrevQw[Slot] = InterpStaging.CurrQw[Slot] = Rotation.w;
        return Slot;
    }

    void FBox3DPhysicsScene::DrainMoveEvents(bool bStageForInterp)
    {
        LUMINA_PROFILE_SCOPE();

        const b3BodyEvents Events = b3World_GetBodyEvents(WorldId);
        if (Events.moveCount == 0)
        {
            return;
        }

        DropCharacterInterpSlots();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        auto RigidStorage = Registry.GetStorage<SRigidBodyComponent>();
        const float KillHeight = World->GetDefaultWorldSettings().WorldKillHeight;

        for (int32 i = 0; i < Events.moveCount; ++i)
        {
            const b3BodyMoveEvent& Event = Events.moveEvents[i];

            // The event carries the user data, so resolving the entity costs no body lookup.
            const ECS::FEntity Entity = UnpackEntity(Event.userData);
            const uint32 Handle = UnpackHandle(Event.userData);
            if (!RigidStorage.Contains(Entity))
            {
                continue;
            }

            // One lookup per event, since every field below lives on this record.
            FBodyRecord* Found = RigidBodies.Find(Entity);
            if (Found == nullptr || Found->Handle != Handle)
            {
                continue;
            }
            FBodyRecord& Record = *Found;

            const FVector3 NewPosition = Box3DUtils::FromB3Vec3(Event.transform.p);
            const FQuat NewRotation = Box3DUtils::FromB3Quat(Event.transform.q);

            if (Handle < BodyAwake.size())
            {
                if (BodyAwake[Handle] == 0)
                {
                    BodyAwake[Handle] = 1;
                    ActivationDrainScratch.push_back({ Entity, true });
                }
                if (Event.fellAsleep)
                {
                    BodyAwake[Handle] = 0;
                    ActivationDrainScratch.push_back({ Entity, false });
                }
            }

            // Gameplay already put its transform where it should draw, and the step only chased that placement.
            if (Handle < BodyAuthoredKinematic.size() && BodyAuthoredKinematic[Handle] != 0)
            {
                Record.LastBodyPosition = NewPosition;
                Record.LastBodyRotation = NewRotation;
                continue;
            }

            const uint32 Slot = StageInterpSlot(Handle, Record.LastBodyPosition, Record.LastBodyRotation);

            if (bStageForInterp)
            {
                InterpStaging.PrevPos[Slot] = Record.LastBodyPosition;
                InterpStaging.PrevQx[Slot] = Record.LastBodyRotation.x;
                InterpStaging.PrevQy[Slot] = Record.LastBodyRotation.y;
                InterpStaging.PrevQz[Slot] = Record.LastBodyRotation.z;
                InterpStaging.PrevQw[Slot] = Record.LastBodyRotation.w;
            }

            InterpStaging.Entities[Slot] = Entity;
            InterpStaging.CurrPos[Slot] = NewPosition;
            InterpStaging.CurrQx[Slot] = NewRotation.x;
            InterpStaging.CurrQy[Slot] = NewRotation.y;
            InterpStaging.CurrQz[Slot] = NewRotation.z;
            InterpStaging.CurrQw[Slot] = NewRotation.w;
            InterpStaging.Flags[Slot] = NewPosition.y < KillHeight ? EInterpFlag::BelowKill : EInterpFlag::Interpolate;

            Record.LastBodyPosition = NewPosition;
            Record.LastBodyRotation = NewRotation;
        }
    }

    void FBox3DPhysicsScene::ResetInterpStaging()
    {
        for (uint32 Handle : StagedBodyHandles)
        {
            if (Handle < BodyStagingSlot.size())
            {
                BodyStagingSlot[Handle] = InvalidBodyHandle;
            }
        }
        StagedBodyHandles.clear();
        InterpStaging.Clear();
        bInterpCharacterTail = false;
    }

    void FBox3DPhysicsScene::DropCharacterInterpSlots()
    {
        if (bInterpCharacterTail)
        {
            InterpStaging.Truncate(InterpBodySlots);
            bInterpCharacterTail = false;
        }
    }

    // nlerp not slerp, since the per-frame alpha is tiny and it drops the per-body trig.
    static void NlerpQuatsSoA(float* Qx, float* Qy, float* Qz, float* Qw,
                              const float* Px, const float* Py, const float* Pz, const float* Pw,
                              uint32 Count, float Alpha)
    {
        using namespace SIMD;
        const VFloat8 A         = VFloat8::Broadcast(Alpha);
        const VFloat8 OneMinusA = VFloat8::Broadcast(1.0f - Alpha);
        const VFloat8 VZero     = VFloat8::Zero();

        uint32 i = 0;
        for (; i + 8 <= Count; i += 8)
        {
            const VFloat8 px = VFloat8::Load(Px + i), py = VFloat8::Load(Py + i),
                          pz = VFloat8::Load(Pz + i), pw = VFloat8::Load(Pw + i);
            VFloat8 cx = VFloat8::Load(Qx + i), cy = VFloat8::Load(Qy + i),
                    cz = VFloat8::Load(Qz + i), cw = VFloat8::Load(Qw + i);

            const VFloat8 Dot  = MulAdd(px, cx, MulAdd(py, cy, MulAdd(pz, cz, pw * cw)));
            const VFloat8 Flip = CmpLt(Dot, VZero);
            cx = Select(Flip, -cx, cx); cy = Select(Flip, -cy, cy);
            cz = Select(Flip, -cz, cz); cw = Select(Flip, -cw, cw);

            VFloat8 ox = MulAdd(px, OneMinusA, cx * A);
            VFloat8 oy = MulAdd(py, OneMinusA, cy * A);
            VFloat8 oz = MulAdd(pz, OneMinusA, cz * A);
            VFloat8 ow = MulAdd(pw, OneMinusA, cw * A);

            const VFloat8 Inv = InvSqrt(MulAdd(ox, ox, MulAdd(oy, oy, MulAdd(oz, oz, ow * ow))));
            (ox * Inv).Store(Qx + i); (oy * Inv).Store(Qy + i);
            (oz * Inv).Store(Qz + i); (ow * Inv).Store(Qw + i);
        }

        const float OneMinus = 1.0f - Alpha;
        for (; i < Count; ++i)
        {
            const float px = Px[i], py = Py[i], pz = Pz[i], pw = Pw[i];
            float cx = Qx[i], cy = Qy[i], cz = Qz[i], cw = Qw[i];
            if (px * cx + py * cy + pz * cz + pw * cw < 0.0f) { cx = -cx; cy = -cy; cz = -cz; cw = -cw; }

            const float ox = px * OneMinus + cx * Alpha, oy = py * OneMinus + cy * Alpha,
                        oz = pz * OneMinus + cz * Alpha, ow = pw * OneMinus + cw * Alpha;
            const float Inv = 1.0f / std::sqrt(ox * ox + oy * oy + oz * oz + ow * ow);
            Qx[i] = ox * Inv; Qy[i] = oy * Inv; Qz[i] = oz * Inv; Qw[i] = ow * Inv;
        }
    }

    void FBox3DPhysicsScene::BuildInterpolatedTransforms(float Alpha)
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        const float KillHeight = World->GetDefaultWorldSettings().WorldKillHeight;

        DropCharacterInterpSlots();
        InterpBodySlots = (uint32)InterpStaging.Entities.size();
        bInterpCharacterTail = true;

        // Characters are driven by the mover rather than the solver, so they never raise move events.
        Registry.View<SCharacterPhysicsComponent>().ForEach([&](ECS::FEntity Entity, SCharacterPhysicsComponent& Component)
        {
            if (!Component.Character)
            {
                return;
            }

            const uint32 Slot = (uint32)InterpStaging.Entities.size();
            InterpStaging.PushBack();

            const FVector3 CurrentPosition = Component.Character->Position;
            FQuat CurrentRotation = Component.Character->Rotation;
            FQuat PreviousRotation = Component.LastBodyRotation;

            // Look input steers the capsule rather than being simulated, so it shows the frame it arrives instead of a step later.
            const SCharacterMovementComponent* Movement = Registry.TryGet<SCharacterMovementComponent>(Entity);
            const SCharacterControllerComponent* Controller = Registry.TryGet<SCharacterControllerComponent>(Entity);
            if (Movement != nullptr && Controller != nullptr && Movement->bUseControllerRotation)
            {
                CurrentRotation = FQuat(FVector3(0.0f, Math::Radians(Controller->LookInput.x), 0.0f));
                PreviousRotation = CurrentRotation;
            }

            InterpStaging.Entities[Slot] = Entity;
            InterpStaging.Flags[Slot] = CurrentPosition.y < KillHeight ? EInterpFlag::BelowKill : EInterpFlag::Interpolate;
            InterpStaging.PrevPos[Slot] = Component.LastBodyPosition;
            InterpStaging.CurrPos[Slot] = CurrentPosition;
            InterpStaging.PrevQx[Slot] = PreviousRotation.x;
            InterpStaging.PrevQy[Slot] = PreviousRotation.y;
            InterpStaging.PrevQz[Slot] = PreviousRotation.z;
            InterpStaging.PrevQw[Slot] = PreviousRotation.w;
            InterpStaging.CurrQx[Slot] = CurrentRotation.x;
            InterpStaging.CurrQy[Slot] = CurrentRotation.y;
            InterpStaging.CurrQz[Slot] = CurrentRotation.z;
            InterpStaging.CurrQw[Slot] = CurrentRotation.w;
        });

        const uint32 Total = (uint32)InterpStaging.Entities.size();
        if (Total == 0)
        {
            return;
        }

        InterpStaging.EnsureLerpCapacity();

        SIMD::LerpArray(reinterpret_cast<float*>(InterpStaging.LerpPos.data()),
                        reinterpret_cast<const float*>(InterpStaging.PrevPos.data()),
                        reinterpret_cast<const float*>(InterpStaging.CurrPos.data()),
                        int(Total) * 3, Alpha);

        Algo::Copy(InterpStaging.CurrQx, InterpStaging.LerpQx.begin());
        Algo::Copy(InterpStaging.CurrQy, InterpStaging.LerpQy.begin());
        Algo::Copy(InterpStaging.CurrQz, InterpStaging.LerpQz.begin());
        Algo::Copy(InterpStaging.CurrQw, InterpStaging.LerpQw.begin());

        NlerpQuatsSoA(InterpStaging.LerpQx.data(), InterpStaging.LerpQy.data(),
                      InterpStaging.LerpQz.data(), InterpStaging.LerpQw.data(),
                      InterpStaging.PrevQx.data(), InterpStaging.PrevQy.data(),
                      InterpStaging.PrevQz.data(), InterpStaging.PrevQw.data(),
                      Total, Alpha);
    }

    // Writes the blend into the transform itself, so cameras, attachments and scripts see one pose and the body keeps the simulated one.
    void FBox3DPhysicsScene::ApplyInterpolatedTransforms()
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        const uint32 Count = (uint32)InterpStaging.Entities.size();
        if (Count == 0)
        {
            return;
        }

        auto TransformStorage = Registry.GetStorage<STransformComponent>();

        ECS::Utils::FlushDirtyPhysicsBodies(Registry);

        const auto PendingTeleport = Registry.GetStorage<FNeedsPhysicsBodyUpdate>();

        InterpApplied.clear();
        InterpApplied.reserve(Count);
        InterpAppliedParented.clear();

        // Structural work stays serial, so the passes below only write components that already exist.
        for (uint32 i = 0; i < Count; ++i)
        {
            const EInterpFlag Flag = InterpStaging.Flags[i];
            const ECS::FEntity Entity = InterpStaging.Entities[i];

            if (Flag == EInterpFlag::Skip || !Registry.IsValid(Entity))
            {
                continue;
            }

            if (Flag == EInterpFlag::BelowKill)
            {
                Registry.Destroy(Entity);
                continue;
            }

            // An authored move outranks the body pose until the teleport reaches the body.
            if (!TransformStorage.Contains(Entity) || PendingTeleport.Contains(Entity))
            {
                continue;
            }

            (TransformStorage.Get(Entity).bIsFlat ? InterpApplied : InterpAppliedParented).push_back(i);
        }

        auto WritePose = [&](uint32 i)
        {
            TransformStorage.Get(InterpStaging.Entities[i]).SetFromPhysics(InterpStaging.LerpPos[i],
                FQuat(InterpStaging.LerpQw[i], InterpStaging.LerpQx[i], InterpStaging.LerpQy[i], InterpStaging.LerpQz[i]));
        };

        const uint32 FlatCount = (uint32)InterpApplied.size();
        const auto WriteFlat = [&](uint32 Index)
        {
            WritePose(InterpApplied[Index]);
        };
        if (FlatCount > InterpParallelThreshold)
        {
            Task::ParallelFor(FlatCount, WriteFlat);
        }
        else
        {
            for (uint32 Index = 0; Index < FlatCount; ++Index)
            {
                WriteFlat(Index);
            }
        }

        // The resolve carries a parented body's pose down to everything attached to it.
        if (!InterpAppliedParented.empty())
        {
            for (uint32 i : InterpAppliedParented)
            {
                WritePose(i);
            }
            ECS::Utils::ResolveAllDirtyTransforms(Registry);
        }
    }

    void FBox3DPhysicsScene::Update(double DeltaTime)
    {
        LUMINA_PROFILE_SCOPE();

        RebuildStaleDynamicMeshBodies(ECS::GetWorldRegistry(*World));
        SynchronizeBodies();

        const Lumina::SDefaultWorldSettings& WorldSettings = World->GetDefaultWorldSettings();
        if (WorldSettingsChanged(WorldSettings))
        {
            ApplyWorldSettings(WorldSettings);
        }

        const float PhysicsRateHz = Math::Max(10.0f, WorldSettings.PhysicsHz);
        const float FixedTimestep = 1.0f / PhysicsRateHz;
        const float MaxAccumulation = (float)WorldSettings.MaxPhysicsSteps * FixedTimestep;
        const int32 SubStepCount = (int32)Math::Clamp(WorldSettings.SolverSubStepCount, 1u, 16u);

        Accumulator = Math::Min(Accumulator + (float)DeltaTime, MaxAccumulation);

        CollisionSteps = Accumulator >= FixedTimestep
            ? Math::Min((uint32)WorldSettings.MaxPhysicsSteps, (uint32)(Accumulator / FixedTimestep))
            : 0;

        // Move events only come from a step, so a frame without one blends the last step's bodies again at its own alpha.
        if (CollisionSteps > 0)
        {
            // Fixed scripts start from the simulated pose, and a body that stops moving rests where it was simulated.
            BuildInterpolatedTransforms(1.0f);
            ApplyInterpolatedTransforms();
            ResetInterpStaging();
        }
        ContactDrainScratch.clear();
        ActivationDrainScratch.clear();

        if (CollisionSteps > 0)
        {

            for (uint32 Step = 0; Step < CollisionSteps; ++Step)
            {
                if (PreStepCallback)
                {
                    // Later steps read the pose the previous one produced, so it is written through at alpha 1.
                    if (Step > 0)
                    {
                        BuildInterpolatedTransforms(1.0f);
                        ApplyInterpolatedTransforms();
                    }

                    PreStepCallback(FixedTimestep);
                }

                SynchronizeBodies();
                ApplyDirtyTransforms(FixedTimestep, CollisionSteps - Step, Step == 0);
                if (Step == 0) { LatchCharacterInput(); }
                ApplyBodyCommands();

                // After the fixed script tick, so input a script sets this step drives this step.
                UpdateVehicles(FixedTimestep);

                b3World_Step(WorldId, FixedTimestep, SubStepCount);

                // Box3D clears its event arrays each step, so they are drained before the next one runs.
                DrainMoveEvents(Step == CollisionSteps - 1);
                DrainStepEvents();

                UpdateCharacters(FixedTimestep);
            }

            MonitorBreakableConstraints(FixedTimestep);

            Accumulator -= (float)CollisionSteps * FixedTimestep;
        }

        LUMINA_PROFILE_VALUE("Physics/AwakeBodies", (int64)b3World_GetAwakeBodyCount(WorldId));
        LUMINA_PROFILE_VALUE("Physics/AwakeContacts", (int64)b3World_GetCounters(WorldId).awakeContactCount);

        if (FBox3DPhysicsContext::IsDebugDrawEnabled())
        {
            FBox3DPhysicsContext::GetDebugRenderer()->DrawWorld(WorldId, World);
        }

        const float InterpolationAlpha = WorldSettings.bEnablePhysicsInterpolation
            ? Math::Clamp(Accumulator / FixedTimestep, 0.0f, 1.0f)
            : 1.0f;

        BuildInterpolatedTransforms(InterpolationAlpha);
    }

    void FBox3DPhysicsScene::DispatchPendingEvents()
    {
        LUMINA_PROFILE_SCOPE();

        // Written before contact callbacks so scripts read fresh transforms.
        ApplyInterpolatedTransforms();
        PoseVehicleWheels();
        DispatchContactEvents();
        DispatchActivationEvents();
    }
}
