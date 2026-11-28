#include <gtest/gtest.h>
#include "Core/Object/ObjectCore.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsScene.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Systems/TerrainCollisionSystem.h"
#include "World/Subsystems/PhysicsSubsystem.h"
#include "WorldTestAccess.h"

#include <cstdio>
#include <thread>

namespace LuminaTerrainCollisionTests
{
    using namespace Lumina;

    void BindTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
        Transform.Bind(Registry, Entity);
        Transform.ResetDirtyState();
    }

    // A tilted plane, which every triangle split reproduces exactly, so a hit anywhere can be checked.
    float PlaneHeight(int32 X, int32 Z, int32 Resolution)
    {
        return 0.3f + 0.2f * float(X) / float(Resolution - 1) + 0.1f * float(Z) / float(Resolution - 1);
    }

    struct FTerrainCollisionWorld
    {
        TStrongObjectPtr<CWorld>                  World;
        TStrongObjectPtr<CTerrainCollisionSystem> System;
        FVector3 TerrainCorner = FVector3(0.0f);
        float    Spacing       = 1.0f;
        float    MaxHeight     = 100.0f;
        int32    Resolution    = 0;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }
        Physics::IPhysicsScene& Scene() const { return *World->GetPhysicsScene(); }
        SExclusiveContext Context() const { return SExclusiveContext(FWorldTestAccess::SystemContext(*World)); }

        explicit FTerrainCollisionWorld(bool bPhysicsScene = true)
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("TerrainCollisionTestWorld"), FGuid::New(), OF_Transient);
            Registry().Ctx().Emplace<CWorld*>(World.Get());
            Registry().GetSignals<STransformComponent>().OnConstruct.Connect<&BindTransform>();
            if (Physics::GetPhysicsContext() == nullptr)
            {
                Physics::Initialize();
            }
            if (bPhysicsScene)
            {
                World->EnsurePhysicsScene();
            }
            System = NewObject<CTerrainCollisionSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System->SetOwningWorld(World.Get());
            System->OnStartup(Context());
        }

        ~FTerrainCollisionWorld()
        {
            System->OnTeardown(Context());
        }

        // Placed far from the origin, where the test world's camera sits, so only bodies stream it.
        ECS::FEntity SpawnTerrain(int32 InResolution, const FVector3& Center = FVector3(20000.0f, 0.0f, 0.0f))
        {
            Resolution = InResolution;
            Spacing    = 1.0f;
            const float Size = Spacing * float(Resolution - 1);
            TerrainCorner = Center - FVector3(Size * 0.5f, 0.0f, Size * 0.5f);

            TVector<float> Heights(Resolution * Resolution);
            for (int32 Z = 0; Z < Resolution; ++Z)
            {
                for (int32 X = 0; X < Resolution; ++X)
                {
                    Heights[Z * Resolution + X] = PlaneHeight(X, Z, Resolution);
                }
            }

            const ECS::FEntity Entity = Registry().Create();
            Registry().Emplace<STransformComponent>(Entity).SetLocalLocation(Center);
            STerrainComponent Terrain;
            Terrain.Resolution    = Resolution;
            Terrain.TileWorldSize = Size;
            Terrain.MaxHeight     = MaxHeight;
            Terrain.SetStore(FTerrainStore::CreateFromArrays(Resolution, Heights, {}, 0));
            Registry().Emplace<STerrainComponent>(Entity, Move(Terrain));
            Registry().Emplace<STerrainColliderComponent>(Entity);
            return Entity;
        }

        ECS::FEntity SpawnMover(const FVector3& At)
        {
            const ECS::FEntity Entity = Registry().Create();
            Registry().Emplace<STransformComponent>(Entity).SetLocalLocation(At);
            Registry().Emplace<SRigidBodyComponent>(Entity).BodyType = EBodyType::Kinematic;
            return Entity;
        }

        void Step()
        {
            ECS::Utils::ResolveAllDirtyTransforms(Registry());
            System->OnExclusiveUpdate(Context());
            Scene().Update(1.0 / 60.0);
            World->FlushCommands();
        }

        // Steps until no patch is cooking and the last cooks have their bodies.
        void Settle(int32 MinFrames = 1)
        {
            for (int32 Frame = 0; Frame < 4000; ++Frame)
            {
                Step();
                if (Frame + 1 >= MinFrames && !System->HasPendingBuilds())
                {
                    Step();
                    return;
                }
                std::this_thread::yield();
            }
            ADD_FAILURE() << "terrain collision never settled";
        }

        FVector3 SampleWorld(int32 X, int32 Z) const
        {
            return TerrainCorner + FVector3(float(X) * Spacing, 0.0f, float(Z) * Spacing);
        }

        TOptional<float> GroundAt(float X, float Z)
        {
            SRayCastSettings Ray;
            Ray.Start = FVector3(X, 1000.0f, Z);
            Ray.End   = FVector3(X, -1000.0f, Z);
            TOptional<SRayResult> Hit = Scene().CastRay(Ray);
            if (!Hit.IsSet() || !Hit->bHit)
            {
                return {};
            }
            return Hit->Location.y;
        }

        // The plane's height at a point given in samples.
        float Expected(float SX, float SZ) const
        {
            return MaxHeight * (0.3f + 0.2f * SX / float(Resolution - 1) + 0.1f * SZ / float(Resolution - 1));
        }
    };

    constexpr float kQuantization = 0.05f;

    TEST(TerrainCollision, SmallTerrainKeepsEveryPatchAndSeamsMeet)
    {
        FTerrainCollisionWorld W;
        W.SpawnTerrain(513);
        W.Settle();

        EXPECT_EQ(W.System->GetResidentPatchCount(), 16);
        EXPECT_EQ(W.Scene().GetBodyCount(), 16u);

        // On a seam between four patches, inside one, and on the map's far corner.
        const float Points[][2] = { { 128.0f, 256.0f }, { 37.5f, 401.25f }, { 511.5f, 511.5f } };
        for (const auto& Point : Points)
        {
            const FVector3 At = W.SampleWorld(0, 0) + FVector3(Point[0], 0.0f, Point[1]);
            const TOptional<float> Ground = W.GroundAt(At.x, At.z);
            ASSERT_TRUE(Ground.IsSet()) << Point[0] << "," << Point[1];
            EXPECT_NEAR(*Ground, W.Expected(Point[0], Point[1]), kQuantization) << Point[0] << "," << Point[1];
        }
        EXPECT_FALSE(W.GroundAt(W.SampleWorld(0, 0).x - 4.0f, W.SampleWorld(0, 0).z + 10.0f).IsSet());
    }

    TEST(TerrainCollision, LargeTerrainStreamsAroundAMovingBody)
    {
        FTerrainCollisionWorld W;
        W.SpawnTerrain(4097);
        const FVector3 Start = W.SampleWorld(1000, 1000) + FVector3(0.0f, 300.0f, 0.0f);
        const ECS::FEntity Mover = W.SpawnMover(Start);
        W.Settle();

        const int32 NearStart = W.System->GetResidentPatchCount();
        EXPECT_GE(NearStart, 1);
        EXPECT_LE(NearStart, 4);
        ASSERT_TRUE(W.GroundAt(Start.x, Start.z).IsSet());
        EXPECT_NEAR(*W.GroundAt(Start.x, Start.z), W.Expected(1000.0f, 1000.0f), kQuantization);

        const FVector3 Far = W.SampleWorld(3500, 300) + FVector3(0.0f, 300.0f, 0.0f);
        EXPECT_FALSE(W.GroundAt(Far.x, Far.z).IsSet());

        W.Registry().Get<STransformComponent>(Mover).SetLocalLocation(Far);
        ECS::Utils::MarkTransformDirty(W.Registry(), Mover);
        // Long enough for the patches left behind to be given back.
        W.Settle(200);

        ASSERT_TRUE(W.GroundAt(Far.x, Far.z).IsSet());
        EXPECT_NEAR(*W.GroundAt(Far.x, Far.z), W.Expected(3500.0f, 300.0f), kQuantization);
        EXPECT_FALSE(W.GroundAt(Start.x, Start.z).IsSet());
        EXPECT_LE(W.System->GetResidentPatchCount(), 4);
    }

    TEST(TerrainCollision, AnEditRebuildsTheCollisionUnderIt)
    {
        FTerrainCollisionWorld W;
        const ECS::FEntity Entity = W.SpawnTerrain(513);
        W.Settle();

        // A raised square straddling a patch seam.
        STerrainComponent& Terrain = W.Registry().Get<STerrainComponent>(Entity);
        const FIntVector2 Min(120, 200), Size(16, 16);
        TVector<float> Raised(Size.x * Size.y, 0.9f);
        Terrain.EnsureStore()->WriteHeights(Min, Size, Raised.GetData());
        Terrain.MarkHeightDirty(Min, Min + Size - FIntVector2(1));
        W.Settle(3);

        const FVector3 Inside = W.SampleWorld(127, 207) + FVector3(0.5f, 0.0f, 0.5f);
        ASSERT_TRUE(W.GroundAt(Inside.x, Inside.z).IsSet());
        EXPECT_NEAR(*W.GroundAt(Inside.x, Inside.z), 0.9f * W.MaxHeight, kQuantization);
        const FVector3 Outside = W.SampleWorld(300, 300) + FVector3(0.5f, 0.0f, 0.5f);
        EXPECT_NEAR(*W.GroundAt(Outside.x, Outside.z), W.Expected(300.5f, 300.5f), kQuantization);
        EXPECT_EQ(W.System->GetResidentPatchCount(), 16);
    }

    TEST(TerrainCollision, ADroppedBoxRestsOnAPatch)
    {
        FTerrainCollisionWorld W;
        W.SpawnTerrain(513);
        W.Settle();

        W.Scene().Simulate();
        const FVector3 Drop = W.SampleWorld(200, 300) + FVector3(0.0f, W.Expected(200.0f, 300.0f) + 1.5f, 0.0f);
        const ECS::FEntity Box = W.Registry().Create();
        W.Registry().Emplace<STransformComponent>(Box).SetLocalLocation(Drop);
        // Dead and grippy, so it lands where it fell instead of tumbling out from under the probe.
        SRigidBodyComponent& Body = W.Registry().Emplace<SRigidBodyComponent>(Box);
        Body.BodyType           = EBodyType::Dynamic;
        Body.RestitutionOverride = 0.0f;
        Body.FrictionOverride    = 1.0f;
        W.Registry().Emplace<SBoxColliderComponent>(Box).HalfExtent = FVector3(0.5f);
        for (int32 Frame = 0; Frame < 240; ++Frame)
        {
            W.Step();
        }

        // The box's top, found by a ray from above, since the bare scene writes no poses back to transforms.
        EXPECT_EQ(W.Scene().GetBodyCount(), 17u);
        const TOptional<float> Top = W.GroundAt(Drop.x, Drop.z);
        ASSERT_TRUE(Top.IsSet());
        EXPECT_NEAR(*Top, W.Expected(200.0f, 300.0f) + 1.0f, 0.3f) << "the box fell through the patch";
        W.Scene().StopSimulate();
    }
}

