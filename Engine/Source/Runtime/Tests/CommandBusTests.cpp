#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "Containers/HashTable.h"
#include "Core/Threading/Thread.h"
#include "TaskSystem/TaskSystem.h"
#include "World/ECS/CommandBus.h"
#include "World/ECS/Registry.h"

using namespace Lumina;

namespace
{
    struct FBusValue
    {
        int32 Value = 0;
    };

    struct FBusTag {};

    void CountCreated(void* Payload, ECS::FRegistry&, ECS::FEntity)
    {
        ++*static_cast<int32*>(Payload);
    }
}

TEST(CommandBus, ReservedEntityBecomesLiveAtFlush)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);

    int32 Created = 0;
    Registry.OnEntityCreated().Connect(&CountCreated, &Created);

    const ECS::FEntity Entity = Bus.Create();
    Bus.Emplace<FBusValue>(Entity, FBusValue{ 7 });
    EXPECT_FALSE(Registry.IsValid(Entity));
    EXPECT_EQ(Registry.NumEntities(), 0);

    Bus.Flush();

    ASSERT_TRUE(Registry.IsValid(Entity));
    EXPECT_EQ(Created, 1);
    EXPECT_EQ(Registry.Get<FBusValue>(Entity).Value, 7);
    EXPECT_FALSE(Bus.HasPending());
}

TEST(CommandBus, ImmediateCreateNeverReusesAnOutstandingReservation)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);

    const ECS::FEntity Reserved = Bus.Create();
    const ECS::FEntity Immediate = Registry.Create();
    EXPECT_NE(Reserved.GetIndex(), Immediate.GetIndex());

    Bus.Flush();
    EXPECT_TRUE(Registry.IsValid(Reserved));
    EXPECT_TRUE(Registry.IsValid(Immediate));
    EXPECT_EQ(Registry.NumEntities(), 2);
}

TEST(CommandBus, ReservationReusesAFreedSlotWithANewVersion)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);

    const ECS::FEntity First = Registry.Create();
    Registry.Destroy(First);

    const ECS::FEntity Reserved = Bus.Create();
    EXPECT_EQ(Reserved.GetIndex(), First.GetIndex());
    EXPECT_NE(Reserved, First);
    EXPECT_FALSE(Registry.IsValid(First));

    Bus.Flush();
    EXPECT_TRUE(Registry.IsValid(Reserved));
    EXPECT_FALSE(Registry.IsValid(First));
}

TEST(CommandBus, DestroyRunsAfterCommandsRecordedBeforeAndAfterIt)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);

    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<FBusValue>(Entity, FBusValue{ 1 });

    int32 Seen = 0;
    Bus.Destroy(Entity);
    Bus.Enqueue([&Seen, Entity](ECS::FRegistry& InRegistry)
    {
        Seen = InRegistry.IsValid(Entity) ? InRegistry.Get<FBusValue>(Entity).Value : -1;
    });

    Bus.Flush();
    EXPECT_EQ(Seen, 1);
    EXPECT_FALSE(Registry.IsValid(Entity));
}

TEST(CommandBus, PatchAndRemoveSkipEntitiesThatLostTheComponent)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);

    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<FBusValue>(Entity, FBusValue{ 2 });
    Registry.Emplace<FBusTag>(Entity);

    Bus.Patch<FBusValue>(Entity, [](FBusValue& Value) { Value.Value *= 10; });
    Bus.Remove<FBusTag>(Entity);
    Bus.Remove<FBusValue>(Entity);
    Bus.Patch<FBusValue>(Entity, [](FBusValue& Value) { Value.Value = -1; });
    Bus.Flush();

    EXPECT_FALSE(Registry.HasAny<FBusValue>(Entity));
    EXPECT_FALSE(Registry.HasAny<FBusTag>(Entity));
    EXPECT_TRUE(Registry.IsValid(Entity));
}

TEST(CommandBus, CommandsRecordedWhileFlushingRunInTheSameFlush)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);

    int32 Order = 0;
    int32 Inner = 0;
    Bus.Enqueue([&]()
    {
        Bus.Enqueue([&]() { Inner = ++Order; });
        ++Order;
    });

    Bus.Flush();
    EXPECT_EQ(Inner, 2);
    EXPECT_FALSE(Bus.HasPending());
}

TEST(CommandBus, DiscardReturnsReservationsAndDropsCommands)
{
    ECS::FRegistry Registry;
    int32 Ran = 0;
    ECS::FEntity Reserved;
    {
        ECS::FCommandBus Bus(Registry);
        Reserved = Bus.Create();
        Bus.Enqueue([&Ran]() { ++Ran; });
        Bus.Discard();
        EXPECT_FALSE(Bus.HasPending());
    }

    EXPECT_EQ(Ran, 0);
    EXPECT_EQ(Registry.NumEntities(), 0);

    const ECS::FEntity Next = Registry.Create();
    EXPECT_EQ(Next.GetIndex(), Reserved.GetIndex());
}

TEST(CommandBus, ManyThreadsRecordCreatesAndComponentsTogether)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);

    constexpr uint32 Count = 20000;
    TVector<ECS::FEntity> Entities(Count);

    Jobs::ParallelFor(Count, [&](uint32 Index)
    {
        const ECS::FEntity Entity = Bus.Create();
        Entities[Index] = Entity;
        Bus.Emplace<FBusValue>(Entity, FBusValue{ static_cast<int32>(Index) });
    }, 64);

    Bus.Flush();

    ASSERT_EQ(Registry.NumEntities(), Count);
    THashSet<uint32> Indices;
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        const ECS::FEntity Entity = Entities[Index];
        ASSERT_TRUE(Registry.IsValid(Entity));
        EXPECT_EQ(Registry.Get<FBusValue>(Entity).Value, static_cast<int32>(Index));
        Indices.Add(Entity.GetIndex());
    }
    EXPECT_EQ(Indices.Num(), Count);
}

TEST(CommandBus, WorkerThreadsAlwaysDefer)
{
    std::atomic<uint32> WorkerItems{ 0 };
    std::atomic<uint32> Deferring{ 0 };
    Jobs::ParallelFor(64, [&](uint32)
    {
        // The caller could otherwise drain every item itself, so it holds here until a worker has run one.
        if (Threading::IsMainThread())
        {
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (WorkerItems.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < Deadline)
            {
                std::this_thread::yield();
            }
            return;
        }

        WorkerItems.fetch_add(1, std::memory_order_release);
        if (ECS::FCommandBus::ShouldDefer())
        {
            Deferring.fetch_add(1, std::memory_order_relaxed);
        }
    }, 1);

    ASSERT_GT(WorkerItems.load(), 0u);
    EXPECT_EQ(Deferring.load(), WorkerItems.load());
    EXPECT_FALSE(ECS::FCommandBus::ShouldDefer());
    {
        const ECS::FCommandBus::FDeferScope Scope(true);
        EXPECT_TRUE(ECS::FCommandBus::ShouldDefer());
    }
}
