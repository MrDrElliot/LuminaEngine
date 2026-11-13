#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "World/ECS/Registry.h"

using namespace Lumina;

TEST(RegistryHintConcurrencyTests, CreateWithHintFromManyThreadsLeavesConsistentFreeList)
{
    constexpr uint32 kThreads = 8;
    constexpr uint32 kTotal = kThreads * 512u;
    constexpr int kRounds = 4;

    ECS::FRegistry Registry{};

    TVector<ECS::FEntity> Created;
    Created.Reserve(kTotal);
    for (uint32 Index = 0; Index < kTotal; ++Index)
    {
        Created.Add(Registry.Create());
    }

    for (int Round = 0; Round < kRounds; ++Round)
    {
        for (const ECS::FEntity Entity : Created)
        {
            Registry.Destroy(Entity);
        }

        std::atomic<bool> bGo = false;
        std::thread Workers[kThreads];
        for (uint32 ThreadIndex = 0; ThreadIndex < kThreads; ++ThreadIndex)
        {
            Workers[ThreadIndex] = std::thread([&Registry, &Created, &bGo, ThreadIndex]
            {
                while (!bGo.load(std::memory_order_acquire)) {}
                for (uint32 Index = ThreadIndex; Index < kTotal; Index += kThreads)
                {
                    (void)Registry.Create(Created[Index]);
                }
            });
        }
        bGo.store(true, std::memory_order_release);
        for (std::thread& Worker : Workers)
        {
            Worker.join();
        }

        for (const ECS::FEntity Entity : Created)
        {
            ASSERT_TRUE(Registry.IsValid(Entity));
        }
    }

    // Every freed slot was reclaimed by hint, so a torn free chain shows up as a live slot handed out again.
    EXPECT_EQ(Registry.Create().GetIndex(), kTotal);
}
