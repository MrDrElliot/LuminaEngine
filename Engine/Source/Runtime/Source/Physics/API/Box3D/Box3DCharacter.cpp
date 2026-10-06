#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "Box3DPhysicsScene.h"

#include <box3d/collision.h>

#include "Box3DCharacterHandle.h"
#include "Box3DInternal.h"
#include "Box3DUtils.h"

#include "Log/Log.h"
#include "Renderer/RendererUtils.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

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

        // A couple of seconds of fixed steps, long past any deferred collider build.
        constexpr uint32 kAwaitingGroundWarnSteps = 120;

        // How often a resting character re-runs a full step anyway, so a world change that raises no wake
        // signal cannot hold it stale. Power of two, and the phase is offset per entity so a settled crowd
        // does not poll on the same step.
        constexpr uint64 kRestPollMask = 15;

        // A character step is microseconds of world queries, so the fan-out pays for itself at a low count.
        constexpr uint32 kCharacterParallelThreshold = 16;
        constexpr uint32 kCharacterParallelGrain = 4;

        // The latch is a few dozen nanoseconds per character, so it only fans out for a crowd.
        constexpr uint32 kCharacterLatchParallelThreshold = 2048;

        // Gathered per move iteration; the extras are only needed to push whatever the mover leaned on.
        struct FMoverPlanes
        {
            b3CollisionPlane    Planes[kMaxMoverPlanes];
            b3Vec3              Points[kMaxMoverPlanes];
            b3ShapeId           Shapes[kMaxMoverPlanes];
            int32               Count = 0;

            b3Pos               Origin{};
            b3BodyId            IgnoreBody{};
            FCollisionProfile   Profile{};
            bool                bPermissive = true;
            bool                bCollideWithCharacters = true;
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

        bool GatherPlanes(b3ShapeId ShapeId, const b3PlaneResult* Results, int32 PlaneCount, void* Context)
        {
            FMoverPlanes& Out = *static_cast<FMoverPlanes*>(Context);

            if (!MoverAcceptsShape(Out, ShapeId))
            {
                return true;
            }

            for (int32 i = 0; i < PlaneCount && Out.Count < kMaxMoverPlanes; ++i)
            {
                Out.Planes[Out.Count] = b3CollisionPlane{ Results[i].plane, FLT_MAX, 0.0f, true };
                Out.Points[Out.Count] = b3Add(Out.Origin, Results[i].point);
                Out.Shapes[Out.Count] = ShapeId;
                ++Out.Count;
            }

            return true;
        }

        bool MoverCastFilter(b3ShapeId ShapeId, void* Context)
        {
            return MoverAcceptsShape(*static_cast<const FMoverPlanes*>(Context), ShapeId);
        }

        // Places the proxy on the character by the next world step, written per body so it runs inside the fan-out.
        void DriveProxy(FPhysicsCharacterHandle& Character, float FixedDt)
        {
            const b3WorldTransform Target{ Box3DUtils::ToB3Vec3(Character.Position), Box3DUtils::ToB3Quat(Character.Rotation) };
            b3Body_SetTargetTransform(Character.ProxyBody, Target, FixedDt, false);
            Character.bProxyInMotion = true;
        }

        // Teleports and seating place the proxy at once, so a velocity left from a target transform must not carry it on.
        void PlaceProxy(FPhysicsCharacterHandle& Character)
        {
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

            if (Movement.bUseControllerRotation && Math::Abs(Movement.PendingLookYaw - Character.RestLookYaw) > LE_SMALL_NUMBER)
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
        PendingCharacters.push_back(Entity);
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
        Handle->Position = Transform->GetLocation();
        Handle->Rotation = Transform->GetRotation();
        Handle->Radius = Component->Radius;
        Handle->HalfHeight = Component->HalfHeight;
        Handle->Padding = Component->Padding;
        Handle->TranslationOffset = Component->TranslationOffset;
        Handle->StepHeight = Component->StepHeight;
        Handle->StickToFloorDistance = Component->StickToFloorDistance;
        Handle->CosMaxSlope = Math::Cos(Math::Radians(Component->MaxSlopeAngle));
        Handle->MaxStrength = Component->MaxStrength;
        Handle->Mass = Component->Mass;
        Handle->MaxCollisionIterations = (int32)Math::Max(Component->MaxCollisionIterations, 1u);
        Handle->bCollideWithCharacters = Component->bCollideWithCharacters;
        Handle->Profile = Component->CollisionProfile;
        Handle->Filter = Box3DUtils::MakeQueryFilter(Component->CollisionProfile);

        Handle->SpawnPosition = Handle->Position;

        // A kinematic proxy so other bodies, queries and contact events still see the character as a body.
        b3BodyDef BodyDef = b3DefaultBodyDef();
        BodyDef.type = b3_kinematicBody;
        BodyDef.position = Box3DUtils::ToB3Vec3(Handle->Position);
        BodyDef.rotation = Box3DUtils::ToB3Quat(Handle->Rotation);
        BodyDef.enableSleep = false;

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

    void FBox3DPhysicsScene::OnCharacterComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (const FBodyRecord* Record = CharacterBodies.Find(Entity))
        {
            if (Record->Handle != InvalidBodyHandle)
            {
                PendingBodyDestructions.push_back(Record->Handle);
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

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

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

            if (Math::LengthSquared(Controller.MoveInput) > LE_SMALL_NUMBER || Math::LengthSquared(Controller.WorldMoveInput) > LE_SMALL_NUMBER)
            {
                const FVector3 Forward = RenderUtils::GetForwardVector(Controller.LookInput.x, 0.0f);
                const FVector3 Right = RenderUtils::GetRightVector(Controller.LookInput.x);
                const FVector3 Up = Math::Cross(Right, Forward);

                FVector3 Direction = Right * Controller.MoveInput.x + Up * Controller.MoveInput.y + Forward * Controller.MoveInput.z
                                   + Controller.WorldMoveInput;
                const float Magnitude = Math::Length(Direction);
                if (Magnitude > LE_SMALL_NUMBER)
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
            Task::ParallelFor(NumControllers, Latch);
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

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        auto View = Registry.View<SCharacterPhysicsComponent, SCharacterMovementComponent>();

        ++CharacterStepCounter;
        int64 RestingCount = 0;

        CharacterWorkScratch.clear();

        // Resting, teleporting and unseated characters resolve here, so the fan-out below is a uniform
        // collide-and-solve over characters that all owe the same work.
        View.ForEach([&](ECS::FEntity Entity, SCharacterPhysicsComponent& Physics, SCharacterMovementComponent& Movement)
        {
            if (!Physics.Character)
            {
                return;
            }

            FPhysicsCharacterHandle& Character = *Physics.Character;

            if (Character.NetDrive == (uint8)ECharacterNetDrive::Commands)
            {
                return;
            }
            if (Character.NetDrive == (uint8)ECharacterNetDrive::FollowTransform)
            {
                FollowTransform(Registry, Entity, Physics, Movement, FixedDt);
                return;
            }

            if (Character.bResting)
            {
                const bool bPollDue = ((CharacterStepCounter + (uint64)(Entity).Value) & kRestPollMask) == 0;
                if (!bPollDue && !ShouldWakeResting(Character, Movement))
                {
                    // The last step's target transform has landed the proxy, so it stops here instead of drifting on.
                    if (Character.bProxyInMotion)
                    {
                        PlaceProxy(Character);
                    }
                    ++RestingCount;
                    return;
                }

                Character.bResting = false;
            }

            if (!ResolveCharacterPreStep(Entity, Physics, Movement))
            {
                return;
            }

            CharacterWorkScratch.push_back({ &Physics, &Movement });
        });

        const uint32 WorkCount = (uint32)CharacterWorkScratch.size();

        const uint32 ThreadSlots = Math::Max(GTaskSystem->GetNumTaskThreads(), 1u);
        if (CharacterPushScratch.size() < ThreadSlots)
        {
            CharacterPushScratch.resize(ThreadSlots);
        }

        for (FCharacterPushBucket& Bucket : CharacterPushScratch)
        {
            Bucket.Pushes.clear();
        }

        if (WorkCount > kCharacterParallelThreshold)
        {
            Task::ParallelFor(WorkCount, [&](uint32 Index, uint32 Thread)
            {
                StepCharacter(CharacterWorkScratch[Index], FixedDt, Thread);
                DriveProxy(*CharacterWorkScratch[Index].Physics->Character, FixedDt);
            },
            kCharacterParallelGrain);
        }
        else
        {
            for (uint32 Index = 0; Index < WorkCount; ++Index)
            {
                StepCharacter(CharacterWorkScratch[Index], FixedDt, 0);
                DriveProxy(*CharacterWorkScratch[Index].Physics->Character, FixedDt);
            }
        }

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
        }

        LUMINA_PROFILE_VALUE("Physics/RestingCharacters", RestingCount);
    }

    bool FBox3DPhysicsScene::ResolveCharacterPreStep(ECS::FEntity Entity, SCharacterPhysicsComponent& Physics, SCharacterMovementComponent& Movement)
    {
        FPhysicsCharacterHandle& Character = *Physics.Character;

        if (Movement.bPendingTeleport)
        {
            Movement.bPendingTeleport = false;

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
        const FVector3 Target = Transform->GetLocation();
        Movement.Velocity = FixedDt > 0.0f ? (Target - Character.Position) / FixedDt : FVector3(0.0f);
        Character.Position = Target;
        Character.Rotation = Transform->GetRotation();
        Character.Velocity = Movement.Velocity;
        Character.bAwaitingGround = false;
        Physics.LastBodyPosition = Character.Position;
        Physics.LastBodyRotation = Character.Rotation;
        PlaceProxy(Character);
    }

    bool FBox3DPhysicsScene::GetCharacterNetState(ECS::FEntity Entity, FCharacterNetState& Out) const
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
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
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
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
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
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
            if (CharacterPushScratch.empty())
            {
                CharacterPushScratch.resize(1);
            }
            CharacterPushScratch[0].Pushes.clear();

            StepCharacter(FCharacterWork{ Physics, Movement }, FixedDt, 0);
            DriveProxy(*Physics->Character, FixedDt);

            // A replay re-walks steps whose pushes already happened, so applying them again would shove twice.
            if (!bReplay)
            {
                for (const FPendingCharacterPush& Push : CharacterPushScratch[0].Pushes)
                {
                    if (b3Body_IsValid(Push.Body))
                    {
                        b3Body_ApplyLinearImpulse(Push.Body, Push.Impulse, Push.Point, true);
                    }
                }
            }
            CharacterPushScratch[0].Pushes.clear();
        }

        Movement->MoveSpeed = AuthoredSpeed;
        return true;
    }

    void FBox3DPhysicsScene::SetCharacterNetDrive(ECS::FEntity Entity, ECharacterNetDrive Drive)
    {
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        if (SCharacterPhysicsComponent* Physics = Registry.TryGet<SCharacterPhysicsComponent>(Entity); Physics != nullptr && Physics->Character)
        {
            Physics->Character->NetDrive = (uint8)Drive;
            Physics->Character->bResting = false;
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
        const float Speed = bSprinting ? Movement.SprintSpeed : Movement.MoveSpeed;
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

        const b3Capsule Mover = Character.MakeMoverCapsule();

        b3Pos Position = Box3DUtils::ToB3Vec3(Character.Position);
        const b3Pos StartPosition = Position;

        FMoverPlanes Gathered;
        Gathered.IgnoreBody = Character.ProxyBody;
        Gathered.Profile = Character.Profile;
        Gathered.bPermissive = Box3DUtils::UsesPermissiveCollisionFilter();
        Gathered.bCollideWithCharacters = Character.bCollideWithCharacters;

        auto GatherAt = [&](b3Pos At)
        {
            Gathered.Count = 0;
            Gathered.Origin = At;
            b3World_CollideMover(WorldId, At, &Mover, Character.Filter, &GatherPlanes, &Gathered);
        };

        auto FindWalkablePlane = [&]()
        {
            int32 Best = INDEX_NONE;
            float BestUp = Character.CosMaxSlope;
            for (int32 i = 0; i < Gathered.Count; ++i)
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

        // Box3D's documented mover order is cast and move first, then gather at the new pose, then solve.
        // Solving before the cast would feed the depenetration push back through the cast, and on a slope
        // that push has a horizontal component, which walks the character downhill every frame.
        b3Vec3 Desired = b3MulSV(FixedDt, Box3DUtils::ToB3Vec3(Character.Velocity));

        // On a walkable floor the move follows the surface instead of being bent onto it by the plane solver.
        // Projection would shave the horizontal delta by the slope cosine and rotate it toward the contour,
        // so solving only the vertical is what keeps the authored heading and speed exact on a ramp.
        if (bWasGrounded && !bJumpedThisStep && Character.GroundNormal.y >= Character.CosMaxSlope
            && Character.GroundNormal.y > kMinRampNormalY)
        {
            const FVector3& Ground = Character.GroundNormal;
            Desired.y = -(Desired.x * Ground.x + Desired.z * Ground.z) / Ground.y;
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

        // Overlap is resolved against a zero target so it can never become motion, which is what made the
        // solve delta walk the character downhill when it was cast. A capsule spawned inside geometry needs
        // several passes to escape; a resting one exits on the first because there is nothing to push out of.
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

        if (bWasGrounded && Character.StepHeight > 0.0f && WantedHorizontal > kMoveTolerance
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

                    Position = b3Add(Stepped, b3MulSV(DownFraction, StepDown));
                    GatherAt(Position);
                    GroundPlane = FindWalkablePlane();
                }
            }
        }

        // Only a move that actually left the ground gets pulled back down. Settling every frame would
        // re-seat the capsule in the floor so the next solve could push it out along the slope again.
        if (GroundPlane == INDEX_NONE && bWasGrounded && !bJumpedThisStep && Character.Velocity.y <= 0.0f)
        {
            const b3Vec3 Drop{ 0.0f, -Character.StickToFloorDistance, 0.0f };
            const float DropFraction = b3World_CastMover(WorldId, Position, &Mover, Drop, Character.Filter, &MoverCastFilter, &Gathered);

            if (DropFraction < 1.0f)
            {
                const b3Pos Landed = b3Add(Position, b3MulSV(DropFraction, Drop));
                GatherAt(Landed);

                const int32 LandedPlane = FindWalkablePlane();
                if (LandedPlane != INDEX_NONE)
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

        const bool bNowGrounded = GroundPlane != INDEX_NONE && !bJumpedThisStep;

        // The landing edge is only visible here, where this step's ground has been resolved against the last one.
        if (bNowGrounded && !bWasGrounded)
        {
            Movement.JumpCount = 0;
        }

        Character.bGrounded = bNowGrounded;

        if (Character.bGrounded)
        {
            Character.GroundNormal = Box3DUtils::FromB3Vec3(Gathered.Planes[GroundPlane].plane.normal);

            const b3BodyId GroundBody = b3Shape_GetBody(Gathered.Shapes[GroundPlane]);
            void* GroundUserData = b3Body_IsValid(GroundBody) ? b3Body_GetUserData(GroundBody) : nullptr;
            Character.GroundEntity = GroundUserData != nullptr ? UnpackEntity(GroundUserData) : ECS::NullEntity;
            Character.GroundVelocity = b3Body_IsValid(GroundBody)
                ? Box3DUtils::FromB3Vec3(b3Body_GetWorldPointVelocity(GroundBody, Gathered.Points[GroundPlane]))
                : FVector3(0.0f);
        }
        else
        {
            Character.GroundNormal = FVector3(0.0f, 1.0f, 0.0f);
            Character.GroundEntity = ECS::NullEntity;
            Character.GroundVelocity = FVector3(0.0f);
        }

        // Rest is only safe against things that cannot move on their own. A neighboring character proxy
        // counts as inert because the mover never pushes one, it is only blocked by it.
        bool bRestableContacts = true;

        // Push whatever the mover leaned on, so crates and props still respond to being walked into.
        for (int32 i = 0; i < Gathered.Count; ++i)
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

            CharacterPushScratch[ThreadSlot].Pushes.push_back({ HitBody, b3MulSV(Impulse, Normal), Point });
        }

        Character.Position = Box3DUtils::FromB3Vec3(Position);

        // b3ClipVector ignores planes with a zero push, and every gather rebuilds the set with zero pushes.
        b3SolvePlanes(b3Vec3_zero, Gathered.Planes, Gathered.Count);

        // Only a wall may take speed away. Clipping horizontal velocity against a walkable floor removes its
        // into-slope component every step, so a character on a ramp settles below its authored speed.
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

        // Only static ground rests. A platform or another character can walk out from under the capsule
        // without any signal the cheap wake check would see.
        const b3BodyId GroundBody = b3Shape_GetBody(Gathered.Shapes[GroundPlane]);
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
