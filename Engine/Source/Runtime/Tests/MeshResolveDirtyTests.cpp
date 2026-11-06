#include <gtest/gtest.h>

#include <thread>

#include "Containers/Span.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"

using namespace Lumina;

namespace MeshResolveDirtyTestsInternal
{
    // A fresh queue starts with a sweep pending, which every test here clears so it sees only its own events.
    FMeshResolveQueue& MakeSettledQueue(ECS::FRegistry& Registry)
    {
        FMeshResolveQueue& Queue = FMeshResolveQueue::Ensure(Registry);
        Queue.BeginFrame();
        for (uint32 Source = 0; Source < (uint32)EMeshResolveSource::Num; ++Source)
        {
            (void)Queue.ConsumeFullSweep((EMeshResolveSource)Source);
            TVector<ECS::FEntity> Discard;
            Queue.TakeDirty((EMeshResolveSource)Source, Discard);
        }
        return Queue;
    }

    TVector<ECS::FEntity> TakeDirty(FMeshResolveQueue& Queue, EMeshResolveSource Source)
    {
        Queue.BeginFrame();
        TVector<ECS::FEntity> Out;
        Queue.TakeDirty(Source, Out);
        return Out;
    }
}

using namespace MeshResolveDirtyTestsInternal;

TEST(MeshResolveDirty, ConstructBindsTheComponentAndQueuesItOnce)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    const ECS::FEntity Entity = Registry.Create();
    SStaticMeshComponent& Component = Registry.Emplace<SStaticMeshComponent>(Entity);

    EXPECT_TRUE(Component.ResolveBinding.IsBound());
    EXPECT_EQ(Component.ResolveBinding.Entity, Entity);
    EXPECT_EQ(Component.CachedEntryState, MESH_RESOLVE_STATE_STALE);

    Component.MarkRenderStateDirty();
    Component.MarkRenderStateDirty();

    const TVector<ECS::FEntity> Dirty = TakeDirty(Queue, EMeshResolveSource::StaticMesh);
    ASSERT_EQ(Dirty.Num(), 1);
    EXPECT_EQ(Dirty[0], Entity);
}

TEST(MeshResolveDirty, AWriteAfterThePassReadsTheComponentQueuesAgain)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    const ECS::FEntity Entity = Registry.Create();
    SSkeletalMeshComponent& Component = Registry.Emplace<SSkeletalMeshComponent>(Entity);
    EXPECT_EQ(TakeDirty(Queue, EMeshResolveSource::SkeletalMesh).Num(), 1);

    Component.ResolveBinding.ClearQueued();
    Component.SetSkeletalMesh(nullptr);

    const TVector<ECS::FEntity> Dirty = TakeDirty(Queue, EMeshResolveSource::SkeletalMesh);
    ASSERT_EQ(Dirty.Num(), 1);
    EXPECT_EQ(Dirty[0], Entity);
}

TEST(MeshResolveDirty, ACopyIsUnboundAndQueuesNothing)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<SStaticMeshComponent>(Entity).ResolveBinding.ClearQueued();
    (void)TakeDirty(Queue, EMeshResolveSource::StaticMesh);

    SStaticMeshComponent Copy = Registry.Get<SStaticMeshComponent>(Entity);
    EXPECT_FALSE(Copy.ResolveBinding.IsBound());

    Copy.MarkRenderStateDirty();
    EXPECT_EQ(Copy.CachedEntryState, MESH_RESOLVE_STATE_STALE);
    EXPECT_TRUE(TakeDirty(Queue, EMeshResolveSource::StaticMesh).IsEmpty());
}

