#include <gtest/gtest.h>
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/ObjectCore.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsScene.h"
#include "World/World.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Systems/FoliageCollisionSystem.h"
#include "WorldTestAccess.h"

namespace LuminaFoliageCollisionTests
{
    using namespace Lumina;

    struct FFoliageCollisionWorld
    {
        TStrongObjectPtr<CWorld>                  World;
        TStrongObjectPtr<CFoliageCollisionSystem> System;
        SMeshCollision                            Collision;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }
        Physics::IPhysicsScene& Scene() const { return *World->GetPhysicsScene(); }
        SExclusiveContext Context() const { return SExclusiveContext(FWorldTestAccess::SystemContext(*World)); }

        FFoliageCollisionWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("FoliageCollisionTestWorld"), FGuid::New(), OF_Transient);
            Registry().Ctx().Emplace<CWorld*>(World.Get());
            if (Physics::GetPhysicsContext() == nullptr)
            {
                Physics::Initialize();
            }
            World->EnsurePhysicsScene();
            System = NewObject<CFoliageCollisionSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System->SetOwningWorld(World.Get());
            System->OnStartup(Context());

            SCollisionPrimitive& Hull = Collision.Primitives.AddDefaultedGetRef();
            Hull.Type = ECollisionPrimitiveType::ConvexHull;
            Hull.HullPoints = { FVector3(0.0f), FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f), FVector3(0.0f, 0.0f, 1.0f) };
        }

        ~FFoliageCollisionWorld()
        {
            System->OnTeardown(Context());
        }

        // Stands in for the deferred bake, which needs a loaded mesh with meshlets.
        ECS::FEntity SpawnBakedFoliage(int32 InstanceCount)
        {
            const ECS::FEntity Entity = Registry().Create();
            TVector<Physics::FStaticInstanceDesc> Descs;
            for (int32 Index = 0; Index < InstanceCount; ++Index)
            {
                Physics::FStaticInstanceDesc& Desc = Descs.EmplaceGetRef();
                Desc.Position = FVector3(Index * 4.0f, 0.0f, 0.0f);
                Desc.Collision = &Collision;
            }
            Registry().Emplace<SFoliageComponent>(Entity).CollisionGroupID = Scene().CreateStaticBodyGroup(Entity, Descs);
            Scene().Update(1.0 / 60.0);
            return Entity;
        }
    };

    TEST(FoliageCollision, DestroyingTheFoliageEntityRemovesItsBodies)
    {
        FFoliageCollisionWorld W;
        const ECS::FEntity Foliage = W.SpawnBakedFoliage(3);
        ASSERT_EQ(W.Scene().GetBodyCount(), 3u);

        W.Registry().Destroy(Foliage);
        W.Scene().Update(1.0 / 60.0);
        EXPECT_EQ(W.Scene().GetBodyCount(), 0u);

        W.SpawnBakedFoliage(3);
        EXPECT_EQ(W.Scene().GetBodyCount(), 3u);
    }

    TEST(FoliageCollision, RemovingTheFoliageComponentRemovesItsBodies)
    {
        FFoliageCollisionWorld W;
        const ECS::FEntity Foliage = W.SpawnBakedFoliage(2);
        ASSERT_EQ(W.Scene().GetBodyCount(), 2u);

        W.Registry().Remove<SFoliageComponent>(Foliage);
        W.Scene().Update(1.0 / 60.0);
        EXPECT_EQ(W.Scene().GetBodyCount(), 0u);
    }
}
