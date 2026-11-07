#include <gtest/gtest.h>
#include "Core/Object/ObjectCore.h"
#include "Physics/API/Box3D/Box3DPhysicsScene.h"
#include "Physics/API/Box3D/Box3DRagdollHandle.h"
#include "Physics/API/Box3D/Box3DCharacterHandle.h"
#include "Renderer/SkeletonResource.h"
#include "World/World.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Events/CollisionEvent.h"
#include "World/Subsystems/WorldSettings.h"

using namespace Lumina;

namespace
{
    class PhysicsSceneLifecycle : public testing::Test
    {
    protected:
        void SetUp() override
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            Registry = &World->GetEntityRegistry();
            Scene = MakeUnique<Physics::FBox3DPhysicsScene>(World);
            Scene->Simulate();
            Dt = 1.0f / Math::Max(10.0f, World->GetDefaultWorldSettings().PhysicsHz);
        }

        void TearDown() override
        {
            Scene->StopSimulate();
            Scene.reset();
        }

        ECS::FEntity Spawn(float X = 0.0f, bool bCollider = true)
        {
            const ECS::FEntity Entity = Registry->Create();
            Registry->Emplace<STransformComponent>(Entity).SetLocation(FVector3(X, 0.0f, 0.0f));
            auto& Body = Registry->Emplace<SRigidBodyComponent>(Entity);
            Body.bUseGravity = false;
            Body.bOverrideMass = true;
            Body.Mass = 1.0f;
            Body.LinearDamping = 0.0f;
            Body.AngularDamping = 0.0f;
            if (bCollider) { Registry->Emplace<SSphereColliderComponent>(Entity).Radius = 0.5f; }
            return Entity;
        }

        void SpawnFloor()
        {
            const ECS::FEntity Floor = Registry->Create();
            Registry->Emplace<STransformComponent>(Floor).SetLocation(FVector3(0.0f, -0.5f, 0.0f));
            Registry->Emplace<SRigidBodyComponent>(Floor).BodyType = EBodyType::Static;
            Registry->Emplace<SBoxColliderComponent>(Floor).HalfExtent = FVector3(20.0f, 0.5f, 20.0f);
        }

        SRayCastSettings Ray(float X = 0.0f)
        {
            SRayCastSettings Query;
            Query.Start = FVector3(X, 0.0f, -3.0f);
            Query.End = FVector3(X, 0.0f, 3.0f);
            return Query;
        }

        CWorld* World = nullptr;
        ECS::FRegistry* Registry = nullptr;
        TUniquePtr<Physics::FBox3DPhysicsScene> Scene;
        float Dt = 0.0f;
    };
}

TEST_F(PhysicsSceneLifecycle, ReadsNeverCreatePendingBodies)
{
    const ECS::FEntity Entity = Spawn();
    Physics::FPhysicsBodyState State;
    for (int i = 0; i < 1000; ++i)
    {
        EXPECT_FALSE(Scene->TryGetBodyState(Entity, State));
        EXPECT_FALSE(Scene->CastRay(Ray()).IsSet());
        EXPECT_EQ(Scene->GetBodyCount(), 0u);
    }
    EXPECT_EQ(Scene->GetBodyStatus(Entity), Physics::EPhysicsBodyStatus::Pending);
    Scene->Update(0.0);
    EXPECT_TRUE(Scene->TryGetBodyState(Entity, State));
    EXPECT_EQ(Scene->GetBodyCount(), 1u);
}

TEST_F(PhysicsSceneLifecycle, SpawnCommandsPreserveOrderAcrossAFrameWithoutAStep)
{
    const ECS::FEntity Entity = Spawn();
    Scene->SetLinearVelocity(Entity, FVector3(2.0f, 0.0f, 0.0f));
    Scene->AddImpulse(Entity, FVector3(3.0f, 0.0f, 0.0f));
    Scene->Update(0.0);
    EXPECT_FLOAT_EQ(Scene->GetLinearVelocity(Entity).x, 0.0f);
    Scene->Update(Dt);
    EXPECT_NEAR(Scene->GetLinearVelocity(Entity).x, 5.0f, 0.001f);
    Scene->SetLinearVelocity(Entity, FVector3(7.0f, 0.0f, 0.0f));
    Scene->AddImpulse(Entity, FVector3(3.0f, 0.0f, 0.0f));
    Scene->SetLinearVelocity(Entity, FVector3(11.0f, 0.0f, 0.0f));
    Scene->Update(Dt);
    EXPECT_NEAR(Scene->GetLinearVelocity(Entity).x, 11.0f, 0.001f);
}