TEST(MeshResolveDirty, AssigningIntoAPooledComponentKeepsItsEntity)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    const ECS::FEntity Entity = Registry.Create();
    SStaticMeshComponent& Component = Registry.Emplace<SStaticMeshComponent>(Entity);
    Component.ResolveBinding.ClearQueued();
    (void)TakeDirty(Queue, EMeshResolveSource::StaticMesh);

    Component = SStaticMeshComponent{};
    EXPECT_EQ(Component.ResolveBinding.Entity, Entity);

    Component.MarkRenderStateDirty();
    const TVector<ECS::FEntity> Dirty = TakeDirty(Queue, EMeshResolveSource::StaticMesh);
    ASSERT_EQ(Dirty.Num(), 1);
    EXPECT_EQ(Dirty[0], Entity);
}

TEST(MeshResolveDirty, SwapAndPopRelocationKeepsTheMovedComponentsEntity)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    ECS::FEntity Entities[3];
    for (ECS::FEntity& Entity : Entities)
    {
        Entity = Registry.Create();
        Registry.Emplace<SStaticMeshComponent>(Entity).ResolveBinding.ClearQueued();
    }
    (void)TakeDirty(Queue, EMeshResolveSource::StaticMesh);

    Registry.Remove<SStaticMeshComponent>(Entities[0]);

    SStaticMeshComponent& Moved = Registry.Get<SStaticMeshComponent>(Entities[2]);
    EXPECT_EQ(Moved.ResolveBinding.Entity, Entities[2]);

    Moved.MarkRenderStateDirty();
    const TVector<ECS::FEntity> Dirty = TakeDirty(Queue, EMeshResolveSource::StaticMesh);
    ASSERT_EQ(Dirty.Num(), 1);
    EXPECT_EQ(Dirty[0], Entities[2]);
}

TEST(MeshResolveDirty, ReplacingAComponentRebindsAndQueuesIt)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<SStaticMeshComponent>(Entity).ResolveBinding.ClearQueued();
    (void)TakeDirty(Queue, EMeshResolveSource::StaticMesh);

    const SStaticMeshComponent Source{};
    SStaticMeshComponent& Replaced = Registry.EmplaceOrReplace<SStaticMeshComponent>(Entity, Source);
    EXPECT_TRUE(Replaced.ResolveBinding.IsBound());

    const TVector<ECS::FEntity> Dirty = TakeDirty(Queue, EMeshResolveSource::StaticMesh);
    ASSERT_EQ(Dirty.Num(), 1);
    EXPECT_EQ(Dirty[0], Entity);
}

TEST(MeshResolveDirty, ComponentsThatPredateTheQueueAreSwept)
{
    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    SStaticMeshComponent& Component = Registry.Emplace<SStaticMeshComponent>(Entity);
    EXPECT_FALSE(Component.ResolveBinding.IsBound());

    FMeshResolveQueue& Queue = FMeshResolveQueue::Ensure(Registry);
    Queue.BeginFrame();
    EXPECT_TRUE(Queue.ConsumeFullSweep(EMeshResolveSource::StaticMesh));
    EXPECT_FALSE(Queue.ConsumeFullSweep(EMeshResolveSource::StaticMesh));
}

TEST(MeshResolveDirty, AWakeReachesOnlyTheWatchersOfItsKey)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();
    const uint64 KeyA = FMeshResolveQueue::HandleKey(7);
    const uint64 KeyB = FMeshResolveQueue::HandleKey(8);

    Queue.Watch(A, EMeshResolveSource::StaticMesh, TSpan<const uint64>(&KeyA, 1));
    Queue.Watch(B, EMeshResolveSource::StaticMesh, TSpan<const uint64>(&KeyB, 1));

    Queue.Wake(KeyA);
    TVector<ECS::FEntity> Dirty = TakeDirty(Queue, EMeshResolveSource::StaticMesh);
    ASSERT_EQ(Dirty.Num(), 1);
    EXPECT_EQ(Dirty[0], A);

    Queue.Watch(A, EMeshResolveSource::StaticMesh, TSpan<const uint64>(&KeyB, 1));
    EXPECT_FALSE(Queue.IsWatching(A, EMeshResolveSource::StaticMesh, KeyA));
    EXPECT_TRUE(Queue.IsWatching(A, EMeshResolveSource::StaticMesh, KeyB));

    Queue.Wake(KeyA);
    EXPECT_TRUE(TakeDirty(Queue, EMeshResolveSource::StaticMesh).IsEmpty());

    Queue.Wake(KeyB);
    Dirty = TakeDirty(Queue, EMeshResolveSource::StaticMesh);
    EXPECT_EQ(Dirty.Num(), 2);
    EXPECT_TRUE(Dirty.Contains(A));
    EXPECT_TRUE(Dirty.Contains(B));
}

