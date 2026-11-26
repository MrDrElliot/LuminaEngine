#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "Box3DPhysicsScene.h"

#include <box3d/collision.h>

#include "Box3DCharacterHandle.h"
#include "Box3DInternal.h"
#include "Box3DUtils.h"

#include "Log/Log.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/RendererUtils.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"
#include "World/Entity/Systems/SystemSingletons.h"
#include "World/Subsystems/WorldSettings.h"

namespace Lumina::Physics
{
    namespace
    {
        // Matches the per-shape buffer b3World_CollideMover fills.
        constexpr int32 kMaxMoverPlanes = 64;
        constexpr float kMoveTolerance = 0.01f;

        // Below this the character is treated as at rest, so slope drift is cut instead of decayed toward zero.
        constexpr float kRestSpeedSq = 0.01f * 0.01f;

        // Below this a floor normal is too close to horizontal for the ramp solve to stay finite.
        constexpr float kMinRampNormalY = 0.1f;

        // A face already this close to vertical acts as a wall unchanged.
        constexpr float kMinSteepNormalY = 0.02f;

        // How far past a ledge's edge the floor under a step is read, enough to clear the bevel of a worn edge.
        constexpr float kLedgeProbeInset = 0.05f;

        // A couple of seconds of fixed steps, long past any deferred collider build.
        constexpr uint32 kAwaitingGroundWarnSteps = 120;

        // Resting characters force a full step this often so an unsignalled world change cannot hold them stale; power of two, phased per entity.
        constexpr uint64 kRestPollMask = 15;

        // A character step is microseconds of world queries, so the fan-out pays for itself at a low count.
        constexpr uint32 kCharacterParallelThreshold = 16;
        constexpr uint32 kCharacterParallelGrain = 4;

        // The latch is a few dozen nanoseconds per character, so it only fans out for a crowd.
        constexpr uint32 kCharacterLatchParallelThreshold = 2048;

        // How long a passing rigid body keeps the characters it came near exact, so a bouncing crate does not toggle their proxies.
        constexpr uint64 kNearBodyHoldSteps = 30;

        // Past a body's reach, so a character it might shove in the next step already has its proxy.
        constexpr float kAwakeBodyMargin = 1.0f;

        // A proxy below this speed falls asleep, low enough that any real walk wakes it again.
        constexpr float kProxySleepThreshold = 0.01f;

        // Slack on a step's reach bound for the speed gained within the step.
        constexpr float kReachSlack = 1.25f;

        // Keeps a massless character from taking an infinite share of a gap.
        constexpr float kMinContactMass = 1.0f;

        // Closer than this two capsules have no usable direction between them, so one is picked from the pair.
        constexpr float kCoincidentDistance = 1.0e-4f;

        // Gathered per move iteration; the extras are only needed to push whatever the mover leaned on.
        struct FMoverPlanes
        {
            b3CollisionPlane    Planes[kMaxMoverPlanes];
            b3Vec3              Points[kMaxMoverPlanes];
            b3ShapeId           Shapes[kMaxMoverPlanes];
            int32               Count = 0;
            // Short of the array when neighbor planes need the tail, so a busy floor cannot crowd them out.
            int32               Capacity = kMaxMoverPlanes;
            float               CosMaxSlope = 0.0f;

            // The first WorldCount planes came from Box3D shapes, the rest stand for other characters.
            int32               WorldCount = 0;

            b3Pos               Origin{};
            b3BodyId            IgnoreBody{};
            SCollisionProfile   Profile{};
            bool                bPermissive = true;
            bool                bCollideWithCharacters = true;
            ECS::FEntity        Entity{};
        };

        bool MoverAcceptsShape(const FMoverPlanes& Query, b3ShapeId ShapeId)
        {
            if (B3_ID_EQUALS(b3Shape_GetBody(ShapeId), Query.IgnoreBody))
            {
                return false;
            }

            if (!Query.bCollideWithCharacters && Box3DUtils::IsCharacterProxyUserData(b3Shape_GetUserData(ShapeId)))
            {
                return false;
            }

            return Box3DUtils::ShouldProfileCollideWithShape(Query.Profile, ShapeId, Query.bPermissive);
        }

        // Too-steep faces collide as a vertical wall, so the offset is rescaled to hold the same contact, since penetration along it is deeper.
        b3Plane UprightIfTooSteep(const b3Plane& Plane, float CosMaxSlope)
        {
            if (Plane.normal.y <= kMinSteepNormalY || Plane.normal.y >= CosMaxSlope)
            {
                return Plane;
            }
            const float Horizontal = b3Length(b3Vec3{ Plane.normal.x, 0.0f, Plane.normal.z });
            const b3Vec3 Upright = b3MulSV(1.0f / Horizontal, b3Vec3{ Plane.normal.x, 0.0f, Plane.normal.z });
            return b3Plane{ Upright, Plane.offset / Horizontal };
        }

        bool GatherPlanes(b3ShapeId ShapeId, const b3PlaneResult* Results, int32 PlaneCount, void* Context)
        {
            FMoverPlanes& Out = *static_cast<FMoverPlanes*>(Context);

            if (!MoverAcceptsShape(Out, ShapeId))
            {
                return true;
            }

            const int32 StartCount = Out.Count;
            for (int32 i = 0; i < PlaneCount && Out.Count < Out.Capacity; ++i)
            {
                Out.Planes[Out.Count] = b3CollisionPlane{ UprightIfTooSteep(Results[i].plane, Out.CosMaxSlope), FLT_MAX, 0.0f, true };
                Out.Points[Out.Count] = b3Add(Out.Origin, Results[i].point);
                Out.Shapes[Out.Count] = ShapeId;
                ++Out.Count;
            }

            if (Out.Count - StartCount < PlaneCount)
            {
                LOG_WARN_ONCE("Box3DCharacter: entity {} gathered more than {} collision planes in one mover step, the rest are dropped.", Out.Entity.Value, kMaxMoverPlanes);
            }

            return true;
        }

        // The crouched capsule drops its center by the height it loses, so the feet stay where they were.
        void ApplyCharacterShape(FPhysicsCharacterHandle& Character, const SCharacterPhysicsComponent& Physics,
                                 const SCharacterMovementComponent* Movement)
        {
            Physics.GetActiveCapsule(Movement, Character.HalfHeight, Character.CrouchDrop);
            Character.Radius            = Physics.Radius;
            Character.TranslationOffset = Physics.TranslationOffset;

            if (b3Shape_IsValid(Character.ProxyShape))
            {
                const b3Capsule Capsule = Character.MakeBodyCapsule();
                b3Shape_SetCapsule(Character.ProxyShape, &Capsule);
            }
        }

        // Everything the mover reads from the component, so a patch takes effect without rebuilding the character.
        void ApplyCharacterSettings(FPhysicsCharacterHandle& Character, const SCharacterPhysicsComponent& Physics,
                                    const SCharacterMovementComponent* Movement)
        {
            Character.Padding                = Physics.Padding;
            Character.StepHeight             = Physics.StepHeight;
            Character.StickToFloorDistance   = Physics.StickToFloorDistance;
            Character.CosMaxSlope            = Math::Cos(Math::Radians(Physics.MaxSlopeAngle));
            Character.MaxStrength            = Physics.MaxStrength;
            Character.Mass                   = Physics.Mass;
            Character.MaxCollisionIterations = (int32)Math::Max(Physics.MaxCollisionIterations, 1u);
            Character.bCollideWithCharacters = Physics.bCollideWithCharacters;
            Character.bLODRelevanceSource    = Physics.bLODRelevanceSource;
            Character.Profile                = Physics.CollisionProfile;
            Character.Filter                 = Box3DUtils::MakeQueryFilter(Physics.CollisionProfile);

            if (b3Shape_IsValid(Character.ProxyShape))
            {
                b3Shape_SetFilter(Character.ProxyShape, Box3DUtils::MakeShapeFilter(Physics.CollisionProfile), true);
                b3Shape_SetUserData(Character.ProxyShape, Box3DUtils::PackProfileUserData(Physics.CollisionProfile, true));
            }

            ApplyCharacterShape(Character, Physics, Movement);
        }

        // A surface facing down that reaches into the standing capsule, which is all that can stop a crouched character standing.
        bool HasRoomToStand(const FPhysicsCharacterHandle& Character, float StandingHalfHeight, ECS::FEntity Entity)
        {
            constexpr float CeilingNormalY = -0.1f;
            constexpr float StandClearance = 0.01f;

            const FVector3& Offset = Character.TranslationOffset;
            const b3Capsule Standing{ b3Vec3{ Offset.x, Offset.y - StandingHalfHeight, Offset.z },
                                      b3Vec3{ Offset.x, Offset.y + StandingHalfHeight, Offset.z }, Character.Radius };

            FMoverPlanes Gathered;
            Gathered.IgnoreBody             = Character.ProxyBody;
            Gathered.Profile                = Character.Profile;
            Gathered.bPermissive            = Box3DUtils::UsesPermissiveCollisionFilter();
            Gathered.bCollideWithCharacters = Character.bCollideWithCharacters;
            Gathered.CosMaxSlope            = Character.CosMaxSlope;
            Gathered.Origin                 = Box3DUtils::ToB3Vec3(Character.Position);
            Gathered.Entity                 = Entity;
            b3World_CollideMover(Character.WorldId, Gathered.Origin, &Standing, Character.Filter, &GatherPlanes, &Gathered);

            const FVector3 Center = Character.Position + Offset;
            for (int32 i = 0; i < Gathered.Count; ++i)
            {
                if (Gathered.Planes[i].plane.normal.y > CeilingNormalY)
                {
                    continue;
                }
                const FVector3 Point(Gathered.Points[i].x, Gathered.Points[i].y, Gathered.Points[i].z);
                const FVector3 OnAxis(Center.x, Math::Clamp(Point.y, Center.y - StandingHalfHeight, Center.y + StandingHalfHeight), Center.z);
                if (Math::Length(Point - OnAxis) < Character.Radius - StandClearance)
                {
                    return false;
                }
            }
            return true;
        }