TEST_F(PhysicsSceneLifecycle, FixedCallbackCanSpawnAndLaunchInTheSameStep)
{
    ECS::FEntity Entity;
    Scene->SetPreStepCallback([&](float)
    {
        Entity = Spawn();
        Scene->SetLinearVelocity(Entity, FVector3(4.0f, 0.0f, 0.0f));
    }, [] { return false; });
    Scene->Update(Dt);
    EXPECT_NEAR(Scene->GetLinearVelocity(Entity).x, 4.0f, 0.001f);
    EXPECT_GT(Scene->GetBodyPosition(Entity).x, 0.0f);
}

TEST_F(PhysicsSceneLifecycle, CommandsWaitForALateCollider)
{
    const ECS::FEntity Entity = Spawn(0.0f, false);
    Scene->AddImpulse(Entity, FVector3(6.0f, 0.0f, 0.0f));
    Scene->Update(Dt);
    EXPECT_EQ(Scene->GetBodyCount(), 0u);
    Registry->Emplace<SSphereColliderComponent>(Entity).Radius = 0.5f;
    Scene->Update(Dt);
    EXPECT_NEAR(Scene->GetLinearVelocity(Entity).x, 6.0f, 0.001f);
}

TEST_F(PhysicsSceneLifecycle, RemovedComponentsCancelTheirPendingCommands)
{
    const ECS::FEntity Entity = Spawn();
    Scene->AddImpulse(Entity, FVector3(9.0f, 0.0f, 0.0f));
    Registry->Remove<SRigidBodyComponent>(Entity);
    Registry->Emplace<SRigidBodyComponent>(Entity).bUseGravity = false;
    Scene->Update(Dt);
    EXPECT_EQ(Scene->GetBodyCount(), 1u);
    EXPECT_FLOAT_EQ(Scene->GetLinearVelocity(Entity).x, 0.0f);
}

TEST_F(PhysicsSceneLifecycle, DestroyedEntitiesCancelCreationAndCommands)
{
    const ECS::FEntity Entity = Spawn();
    Scene->AddImpulse(Entity, FVector3(9.0f, 0.0f, 0.0f));
    Registry->Destroy(Entity);
    const ECS::FEntity Replacement = Spawn();
    Scene->Update(Dt);
    EXPECT_EQ(Scene->GetBodyCount(), 1u);
    EXPECT_FLOAT_EQ(Scene->GetLinearVelocity(Replacement).x, 0.0f);
    EXPECT_EQ(Scene->GetBodyStatus(Entity), Physics::EPhysicsBodyStatus::Missing);
}

TEST_F(PhysicsSceneLifecycle, RetainedHitTargetsCannotReachRecycledBodies)
{
    const ECS::FEntity Entity = Spawn();
    Scene->Update(0.0);
    const auto Hit = Scene->CastRay(Ray());
    ASSERT_TRUE(Hit.IsSet());
    Registry->Destroy(Entity);
    const ECS::FEntity Replacement = Spawn();
    Scene->Update(0.0);
    Physics::FPhysicsBodyState State;
    EXPECT_FALSE(Scene->TryGetTargetState(Hit->Target, State));
    Scene->AddForceAtTarget(Hit->Target, FVector3(1000.0f, 0.0f, 0.0f), FVector3(0.0f));
    Scene->Update(Dt);
    EXPECT_FLOAT_EQ(Scene->GetLinearVelocity(Replacement).x, 0.0f);
}

TEST_F(PhysicsSceneLifecycle, EntityFiltersNeedNoBodyLookup)
{
    const ECS::FEntity Entity = Spawn();
    auto Query = Ray();
    Query.IgnoreEntities.Add(Entity);
    Scene->Update(0.0);
    EXPECT_TRUE(Scene->CastRay(Ray()).IsSet());
    EXPECT_FALSE(Scene->CastRay(Query).IsSet());
}

