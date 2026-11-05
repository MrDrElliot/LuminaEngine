#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "Core/Threading/Atomic.h"
#include "Core/Threading/Sync.h"
#include "Core/Threading/Thread.h"

namespace LuminaRecursiveMutexTests
{
    using namespace Lumina;

    TEST(RecursiveMutex, ReentersOnTheOwningThread)
    {
        FRecursiveMutex Mutex;

        Mutex.Lock();
        EXPECT_TRUE(Mutex.TryLock());
        Mutex.Lock();
        Mutex.Unlock();
        Mutex.Unlock();
        Mutex.Unlock();

        EXPECT_TRUE(Mutex.TryLock());
        Mutex.Unlock();
    }

    TEST(RecursiveMutex, TryLockFailsOnAnotherThreadWhileHeld)
    {
        FRecursiveMutex Mutex;
        bool bAcquired = true;

        Mutex.Lock();
        Mutex.Lock();

        std::thread Other([&] { bAcquired = Mutex.TryLock(); });
        Other.join();

        EXPECT_FALSE(bAcquired);
        Mutex.Unlock();
        Mutex.Unlock();
    }

    TEST(RecursiveMutex, ReleasedOnlyAfterTheOutermostUnlock)
    {
        FRecursiveMutex Mutex;

        Mutex.Lock();
        Mutex.Lock();
        Mutex.Unlock();

        bool bWhileHeld = true;
        std::thread Held([&] { bWhileHeld = Mutex.TryLock(); });
        Held.join();
        EXPECT_FALSE(bWhileHeld);

        Mutex.Unlock();

        bool bAfterRelease = false;
        std::thread Free([&]
        {
            bAfterRelease = Mutex.TryLock();
            if (bAfterRelease)
            {
                Mutex.Unlock();
            }
        });
        Free.join();
        EXPECT_TRUE(bAfterRelease);
    }

    TEST(RecursiveMutex, ExcludesContendingThreads)
    {
        constexpr uint32 kThreads = 8;
        constexpr uint32 kIterations = 20000;

        FRecursiveMutex Mutex;
        TAtomic<int32> Inside{0};
        TAtomic<int32> MaxInside{0};
        uint64 Counter = 0;

        std::thread Threads[kThreads];
        for (std::thread& Thread : Threads)
        {
            Thread = std::thread([&]
            {
                for (uint32 Index = 0; Index < kIterations; ++Index)
                {
                    FRecursiveScopeLock Outer(Mutex);
                    FRecursiveScopeLock Inner(Mutex);

                    const int32 Now = Inside.fetch_add(1, std::memory_order_acq_rel) + 1;
                    for (int32 Seen = MaxInside.load(); Now > Seen && !MaxInside.compare_exchange_weak(Seen, Now);) {}

                    ++Counter;
                    Inside.fetch_sub(1, std::memory_order_acq_rel);
                }
            });
        }

        for (std::thread& Thread : Threads)
        {
            Thread.join();
        }

        EXPECT_EQ(MaxInside.load(), 1);
        EXPECT_EQ(Counter, static_cast<uint64>(kThreads) * kIterations);
    }

    TEST(RecursiveMutex, BlockedThreadAcquiresAfterRelease)
    {
        FRecursiveMutex Mutex;
        TAtomic<uint32> Entered{0};

        Mutex.Lock();
        Mutex.Lock();

        std::thread Waiter([&]
        {
            FRecursiveScopeLock Lock(Mutex);
            Entered.store(1, std::memory_order_release);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(Entered.load(std::memory_order_acquire), 0u);

        Mutex.Unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_EQ(Entered.load(std::memory_order_acquire), 0u);

        Mutex.Unlock();
        Waiter.join();
        EXPECT_EQ(Entered.load(), 1u);
    }

    TEST(Semaphore, TryAcquireConsumesPermits)
    {
        FSemaphore Semaphore(2);

        EXPECT_TRUE(Semaphore.TryAcquire());
        EXPECT_TRUE(Semaphore.TryAcquire());
        EXPECT_FALSE(Semaphore.TryAcquire());

        Semaphore.Release(3);
        EXPECT_TRUE(Semaphore.TryAcquire());
        EXPECT_TRUE(Semaphore.TryAcquire());
        EXPECT_TRUE(Semaphore.TryAcquire());
        EXPECT_FALSE(Semaphore.TryAcquire());
    }

    TEST(Semaphore, ReleaseWakesBlockedAcquirers)
    {
        constexpr uint32 kThreads = 6;

        FSemaphore Semaphore;
        TAtomic<uint32> Acquired{0};

        std::thread Threads[kThreads];
        for (std::thread& Thread : Threads)
        {
            Thread = std::thread([&]
            {
                Semaphore.Acquire();
                Acquired.fetch_add(1, std::memory_order_acq_rel);
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(Acquired.load(), 0u);

        Semaphore.Release(kThreads);
        for (std::thread& Thread : Threads)
        {
            Thread.join();
        }

        EXPECT_EQ(Acquired.load(), kThreads);
        EXPECT_FALSE(Semaphore.TryAcquire());
    }
}