namespace LuminaTerrainCollisionTests
{
    TEST(TerrainCollision, GroundQueriesMeetTerrainWhoseCollisionHasNotStreamedIn)
    {
        FTerrainCollisionWorld W;
        const ECS::FEntity Terrain = W.SpawnTerrain(4097);
        W.Settle();
        ASSERT_EQ(W.System->GetResidentPatchCount(), 0) << "nothing should have asked for collision yet";

        const FVector3 Far = W.SampleWorld(3000, 700) + FVector3(0.25f, 0.0f, 0.75f);
        EXPECT_FALSE(W.GroundAt(Far.x, Far.z).IsSet());

        const float Ground = W.World->GetPhysics()->GetGroundHeight(FVector3(Far.x, 500.0f, Far.z), 1000.0f, ECS::NullEntity, -1.0f);
        EXPECT_NEAR(Ground, W.Expected(3000.25f, 700.75f), kQuantization);

        const SRayResult Hit = W.World->GetPhysics()->Raycast(FVector3(Far.x, 500.0f, Far.z), FVector3(Far.x, -500.0f, Far.z), ECS::NullEntity);
        ASSERT_TRUE(Hit.bHit);
        EXPECT_EQ(Hit.Entity, Terrain.Value);
        EXPECT_GT(Hit.Normal.y, 0.9f);
        EXPECT_NEAR(Hit.Fraction, (500.0f - Hit.Location.y) / 1000.0f, 1e-3f);

        // Ignoring the terrain, or asking only for dynamic bodies, finds nothing there.
        EXPECT_FALSE(W.World->GetPhysics()->Raycast(FVector3(Far.x, 500.0f, Far.z), FVector3(Far.x, -500.0f, Far.z), Terrain).bHit);
        EXPECT_FALSE(W.World->GetPhysics()->Raycast(FVector3(Far.x, 500.0f, Far.z), FVector3(Far.x, -500.0f, Far.z), ECS::NullEntity, ECollisionProfiles::Dynamic).bHit);
        // A segment that stops short of the ground misses.
        EXPECT_FALSE(W.World->GetPhysics()->Raycast(FVector3(Far.x, 500.0f, Far.z), FVector3(Far.x, 200.0f, Far.z), ECS::NullEntity).bHit);
    }

    TEST(TerrainCollision, AWorldWithoutPhysicsStillFindsTheGround)
    {
        FTerrainCollisionWorld W(false);
        W.SpawnTerrain(513);
        ECS::Utils::ResolveAllDirtyTransforms(W.Registry());
        const FVector3 At = W.SampleWorld(100, 400) + FVector3(0.5f, 0.0f, 0.5f);
        const float Ground = W.World->GetPhysics()->GetGroundHeight(FVector3(At.x, 500.0f, At.z), 1000.0f, ECS::NullEntity, -1.0f);
        EXPECT_NEAR(Ground, W.Expected(100.5f, 400.5f), kQuantization);
    }
}