TEST_F(PhysicsSceneLifecycle, ForceIsConsumedByOneFixedStep)
{
    const ECS::FEntity Entity = Spawn();
    Scene->AddForce(Entity, FVector3(12.0f, 0.0f, 0.0f));
    Scene->Update(0.0);
    Scene->Update(2.0f * Dt);
    EXPECT_NEAR(Scene->GetLinearVelocity(Entity).x, 12.0f * Dt, 0.001f);
}

TEST_F(PhysicsSceneLifecycle, RepeatedColliderChangesDoNotDuplicateBodies)
{
    const ECS::FEntity Entity = Spawn();
    Registry->Patch<SSphereColliderComponent>(Entity, [](auto& Sphere) { Sphere.Radius = 0.75f; });
    Registry->Patch<SRigidBodyComponent>(Entity, [](auto& Body) { Body.Mass = 2.0f; });
    Scene->Update(0.0);
    EXPECT_EQ(Scene->GetBodyCount(), 1u);
    EXPECT_NEAR(Scene->GetBodyMass(Entity), 2.0f, 0.001f);
}

TEST_F(PhysicsSceneLifecycle, ConstraintsWaitForBothPendingBodies)
{
    const ECS::FEntity A = Spawn(-2.0f);
    const ECS::FEntity B = Spawn(2.0f, false);
    Physics::SConstraintDesc Desc;
    Desc.BodyA = A;
    Desc.BodyB = B;
    const uint32 Constraint = Scene->CreateConstraint(Desc);
    ASSERT_NE(Constraint, 0u);
    Scene->Update(0.0);
    EXPECT_EQ((int)Scene->GetLiveJointCount(), 0);
    Registry->Emplace<SSphereColliderComponent>(B);
    Scene->Update(0.0);
    EXPECT_EQ((int)Scene->GetLiveJointCount(), 1);
    Scene->DestroyConstraint(Constraint);
    Scene->Update(0.0);
    EXPECT_EQ((int)Scene->GetLiveJointCount(), 0);
}

TEST_F(PhysicsSceneLifecycle, RagdollRequestsOwnTemporaryPoseDataAndFilterByOwner)
{
    const ECS::FEntity Owner = Registry->Create();
    TSharedPtr<FPhysicsRagdollHandle> Handle;
    {
        FSkeletonResource Skeleton;
        FSkeletonResource::FBoneInfo Bone;
        Bone.Name = FName("root");
        Bone.ParentIndex = Constants::kIndexNone;
        Bone.LocalTransform = FMatrix4(1.0f);
        Bone.InvBindMatrix = FMatrix4(1.0f);
        Skeleton.Bones.Add(Bone);
        Skeleton.BoneNameToIndex[Bone.Name] = 0;
        TVector<FMatrix4> Globals{ FMatrix4(1.0f) };
        Physics::FRagdollDesc Desc;
        Desc.Entity = Owner;
        Desc.Skeleton = &Skeleton;
        Desc.ComponentBoneGlobals = &Globals;
        Desc.EntityToWorld = FMatrix4(1.0f);
        Handle = Scene->CreateRagdoll(Desc);
    }
    ASSERT_TRUE(Handle);
    EXPECT_FALSE(Scene->IsRagdollReady(*Handle));
    Scene->Update(0.0);
    EXPECT_TRUE(Scene->IsRagdollReady(*Handle));
    EXPECT_EQ(Scene->GetBodyCount(), 1u);
    auto Query = Ray();
    Query.IgnoreEntities.Add(Owner);
    EXPECT_TRUE(Scene->CastRay(Ray()).IsSet());
    EXPECT_FALSE(Scene->CastRay(Query).IsSet());
    Scene->DestroyRagdoll(Handle);
    Scene->Update(0.0);
    EXPECT_EQ(Scene->GetBodyCount(), 0u);
}