        bool MoverCastFilter(b3ShapeId ShapeId, void* Context)
        {
            return MoverAcceptsShape(*static_cast<const FMoverPlanes*>(Context), ShapeId);
        }

        // The floor a ray finds, which reports the face itself where the capsule only sees the radial normal of an edge.
        struct FFloorHit
        {
            const FMoverPlanes* Query = nullptr;
            b3ShapeId           Shape{};
            b3Pos               Point{};
            b3Vec3              Normal{ 0.0f, 1.0f, 0.0f };
            bool                bHit = false;
        };

        float ClosestFloorHit(b3ShapeId ShapeId, b3Pos Point, b3Vec3 Normal, float Fraction, uint64_t, int, int, void* Context)
        {
            FFloorHit& Hit = *static_cast<FFloorHit*>(Context);
            if (!MoverAcceptsShape(*Hit.Query, ShapeId))
            {
                return -1.0f;
            }
            Hit.Shape  = ShapeId;
            Hit.Point  = Point;
            Hit.Normal = Normal;
            Hit.bHit   = true;
            return Fraction;
        }

        // Places the proxy on the character by the next world step, written per body so it runs inside the fan-out.
        // False means the proxy sleeps away from its character, and waking it is left to a serial caller passing bWake.
        bool DriveProxy(FPhysicsCharacterHandle& Character, float FixedDt, bool bWake)
        {
            if (!Character.bProxyEnabled || !Box3DUtils::IsSafePose(Character.Position, Character.Rotation))
            {
                return true;
            }
            const b3WorldTransform Target{ Box3DUtils::ToB3Vec3(Character.Position), Box3DUtils::ToB3Quat(Character.Rotation) };
            if (!bWake && !b3Body_IsAwake(Character.ProxyBody))
            {
                return b3LengthSquared(b3SubPos(Target.p, b3Body_GetPosition(Character.ProxyBody))) < kMoveTolerance * kMoveTolerance;
            }
            b3Body_SetTargetTransform(Character.ProxyBody, Target, FixedDt, bWake);
            Character.bProxyInMotion = true;
            return true;
        }

        // Enabled where the character stands now, since the proxy did not follow while it was off.
        void SetProxyEnabled(FPhysicsCharacterHandle& Character, bool bEnabled)
        {
            if (!b3Body_IsValid(Character.ProxyBody))
            {
                return;
            }
            if (bEnabled)
            {
                if (Box3DUtils::IsSafePose(Character.Position, Character.Rotation))
                {
                    b3Body_SetTransform(Character.ProxyBody, Box3DUtils::ToB3Vec3(Character.Position), Box3DUtils::ToB3Quat(Character.Rotation));
                }
                b3Body_Enable(Character.ProxyBody);
            }
            else
            {
                b3Body_Disable(Character.ProxyBody);
            }
            Character.bProxyEnabled = bEnabled;
            Character.bProxyInMotion = false;
        }

        // A remote player keeps everything near it exact on the host, where no camera stands in for it.
        bool IsRelevanceSource(const FPhysicsCharacterHandle& Character)
        {
            return Character.bLODRelevanceSource || Character.NetDrive == (uint8)ECharacterNetDrive::Commands;
        }

        // Teleports and seating place the proxy at once, so a velocity left from a target transform must not carry it on.
        void PlaceProxy(FPhysicsCharacterHandle& Character)
        {
            if (!Box3DUtils::IsSafePose(Character.Position, Character.Rotation))
            {
                return;
            }
            b3Body_SetTransform(Character.ProxyBody, Box3DUtils::ToB3Vec3(Character.Position), Box3DUtils::ToB3Quat(Character.Rotation));
            if (Character.bProxyInMotion)
            {
                b3Body_SetLinearVelocity(Character.ProxyBody, b3Vec3_zero);
                b3Body_SetAngularVelocity(Character.ProxyBody, b3Vec3_zero);
                Character.bProxyInMotion = false;
            }
        }

        // A resting character runs none of the world queries below, so everything that could move it wakes it here.
        bool ShouldWakeResting(const FPhysicsCharacterHandle& Character, const SCharacterMovementComponent& Movement)
        {
            if (Movement.bHasPendingMoveInput || Movement.bPendingJump || Movement.bPendingLaunch || Movement.bPendingTeleport)
            {
                return true;
            }

            if (Movement.bUseControllerRotation && Math::Abs(Movement.PendingLookYaw - Character.RestLookYaw) > Math::kSmallNumber)
            {
                return true;
            }

            // A script can write Velocity onto the component directly instead of staging a launch.
            if (Math::LengthSquared(Movement.Velocity) > kRestSpeedSq)
            {
                return true;
            }

            return !b3Body_IsValid(Character.RestGroundBody);
        }

    }