TEST(MeshResolveDirty, AnEntityWatchesManyKeysPerSource)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    const ECS::FEntity Entity = Registry.Create();
    const uint64 Keys[3] = { FMeshResolveQueue::HandleKey(1), FMeshResolveQueue::HandleKey(2), FMeshResolveQueue::HandleKey(3) };
    Queue.Watch(Entity, EMeshResolveSource::Foliage, Keys);

    for (uint64 Key : Keys)
    {
        EXPECT_TRUE(Queue.IsWatching(Entity, EMeshResolveSource::Foliage, Key));
        EXPECT_FALSE(Queue.IsWatching(Entity, EMeshResolveSource::StaticMesh, Key));
    }

    Queue.Wake(Keys[2]);
    EXPECT_EQ(TakeDirty(Queue, EMeshResolveSource::Foliage).Num(), 1);

    Queue.Watch(Entity, EMeshResolveSource::Foliage, {});
    for (uint64 Key : Keys)
    {
        EXPECT_FALSE(Queue.IsWatching(Entity, EMeshResolveSource::Foliage, Key));
    }
}

TEST(MeshResolveDirty, RemovingTheComponentStopsItsWatch)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<SStaticMeshComponent>(Entity);
    (void)TakeDirty(Queue, EMeshResolveSource::StaticMesh);

    const uint64 Key = FMeshResolveQueue::HandleKey(3);
    Queue.Watch(Entity, EMeshResolveSource::StaticMesh, TSpan<const uint64>(&Key, 1));

    Registry.Remove<SStaticMeshComponent>(Entity);
    EXPECT_FALSE(Queue.IsWatching(Entity, EMeshResolveSource::StaticMesh, Key));

    Queue.Wake(Key);
    EXPECT_TRUE(TakeDirty(Queue, EMeshResolveSource::StaticMesh).IsEmpty());
}

TEST(MeshResolveDirty, AnInvalidatedMaterialWakesItsReadersInEveryRegistry)
{
    ECS::FRegistry First;
    ECS::FRegistry Second;
    FMeshResolveQueue& FirstQueue  = MakeSettledQueue(First);
    FMeshResolveQueue& SecondQueue = MakeSettledQueue(Second);

    static int Material = 0;
    const uint64 Key = FMeshResolveQueue::DependencyKey(&Material);

    const ECS::FEntity InFirst  = First.Create();
    const ECS::FEntity InSecond = Second.Create();
    FirstQueue.Watch(InFirst, EMeshResolveSource::DynamicMesh, TSpan<const uint64>(&Key, 1));
    SecondQueue.Watch(InSecond, EMeshResolveSource::DynamicMesh, TSpan<const uint64>(&Key, 1));

    const uint32 PendingBefore = FMeshResolveCache::GetPendingGeneration();
    FMeshResolveCache::InvalidateDependency(&Material);
    EXPECT_NE(FMeshResolveCache::GetPendingGeneration(), PendingBefore);

    const TVector<ECS::FEntity> FirstDirty = TakeDirty(FirstQueue, EMeshResolveSource::DynamicMesh);
    const TVector<ECS::FEntity> SecondDirty = TakeDirty(SecondQueue, EMeshResolveSource::DynamicMesh);
    ASSERT_EQ(FirstDirty.Num(), 1);
    ASSERT_EQ(SecondDirty.Num(), 1);
    EXPECT_EQ(FirstDirty[0], InFirst);
    EXPECT_EQ(SecondDirty[0], InSecond);
}

