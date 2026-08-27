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

        // A couple of seconds of fixed steps, long past any deferred collider build.
        constexpr uint32 kAwaitingGroundWarnSteps = 120;

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

    }


    void FBox3DPhysicsScene::OnCharacterComponentConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        if (BodyBatchDepth > 0)
        {
            BatchedCharacterCreations.push_back(Entity);
            return;
        }

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

        Component->Character = Move(Handle);
        Component->LastBodyPosition = Component->Character->Position;
        Component->LastBodyRotation = Component->Character->Rotation;
    }

    void FBox3DPhysicsScene::OnCharacterComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        SCharacterPhysicsComponent* Component = Registry.TryGet<SCharacterPhysicsComponent>(Entity);
        if (Component == nullptr || !Component->Character)
        {
            return;
        }

        FPhysicsCharacterHandle& Handle = *Component->Character;
        if (b3Body_IsValid(Handle.ProxyBody))
        {
            b3DestroyBody(Handle.ProxyBody);
            Handle.ProxyBody = b3_nullBodyId;
        }

        UnregisterBody(Handle.ProxyBodyHandle);
        Component->Character.reset();
    }

    void FBox3DPhysicsScene::LatchCharacterInput()
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        Registry.View<SCharacterControllerComponent, SCharacterMovementComponent>().ForEach(
            [&](SCharacterControllerComponent& Controller, SCharacterMovementComponent& Movement)
        {
            if (Math::LengthSquared(Controller.MoveInput) > LE_SMALL_NUMBER)
            {
                const FVector3 Forward = RenderUtils::GetForwardVector(Controller.LookInput.x, 0.0f);
                const FVector3 Right = RenderUtils::GetRightVector(Controller.LookInput.x);
                const FVector3 Up = Math::Cross(Right, Forward);

                FVector3 Direction = Right * Controller.MoveInput.x + Up * Controller.MoveInput.y + Forward * Controller.MoveInput.z;
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
            Controller.MoveInput = {};

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
        });
    }

    void FBox3DPhysicsScene::UpdateCharacters(float FixedDt)
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        auto View = Registry.View<SCharacterPhysicsComponent, SCharacterMovementComponent>();

        // Impulses are staged here so the collide-and-solve pass stays a read-only world query.
        struct FPendingPush
        {
            b3BodyId    Body;
            b3Vec3      Impulse;
            b3Vec3      Point;
        };
        static thread_local TVector<FPendingPush> Pushes;
        Pushes.clear();

        View.ForEach([&](ECS::FEntity Entity, SCharacterPhysicsComponent& Physics, SCharacterMovementComponent& Movement)
        {
            if (!Physics.Character)
            {
                return;
            }

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
                Character.GroundEntity = 0xFFFFFFFFu;

                Movement.Velocity = FVector3(0.0f);
                Movement.bGrounded = false;
                Movement.GroundEntity = 0xFFFFFFFFu;
                Movement.GroundNormal = FVector3(0.0f, 1.0f, 0.0f);

                // Reseeds the interp snapshot so the render transform does not streak across the jump.
                Physics.LastBodyPosition = Character.Position;
                b3Body_SetTransform(Character.ProxyBody, Box3DUtils::ToB3Vec3(Character.Position), Box3DUtils::ToB3Quat(Character.Rotation));
                return;
            }

            // The capsule is centered on the entity origin, so a ground-level spawn starts buried, and Box3D's
            // mover ignores initial overlap and can never climb out. A dynamic mesh collider also defers until
            // its CPU data is ready, so the ground can appear several frames after the character does. Holding
            // at the spawn until there is something to stand on covers both.
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

                    b3Body_SetTransform(Character.ProxyBody, Box3DUtils::ToB3Vec3(Character.Position),
                        Box3DUtils::ToB3Quat(Character.Rotation));
                    return;
                }

                if (Result == EMoverSeatResult::Seated)
                {
                    const FVector3 SeatedPosition = Box3DUtils::FromB3Vec3(Seated);
                    LOG_DISPLAY("Character on entity {} spawned inside geometry and was seated from y {:.2f} to {:.2f} "
                             "after {} step(s) waiting for ground.",
                        (Entity).Value, Character.Position.y, SeatedPosition.y, Character.AwaitingGroundSteps);

                    Character.Position = SeatedPosition;
                    Character.Velocity = FVector3(0.0f);
                    Movement.Velocity = FVector3(0.0f);
                }
                else
                {
                    LOG_DISPLAY("Character on entity {} starts airborne at y {:.2f} after {} step(s) waiting for ground.",
                        (Entity).Value, Character.SpawnPosition.y, Character.AwaitingGroundSteps);
                }

                Character.bAwaitingGround = false;
            }

            // Snapshotting each substep leaves the pose from before the last one, which is what interp blends from.
            Physics.LastBodyPosition = Character.Position;
            Physics.LastBodyRotation = Character.Rotation;

            const bool bHasMovementInput = Movement.bHasPendingMoveInput;
            const FVector3 DesiredDirection = Movement.PendingMoveDirection;
            const float TargetSpeed = bHasMovementInput ? Movement.MoveSpeed * Movement.PendingMoveThrottle : 0.0f;
            const FVector3 TargetVelocity = DesiredDirection * TargetSpeed;

            // Ground state was resolved at the end of the previous substep.
            const bool bWasGrounded = Character.bGrounded;

            Movement.bGrounded = Character.bGrounded;
            Movement.GroundNormal = Character.GroundNormal;
            Movement.GroundEntity = Character.GroundEntity;

            if (!bWasGrounded && Movement.bGrounded)
            {
                Movement.JumpCount = 0;
            }

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
            const b3Vec3 Desired = b3MulSV(FixedDt, Box3DUtils::ToB3Vec3(Character.Velocity));
            const float TravelFraction = b3World_CastMover(WorldId, Position, &Mover, Desired, Character.Filter, &MoverCastFilter, &Gathered);

            Position = b3Add(Position, b3MulSV(TravelFraction, Desired));

            GatherAt(Position);

            // The solver slides the unused remainder along whatever was hit, and corrects any overlap.
            const b3Vec3 Remaining = b3MulSV(1.0f - TravelFraction, Desired);
            const b3PlaneSolverResult Solved = b3SolvePlanes(Remaining, Gathered.Planes, Gathered.Count);
            Position = b3Add(Position, Solved.delta);


            // Overlap is resolved against a zero target so it can never become motion, which is what made the
            // solve delta walk the character downhill when it was cast. A capsule spawned inside geometry needs
            // several passes to escape; a resting one exits on the first because there is nothing to push out of.
            bool bPushedOut = false;
            for (int32 Recovery = 0; Recovery < Character.MaxCollisionIterations; ++Recovery)
            {
                GatherAt(Position);
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

            Character.bGrounded = GroundPlane != INDEX_NONE && !bJumpedThisStep;

            if (Character.bGrounded)
            {
                Character.GroundNormal = Box3DUtils::FromB3Vec3(Gathered.Planes[GroundPlane].plane.normal);

                const b3BodyId GroundBody = b3Shape_GetBody(Gathered.Shapes[GroundPlane]);
                void* GroundUserData = b3Body_IsValid(GroundBody) ? b3Body_GetUserData(GroundBody) : nullptr;
                Character.GroundEntity = GroundUserData != nullptr ? (UnpackEntity(GroundUserData)).Value : 0xFFFFFFFFu;
                Character.GroundVelocity = b3Body_IsValid(GroundBody)
                    ? Box3DUtils::FromB3Vec3(b3Body_GetWorldPointVelocity(GroundBody, Gathered.Points[GroundPlane]))
                    : FVector3(0.0f);
            }
            else
            {
                Character.GroundNormal = FVector3(0.0f, 1.0f, 0.0f);
                Character.GroundEntity = 0xFFFFFFFFu;
                Character.GroundVelocity = FVector3(0.0f);
            }

            // Push whatever the mover leaned on, so crates and props still respond to being walked into.
            for (int32 i = 0; i < Gathered.Count; ++i)
            {
                const b3BodyId HitBody = b3Shape_GetBody(Gathered.Shapes[i]);
                if (!b3Body_IsValid(HitBody) || b3Body_GetType(HitBody) != b3_dynamicBody)
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

                Pushes.push_back({ HitBody, b3MulSV(Impulse, Normal), Point });
            }

            Character.Position = Box3DUtils::FromB3Vec3(Position);

            // b3ClipVector ignores planes with a zero push, and every gather rebuilds the set with zero pushes.
            b3SolvePlanes(b3Vec3_zero, Gathered.Planes, Gathered.Count);

            // Without this, velocity accumulates every frame the mover is pressed against a surface.
            Character.Velocity = Box3DUtils::FromB3Vec3(b3ClipVector(Box3DUtils::ToB3Vec3(Character.Velocity), Gathered.Planes, Gathered.Count));
            Movement.Velocity = Character.Velocity;

            // The proxy follows the mover so queries, contacts and other bodies stay in sync.
            b3Body_SetTransform(Character.ProxyBody, Box3DUtils::ToB3Vec3(Character.Position), Box3DUtils::ToB3Quat(Character.Rotation));
        });

        for (const FPendingPush& Push : Pushes)
        {
            if (b3Body_IsValid(Push.Body))
            {
                b3Body_ApplyLinearImpulse(Push.Body, Push.Impulse, Push.Point, true);
            }
        }
    }
}