// Entity calls reach every limb of a simulating ragdoll, and one limb can be driven alone through its bone's body target.
TEST_F(PhysicsSceneLifecycle, RagdollTakesEntityVelocityAndPerLimbTargets)
{
    const ECS::FEntity Owner = Registry->Create();
    TSharedPtr<FPhysicsRagdollHandle> Handle;
    {
        FSkeletonResource Skeleton;
        const FMatrix4 ChildGlobal = Math::Translate(FMatrix4(1.0f), FVector3(0.0f, 1.0f, 0.0f));
        const char* Names[] = { "root", "child" };
        const FMatrix4 Globals[] = { FMatrix4(1.0f), ChildGlobal };
        for (int32 Index = 0; Index < 2; ++Index)
        {
            FSkeletonResource::FBoneInfo Bone;
            Bone.Name = FName(Names[Index]);
            Bone.ParentIndex = Index == 0 ? Constants::kIndexNone : 0;
            Bone.LocalTransform = Globals[Index];
            Bone.InvBindMatrix = Math::Inverse(Globals[Index]);
            Skeleton.Bones.Add(Bone);
            Skeleton.BoneNameToIndex[Bone.Name] = Index;
        }
        TVector<FMatrix4> BoneGlobals{ Globals[0], Globals[1] };
        Physics::FRagdollDesc Desc;
        Desc.Entity = Owner;
        Desc.Skeleton = &Skeleton;
        Desc.ComponentBoneGlobals = &BoneGlobals;
        Desc.EntityToWorld = FMatrix4(1.0f);
        Handle = Scene->CreateRagdoll(Desc);
    }
    ASSERT_TRUE(Handle);
    EXPECT_FALSE(Scene->GetRagdollBoneTarget(Owner, FName("child")).IsSet());
    Scene->Update(0.0);
    ASSERT_TRUE(Scene->IsRagdollReady(*Handle));

    const SPhysicsBodyTarget Root = Scene->GetRagdollBoneTarget(Owner, FName("root"));
    const SPhysicsBodyTarget Child = Scene->GetRagdollBoneTarget(Owner, FName("child"));
    ASSERT_TRUE(Root.IsSet());
    ASSERT_TRUE(Child.IsSet());
    EXPECT_NE(Root.Slot, Child.Slot);
    EXPECT_FALSE(Scene->GetRagdollBoneTarget(Owner, FName("missing")).IsSet());
    EXPECT_GT(Scene->GetBodyMass(Owner), 0.0f);

    Scene->SetLinearVelocity(Owner, FVector3(3.0f, 0.0f, 0.0f));
    Scene->Update(Dt);
    Physics::FPhysicsBodyState RootState, ChildState;
    ASSERT_TRUE(Scene->TryGetTargetState(Root, RootState));
    ASSERT_TRUE(Scene->TryGetTargetState(Child, ChildState));
    EXPECT_NEAR(RootState.LinearVelocity.x, 3.0f, 0.05f);
    EXPECT_NEAR(ChildState.LinearVelocity.x, 3.0f, 0.05f);
    EXPECT_NEAR(Scene->GetLinearVelocity(Owner).x, 3.0f, 0.05f);

    // The joint shares the limb's push with the rest of the ragdoll, so only the momentum it added is conserved.
    const float ChildMass = ChildState.Mass;
    Scene->SetTargetLinearVelocity(Child, FVector3(0.0f, 0.0f, 5.0f));
    Scene->Update(Dt);
    EXPECT_NEAR(Scene->GetLinearVelocity(Owner).z * Scene->GetBodyMass(Owner), ChildMass * 5.0f, ChildMass * 0.25f);
    EXPECT_NEAR(Scene->GetLinearVelocity(Owner).x, 3.0f, 0.05f);

    Scene->DestroyRagdoll(Handle);
    Scene->Update(0.0);
    EXPECT_FALSE(Scene->GetRagdollBoneTarget(Owner, FName("root")).IsSet());
    EXPECT_FALSE(Scene->TryGetTargetState(Root, RootState));
}

// A sweep's radius reaches bodies the bare ray along the same line misses.
TEST_F(PhysicsSceneLifecycle, ShapeSweepsReachPastTheRayLine)
{
    const ECS::FEntity Entity = Spawn();
    Scene->Update(0.0);

    const FVector3 Start(0.6f, 0.0f, -3.0f);
    const FVector3 End(0.6f, 0.0f, 3.0f);
    EXPECT_FALSE(Scene->CastRay(Ray(0.6f)).IsSet());

    for (const SQueryShape& Shape : { SQueryShape::Sphere(0.2f), SQueryShape::Box(FVector3(0.2f), FQuat::Identity()),
                                      SQueryShape::Capsule(0.2f, 0.5f, FQuat::Identity()) })
    {
        const TOptional<SRayResult> Hit = Scene->CastShapeClosest(Shape, Start, End, ECollisionProfiles::All, {});
        ASSERT_TRUE(Hit.IsSet()) << "shape " << (int)Shape.Type;
        EXPECT_EQ(Hit->Entity, Entity.Value);
        EXPECT_LT(Hit->Location.z, 0.0f) << "the sweep touches the near side of the sphere";

        TVector<SRayResult> All;
        Scene->CastShapeAll(Shape, Start, End, ECollisionProfiles::All, {}, All);
        EXPECT_EQ(All.Num(), 1);
    }
}

