#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "World/ECS/EventDispatcher.h"
#include "Box3DPhysicsScene.h"

#include "Box3DCharacterHandle.h"
#include "Box3DInternal.h"
#include "Box3DPhysics.h"
#include "Box3DUtils.h"

#include "Core/Console/ConsoleVariable.h"
#include "Core/Math/SIMD/SIMD.h"
#include "Log/Log.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Events/CollisionEvent.h"
#include "World/Subsystems/WorldSettings.h"
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

        // One rebuild after the bulk spawn beats the incremental inserts each static shape would have done.
        b3World_RebuildStaticTree(WorldId);

        Registry.GetSignals<SCharacterPhysicsComponent>().OnConstruct.Connect<&FBox3DPhysicsScene::OnCharacterComponentConstructed>(this);
        Registry.GetSignals<SCharacterPhysicsComponent>().OnDestroy.Connect<&FBox3DPhysicsScene::OnCharacterComponentDestroyed>(this);

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

        ECS::FEventDispatcher& Dispatcher = *World->GetSingleton<ECS::FEventDispatcher*>();
        Dispatcher.Sink<SImpulseEvent>().Connect<&FBox3DPhysicsScene::OnImpulseEvent>(this);
        Dispatcher.Sink<SForceEvent>().Connect<&FBox3DPhysicsScene::OnForceEvent>(this);
        Dispatcher.Sink<STorqueEvent>().Connect<&FBox3DPhysicsScene::OnTorqueEvent>(this);
        Dispatcher.Sink<SAngularImpulseEvent>().Connect<&FBox3DPhysicsScene::OnAngularImpulseEvent>(this);
        Dispatcher.Sink<SSetVelocityEvent>().Connect<&FBox3DPhysicsScene::OnSetVelocityEvent>(this);
        Dispatcher.Sink<SSetAngularVelocityEvent>().Connect<&FBox3DPhysicsScene::OnSetAngularVelocityEvent>(this);
        Dispatcher.Sink<SAddImpulseAtPositionEvent>().Connect<&FBox3DPhysicsScene::OnAddImpulseAtPositionEvent>(this);
        Dispatcher.Sink<SAddForceAtPositionEvent>().Connect<&FBox3DPhysicsScene::OnAddForceAtPositionEvent>(this);
        Dispatcher.Sink<SSetGravityFactorEvent>().Connect<&FBox3DPhysicsScene::OnSetGravityFactorEvent>(this);
    }

    void FBox3DPhysicsScene::StopSimulate()
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        Registry.GetSignals<SCharacterPhysicsComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnCharacterComponentConstructed>(this);
        Registry.GetSignals<SCharacterPhysicsComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnCharacterComponentDestroyed>(this);

        Registry.GetSignals<SRigidBodyComponent>().OnUpdate.Disconnect<&FBox3DPhysicsScene::OnRigidBodyComponentUpdated>(this);
        Registry.GetSignals<SRigidBodyComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnRigidBodyComponentConstructed>(this);
        Registry.GetSignals<SRigidBodyComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnRigidBodyComponentDestroyed>(this);

        Registry.GetSignals<SPhysicsConstraintComponent>().OnConstruct.Disconnect<&FBox3DPhysicsScene::OnConstraintComponentConstructed>(this);
        Registry.GetSignals<SPhysicsConstraintComponent>().OnDestroy.Disconnect<&FBox3DPhysicsScene::OnConstraintComponentDestroyed>(this);

        ECS::FEventDispatcher& Dispatcher = *World->GetSingleton<ECS::FEventDispatcher*>();
        Dispatcher.Sink<SImpulseEvent>().Disconnect<&FBox3DPhysicsScene::OnImpulseEvent>(this);
        Dispatcher.Sink<SForceEvent>().Disconnect<&FBox3DPhysicsScene::OnForceEvent>(this);
        Dispatcher.Sink<STorqueEvent>().Disconnect<&FBox3DPhysicsScene::OnTorqueEvent>(this);
        Dispatcher.Sink<SAngularImpulseEvent>().Disconnect<&FBox3DPhysicsScene::OnAngularImpulseEvent>(this);
        Dispatcher.Sink<SSetVelocityEvent>().Disconnect<&FBox3DPhysicsScene::OnSetVelocityEvent>(this);
        Dispatcher.Sink<SSetAngularVelocityEvent>().Disconnect<&FBox3DPhysicsScene::OnSetAngularVelocityEvent>(this);
        Dispatcher.Sink<SAddImpulseAtPositionEvent>().Disconnect<&FBox3DPhysicsScene::OnAddImpulseAtPositionEvent>(this);
        Dispatcher.Sink<SAddForceAtPositionEvent>().Disconnect<&FBox3DPhysicsScene::OnAddForceAtPositionEvent>(this);
        Dispatcher.Sink<SSetGravityFactorEvent>().Disconnect<&FBox3DPhysicsScene::OnSetGravityFactorEvent>(this);

        Registry.View<SRigidBodyComponent>().ForEach([&](ECS::FEntity EntityID, SRigidBodyComponent&)
        {
            OnRigidBodyComponentDestroyed(Registry, EntityID);
        });

        // The hook is already disconnected above, so the proxies have to be released by hand like the bodies.
        Registry.View<SCharacterPhysicsComponent>().ForEach([&](ECS::FEntity EntityID, SCharacterPhysicsComponent&)
        {
            OnCharacterComponentDestroyed(Registry, EntityID);
        });

        DestroyAllStaticBodyGroups();
    }

    void FBox3DPhysicsScene::ApplyDirtyTransforms(float FixedDt)
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        ECS::Utils::FlushDirtyPhysicsBodies(Registry);

        auto BodySyncView = Registry.View<SRigidBodyComponent, FNeedsPhysicsBodyUpdate>();

        // Box3D body writes touch shared world arrays, so this stays serial rather than a ParallelFor.
        for (auto [Entity, BodyComponent, Update] : BodySyncView.Each())
        {
            const b3BodyId BodyId = ResolveBody(BodyComponent.BodyID);
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
                BodyComponent.LastBodyPosition = TargetLocation;
                BodyComponent.LastBodyRotation = TargetRotation;
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
                    b3Body_SetTargetTransform(BodyId, b3WorldTransform{ Position, Rotation }, FixedDt, Update.bActivate);
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

        // Carried forward with the payload intact, since a blanket clear lost a spawn-then-SetLocation.
        RetryBodyUpdates.clear();
        for (auto [Entity, BodyComponent, Update] : BodySyncView.Each())
        {
            if (BodyComponent.BodyID == InvalidBodyHandle)
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

            SRigidBodyComponent& BodyComponent = RigidStorage.Get(Entity);
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

            const uint32 Slot = StageInterpSlot(Handle, BodyComponent.LastBodyPosition, BodyComponent.LastBodyRotation);

            if (bStageForInterp)
            {
                InterpStaging.PrevPos[Slot] = BodyComponent.LastBodyPosition;
                InterpStaging.PrevQx[Slot] = BodyComponent.LastBodyRotation.x;
                InterpStaging.PrevQy[Slot] = BodyComponent.LastBodyRotation.y;
                InterpStaging.PrevQz[Slot] = BodyComponent.LastBodyRotation.z;
                InterpStaging.PrevQw[Slot] = BodyComponent.LastBodyRotation.w;
            }

            InterpStaging.Entities[Slot] = Entity;
            InterpStaging.CurrPos[Slot] = NewPosition;
            InterpStaging.CurrQx[Slot] = NewRotation.x;
            InterpStaging.CurrQy[Slot] = NewRotation.y;
            InterpStaging.CurrQz[Slot] = NewRotation.z;
            InterpStaging.CurrQw[Slot] = NewRotation.w;
            InterpStaging.Flags[Slot] = NewPosition.y < KillHeight ? EInterpFlag::BelowKill : EInterpFlag::Interpolate;

            BodyComponent.LastBodyPosition = NewPosition;
            BodyComponent.LastBodyRotation = NewRotation;
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
            const FQuat CurrentRotation = Component.Character->Rotation;

            InterpStaging.Entities[Slot] = Entity;
            InterpStaging.Flags[Slot] = CurrentPosition.y < KillHeight ? EInterpFlag::BelowKill : EInterpFlag::Interpolate;
            InterpStaging.PrevPos[Slot] = Component.LastBodyPosition;
            InterpStaging.CurrPos[Slot] = CurrentPosition;
            InterpStaging.PrevQx[Slot] = Component.LastBodyRotation.x;
            InterpStaging.PrevQy[Slot] = Component.LastBodyRotation.y;
            InterpStaging.PrevQz[Slot] = Component.LastBodyRotation.z;
            InterpStaging.PrevQw[Slot] = Component.LastBodyRotation.w;
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

    void FBox3DPhysicsScene::ApplyInterpolatedTransforms()
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 Count = (uint32)InterpStaging.Entities.size();
        if (Count == 0)
        {
            return;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        auto RenderStorage = Registry.GetStorage<FRenderTransform>();

        ECS::Utils::FlushDirtyPhysicsBodies(Registry);

        const auto PendingTeleport = Registry.GetStorage<FNeedsPhysicsBodyUpdate>();

        InterpApplied.clear();
        InterpApplied.reserve(Count);

        bool bAnyParented = false;

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

            if (!TransformStorage.Contains(Entity))
            {
                continue;
            }

            // An authored move outranks the body pose, so drop the override and show the target.
            if (PendingTeleport.Contains(Entity))
            {
                if (RenderStorage.Contains(Entity))
                {
                    RenderStorage.RemoveEntity(Entity);
                }
                continue;
            }

            STransformComponent& TransformComponent = TransformStorage.Get(Entity);
            TransformComponent.SetFromPhysics(InterpStaging.CurrPos[i],
                FQuat(InterpStaging.CurrQw[i], InterpStaging.CurrQx[i], InterpStaging.CurrQy[i], InterpStaging.CurrQz[i]));

            bAnyParented |= !TransformComponent.bIsFlat;

            InterpApplied.push_back(i);
        }

        // A flat body resolved itself in the write above, so the whole registry sweep is owed to nobody.
        if (bAnyParented)
        {
            ECS::Utils::ResolveAllDirtyTransforms(Registry);
        }

        // The override has to exist before the writes below, which may run on workers and cannot grow a pool.
        for (uint32 i : InterpApplied)
        {
            const ECS::FEntity Entity = InterpStaging.Entities[i];
            if (!RenderStorage.Contains(Entity))
            {
                RenderStorage.Emplace(Entity, FRenderTransform{});
            }
        }

        auto WriteRenderPose = [&](uint32 Index)
        {
            const uint32 i = InterpApplied[Index];
            const ECS::FEntity Entity = InterpStaging.Entities[i];

            FTransform RenderPose = TransformStorage.Get(Entity).GetWorldTransformCached();
            RenderPose.SetLocation(InterpStaging.LerpPos[i]);
            RenderPose.SetRotation(FQuat(InterpStaging.LerpQw[i], InterpStaging.LerpQx[i],
                                         InterpStaging.LerpQy[i], InterpStaging.LerpQz[i]));

            RenderStorage.Get(Entity).Matrix = RenderPose.GetMatrix();
        };

        const uint32 AppliedCount = (uint32)InterpApplied.size();
        if (AppliedCount > InterpParallelThreshold)
        {
            Task::ParallelFor(AppliedCount, WriteRenderPose);
        }
        else
        {
            for (uint32 Index = 0; Index < AppliedCount; ++Index)
            {
                WriteRenderPose(Index);
            }
        }
    }

    void FBox3DPhysicsScene::Update(double DeltaTime)
    {
        LUMINA_PROFILE_SCOPE();

        bStepInProgress.store(true, std::memory_order_release);
        struct FStepGuard { TAtomic<bool>& F; ~FStepGuard() { F.store(false, std::memory_order_release); } } StepGuard{ bStepInProgress };

        RebuildStaleDynamicMeshBodies(ECS::GetWorldRegistry(*World));

        {
            TQueue<ECS::FEntity> PendingThisStep;
            {
                FScopeLock Lock(PendingRigidBodyMutex);
                PendingThisStep.swap(PendingRigidBodyCreations);
            }

            PendingDrainScratch.clear();
            while (!PendingThisStep.empty())
            {
                const ECS::FEntity Entity = PendingThisStep.front();
                PendingThisStep.pop();

                if (World->IsValidEntity(Entity))
                {
                    PendingDrainScratch.push_back(Entity);
                }
            }

            CreateRigidBodiesBatched(PendingDrainScratch);
        }

        DrainPendingConstraints();

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

        ResetInterpStaging();
        ContactDrainScratch.clear();
        ActivationDrainScratch.clear();

        if (CollisionSteps > 0)
        {
            ApplyDirtyTransforms(FixedTimestep);
            LatchCharacterInput();

            for (uint32 Step = 0; Step < CollisionSteps; ++Step)
            {
                b3World_Step(WorldId, FixedTimestep, SubStepCount);

                // Box3D clears its event arrays each step, so they are drained before the next one runs.
                DrainMoveEvents(Step == CollisionSteps - 1);
                DrainStepEvents();

                UpdateCharacters(FixedTimestep);
            }

            MonitorBreakableConstraints(FixedTimestep);

            Accumulator -= (float)CollisionSteps * FixedTimestep;
        }

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
        DispatchContactEvents();
        DispatchActivationEvents();
    }
}
