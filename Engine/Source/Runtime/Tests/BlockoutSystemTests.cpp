#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

#include "Containers/Algorithm.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "World/World.h"
#include "World/Entity/Components/BlockoutComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Systems/BlockoutSystem.h"
#include "World/Subsystems/BlockoutMeshBuilder.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace Lumina
{
    // Reaches CBlockoutSystem's queuing pass and its result, with no Rebuild and so no need for a renderer.
    struct FBlockoutSystemTestAccess
    {
        static void GatherDirty(CBlockoutSystem& System) { System.GatherDirty(); }
        static const TVector<ECS::FEntity>& DirtyScratch(const CBlockoutSystem& System) { return System.DirtyScratch; }
    };
}

namespace
{
    struct FBlockoutTestWorld
    {
        CWorld*                  World  = nullptr;
        TStrongObjectPtr<CWorld> WorldOwner;
        CBlockoutSystem*         System = nullptr;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }

        FBlockoutTestWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("BlockoutSystemTestWorld"), FGuid::New(), OF_Transient);
            WorldOwner = World;
            Registry().Ctx().Emplace<CWorld*>(World);

            System = NewObject<CBlockoutSystem>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System->SetOwningWorld(World);
            System->ConfigureSystem();
        }

        ~FBlockoutTestWorld()
        {
            WorldOwner.Reset();
        }

        ECS::FEntity SpawnShape()
        {
            const ECS::FEntity Entity = Registry().Create();
            Registry().Emplace<SBlockoutComponent>(Entity);
            Registry().Emplace<SDynamicMeshComponent>(Entity);
            return Entity;
        }

        void AddCollider(ECS::FEntity Entity)
        {
            Registry().Emplace<SDynamicMeshColliderComponent>(Entity);
        }

        // Stands in for a prior frame's Rebuild with no renderer, built with no collider at build time.
        void SettleAsBuiltWithoutCollider(ECS::FEntity Entity)
        {
            SBlockoutComponent& Shape = Registry().Get<SBlockoutComponent>(Entity);
            Shape.bBuilt = true;
            Shape.BuiltHash = BlockoutMesh::HashParameters(Shape);
            Shape.BuiltMaterial = Shape.Material.Get();
            Registry().Get<SDynamicMeshComponent>(Entity).bKeepCPUMeshletData = false;
        }

        void GatherDirty()
        {
            FBlockoutSystemTestAccess::GatherDirty(*System);
        }

        const TVector<ECS::FEntity>& DirtyScratch() const
        {
            return FBlockoutSystemTestAccess::DirtyScratch(*System);
        }
    };
}

TEST(BlockoutSystem, UnbuiltShapeIsQueued)
{
    FBlockoutTestWorld W;
    const ECS::FEntity Entity = W.SpawnShape();

    W.GatherDirty();

    EXPECT_EQ(W.DirtyScratch().Num(), 1);
    EXPECT_TRUE(W.DirtyScratch().Contains(Entity));
}

TEST(BlockoutSystem, ColliderAddedAfterTheBuildQueuesExactlyOnce)
{
    FBlockoutTestWorld W;
    const ECS::FEntity Entity = W.SpawnShape();
    W.GatherDirty();
    W.SettleAsBuiltWithoutCollider(Entity);

    W.AddCollider(Entity);
    W.GatherDirty();

    EXPECT_EQ(W.DirtyScratch().Num(), 1) << "the collider must queue the shape exactly once";
    EXPECT_TRUE(W.DirtyScratch().Contains(Entity));
}

TEST(BlockoutSystem, ShapeChangedAndColliderAddedInTheSamePassStillQueuesOnce)
{
    FBlockoutTestWorld W;
    const ECS::FEntity Entity = W.SpawnShape();
    W.GatherDirty();
    W.SettleAsBuiltWithoutCollider(Entity);

    // The resize marks it via ChangedShapes, the new collider marks it via NewColliders, same pass.
    W.Registry().Get<SBlockoutComponent>(Entity).Size.y += 1.0f;
    W.AddCollider(Entity);
    W.GatherDirty();

    EXPECT_EQ(W.DirtyScratch().Num(), 1) << "a shape touched by both paths in the same pass must still be queued only once";
    EXPECT_TRUE(W.DirtyScratch().Contains(Entity));
}

TEST(BlockoutSystem, SettledShapeIsNeverRequeued)
{
    FBlockoutTestWorld W;
    const ECS::FEntity Settled = W.SpawnShape();
    W.GatherDirty();
    W.SettleAsBuiltWithoutCollider(Settled);

    for (int32 Index = 0; Index < 8; ++Index)
    {
        W.SpawnShape();
    }
    W.GatherDirty();

    // The settled shape gets neither a resize nor a collider this pass, so it must not reappear.
    EXPECT_FALSE(W.DirtyScratch().Contains(Settled));
}

namespace
{
    // Settles Count shapes with no collider, the starting point every scan-cost case below shares.
    TVector<ECS::FEntity> SettleShapes(FBlockoutTestWorld& W, int32 Count)
    {
        TVector<ECS::FEntity> Entities;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Entities.Add(W.SpawnShape());
        }
        W.GatherDirty();
        for (const ECS::FEntity Entity : Entities)
        {
            W.SettleAsBuiltWithoutCollider(Entity);
        }
        return Entities;
    }

    // Count freshly dirtied shapes plus Count settled shapes newly given a collider, each a membership test.
    double TimeMixedPass(int32 Count)
    {
        FBlockoutTestWorld W;
        const TVector<ECS::FEntity> Settled = SettleShapes(W, Count);
        for (const ECS::FEntity Entity : Settled)
        {
            W.AddCollider(Entity);
        }
        for (int32 Index = 0; Index < Count; ++Index)
        {
            W.SpawnShape();
        }

        const auto T0 = std::chrono::high_resolution_clock::now();
        W.GatherDirty();
        const auto T1 = std::chrono::high_resolution_clock::now();

        EXPECT_EQ(W.DirtyScratch().Num(), Count * 2);
        return std::chrono::duration<double, std::milli>(T1 - T0).count();
    }

    double MedianOf(int32 Count, int32 Repeats)
    {
        TVector<double> Samples;
        for (int32 Run = 0; Run < Repeats; ++Run)
        {
            Samples.Add(TimeMixedPass(Count));
        }
        Algo::Sort(Samples.begin(), Samples.end());
        return Samples[Samples.Num() / 2];
    }
}

// A quadratic Contains scan turns a 10x bigger pass into roughly 100x, not the 10x a flat scan costs.
TEST(BlockoutSystem, NewColliderScanStaysFlatAsDirtyScratchGrows)
{
    constexpr int32 kSmall = 2000;
    constexpr int32 kLarge = 20000;

    const double SmallMs = MedianOf(kSmall, 5);
    const double LargeMs = MedianOf(kLarge, 3);
    std::printf("[bench] BlockoutSystem dirty pass, %d entities %.3f ms, %d entities %.3f ms\n",
        kSmall * 2, SmallMs, kLarge * 2, LargeMs);

    const double Ratio = LargeMs / Math::Max(SmallMs, 0.001);
    EXPECT_LT(Ratio, 40.0) << "a 10x bigger pass costing anywhere near 100x longer points at a reintroduced scan";
}