TEST_F(PhysicsSceneLifecycle, AnyHitAndSingleEntityRays)
{
    const ECS::FEntity Near = Spawn(0.0f);
    const ECS::FEntity Far = Spawn(3.0f);
    Scene->Update(0.0);

    SRayCastSettings Along;
    Along.Start = FVector3(-5.0f, 0.0f, 0.0f);
    Along.End = FVector3(5.0f, 0.0f, 0.0f);
    EXPECT_TRUE(Scene->CastRayAny(Along));
    Along.IgnoreEntities.Add(Near);
    Along.IgnoreEntities.Add(Far);
    EXPECT_FALSE(Scene->CastRayAny(Along)) << "both blockers are ignored";

    const TOptional<SRayResult> Hit = Scene->CastRayAgainst(Far, Along.Start, Along.End);
    ASSERT_TRUE(Hit.IsSet()) << "the near body in front does not block a ray aimed at one entity";
    EXPECT_EQ(Hit->Entity, Far.Value);
    EXPECT_NEAR(Hit->Location.x, 2.5f, 0.05f);
    EXPECT_FALSE(Scene->CastRayAgainst(Far, FVector3(-5.0f, 2.0f, 0.0f), FVector3(5.0f, 2.0f, 0.0f)).IsSet());
}

TEST_F(PhysicsSceneLifecycle, OverlapsFollowShapeRotationAndLayerMask)
{
    const ECS::FEntity Entity = Spawn();
    Scene->Update(0.0);

    ECS::FEntity Found[4];
    const FVector3 Above(0.0f, 1.2f, 0.0f);
    EXPECT_EQ(Scene->OverlapShape(SQueryShape::Capsule(0.1f, 1.0f, FQuat::Identity()), Above, ECollisionProfiles::All, {}, Found), 1)
        << "a standing capsule reaches down into the sphere";
    EXPECT_EQ(Found[0], Entity);

    const FQuat Lying = Math::FromAxisAngle(FVector3(0.0f, 0.0f, 1.0f), Math::HalfPi<float>());
    EXPECT_EQ(Scene->OverlapShape(SQueryShape::Capsule(0.1f, 1.0f, Lying), Above, ECollisionProfiles::All, {}, Found), 0)
        << "the same capsule lying down stays above it";

    EXPECT_EQ(Scene->OverlapShape(SQueryShape::Sphere(1.0f), FVector3(0.0f), (ECollisionProfiles)0, {}, Found), 0)
        << "an empty layer mask matches nothing";
}

TEST_F(PhysicsSceneLifecycle, ClosestPointAndBoundsMeasureTheColliders)
{
    const ECS::FEntity Entity = Spawn();
    Scene->Update(0.0);

    FVector3 Closest;
    ASSERT_TRUE(Scene->GetClosestPointOnBody(Entity, FVector3(3.0f, 0.0f, 0.0f), Closest));
    EXPECT_NEAR(Closest.x, 0.5f, 0.01f);

    SAABB Bounds;
    ASSERT_TRUE(Scene->GetBodyBounds(Entity, Bounds));
    EXPECT_NEAR(Bounds.Min.x, -0.5f, 0.05f);
    EXPECT_NEAR(Bounds.Max.y, 0.5f, 0.05f);

    EXPECT_FALSE(Scene->GetBodyBounds(ECS::NullEntity, Bounds));
}

namespace
{
    // What a system draining the queues sees, checked against the character's own delegates.
    struct FCharacterEventCounter
    {
        ECS::TEventReader<SCharacterLandedEvent> LandedReader;
        ECS::TEventReader<SCharacterJumpedEvent> JumpedReader;
        int32 Landings = 0;
        int32 Jumps = 0;
        float LastImpactSpeed = 0.0f;
        int32 LastJumpCount = 0;