    void FBox3DPhysicsScene::OnCharacterComponentConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        FBodyRecord& Record = CharacterBodies.FindOrAdd(Entity);
        Record.Revision = NextBindingRevision++;
        PendingCharacters.Add(Entity);
    }

    void FBox3DPhysicsScene::CreateCharacter(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {

        SCharacterPhysicsComponent* Component = Registry.TryGet<SCharacterPhysicsComponent>(Entity);
        if (Component == nullptr || Component->Character)
        {
            return;
        }

        const STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        TSharedPtr<FPhysicsCharacterHandle> Handle = MakeShared<FPhysicsCharacterHandle>();
        Handle->WorldId = WorldId;
        Handle->Position = Transform->GetLocalLocation();
        Handle->Rotation = Transform->GetLocalRotation();
        ApplyCharacterSettings(*Handle, *Component, Registry.TryGet<SCharacterMovementComponent>(Entity));

        Handle->SpawnPosition = Handle->Position;

        // A kinematic proxy so other bodies, queries and contact events still see the character as a body.
        b3BodyDef BodyDef = b3DefaultBodyDef();
        BodyDef.type = b3_kinematicBody;
        BodyDef.position = Box3DUtils::ToB3Vec3(Handle->Position);
        BodyDef.rotation = Box3DUtils::ToB3Quat(Handle->Rotation);
        // Asleep once it stops, so a standing crowd leaves the solver until the drive wakes it.
        BodyDef.enableSleep = true;
        BodyDef.sleepThreshold = kProxySleepThreshold;

        // Created off under character LOD, since the first step enables only the few that are near something.
        Handle->bProxyEnabled = World->GetDefaultWorldSettings().CharacterLODDistance <= 0.0f;
        BodyDef.isEnabled = Handle->bProxyEnabled;

        Handle->ProxyBody = b3CreateBody(WorldId, &BodyDef);
        if (!b3Body_IsValid(Handle->ProxyBody))
        {
            return;
        }

        Handle->ProxyBodyHandle = RegisterBody(Handle->ProxyBody);
        b3Body_SetUserData(Handle->ProxyBody, PackBodyUserData(Entity, Handle->ProxyBodyHandle));

        const b3Capsule LocalCapsule = Handle->MakeBodyCapsule();

        b3ShapeDef ShapeDef = b3DefaultShapeDef();
        ShapeDef.filter = Box3DUtils::MakeShapeFilter(Component->CollisionProfile);
        ShapeDef.userData = Box3DUtils::PackProfileUserData(Component->CollisionProfile, true);
        ShapeDef.enableCustomFiltering = Box3DUtils::UsesPermissiveCollisionFilter();
        ShapeDef.enableContactEvents = true;
        ShapeDef.enableSensorEvents = true;
        Handle->ProxyShape = b3CreateCapsuleShape(Handle->ProxyBody, &ShapeDef, &LocalCapsule);

        FBodyRecord& Record = CharacterBodies.FindOrAdd(Entity);
        Record.Handle = Handle->ProxyBodyHandle;
        Record.Status = EPhysicsBodyStatus::Ready;
        Component->Character = Move(Handle);
        Component->LastBodyPosition = Component->Character->Position;
        Component->LastBodyRotation = Component->Character->Rotation;
    }

    void FBox3DPhysicsScene::OnCharacterComponentUpdated(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        PendingCharacterUpdates.Add(Entity);
    }

    void FBox3DPhysicsScene::ApplyPendingCharacterUpdates(ECS::FRegistry& Registry)
    {
        for (ECS::FEntity Entity : PendingCharacterUpdates)
        {
            SCharacterPhysicsComponent* Physics = Registry.IsValid(Entity) ? Registry.TryGet<SCharacterPhysicsComponent>(Entity) : nullptr;
            if (Physics != nullptr && Physics->Character)
            {
                ApplyCharacterSettings(*Physics->Character, *Physics, Registry.TryGet<SCharacterMovementComponent>(Entity));
                Physics->Character->bResting = false;
            }
        }
        PendingCharacterUpdates.Reset();
    }

    void FBox3DPhysicsScene::UpdateCrouch(ECS::FEntity Entity, SCharacterPhysicsComponent& Physics, SCharacterMovementComponent& Movement)
    {
        const bool bWantsCrouched = Movement.bWantsToCrouch && Movement.bCanCrouch;
        if (bWantsCrouched == Movement.bIsCrouched)
        {
            return;
        }

        FPhysicsCharacterHandle& Character = *Physics.Character;
        if (!bWantsCrouched && !HasRoomToStand(Character, Physics.HalfHeight, Entity))
        {
            return;
        }

        Movement.bIsCrouched = bWantsCrouched;
        ApplyCharacterShape(Character, Physics, &Movement);
        Character.bResting = false;

        SCharacterCrouchEvent& Event = CrouchEvents.EmplaceGetRef();
        Event.Entity    = Entity;
        Event.bCrouched = bWantsCrouched;
    }

    void FBox3DPhysicsScene::OnCharacterComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (const FBodyRecord* Record = CharacterBodies.Find(Entity))
        {
            if (Record->Handle != InvalidBodyHandle)
            {
                PendingBodyDestructions.Add(Record->Handle);
            }
            CharacterBodies.Remove(Entity);
        }
        if (SCharacterPhysicsComponent* Component = Registry.TryGet<SCharacterPhysicsComponent>(Entity))
        {
            if (Component->Character) { Component->Character->ProxyBody = b3_nullBodyId; }
            Component->Character.reset();
        }
    }

    void FBox3DPhysicsScene::LatchCharacterInput()
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = World->GetEntityRegistry();

        auto Controllers = Registry.GetStorage<SCharacterControllerComponent>();
        auto Movements   = Registry.GetStorage<SCharacterMovementComponent>();
        const ECS::FEntity* ControllerEntities = Controllers.GetDenseData();
        const uint32 NumControllers = (uint32)Controllers.GetDenseSize();

        // Each entity's latch touches only its own two components, so a crowd fans out over the dense controller pool.
        const auto Latch = [&](uint32 DenseIndex)
        {
            const ECS::FEntity Entity = ControllerEntities[DenseIndex];
            SCharacterMovementComponent* FoundMovement = Entity.IsTombstone() ? nullptr : Movements.TryGet(Entity);
            if (FoundMovement == nullptr)
            {
                return;
            }
            SCharacterControllerComponent& Controller = Controllers.GetAtDense(DenseIndex);
            SCharacterMovementComponent& Movement = *FoundMovement;

            if (Math::LengthSquared(Controller.MoveInput) > Math::kSmallNumber || Math::LengthSquared(Controller.WorldMoveInput) > Math::kSmallNumber)
            {
                const FVector3 Forward = RenderUtils::GetForwardVector(Controller.LookInput.x, 0.0f);
                const FVector3 Right = RenderUtils::GetRightVector(Controller.LookInput.x);
                const FVector3 Up = Math::Cross(Right, Forward);

                FVector3 Direction = Right * Controller.MoveInput.x + Up * Controller.MoveInput.y + Forward * Controller.MoveInput.z
                                   + Controller.WorldMoveInput;
                const float Magnitude = Math::Length(Direction);
                if (Magnitude > Math::kSmallNumber)
                {
                    Movement.PendingMoveDirection = Direction / Magnitude;
                    Movement.PendingMoveThrottle = Math::Min(Magnitude, 1.0f);
                    Movement.bHasPendingMoveInput = true;
                }
                else
                {
                    Movement.PendingMoveDirection = FVector3(0.0f);
                    Movement.bHasPendingMoveInput = false;
                }
            }
            else
            {
                Movement.PendingMoveDirection = FVector3(0.0f);
                Movement.bHasPendingMoveInput = false;
            }

            Movement.PendingLookYaw = Controller.LookInput.x;
            Movement.PendingButtons = Controller.Buttons;
            Controller.MoveInput = {};
            Controller.WorldMoveInput = {};

            if (Controller.bJumpPressed)
            {
                Movement.bPendingJump = true;
                Controller.bJumpPressed = false;
            }

            Movement.bWantsToCrouch = Controller.bWantsToCrouch;

            if (Controller.bLaunchRequested)
            {
                Controller.bLaunchRequested = false;
                Movement.PendingLaunchVelocity = Controller.PendingLaunchVelocity;
                Movement.bLaunchOverrideHorizontal = Controller.bLaunchOverrideHorizontal;
                Movement.bLaunchOverrideVertical = Controller.bLaunchOverrideVertical;
                Movement.bPendingLaunch = true;
            }

            if (Controller.bTeleportRequested)
            {
                Controller.bTeleportRequested = false;
                Movement.PendingTeleportLocation = Controller.PendingTeleportLocation;
                Movement.bPendingTeleport = true;
            }
        };

        if (NumControllers > kCharacterLatchParallelThreshold)
        {
            Jobs::ParallelFor(NumControllers, Latch);
        }
        else
        {
            for (uint32 DenseIndex = 0; DenseIndex < NumControllers; ++DenseIndex)
            {
                Latch(DenseIndex);
            }
        }
    }

    void FBox3DPhysicsScene::UpdateCharacters(float FixedDt)
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = World->GetEntityRegistry();

        ++CharacterStepCounter;
        int64 RestingCount = 0;

        PlatformTime::FStopwatch Phase;

        const SDefaultWorldSettings& Settings = World->GetDefaultWorldSettings();
        CharacterLODRangeSq = Settings.CharacterLODDistance > 0.0f ? Settings.CharacterLODDistance * Settings.CharacterLODDistance : 0.0f;
        CharacterLODInterval = Math::Clamp(Settings.CharacterLODStepInterval, 1u, 8u);
        CharacterRelevancePoints.Reset();
        if (CharacterLODRangeSq > 0.0f)
        {
            if (const FResolvedSceneView* View = World->GetResolvedView())
            {
                CharacterRelevancePoints.Add(View->ViewVolume.GetViewPosition());
            }
            CharacterRelevancePoints.Append(CharacterRelevanceSources);
        }

        auto PhysicsStorage = Registry.GetStorage<SCharacterPhysicsComponent>();
        auto MovementStorage = Registry.GetStorage<SCharacterMovementComponent>();
        const ECS::FEntity* Entities = PhysicsStorage.GetDenseData();
        const uint32 Num = (uint32)PhysicsStorage.GetDenseSize();
        CharacterPrepStates.SetNum(Num);
        CharacterPrepMovements.SetNum(Num);

        const auto Classify = [&](uint32 Dense)
        {
            const ECS::FEntity Entity = Entities[Dense];
            SCharacterMovementComponent* Movement = Entity.IsTombstone() ? nullptr : MovementStorage.TryGet(Entity);
            SCharacterPhysicsComponent* Physics = Movement != nullptr ? &PhysicsStorage.GetAtDense(Dense) : nullptr;
            CharacterPrepMovements[Dense] = Movement;
            CharacterPrepStates[Dense] = Physics != nullptr && Physics->Character
                ? PrepareCharacter(Registry, Entity, *Physics, *Movement, FixedDt, false)
                : ECharacterPrep::Skip;
        };
        if (Num > kCharacterLatchParallelThreshold)
        {
            Jobs::ParallelFor(Num, Classify);
        }
        else
        {
            for (uint32 Dense = 0; Dense < Num; ++Dense)
            {
                Classify(Dense);
            }
        }

        // Teleports, seating, crouching and proxy placement write Box3D, so the few characters that need one finish here.
        for (uint32 Dense = 0; Dense < Num; ++Dense)
        {
            if (CharacterPrepStates[Dense] == ECharacterPrep::Serial)
            {
                CharacterPrepStates[Dense] = PrepareCharacter(Registry, Entities[Dense], PhysicsStorage.GetAtDense(Dense),
                    *CharacterPrepMovements[Dense], FixedDt, true);
            }
        }

        // Laid out in dense order, so the step and its contacts come out the same on every run.
        constexpr uint32 kPrepChunk = 2048;
        const uint32 ChunkCount = (Num + kPrepChunk - 1) / kPrepChunk;
        CharacterPrepChunks.SetNum(ChunkCount);
        const auto CountChunk = [&](uint32 Chunk)
        {
            FCharacterPrepChunk& Counts = CharacterPrepChunks[Chunk];
            Counts = FCharacterPrepChunk();
            const uint32 End = Math::Min(Num, (Chunk + 1) * kPrepChunk);
            for (uint32 Dense = Chunk * kPrepChunk; Dense < End; ++Dense)
            {
                const ECharacterPrep State = CharacterPrepStates[Dense];
                if (State == ECharacterPrep::Skip)
                {
                    continue;
                }
                const FPhysicsCharacterHandle& Character = *PhysicsStorage.GetAtDense(Dense).Character;
                ++Counts.Contacts;
                Counts.Works += State == ECharacterPrep::Step ? 1u : 0u;
                Counts.Resting += State == ECharacterPrep::Rest ? 1u : 0u;
                Counts.Sources += IsRelevanceSource(Character) ? 1u : 0u;
                Counts.MaxRadius = Math::Max(Counts.MaxRadius, Character.Radius);
            }
        };
        Jobs::ParallelFor(ChunkCount, CountChunk);

        uint32 TotalContacts = 0;
        uint32 TotalWorks = 0;
        CharacterContactMaxRadius = 0.0f;
        CharacterContactMaxReach = 0.0f;
        for (FCharacterPrepChunk& Counts : CharacterPrepChunks)
        {
            Counts.ContactStart = TotalContacts;
            Counts.WorkStart = TotalWorks;
            TotalContacts += Counts.Contacts;
            TotalWorks += Counts.Works;
            RestingCount += Counts.Resting;
            CharacterContactMaxRadius = Math::Max(CharacterContactMaxRadius, Counts.MaxRadius);
        }
        CharacterContacts.SetNum(TotalContacts);
        CharacterWorkScratch.SetNum(TotalWorks);

        // Sources are rare, so only the runs holding one are walked again, and next step judges distance from here.
        CharacterRelevanceSources.Reset();
        for (uint32 Chunk = 0; Chunk < ChunkCount; ++Chunk)
        {
            if (CharacterPrepChunks[Chunk].Sources == 0)
            {
                continue;
            }
            const uint32 End = Math::Min(Num, (Chunk + 1) * kPrepChunk);
            for (uint32 Dense = Chunk * kPrepChunk; Dense < End; ++Dense)
            {
                if (CharacterPrepStates[Dense] != ECharacterPrep::Skip && IsRelevanceSource(*PhysicsStorage.GetAtDense(Dense).Character))
                {
                    CharacterRelevanceSources.Add(PhysicsStorage.GetAtDense(Dense).Character->Position);
                }
            }
        }

        const auto FillChunk = [&](uint32 Chunk)
        {
            FCharacterPrepChunk& Counts = CharacterPrepChunks[Chunk];
            uint32 ContactIndex = Counts.ContactStart;
            uint32 WorkIndex = Counts.WorkStart;
            const uint32 End = Math::Min(Num, (Chunk + 1) * kPrepChunk);
            for (uint32 Dense = Chunk * kPrepChunk; Dense < End; ++Dense)
            {
                const ECharacterPrep State = CharacterPrepStates[Dense];
                if (State == ECharacterPrep::Skip)
                {
                    continue;
                }
                SCharacterPhysicsComponent& Physics = PhysicsStorage.GetAtDense(Dense);
                const FPhysicsCharacterHandle& Character = *Physics.Character;
                const ECS::FEntity Entity = Entities[Dense];

                FCharacterContactBody& Body = CharacterContacts[ContactIndex];
                Body = FCharacterContactBody();
                Body.Character = &*Physics.Character;
                Body.Profile = Character.Profile;
                Body.Entity = Entity;
                Body.Radius = Character.Radius;
                Body.InvMass = 1.0f / Math::Max(Character.Mass, kMinContactMass);
                Body.bMovable = State == ECharacterPrep::Step;
                Body.bCollides = Character.bCollideWithCharacters;

                if (State == ECharacterPrep::Step)
                {
                    // Velocity eases toward the commanded speed, so neither one is exceeded short of a launch.
                    const SCharacterMovementComponent& Movement = *CharacterPrepMovements[Dense];
                    const float Speed = Math::Max(Math::Length(FVector3(Character.Velocity.x, 0.0f, Character.Velocity.z)),
                        Math::Max(Movement.MoveSpeed, Movement.SprintSpeed)) + Math::Length(Character.GroundVelocity);
                    Body.Reach = Speed * FixedDt * (float)Character.LODInterval * kReachSlack;
                    Counts.MaxReach = Math::Max(Counts.MaxReach, Body.Reach);
                    CharacterWorkScratch[WorkIndex++] = FCharacterWork{ &Physics, CharacterPrepMovements[Dense], Entity, ContactIndex, false,
                        FixedDt * (float)Character.LODInterval };
                }
                ++ContactIndex;
            }
        };
        Jobs::ParallelFor(ChunkCount, FillChunk);
        for (const FCharacterPrepChunk& Counts : CharacterPrepChunks)
        {
            CharacterContactMaxReach = Math::Max(CharacterContactMaxReach, Counts.MaxReach);
        }

        if (!CrouchEvents.IsEmpty())
        {
            ECS::SendEvents(World->GetEvents(), TSpan<const SCharacterCrouchEvent>(CrouchEvents.GetData(), CrouchEvents.Num()));
            CrouchEvents.Reset();
        }

        const uint32 WorkCount = (uint32)CharacterWorkScratch.Num();
        LastUpdateTimings.CharacterPrepMs += Phase.ElapsedMilliseconds();
        Phase.Restart();

        BuildCharacterContacts();

        // Anything awake that could reach a character before the next step keeps that character exact, its proxy on first.
        if (CharacterLODRangeSq > 0.0f && !CharacterContacts.IsEmpty())
        {
            for (const FVector4& Reach : AwakeBodyReach)
            {
                const float Range = Reach.w + kAwakeBodyMargin + CharacterContactMaxRadius;
                CharacterContactGrid.ForEachRowSpan(Reach.x, Reach.z, Range, [&](int32 Begin, int32 End)
                {
                    for (int32 Slot = Begin; Slot < End; ++Slot)
                    {
                        const float DX = CharacterContactGrid.SortedPosX[Slot] - Reach.x;
                        const float DZ = CharacterContactGrid.SortedPosZ[Slot] - Reach.z;
                        if (DX * DX + DZ * DZ > Range * Range)
                        {
                            continue;
                        }
                        FPhysicsCharacterHandle& Character = *CharacterContacts[CharacterContactGrid.SortedIndices[Slot]].Character;
                        Character.NearBodyStep = CharacterStepCounter;
                        if (!Character.bProxyEnabled)
                        {
                            SetProxyEnabled(Character, true);
                        }
                    }
                });
            }
        }
        LastUpdateTimings.CharacterContactMs += Phase.ElapsedMilliseconds();
        LastUpdateTimings.SteppedCharacters += WorkCount;
        LastUpdateTimings.RestingCharacters += (uint32)RestingCount;
        Phase.Restart();

        const uint32 ThreadSlots = Math::Max(Jobs::GetNumThreadSlots(), 1u);
        if (static_cast<uint32>(CharacterPushScratch.Num()) < ThreadSlots)
        {
            CharacterPushScratch.SetNum(ThreadSlots);
        }

        for (FCharacterPushBucket& Bucket : CharacterPushScratch)
        {
            Bucket.Clear();
        }

        CharacterProxyWakes.SetNum(WorkCount);
        if (WorkCount > kCharacterParallelThreshold)
        {
            Jobs::ParallelFor(WorkCount, [&](uint32 Index, uint32 Thread)
            {
                StepCharacter(CharacterWorkScratch[Index], CharacterWorkScratch[Index].StepDt, Thread);
                CharacterProxyWakes[Index] = DriveProxy(*CharacterWorkScratch[Index].Physics->Character, FixedDt, false) ? 0 : 1;
            },
            kCharacterParallelGrain);

            for (uint32 Index = 0; Index < WorkCount; ++Index)
            {
                if (CharacterProxyWakes[Index] != 0)
                {
                    DriveProxy(*CharacterWorkScratch[Index].Physics->Character, FixedDt, true);
                }
            }
        }
        else
        {
            for (uint32 Index = 0; Index < WorkCount; ++Index)
            {
                StepCharacter(CharacterWorkScratch[Index], CharacterWorkScratch[Index].StepDt, 0);
                DriveProxy(*CharacterWorkScratch[Index].Physics->Character, FixedDt, true);
            }
        }

        LastUpdateTimings.CharacterStepMs += Phase.ElapsedMilliseconds();
        Phase.Restart();

        // Pushes go through Box3D's shared body arrays, so they land after the fan-out.

        for (const FCharacterPushBucket& Bucket : CharacterPushScratch)
        {
            for (const FPendingCharacterPush& Push : Bucket.Pushes)
            {
                if (b3Body_IsValid(Push.Body))
                {
                    b3Body_ApplyLinearImpulse(Push.Body, Push.Impulse, Push.Point, true);
                }
            }
            for (FPhysicsCharacterHandle* Woken : Bucket.Wakes)
            {
                Woken->bResting = false;
            }
            DeliverCharacterEvents(Bucket);
        }
        LastUpdateTimings.CharacterApplyMs += Phase.ElapsedMilliseconds();

        LUMINA_PROFILE_VALUE("Physics/RestingCharacters", RestingCount);
    }

    void FBox3DPhysicsScene::BuildCharacterContacts()
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 ContactCount = (uint32)CharacterContacts.Num();
        CharacterContactX.SetNum(ContactCount);
        CharacterContactZ.SetNum(ContactCount);

        const auto Snapshot = [&](uint32 Index)
        {
            FCharacterContactBody& Body = CharacterContacts[Index];
            const FPhysicsCharacterHandle& Character = *Body.Character;
            const float CenterY = Character.Position.y + Character.TranslationOffset.y - Character.CrouchDrop;
            Body.Bottom = CenterY - Character.HalfHeight - Character.Radius;
            Body.Top = CenterY + Character.HalfHeight + Character.Radius;
            CharacterContactX[Index] = Character.Position.x + Character.TranslationOffset.x;
            CharacterContactZ[Index] = Character.Position.z + Character.TranslationOffset.z;
        };
        if (ContactCount > kCharacterLatchParallelThreshold)
        {
            Jobs::ParallelFor(ContactCount, Snapshot);
        }
        else
        {
            for (uint32 Index = 0; Index < ContactCount; ++Index)
            {
                Snapshot(Index);
            }
        }

        const bool bAnyPairs = ContactCount > 1 && CharacterContactMaxRadius > 0.0f;
        CharacterContactGrid.Build(CharacterContactX.GetData(), CharacterContactZ.GetData(), bAnyPairs ? (int32)ContactCount : 0,
            2.0f * CharacterContactMaxRadius);
    }

    void FBox3DPhysicsScene::GatherCharacterPlanes(const FCharacterWork& Work, float Reach, FCharacterPlanes& Out, uint32 ThreadSlot)
    {
        Out.Count = 0;
        const FCharacterContactBody& Self = CharacterContacts[Work.ContactIndex];
        if (!Self.bCollides)
        {
            return;
        }

        const bool bPermissive = Box3DUtils::UsesPermissiveCollisionFilter();
        const auto Accepts = [bPermissive](const SCollisionProfile& A, const SCollisionProfile& B)
        {
            return bPermissive ? A.ShouldCollide(B) : (A.Mask & B.Layer) != (ECollisionProfiles)0;
        };

        const float SelfX = CharacterContactX[Work.ContactIndex];
        const float SelfZ = CharacterContactZ[Work.ContactIndex];
        const float SelfReach = Math::Max(Reach, Self.Reach);

        CharacterContactGrid.ForEachRowSpan(SelfX, SelfZ, Self.Radius + CharacterContactMaxRadius + SelfReach + CharacterContactMaxReach,
            [&](int32 Begin, int32 End)
        {
            for (int32 Slot = Begin; Slot < End; ++Slot)
            {
                const int32 OtherIndex = CharacterContactGrid.SortedIndices[Slot];
                if (OtherIndex == (int32)Work.ContactIndex)
                {
                    continue;
                }

                const FCharacterContactBody& Other = CharacterContacts[OtherIndex];
                const float DX = SelfX - CharacterContactGrid.SortedPosX[Slot];
                const float DZ = SelfZ - CharacterContactGrid.SortedPosZ[Slot];
                const float Contact = Self.Radius + Other.Radius;
                const float PairReach = SelfReach + Other.Reach;
                const float Range = Contact + PairReach;
                const float DistSq = DX * DX + DZ * DZ;
                if (DistSq >= Range * Range || !Other.bCollides || Self.Top + PairReach <= Other.Bottom || Other.Top + PairReach <= Self.Bottom
                    || !Accepts(Self.Profile, Other.Profile))
                {
                    continue;
                }

                // Where the masses balance between two stepped characters, while one that is not stepping holds still.
                const float Share = Other.bMovable && Accepts(Other.Profile, Self.Profile)
                    ? Self.InvMass / (Self.InvMass + Other.InvMass)
                    : 1.0f;

                const float Dist = Math::Sqrt(DistSq);
                b3Vec3 Normal;
                if (Dist > kCoincidentDistance)
                {
                    Normal = b3Vec3{ DX / Dist, 0.0f, DZ / Dist };
                }
                else
                {
                    // Hashed from the pair and signed by order, so the two sides leave in opposite directions.
                    const uint32 Low = Math::Min(Self.Entity.Value, Other.Entity.Value);
                    const uint32 High = Math::Max(Self.Entity.Value, Other.Entity.Value);
                    const float Angle = (float)(((Low * 0x9E3779B1u) ^ (High * 0x85EBCA77u)) & 0xFFFFu) * (Math::TwoPi<float>() / 65536.0f);
                    const float Sign = Self.Entity.Value < Other.Entity.Value ? 1.0f : -1.0f;
                    Normal = b3Vec3{ Math::Cos(Angle) * Sign, 0.0f, Math::Sin(Angle) * Sign };
                }

                const float Gap = Dist - Contact;
                const float Allowed = Gap * Share;

                // A resting character only moves once woken, and it has to before an overlap with it can part.
                if (Gap < 0.0f && !Other.bMovable && Other.Character->bResting)
                {
                    CharacterPushScratch[ThreadSlot].Wakes.Add(Other.Character);
                }

                // A full set keeps the tightest bounds, since a loose one far away rarely limits the move.
                int32 Target = Out.Count;
                if (Out.Count == kMaxCharacterPlanes)
                {
                    Target = 0;
                    for (int32 i = 1; i < kMaxCharacterPlanes; ++i)
                    {
                        if (Out.Allowed[i] > Out.Allowed[Target])
                        {
                            Target = i;
                        }
                    }
                    if (Out.Allowed[Target] <= Allowed)
                    {
                        continue;
                    }
                }
                else
                {
                    ++Out.Count;
                }
                Out.Normals[Target] = Normal;
                Out.Allowed[Target] = Allowed;
            }
        });
    }

    void FBox3DPhysicsScene::DeliverCharacterEvents(const FCharacterPushBucket& Bucket)
    {
        ECS::FEventQueues& Events = World->GetEvents();
        ECS::SendEvents(Events, TSpan<const SCharacterLandedEvent>(Bucket.Landings.GetData(), Bucket.Landings.Num()));
        ECS::SendEvents(Events, TSpan<const SCharacterJumpedEvent>(Bucket.Jumps.GetData(), Bucket.Jumps.Num()));
    }

    ECharacterPrep FBox3DPhysicsScene::PrepareCharacter(ECS::FRegistry& Registry, ECS::FEntity Entity, SCharacterPhysicsComponent& Physics,
        SCharacterMovementComponent& Movement, float FixedDt, bool bMayWriteBox3D)
    {
        FPhysicsCharacterHandle& Character = *Physics.Character;

        // Prediction steps a net-driven character against proxies on both ends, so its own proxy never leaves.
        if (Character.NetDrive != (uint8)ECharacterNetDrive::Simulated && !Character.bProxyEnabled)
        {
            if (!bMayWriteBox3D)
            {
                return ECharacterPrep::Serial;
            }
            SetProxyEnabled(Character, true);
        }
        if (Character.NetDrive == (uint8)ECharacterNetDrive::Commands)
        {
            return ECharacterPrep::Hold;
        }
        if (Character.NetDrive == (uint8)ECharacterNetDrive::FollowTransform)
        {
            if (!bMayWriteBox3D)
            {
                return ECharacterPrep::Serial;
            }
            FollowTransform(Registry, Entity, Physics, Movement, FixedDt);
            return ECharacterPrep::Hold;
        }

        // Past the LOD distance a character keeps no proxy and moves every few steps, unless something it could touch is near.
        bool bNear = true;
        if (CharacterLODRangeSq > 0.0f)
        {
            bNear = Character.NearBodyStep != 0 && CharacterStepCounter - Character.NearBodyStep <= kNearBodyHoldSteps;
            for (int32 i = 0; i < CharacterRelevancePoints.Num() && !bNear; ++i)
            {
                bNear = Math::DistanceSquared(CharacterRelevancePoints[i], Character.Position) <= CharacterLODRangeSq;
            }
        }
        if (bNear != Character.bProxyEnabled)
        {
            if (!bMayWriteBox3D)
            {
                return ECharacterPrep::Serial;
            }
            SetProxyEnabled(Character, bNear);
        }
        Character.LODInterval = bNear ? 1 : (uint8)CharacterLODInterval;

        // Resizing the proxy shape writes Box3D state the parallel step only reads.
        if ((Movement.bWantsToCrouch && Movement.bCanCrouch) != Movement.bIsCrouched)
        {
            if (!bMayWriteBox3D)
            {
                return ECharacterPrep::Serial;
            }
            UpdateCrouch(Entity, Physics, Movement);
        }

        if (Character.bResting)
        {
            const bool bPollDue = ((CharacterStepCounter + (uint64)(Entity).Value) & kRestPollMask) == 0;
            if (!bPollDue && !ShouldWakeResting(Character, Movement))
            {
                // The last step's target transform has landed the proxy, so it stops here instead of drifting on.
                if (Character.bProxyInMotion)
                {
                    if (!bMayWriteBox3D)
                    {
                        return ECharacterPrep::Serial;
                    }
                    PlaceProxy(Character);
                }
                return ECharacterPrep::Rest;
            }

            Character.bResting = false;
        }

        if (Movement.bPendingTeleport || Character.bAwaitingGround)
        {
            if (!bMayWriteBox3D)
            {
                return ECharacterPrep::Serial;
            }
            if (!ResolveCharacterPreStep(Entity, Physics, Movement))
            {
                return ECharacterPrep::Hold;
            }
        }

        // Phased by entity so a far crowd spreads its moves evenly over the span.
        if (Character.LODInterval > 1 && ((CharacterStepCounter + (uint64)Entity.Value) % Character.LODInterval) != 0)
        {
            Character.LODStepsHeld = (uint8)Math::Min<uint32>(Character.LODStepsHeld + 1u, 255u);
            return ECharacterPrep::Hold;
        }
        Character.LODStepsHeld = 0;
        return ECharacterPrep::Step;
    }

    bool FBox3DPhysicsScene::ResolveCharacterPreStep(ECS::FEntity Entity, SCharacterPhysicsComponent& Physics, SCharacterMovementComponent& Movement)
    {
        FPhysicsCharacterHandle& Character = *Physics.Character;

        if (Movement.bPendingTeleport)
        {
            Movement.bPendingTeleport = false;
            if (!Box3DUtils::IsWithin(Movement.PendingTeleportLocation, Box3DUtils::MaxSafeCoordinate))
            {
                LOG_WARN("Character on entity {} ignored a teleport to a NaN, infinite or out of range location", (Entity).Value);
                return true;
            }

            // A teleport lands the same way a spawn does, so it gets seated instead of dropped into geometry.
            Character.SpawnPosition = Movement.PendingTeleportLocation;
            Character.bAwaitingGround = true;
            Character.AwaitingGroundSteps = 0;

            Character.Position = Movement.PendingTeleportLocation;
            Character.Velocity = FVector3(0.0f);
            Character.bGrounded = false;
            Character.GroundNormal = FVector3(0.0f, 1.0f, 0.0f);
            Character.GroundEntity = ECS::NullEntity;

            Movement.Velocity = FVector3(0.0f);
            Movement.bGrounded = false;
            Movement.GroundEntity = ECS::NullEntity;
            Movement.GroundNormal = FVector3(0.0f, 1.0f, 0.0f);

            // Reseeds the interp snapshot so the render transform does not streak across the jump.
            Physics.LastBodyPosition = Character.Position;
            PlaceProxy(Character);
            return false;
        }

        // A centered capsule spawns buried and a mesh collider can land frames late, so hold at the spawn until there is ground.
        if (Character.bAwaitingGround)
        {
            b3Vec3 Seated;
            const EMoverSeatResult Result = TrySeatMoverOnGround(WorldId, Character.MakeMoverCapsule(),
                Box3DUtils::ToB3Vec3(Character.SpawnPosition), Character.Filter, Character.Profile,
                Character.StickToFloorDistance, Character.ProxyBody, Seated);

            if (Result == EMoverSeatResult::NoGeometry)
            {
                ++Character.AwaitingGroundSteps;
                if (Character.AwaitingGroundSteps == kAwaitingGroundWarnSteps)
                {
                    LOG_WARN("Character on entity {} has found nothing to stand on below y {:.2f} after {} steps; "
                             "it is held at its spawn until a collider its profile can see appears.",
                        (Entity).Value, Character.SpawnPosition.y, kAwaitingGroundWarnSteps);
                }

                Character.Position = Character.SpawnPosition;
                Character.Velocity = FVector3(0.0f);
                Movement.Velocity = FVector3(0.0f);

                Physics.LastBodyPosition = Character.Position;
                Physics.LastBodyRotation = Character.Rotation;

                PlaceProxy(Character);
                return false;
            }

            if (Result == EMoverSeatResult::Seated)
            {
                const FVector3 SeatedPosition = Box3DUtils::FromB3Vec3(Seated);
                LOG_DEBUG("Character on entity {} spawned inside geometry and was seated from y {:.2f} to {:.2f} "
                          "after {} step(s) waiting for ground.",
                    (Entity).Value, Character.Position.y, SeatedPosition.y, Character.AwaitingGroundSteps);

                Character.Position = SeatedPosition;
                Character.Velocity = FVector3(0.0f);
                Movement.Velocity = FVector3(0.0f);

                // Placed on the ground rather than arriving there, so its first step does not report a landing.
                Character.bGrounded = true;
            }
            else
            {
                LOG_DEBUG("Character on entity {} starts airborne at y {:.2f} after {} step(s) waiting for ground.",
                    (Entity).Value, Character.SpawnPosition.y, Character.AwaitingGroundSteps);
            }

            Character.bAwaitingGround = false;
        }

        return true;
    }

    void FBox3DPhysicsScene::FollowTransform(ECS::FRegistry& Registry, ECS::FEntity Entity, SCharacterPhysicsComponent& Physics,
        SCharacterMovementComponent& Movement, float FixedDt)
    {
        FPhysicsCharacterHandle& Character = *Physics.Character;
        const STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return;
        }

        // The velocity is derived, so footsteps and animation on a mirrored character still see it move.
        const FVector3 Target = Transform->GetLocalLocation();
        Movement.Velocity = FixedDt > 0.0f ? (Target - Character.Position) / FixedDt : FVector3(0.0f);
        Character.Position = Target;
        Character.Rotation = Transform->GetLocalRotation();
        Character.Velocity = Movement.Velocity;
        Character.bAwaitingGround = false;
        Physics.LastBodyPosition = Character.Position;
        Physics.LastBodyRotation = Character.Rotation;
        PlaceProxy(Character);
    }

    bool FBox3DPhysicsScene::GetCharacterNetState(ECS::FEntity Entity, FCharacterNetState& Out) const
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        const SCharacterPhysicsComponent* Physics = Registry.TryGet<SCharacterPhysicsComponent>(Entity);
        const SCharacterMovementComponent* Movement = Registry.TryGet<SCharacterMovementComponent>(Entity);
        if (Physics == nullptr || !Physics->Character || Movement == nullptr)
        {
            return false;
        }
        const FPhysicsCharacterHandle& Character = *Physics->Character;
        Out.Position       = Character.Position;
        Out.Rotation       = Character.Rotation;
        Out.Velocity       = Movement->Velocity;
        Out.GroundNormal   = Character.GroundNormal;
        Out.GroundVelocity = Character.GroundVelocity;
        Out.JumpCount      = Movement->JumpCount;
        Out.bGrounded      = Character.bGrounded;
        return true;
    }

    bool FBox3DPhysicsScene::SetCharacterNetState(ECS::FEntity Entity, const FCharacterNetState& State)
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        SCharacterPhysicsComponent* Physics = Registry.TryGet<SCharacterPhysicsComponent>(Entity);
        SCharacterMovementComponent* Movement = Registry.TryGet<SCharacterMovementComponent>(Entity);
        if (Physics == nullptr || !Physics->Character || Movement == nullptr)
        {
            return false;
        }
        FPhysicsCharacterHandle& Character = *Physics->Character;
        Character.Position       = State.Position;
        Character.Rotation       = State.Rotation;
        Character.Velocity       = State.Velocity;
        Character.GroundNormal   = State.GroundNormal;
        Character.GroundVelocity = State.GroundVelocity;
        Character.bGrounded      = State.bGrounded;
        Character.bAwaitingGround = false;
        Character.bResting        = false;
        Movement->Velocity     = State.Velocity;
        Movement->JumpCount    = State.JumpCount;
        Movement->bGrounded    = State.bGrounded;
        Movement->GroundNormal = State.GroundNormal;
        Physics->LastBodyPosition = State.Position;
        Physics->LastBodyRotation = State.Rotation;
        return true;
    }

    bool FBox3DPhysicsScene::SimulateCharacterStep(ECS::FEntity Entity, const FCharacterMoveInput& Input, float FixedDt, bool bReplay)
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        SCharacterPhysicsComponent* Physics = Registry.TryGet<SCharacterPhysicsComponent>(Entity);
        SCharacterMovementComponent* Movement = Registry.TryGet<SCharacterMovementComponent>(Entity);
        if (Physics == nullptr || !Physics->Character || Movement == nullptr)
        {
            return false;
        }

        Movement->PendingMoveDirection = Input.Direction;
        Movement->PendingMoveThrottle  = Input.Throttle;
        Movement->bHasPendingMoveInput = Input.bHasMove;
        Movement->PendingLookYaw       = Input.LookYaw;
        Movement->PendingButtons       = Input.Buttons;
        Movement->bPendingJump         = Movement->bPendingJump || Input.bJump;

        const float AuthoredSpeed = Movement->MoveSpeed;
        if (Input.MoveSpeed > 0.0f)
        {
            Movement->MoveSpeed = Input.MoveSpeed;
        }

        Physics->Character->bResting = false;
        if (ResolveCharacterPreStep(Entity, *Physics, *Movement))
        {
            if (CharacterPushScratch.IsEmpty())
            {
                CharacterPushScratch.SetNum(1);
            }
            CharacterPushScratch[0].Clear();

            StepCharacter(FCharacterWork{ Physics, Movement, Entity, 0, true }, FixedDt, 0);
            DriveProxy(*Physics->Character, FixedDt, true);

            // A replay re-walks steps whose pushes and landings already happened, so repeating them would shove and announce twice.
            if (!bReplay)
            {
                for (const FPendingCharacterPush& Push : CharacterPushScratch[0].Pushes)
                {
                    if (b3Body_IsValid(Push.Body))
                    {
                        b3Body_ApplyLinearImpulse(Push.Body, Push.Impulse, Push.Point, true);
                    }
                }
                DeliverCharacterEvents(CharacterPushScratch[0]);
            }
            CharacterPushScratch[0].Clear();
        }

        Movement->MoveSpeed = AuthoredSpeed;
        return true;
    }

    void FBox3DPhysicsScene::SetCharacterNetDrive(ECS::FEntity Entity, ECharacterNetDrive Drive)
    {
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        if (SCharacterPhysicsComponent* Physics = Registry.TryGet<SCharacterPhysicsComponent>(Entity); Physics != nullptr && Physics->Character)
        {
            Physics->Character->NetDrive = (uint8)Drive;
            Physics->Character->bResting = false;

            // A command can be stepped before the next prep pass would switch the proxy on.
            if (Drive != ECharacterNetDrive::Simulated && !Physics->Character->bProxyEnabled)
            {
                SetProxyEnabled(*Physics->Character, true);
            }
        }
    }

    void FBox3DPhysicsScene::StepCharacter(const FCharacterWork& Work, float FixedDt, uint32 ThreadSlot)
    {
        SCharacterPhysicsComponent& Physics = *Work.Physics;
        SCharacterMovementComponent& Movement = *Work.Movement;
        FPhysicsCharacterHandle& Character = *Physics.Character;

        // Snapshotting each substep leaves the pose from before the last one, which is what interp blends from.
        Physics.LastBodyPosition = Character.Position;
        Physics.LastBodyRotation = Character.Rotation;

        const bool bHasMovementInput = Movement.bHasPendingMoveInput;
        const FVector3 DesiredDirection = Movement.PendingMoveDirection;
        const bool bSprinting = Movement.SprintSpeed > 0.0f && (Movement.PendingButtons & Movement.SprintButtons) != 0;
        const float Speed = (bSprinting ? Movement.SprintSpeed : Movement.MoveSpeed) * (Movement.bIsCrouched ? Movement.CrouchSpeedScale : 1.0f);
        const float TargetSpeed = bHasMovementInput ? Speed * Movement.PendingMoveThrottle : 0.0f;
        const FVector3 TargetVelocity = DesiredDirection * TargetSpeed;

        // Ground state was resolved at the end of the previous substep.
        const bool bWasGrounded = Character.bGrounded;

        Movement.bGrounded = Character.bGrounded;
        Movement.GroundNormal = Character.GroundNormal;
        Movement.GroundEntity = Character.GroundEntity;

        FQuat TargetRotation = Character.Rotation;
        if (Movement.bUseControllerRotation)
        {
            TargetRotation = FQuat(FVector3(0.0f, Math::Radians(Movement.PendingLookYaw), 0.0f));
        }
        else if (Movement.bOrientRotationToMovement && bHasMovementInput)
        {
            const float TargetYaw = Math::Atan2(DesiredDirection.x, DesiredDirection.z);
            const FQuat Yawed = FQuat(FVector3(0.0f, TargetYaw, 0.0f));
            TargetRotation = Math::Slerp(TargetRotation, Yawed, Math::Clamp(Movement.RotationRate * FixedDt, 0.0f, 1.0f));
        }

        FVector3 HorizontalVelocity(Movement.Velocity.x, 0.0f, Movement.Velocity.z);
        const float CurrentSpeed = Math::Length(HorizontalVelocity);

        if (bHasMovementInput)
        {
            const float Accel = Movement.bGrounded ? Movement.Acceleration : Movement.Acceleration * Movement.AirControl;
            const float Blend = Math::Clamp(Accel * FixedDt, 0.0f, 1.0f);
            HorizontalVelocity = Math::Mix(HorizontalVelocity, TargetVelocity, Blend);
        }
        else if (Movement.bGrounded)
        {
            const float NewSpeed = Math::Max(0.0f, CurrentSpeed - Movement.Deceleration * FixedDt);

            HorizontalVelocity = CurrentSpeed > 0.001f
                ? Math::Normalize(HorizontalVelocity) * NewSpeed
                : FVector3(0.0f);

            HorizontalVelocity *= Math::Max(0.0f, 1.0f - Movement.GroundFriction * FixedDt);

            // A standing character comes to a hard stop rather than decaying toward zero forever.
            if (Math::LengthSquared(HorizontalVelocity) < kRestSpeedSq)
            {
                HorizontalVelocity = FVector3(0.0f);
            }
        }
        else
        {
            HorizontalVelocity *= Math::Max(0.0f, 1.0f - (Movement.GroundFriction * 0.1f) * FixedDt);
        }

        Movement.Velocity.x = HorizontalVelocity.x + Character.GroundVelocity.x;
        Movement.Velocity.z = HorizontalVelocity.z + Character.GroundVelocity.z;

        if (Movement.bGrounded)
        {
            Movement.Velocity.y = Character.GroundVelocity.y;
        }
        else
        {
            Movement.Velocity.y += Movement.Gravity * FixedDt;
        }

        bool bJumpedThisStep = false;
        if (Movement.bPendingJump)
        {
            Movement.bPendingJump = false;

            if (Movement.JumpCount < Movement.MaxJumpCount)
            {
                Movement.Velocity.y = Movement.JumpSpeed;
                ++Movement.JumpCount;
                bJumpedThisStep = true;

                SCharacterJumpedEvent& Jumped = CharacterPushScratch[ThreadSlot].Jumps.EmplaceGetRef();
                Jumped.Entity    = Work.Entity;
                Jumped.JumpCount = Movement.JumpCount;
                Jumped.Velocity  = Movement.Velocity;
            }
        }

        // Applied after the ground and jump blocks so an upward impulse survives while still grounded.
        if (Movement.bPendingLaunch)
        {
            Movement.bPendingLaunch = false;

            if (Movement.bLaunchOverrideHorizontal)
            {
                Movement.Velocity.x = Movement.PendingLaunchVelocity.x;
                Movement.Velocity.z = Movement.PendingLaunchVelocity.z;
            }
            else
            {
                Movement.Velocity.x += Movement.PendingLaunchVelocity.x;
                Movement.Velocity.z += Movement.PendingLaunchVelocity.z;
            }

            Movement.Velocity.y = Movement.bLaunchOverrideVertical
                ? Movement.PendingLaunchVelocity.y
                : Movement.Velocity.y + Movement.PendingLaunchVelocity.y;

            bJumpedThisStep |= Movement.Velocity.y > 0.0f;
        }

        Character.Rotation = TargetRotation;
        Character.Velocity = Movement.Velocity;
        const float FallSpeed = Math::Max(-Character.Velocity.y, 0.0f);

        const b3Capsule Mover = Character.MakeMoverCapsule();

        b3Pos Position = Box3DUtils::ToB3Vec3(Character.Position);
        const b3Pos StartPosition = Position;

        FMoverPlanes Gathered;
        Gathered.IgnoreBody = Character.ProxyBody;
        Gathered.Profile = Character.Profile;
        Gathered.bPermissive = Box3DUtils::UsesPermissiveCollisionFilter();
        // In a crowd step the contact pass meets other characters, so the mover sees only the world.
        Gathered.bCollideWithCharacters = Work.bProxyCharacterContacts && Character.bCollideWithCharacters;
        Gathered.CosMaxSlope = Character.CosMaxSlope;
        Gathered.Entity = Work.Entity;

        FCharacterPlanes Neighbors;

        auto GatherAt = [&](b3Pos At)
        {
            Gathered.Count = 0;
            Gathered.Origin = At;
            b3World_CollideMover(WorldId, At, &Mover, Character.Filter, &GatherPlanes, &Gathered);
            Gathered.WorldCount = Gathered.Count;

            // Fixed where the step began, so gathering again never moves a neighbor's bound.
            const b3Vec3 Moved = b3SubPos(At, StartPosition);
            for (int32 i = 0; i < Neighbors.Count && Gathered.Count < kMaxMoverPlanes; ++i)
            {
                const b3Vec3 Normal = Neighbors.Normals[i];
                Gathered.Planes[Gathered.Count] = b3CollisionPlane{ b3Plane{ Normal, -Neighbors.Allowed[i] - b3Dot(Normal, Moved) }, FLT_MAX, 0.0f, true };
                Gathered.Points[Gathered.Count] = b3Add(At, b3MulSV(-Character.Radius, Normal));
                Gathered.Shapes[Gathered.Count] = b3_nullShapeId;
                ++Gathered.Count;
            }
        };

        auto FindWalkablePlane = [&]()
        {
            int32 Best = Constants::kIndexNone;
            float BestUp = Character.CosMaxSlope;
            for (int32 i = 0; i < Gathered.WorldCount; ++i)
            {
                const float Up = Gathered.Planes[i].plane.normal.y;
                if (Up >= BestUp)
                {
                    BestUp = Up;
                    Best = i;
                }
            }
            return Best;
        };

        // A capsule landing on a ledge rests on its rounded edge, whose normal reads too steep, so the face just past the edge decides.
        FFloorHit LedgeFloor;
        LedgeFloor.Query = &Gathered;
        auto FindLedgeFloor = [&](const b3Vec3& Forward, float ForwardLength)
        {
            const float LowerCenterY = Gathered.Origin.y + Math::Min(Mover.center1.y, Mover.center2.y);
            int32 Edge = Constants::kIndexNone;
            for (int32 i = 0; i < Gathered.WorldCount; ++i)
            {
                if (Gathered.Points[i].y < LowerCenterY && (Edge == Constants::kIndexNone || Gathered.Points[i].y > Gathered.Points[Edge].y))
                {
                    Edge = i;
                }
            }
            if (Edge == Constants::kIndexNone || ForwardLength <= 0.0f)
            {
                return false;
            }

            const b3Vec3 Inset  = b3MulSV(kLedgeProbeInset / ForwardLength, Forward);
            const b3Pos  Origin = b3Add(Gathered.Points[Edge], b3Vec3{ Inset.x, Character.StepHeight, Inset.z });
            LedgeFloor.bHit = false;
            b3World_CastRay(WorldId, Origin, b3Vec3{ 0.0f, -2.0f * Character.StepHeight, 0.0f }, Character.Filter, &ClosestFloorHit, &LedgeFloor);
            LedgeFloor.bHit = LedgeFloor.bHit && LedgeFloor.Normal.y >= Character.CosMaxSlope;
            return LedgeFloor.bHit;
        };

        // Box3D's mover order is cast, gather at the new pose, then solve; solving first walks the character downhill on slopes.
        b3Vec3 Desired = b3MulSV(FixedDt, Box3DUtils::ToB3Vec3(Character.Velocity));

        // On a walkable floor solve only the vertical, since projection would shave the horizontal delta by the slope cosine and bend the heading.
        if (bWasGrounded && !bJumpedThisStep && Character.GroundNormal.y >= Character.CosMaxSlope
            && Character.GroundNormal.y > kMinRampNormalY)
        {
            const FVector3& Ground = Character.GroundNormal;
            Desired.y = -(Desired.x * Ground.x + Desired.z * Ground.z) / Ground.y;
        }
        if (!Work.bProxyCharacterContacts && Character.bCollideWithCharacters)
        {
            GatherCharacterPlanes(Work, b3Length(Desired), Neighbors, ThreadSlot);
            Gathered.Capacity = kMaxMoverPlanes - Neighbors.Count;
        }

        const float TravelFraction = b3World_CastMover(WorldId, Position, &Mover, Desired, Character.Filter, &MoverCastFilter, &Gathered);

        Position = b3Add(Position, b3MulSV(TravelFraction, Desired));

        GatherAt(Position);

        // The solver slides the unused remainder along whatever was hit, and corrects any overlap.
        const b3Vec3 Remaining = b3MulSV(1.0f - TravelFraction, Desired);
        const b3PlaneSolverResult Solved = b3SolvePlanes(Remaining, Gathered.Planes, Gathered.Count);
        Position = b3Add(Position, Solved.delta);

        // A solve that barely moved the capsule leaves the planes just gathered still valid for the first recovery pass.
        bool bPlanesCurrent = b3LengthSquared(Solved.delta) < kMoveTolerance * kMoveTolerance * 0.01f;

        // Overlap is resolved against a zero target so it can never become motion; a capsule spawned inside geometry needs several passes to escape.
        bool bPushedOut = false;
        for (int32 Recovery = 0; Recovery < Character.MaxCollisionIterations; ++Recovery)
        {
            if (!bPlanesCurrent)
            {
                GatherAt(Position);
            }
            bPlanesCurrent = false;
            bPushedOut = false;

            const b3PlaneSolverResult Push = b3SolvePlanes(b3Vec3_zero, Gathered.Planes, Gathered.Count);
            if (b3LengthSquared(Push.delta) < kMoveTolerance * kMoveTolerance)
            {
                break;
            }

            Position = b3Add(Position, Push.delta);
            bPushedOut = true;
        }

        if (bPushedOut)
        {
            GatherAt(Position);
        }

        int32 GroundPlane = FindWalkablePlane();

        // Stair handling lifts by the step height, retries the blocked move, then settles back down.
        const float WantedHorizontal = b3Length(b3Vec3{ Desired.x, 0.0f, Desired.z });
        const b3Vec3 Achieved = b3Sub(Position, StartPosition);
        const float AchievedHorizontal = b3Length(b3Vec3{ Achieved.x, 0.0f, Achieved.z });

        if (bWasGrounded && TravelFraction < 1.0f && Character.StepHeight > 0.0f && WantedHorizontal > kMoveTolerance
            && AchievedHorizontal < WantedHorizontal * 0.9f)
        {
            const b3Vec3 StepUp{ 0.0f, Character.StepHeight, 0.0f };
            const float UpFraction = b3World_CastMover(WorldId, StartPosition, &Mover, StepUp, Character.Filter, &MoverCastFilter, &Gathered);

            if (UpFraction > 0.5f)
            {
                const b3Pos Raised = b3Add(StartPosition, b3MulSV(UpFraction, StepUp));
                const b3Vec3 Forward{ Desired.x, 0.0f, Desired.z };
                const float ForwardFraction = b3World_CastMover(WorldId, Raised, &Mover, Forward, Character.Filter, &MoverCastFilter, &Gathered);

                if (ForwardFraction * WantedHorizontal > AchievedHorizontal + kMoveTolerance)
                {
                    const b3Pos Stepped = b3Add(Raised, b3MulSV(ForwardFraction, Forward));
                    const b3Vec3 StepDown{ 0.0f, -Character.StepHeight, 0.0f };
                    const float DownFraction = b3World_CastMover(WorldId, Stepped, &Mover, StepDown, Character.Filter, &MoverCastFilter, &Gathered);

                    // A step only counts when it lands on something walkable, or a steep face is climbed a step at a time.
                    const b3Pos Landed = b3Add(Stepped, b3MulSV(DownFraction, StepDown));
                    GatherAt(Landed);
                    const int32 LandedPlane = DownFraction < 1.0f ? FindWalkablePlane() : Constants::kIndexNone;
                    if (LandedPlane != Constants::kIndexNone)
                    {
                        Position    = Landed;
                        GroundPlane = LandedPlane;
                    }
                    else if (DownFraction < 1.0f && FindLedgeFloor(Forward, WantedHorizontal))
                    {
                        // The planes now come from the landing, so an index found before the step no longer names the ground.
                        Position = Landed;
                        GroundPlane = Constants::kIndexNone;
                    }
                    else
                    {
                        // The earlier index may name a plane gathered at a slightly different pose, which this set no longer holds.
                        GatherAt(Position);
                        GroundPlane = FindWalkablePlane();
                    }
                }
            }
        }

        // Only a move that left the ground is pulled back down, since settling every frame would re-seat the capsule in the floor.
        if (GroundPlane == Constants::kIndexNone && !LedgeFloor.bHit && bWasGrounded && !bJumpedThisStep && Character.Velocity.y <= 0.0f)
        {
            const b3Vec3 Drop{ 0.0f, -Character.StickToFloorDistance, 0.0f };
            const float DropFraction = b3World_CastMover(WorldId, Position, &Mover, Drop, Character.Filter, &MoverCastFilter, &Gathered);

            if (DropFraction < 1.0f)
            {
                const b3Pos Landed = b3Add(Position, b3MulSV(DropFraction, Drop));
                GatherAt(Landed);

                const int32 LandedPlane = FindWalkablePlane();
                if (LandedPlane != Constants::kIndexNone)
                {
                    Position = Landed;
                    GroundPlane = LandedPlane;
                }
                else
                {
                    GatherAt(Position);
                }
            }
        }

        const bool bOnLedge = GroundPlane == Constants::kIndexNone && LedgeFloor.bHit;
        const bool bNowGrounded = (GroundPlane != Constants::kIndexNone || bOnLedge) && !bJumpedThisStep;
        const b3ShapeId GroundShape = bOnLedge ? LedgeFloor.Shape : (GroundPlane != Constants::kIndexNone ? Gathered.Shapes[GroundPlane] : b3ShapeId{});

        // The landing edge is only visible here, where this step's ground has been resolved against the last one.
        const bool bLanded = bNowGrounded && !bWasGrounded;
        if (bLanded)
        {
            Movement.JumpCount = 0;
        }

        Character.bGrounded = bNowGrounded;

        if (Character.bGrounded)
        {
            Character.GroundNormal = Box3DUtils::FromB3Vec3(bOnLedge ? LedgeFloor.Normal : Gathered.Planes[GroundPlane].plane.normal);

            // Entity 0 on body handle 0 packs to a null pointer, so a null test would lose the first entity's ground.
            const b3BodyId GroundBody = b3Shape_GetBody(GroundShape);
            Character.GroundEntity = EntityOfBody(GroundBody);
            Character.GroundVelocity = b3Body_IsValid(GroundBody)
                ? Box3DUtils::FromB3Vec3(b3Body_GetWorldPointVelocity(GroundBody, bOnLedge ? LedgeFloor.Point : Gathered.Points[GroundPlane]))
                : FVector3(0.0f);
        }
        else
        {
            Character.GroundNormal = FVector3(0.0f, 1.0f, 0.0f);
            Character.GroundEntity = ECS::NullEntity;
            Character.GroundVelocity = FVector3(0.0f);
        }

        // Published now as well, since a character that settles takes no further step to copy it over.
        Movement.bGrounded    = Character.bGrounded;
        Movement.GroundNormal = Character.GroundNormal;
        Movement.GroundEntity = Character.GroundEntity;

        if (bLanded)
        {
            SCharacterLandedEvent& Landed = CharacterPushScratch[ThreadSlot].Landings.EmplaceGetRef();
            Landed.Entity       = Work.Entity;
            Landed.ImpactSpeed  = FallSpeed;
            Landed.GroundNormal = Character.GroundNormal;
            Landed.GroundEntity = Character.GroundEntity;
        }

        // Rest is only safe against things that cannot move on their own; character proxies count as inert because the mover never pushes one.
        bool bRestableContacts = true;

        // Push whatever the mover leaned on, so crates and props still respond to being walked into.
        for (int32 i = 0; i < Gathered.WorldCount; ++i)
        {
            const b3BodyId HitBody = b3Shape_GetBody(Gathered.Shapes[i]);
            if (!b3Body_IsValid(HitBody))
            {
                continue;
            }

            const b3BodyType HitType = b3Body_GetType(HitBody);
            if (HitType != b3_staticBody && !Box3DUtils::IsCharacterProxyUserData(b3Shape_GetUserData(Gathered.Shapes[i])))
            {
                bRestableContacts = false;
            }

            if (HitType != b3_dynamicBody)
            {
                continue;
            }

            const b3Vec3 Normal = b3Neg(Gathered.Planes[i].plane.normal);
            const b3Vec3 Point = Gathered.Points[i];

            const float InvMassB = b3Body_GetInverseMass(HitBody);
            const b3Matrix3 InvInertiaB = b3Body_GetWorldInverseRotationalInertia(HitBody);

            const float InvMassCharacter = Character.Mass > 0.0f ? 1.0f / Character.Mass : 0.0f;
            const b3Vec3 RadiusB = b3SubPos(Point, b3Body_GetWorldCenter(HitBody));
            const b3Vec3 CrossB = b3Cross(RadiusB, Normal);
            const float NormalK = InvMassCharacter + InvMassB + b3Dot(CrossB, b3MulMV(InvInertiaB, CrossB));
            if (NormalK <= 0.0f)
            {
                continue;
            }

            const b3Vec3 VelocityB = b3Add(b3Body_GetLinearVelocity(HitBody), b3Cross(b3Body_GetAngularVelocity(HitBody), RadiusB));
            const float NormalVelocity = b3Dot(b3Sub(VelocityB, Box3DUtils::ToB3Vec3(Character.Velocity)), Normal);

            float Impulse = Math::Max(-NormalVelocity / NormalK, 0.0f);
            Impulse = Math::Min(Impulse, Character.MaxStrength * FixedDt);

            CharacterPushScratch[ThreadSlot].Pushes.Emplace(HitBody, b3MulSV(Impulse, Normal), Point);
        }

        // Stepping up, settling and pushing out of the floor move the capsule outside the solves, so the neighbor bounds hold once more here.
        if (!Work.bProxyCharacterContacts && Character.bCollideWithCharacters)
        {
            // A push out of the world can carry the capsule past the reach its neighbors were gathered for.
            const b3Vec3 Moved = b3SubPos(Position, StartPosition);
            const float MovedFlat = b3Length(b3Vec3{ Moved.x, 0.0f, Moved.z });
            if (MovedFlat > Math::Max(b3Length(Desired), CharacterContacts[Work.ContactIndex].Reach))
            {
                GatherCharacterPlanes(Work, MovedFlat, Neighbors, ThreadSlot);
            }

            b3Pos Bounded = Position;
            for (int32 i = 0; i < Neighbors.Count; ++i)
            {
                const float Separation = b3Dot(Neighbors.Normals[i], b3SubPos(Bounded, StartPosition)) + Neighbors.Allowed[i];
                if (Separation < 0.0f)
                {
                    Bounded = b3Add(Bounded, b3MulSV(-Separation, Neighbors.Normals[i]));
                }
            }

            // Cast rather than placed, so the world still wins and an overlap it leaves parts on the next step.
            b3Vec3 Correction = b3SubPos(Bounded, Position);
            if (Character.bGrounded)
            {
                // Along the floor, so a shove on a slope neither lifts the capsule off it nor buries it.
                const b3Vec3 Up = Box3DUtils::ToB3Vec3(Character.GroundNormal);
                Correction = b3MulSub(Correction, b3Dot(Correction, Up), Up);
            }
            if (b3LengthSquared(Correction) > 0.0f)
            {
                const float Fraction = b3World_CastMover(WorldId, Position, &Mover, Correction, Character.Filter, &MoverCastFilter, &Gathered);
                Position = b3Add(Position, b3MulSV(Fraction, Correction));
            }
        }

        Character.Position = Box3DUtils::FromB3Vec3(Position);

        // b3ClipVector ignores planes with a zero push, and every gather rebuilds the set with zero pushes.
        b3SolvePlanes(b3Vec3_zero, Gathered.Planes, Gathered.Count);

        // Only a wall may take speed away, or clipping against a walkable floor slows a character on a ramp below its authored speed.
        for (int32 i = 0; i < Gathered.Count; ++i)
        {
            if (Gathered.Planes[i].plane.normal.y >= Character.CosMaxSlope)
            {
                Gathered.Planes[i].clipVelocity = false;
            }
        }

        // Without this, velocity accumulates every frame the mover is pressed against a surface.
        Character.Velocity = Box3DUtils::FromB3Vec3(b3ClipVector(Box3DUtils::ToB3Vec3(Character.Velocity), Gathered.Planes, Gathered.Count));
        Movement.Velocity = Character.Velocity;

        const bool bSettled = Character.bGrounded
            && bRestableContacts
            && !bHasMovementInput
            && Math::LengthSquared(Character.Velocity) < kRestSpeedSq
            && Math::LengthSquared(Character.GroundVelocity) < kRestSpeedSq
            && b3LengthSquared(b3Sub(Position, StartPosition)) < kMoveTolerance * kMoveTolerance;

        if (!bSettled)
        {
            return;
        }

        // Only static ground rests, since a platform or another character can move out from under the capsule without any signal the wake check sees.
        const b3BodyId GroundBody = b3Shape_GetBody(GroundShape);
        if (!b3Body_IsValid(GroundBody) || b3Body_GetType(GroundBody) != b3_staticBody)
        {
            return;
        }

        Character.Velocity = FVector3(0.0f);
        Movement.Velocity = FVector3(0.0f);

        Character.bResting = true;
        Character.RestGroundBody = GroundBody;
        Character.RestLookYaw = Movement.PendingLookYaw;
    }
}