TEST(MeshResolveDirty, AnEpochBumpSweepsEveryRegistry)
{
    ECS::FRegistry First;
    ECS::FRegistry Second;
    FMeshResolveQueue& FirstQueue  = MakeSettledQueue(First);
    FMeshResolveQueue& SecondQueue = MakeSettledQueue(Second);

    FirstQueue.BeginFrame();
    EXPECT_FALSE(FirstQueue.ConsumeFullSweep(EMeshResolveSource::StaticMesh));

    FMeshResolveCache::BumpEpoch();

    FirstQueue.BeginFrame();
    SecondQueue.BeginFrame();
    for (uint32 Source = 0; Source < (uint32)EMeshResolveSource::Num; ++Source)
    {
        EXPECT_TRUE(FirstQueue.ConsumeFullSweep((EMeshResolveSource)Source));
        EXPECT_TRUE(SecondQueue.ConsumeFullSweep((EMeshResolveSource)Source));
    }
}

TEST(MeshResolveDirty, MarkingFoliageStalesItsTypesAndQueuesTheEntity)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    const ECS::FEntity Entity = Registry.Create();
    SFoliageComponent& Foliage = Registry.Emplace<SFoliageComponent>(Entity);
    Foliage.Types.AddDefaulted();
    (void)TakeDirty(Queue, EMeshResolveSource::Foliage);

    Foliage.Types[0].CachedEntryState = MESH_RESOLVE_STATE_NO_MESH;
    FMeshResolveQueue::MarkFoliageDirty(Registry, Entity);

    EXPECT_EQ(Foliage.Types[0].CachedEntryState, MESH_RESOLVE_STATE_STALE);
    const TVector<ECS::FEntity> Dirty = TakeDirty(Queue, EMeshResolveSource::Foliage);
    ASSERT_EQ(Dirty.Num(), 1);
    EXPECT_EQ(Dirty[0], Entity);
}

TEST(MeshResolveDirty, DynamicMeshesQueueUnderTheirOwnSource)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<SDynamicMeshComponent>(Entity);

    EXPECT_TRUE(TakeDirty(Queue, EMeshResolveSource::StaticMesh).IsEmpty());
    const TVector<ECS::FEntity> Dirty = TakeDirty(Queue, EMeshResolveSource::DynamicMesh);
    ASSERT_EQ(Dirty.Num(), 1);
    EXPECT_EQ(Dirty[0], Entity);
}

TEST(MeshResolveDirty, WritesFromManyThreadsAreAllQueued)
{
    ECS::FRegistry Registry;
    FMeshResolveQueue& Queue = MakeSettledQueue(Registry);

    constexpr int32 kThreads = 4;
    constexpr int32 kPerThread = 512;

    TVector<ECS::FEntity> Entities;
    for (int32 i = 0; i < kThreads * kPerThread; ++i)
    {
        const ECS::FEntity Entity = Registry.Create();
        Registry.Emplace<SStaticMeshComponent>(Entity);
        Entities.Add(Entity);
    }

    TVector<SStaticMeshComponent*> Components;
    for (ECS::FEntity Entity : Entities)
    {
        SStaticMeshComponent& Component = Registry.Get<SStaticMeshComponent>(Entity);
        Component.ResolveBinding.ClearQueued();
        Components.Add(&Component);
    }
    (void)TakeDirty(Queue, EMeshResolveSource::StaticMesh);

    TVector<std::thread> Workers;
    for (int32 t = 0; t < kThreads; ++t)
    {
        Workers.Emplace([&Components, t]()
        {
            for (int32 i = 0; i < kPerThread; ++i)
            {
                SStaticMeshComponent& Component = *Components[t * kPerThread + i];
                Component.QueueRenderResolve();
                Component.QueueRenderResolve();
            }
        });
    }
    for (std::thread& Worker : Workers)
    {
        Worker.join();
    }

    EXPECT_EQ(TakeDirty(Queue, EMeshResolveSource::StaticMesh).Num(), kThreads * kPerThread);
}