        void Drain(ECS::FEventQueues& Queues)
        {
            for (const SCharacterLandedEvent& Event : ECS::ReadEvents(Queues, LandedReader))
            {
                ++Landings;
                LastImpactSpeed = Event.ImpactSpeed;
            }
            for (const SCharacterJumpedEvent& Event : ECS::ReadEvents(Queues, JumpedReader))
            {
                ++Jumps;
                LastJumpCount = Event.JumpCount;
            }
        }
    };
}

TEST_F(PhysicsSceneLifecycle, CharacterLandingAndJumpReachTheDelegateAndTheQueue)
{
    SpawnFloor();

    const ECS::FEntity Character = Registry->Create();
    Registry->Emplace<STransformComponent>(Character).SetLocation(FVector3(0.0f, 3.0f, 0.0f));
    Registry->Emplace<SCharacterPhysicsComponent>(Character);
    SCharacterMovementComponent& Movement = Registry->Emplace<SCharacterMovementComponent>(Character);

    int32 DelegateLandings = 0;
    int32 DelegateJumps = 0;
    Movement.OnLanded.AddLambda([&](const SCharacterLandedEvent& Event)
    {
        ++DelegateLandings;
        EXPECT_EQ(Event.Entity, Character);
        EXPECT_GT(Event.GroundNormal.y, 0.9f);
    });
    Movement.OnJumped.AddLambda([&](const SCharacterJumpedEvent& Event)
    {
        ++DelegateJumps;
        EXPECT_EQ(Event.Entity, Character);
        EXPECT_GT(Event.Velocity.y, 0.0f);
    });

    FCharacterEventCounter Counter;
    auto Step = [&](int32 Steps)
    {
        for (int32 Index = 0; Index < Steps; ++Index)
        {
            Scene->Update(Dt);
            World->FlushCommands();
            Counter.Drain(World->GetEvents());
            World->GetEvents().Advance();
        }
    };

    Step(120);
    EXPECT_TRUE(Registry->Get<SCharacterMovementComponent>(Character).bGrounded);
    EXPECT_EQ(DelegateLandings, 1) << "falling three meters onto the floor is one landing";
    EXPECT_EQ(Counter.Landings, 1);
    EXPECT_GT(Counter.LastImpactSpeed, 1.0f) << "the fall carries real speed into the landing";
    EXPECT_EQ(Counter.Jumps, 0);

    Registry->Get<SCharacterMovementComponent>(Character).bPendingJump = true;
    Step(1);
    EXPECT_EQ(DelegateJumps, 1);
    EXPECT_EQ(Counter.Jumps, 1);
    EXPECT_EQ(Counter.LastJumpCount, 1) << "the jump off the ground is the first since landing";

    Step(120);
    EXPECT_EQ(DelegateLandings, 2) << "coming back down from the jump is the second landing";
    EXPECT_EQ(Counter.Landings, 2);
    EXPECT_EQ(Counter.Jumps, 1) << "standing still afterwards raises nothing further";
}

TEST_F(PhysicsSceneLifecycle, CharacterSpawnedOnTheFloorDoesNotReportALanding)
{
    SpawnFloor();

    const ECS::FEntity Character = Registry->Create();
    Registry->Emplace<STransformComponent>(Character).SetLocation(FVector3(0.0f, 0.0f, 0.0f));
    Registry->Emplace<SCharacterPhysicsComponent>(Character);
    Registry->Emplace<SCharacterMovementComponent>(Character);

    FCharacterEventCounter Counter;
    for (int32 Index = 0; Index < 60; ++Index)
    {
        Scene->Update(Dt);
        World->FlushCommands();
        Counter.Drain(World->GetEvents());
        World->GetEvents().Advance();
    }

    EXPECT_TRUE(Registry->Get<SCharacterMovementComponent>(Character).bGrounded);
    EXPECT_EQ(Counter.Landings, 0) << "a character placed on the ground never arrived there";
}

