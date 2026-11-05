#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsScene.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

namespace
{
    void BindHierarchyTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        Transform.Bind(Registry, Entity);
        Transform.ResetDirtyState();
    }

    // A bare world with a physics scene, for bodies that sit under a parent.
    struct FPhysicsHierarchyWorld
    {
        CWorld* World = nullptr;

        // Released before the harness stops the task system, which the world's physics scene still runs on.
        TStrongObjectPtr<CWorld> WorldOwner;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }
        Physics::IPhysicsScene& Scene() const { return *World->GetPhysicsScene(); }

        FPhysicsHierarchyWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("PhysicsHierarchyTestWorld"), FGuid::New(), OF_Transient);
            WorldOwner = World;
            ECS::FRegistry& R = Registry();
            R.Ctx().Emplace<CWorld*>(World);
            R.GetSignals<STransformComponent>().OnConstruct.Connect<&BindHierarchyTransform>();

            // The engine starts physics at boot, and the test harness does not.
            if (Physics::GetPhysicsContext() == nullptr)
            {
                Physics::Initialize();
            }
            World->EnsurePhysicsScene();
        }

        ~FPhysicsHierarchyWorld()
        {
            WorldOwner.Reset();
        }

        ECS::FEntity Spawn(const FVector3& Location, ECS::FEntity Parent = ECS::NullEntity)
        {
            ECS::FRegistry& R = Registry();
            const ECS::FEntity Entity = R.Create();
            R.Emplace<STransformComponent>(Entity).SetLocation(Location);
            if (!Parent.IsNull())
            {
                ECS::Utils::ReparentEntity(R, Entity, Parent, false);
            }
            return Entity;
        }

        void AddBox(ECS::FEntity Entity, EBodyType Type)
        {
            Registry().Emplace<SRigidBodyComponent>(Entity).BodyType = Type;
            Registry().Emplace<SBoxColliderComponent>(Entity);
        }

        void Step(int32 Frames = 1)
        {
            for (int32 i = 0; i < Frames; ++i)
            {
                ECS::Utils::ResolveAllDirtyTransforms(Registry());
                Scene().Update(1.0 / 60.0);
                ECS::Utils::ResolveAllDirtyTransforms(Registry());
            }
        }

        bool HitsAt(ECS::FEntity Entity, float X)
        {
            return Scene().CastRayAgainst(Entity, FVector3(X, 5.0f, 0.0f), FVector3(X, -5.0f, 0.0f)).IsSet();
        }
    };
}

// A child's body sits where the child is in the world, not at its offset from the parent.
TEST(PhysicsHierarchy, AChildColliderIsPlacedByItsWorldTransform)
{
    FPhysicsHierarchyWorld W;
    const ECS::FEntity Parent = W.Spawn(FVector3(10.0f, 0.0f, 0.0f));
    const ECS::FEntity Child  = W.Spawn(FVector3(2.0f, 0.0f, 0.0f), Parent);
    W.AddBox(Child, EBodyType::Static);
    W.Step();

    EXPECT_TRUE(W.HitsAt(Child, 12.0f));
    EXPECT_FALSE(W.HitsAt(Child, 2.0f)) << "the parent-relative offset is not a world position";
}

// Moving a body-less parent carries the child's body with it.
TEST(PhysicsHierarchy, AChildColliderFollowsItsParent)
{
    FPhysicsHierarchyWorld W;
    const ECS::FEntity Parent = W.Spawn(FVector3(10.0f, 0.0f, 0.0f));
    const ECS::FEntity Child  = W.Spawn(FVector3(2.0f, 0.0f, 0.0f), Parent);
    W.AddBox(Child, EBodyType::Static);
    W.Step();

    W.Registry().Get<STransformComponent>(Parent).SetLocation(FVector3(20.0f, 0.0f, 0.0f));
    W.Step();

    EXPECT_TRUE(W.HitsAt(Child, 22.0f));
    EXPECT_FALSE(W.HitsAt(Child, 12.0f));
}

// A simulated child is written back relative to its parent, so its world pose is the body's.
TEST(PhysicsHierarchy, ASimulatedChildWritesBackItsLocalPose)
{
    FPhysicsHierarchyWorld W;
    const ECS::FEntity Parent = W.Spawn(FVector3(0.0f, 10.0f, 0.0f));
    const ECS::FEntity Child  = W.Spawn(FVector3(3.0f, 0.0f, 0.0f), Parent);
    W.AddBox(Child, EBodyType::Dynamic);
    W.Step(30);

    const FVector3 World = ECS::Utils::ComputeWorldTransform(W.Registry(), Child).GetLocation();
    EXPECT_NEAR(World.x, 3.0f, 1e-3f);
    EXPECT_LT(World.y, 10.0f) << "it falls from where it started";
    EXPECT_GT(World.y, 0.0f) << "not the world pose added onto the parent a second time";

    EXPECT_TRUE(W.Scene().CastRayAgainst(Child, FVector3(3.0f, World.y + 2.0f, 0.0f), FVector3(3.0f, World.y - 2.0f, 0.0f)).IsSet())
        << "the entity and its body agree";
}