// A character the mover leaves standing inside a trigger stays overlapped until the trigger goes away.
TEST_F(PhysicsSceneLifecycle, ACharacterStandingInATriggerStaysOverlapped)
{
    SpawnFloor();

    for (const EBodyType TriggerType : { EBodyType::Static, EBodyType::Dynamic })
    {
        const ECS::FEntity Character = Registry->Create();
        Registry->Emplace<STransformComponent>(Character).SetLocation(FVector3(0.0f, 0.0f, 0.0f));
        Registry->Emplace<SCharacterPhysicsComponent>(Character);
        Registry->Emplace<SCharacterMovementComponent>(Character);

        ECS::TEventReader<SCollisionEvent> Reader;
        int32 Begins = 0;
        int32 Ends = 0;
        auto Step = [&](int32 Steps)
        {
            for (int32 Index = 0; Index < Steps; ++Index)
            {
                Scene->Update(Dt);
                Scene->DispatchPendingEvents();
                World->FlushCommands();
                for (const SCollisionEvent& Event : ECS::ReadEvents(World->GetEvents(), Reader))
                {
                    if (Event.Entity == Character)
                    {
                        Begins += Event.Kind == ECollisionEventKind::OverlapBegin;
                        Ends += Event.Kind == ECollisionEventKind::OverlapEnd;
                    }
                }
                World->GetEvents().Advance();
            }
        };
        Step(60);

        const FVector3 Standing = Registry->Get<SCharacterPhysicsComponent>(Character).Character->Position;
        const ECS::FEntity Trigger = Registry->Create();
        Registry->Emplace<STransformComponent>(Trigger).SetLocation(Standing);
        SRigidBodyComponent& Body = Registry->Emplace<SRigidBodyComponent>(Trigger);
        Body.BodyType = TriggerType;
        Body.bUseGravity = false;
        SSphereColliderComponent& Sphere = Registry->Emplace<SSphereColliderComponent>(Trigger);
        Sphere.Radius = 3.0f;
        Sphere.bIsTrigger = true;

        Step(60);
        EXPECT_EQ(Begins, 1) << "trigger body type " << (int)TriggerType;
        EXPECT_EQ(Ends, 0) << "the character never left the trigger, body type " << (int)TriggerType;
        EXPECT_NEAR(Math::Length(Registry->Get<SCharacterPhysicsComponent>(Character).Character->Position - Standing), 0.0f, 0.001f)
            << "a trigger never pushes the character out of itself";

        Registry->Destroy(Trigger);
        Step(2);
        EXPECT_EQ(Begins, 1);
        EXPECT_EQ(Ends, 1) << "destroying the trigger ends the overlap, body type " << (int)TriggerType;

        Registry->Destroy(Character);
    }
}

namespace
{
    // The lowest point of the character's capsule, which crouching and standing must leave where it is.
    float CapsuleBottom(const SCharacterPhysicsComponent& Physics)
    {
        const FPhysicsCharacterHandle& Character = *Physics.Character;
        return Character.Position.y + Character.TranslationOffset.y - Character.CrouchDrop - Character.HalfHeight - Character.Radius;
    }
}

// Crouching keeps the feet down, and standing waits under a low ceiling until the ceiling is gone.
TEST_F(PhysicsSceneLifecycle, CrouchKeepsTheFeetAndWaitsForRoomToStand)
{
    SpawnFloor();

    const ECS::FEntity Character = Registry->Create();
    Registry->Emplace<STransformComponent>(Character).SetLocation(FVector3(0.0f, 1.0f, 0.0f));
    Registry->Emplace<SCharacterPhysicsComponent>(Character);
    Registry->Emplace<SCharacterMovementComponent>(Character);

    int32 Crouches = 0;
    int32 Stands = 0;
    Registry->Get<SCharacterMovementComponent>(Character).OnCrouchChanged.AddLambda([&](const SCharacterCrouchEvent& Event)
    {
        EXPECT_EQ(Event.Entity, Character);
        (Event.bCrouched ? Crouches : Stands) += 1;
    });

    auto Step = [&](int32 Steps)
    {
        for (int32 Index = 0; Index < Steps; ++Index)
        {
            Scene->Update(Dt);
            World->FlushCommands();
            World->GetEvents().Advance();
        }
    };

    Step(90);
    const SCharacterPhysicsComponent& Physics = Registry->Get<SCharacterPhysicsComponent>(Character);
    ASSERT_TRUE(Physics.Character);
    const float Feet = CapsuleBottom(Physics);
    const float StandingTop = Feet + 2.0f * (Physics.HalfHeight + Physics.Radius);

    Registry->Get<SCharacterMovementComponent>(Character).bWantsToCrouch = true;
    Step(2);
    const SCharacterMovementComponent& Movement = Registry->Get<SCharacterMovementComponent>(Character);
    EXPECT_TRUE(Movement.bIsCrouched);
    EXPECT_FLOAT_EQ(Physics.Character->HalfHeight, Movement.CrouchHalfHeight);
    EXPECT_NEAR(CapsuleBottom(Physics), Feet, 0.01f) << "the capsule shrinks from the top, so the feet stay on the floor";
    EXPECT_EQ(Crouches, 1);

    // A ceiling between the crouched head and where a standing head would be.
    const float CrouchedTop = Feet + 2.0f * (Movement.CrouchHalfHeight + Physics.Radius);
    const ECS::FEntity Ceiling = Registry->Create();
    Registry->Emplace<STransformComponent>(Ceiling).SetLocation(FVector3(0.0f, (CrouchedTop + StandingTop) * 0.5f + 0.25f, 0.0f));
    Registry->Emplace<SRigidBodyComponent>(Ceiling).BodyType = EBodyType::Static;
    Registry->Emplace<SBoxColliderComponent>(Ceiling).HalfExtent = FVector3(5.0f, 0.25f, 5.0f);
    Step(2);

    Registry->Get<SCharacterMovementComponent>(Character).bWantsToCrouch = false;
    Step(10);
    EXPECT_TRUE(Movement.bIsCrouched) << "there is no room to stand under the ceiling";
    EXPECT_EQ(Stands, 0);

    Registry->Destroy(Ceiling);
    Step(10);
    EXPECT_FALSE(Movement.bIsCrouched) << "with the ceiling gone the character stands";
    EXPECT_FLOAT_EQ(Physics.Character->HalfHeight, Physics.HalfHeight);
    EXPECT_NEAR(CapsuleBottom(Physics), Feet, 0.01f);
    EXPECT_EQ(Stands, 1);
}

// A runtime resize reaches the running character and its proxy shape without rebuilding it.
TEST_F(PhysicsSceneLifecycle, ResizingACharacterAppliesInPlace)
{
    const ECS::FEntity Character = Registry->Create();
    Registry->Emplace<STransformComponent>(Character).SetLocation(FVector3(0.0f, 2.0f, 0.0f));
    Registry->Emplace<SCharacterPhysicsComponent>(Character);
    Registry->Emplace<SCharacterMovementComponent>(Character);
    Scene->Update(Dt);

    const SCharacterPhysicsComponent& Physics = Registry->Get<SCharacterPhysicsComponent>(Character);
    ASSERT_TRUE(Physics.Character);
    const FPhysicsCharacterHandle* Before = Physics.Character.Handle.get();

    World->GetPhysics()->SetCapsuleSize(Character, 0.5f, 0.8f);
    Scene->Update(Dt);

    EXPECT_EQ(Physics.Character.Handle.get(), Before) << "the same character carries on, only resized";
    EXPECT_FLOAT_EQ(Physics.Character->Radius, 0.5f);
    EXPECT_FLOAT_EQ(Physics.Character->HalfHeight, 0.8f);

    // Above the old capsule's top but inside the new one, so only the resized proxy can stop this ray.
    const FVector3 Center = Physics.Character->Position + Physics.Character->TranslationOffset;
    SRayCastSettings Query;
    Query.Start = Center + FVector3(-3.0f, 1.1f, 0.0f);
    Query.End   = Center + FVector3(3.0f, 1.1f, 0.0f);
    const TOptional<SRayResult> Hit = Scene->CastRay(Query);
    ASSERT_TRUE(Hit.IsSet()) << "the proxy body other queries see is still the old size";
    EXPECT_EQ(Hit->Entity, (uint32)Character);
}

// Rebuilding a moving body for a new size keeps it moving rather than stopping it dead.
TEST_F(PhysicsSceneLifecycle, ARebuiltBodyKeepsItsVelocity)
{
    const ECS::FEntity Ball = Spawn();
    Scene->Update(Dt);
    Scene->SetLinearVelocity(Ball, FVector3(3.0f, 0.0f, 0.0f));
    Scene->Update(Dt);

    World->GetPhysics()->SetSphereRadius(Ball, 1.0f);
    Scene->Update(Dt);

    EXPECT_NEAR(Scene->GetLinearVelocity(Ball).x, 3.0f, 0.05f);
}
