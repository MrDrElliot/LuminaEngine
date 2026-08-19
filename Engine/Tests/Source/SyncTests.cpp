#include <gtest/gtest.h>

#include <atomic>
#include <vector>

#include "Core/Threading/Sync.h"
#include "Core/Threading/Thread.h"
#include "Platform/Time/PlatformTime.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaSyncTests
{
    namespace PlatformTime = Lumina::PlatformTime;
    using Lumina::FMutex;
    using Lumina::FSharedMutex;
    using Lumina::FRecursiveMutex;
    using Lumina::FScopeLock;
    using Lumina::FReadScopeLock;
    using Lumina::FWriteScopeLock;
    using Lumina::FRecursiveScopeLock;
    using Lumina::FUniqueLock;
    using Lumina::FConditionVariable;
    using Lumina::FOnceFlag;
    using Lumina::FThread;
    using Lumina::int32;
    using Lumina::uint32;
    using Lumina::uint64;

    constexpr uint32 kThreadCount = 8;
    constexpr uint32 kIterations = 20000;

    TEST(Mutex, SerializesConcurrentIncrements)
    {
        FMutex Mutex;
        uint64 Counter = 0;

        std::vector<FThread> Threads;
        Threads.reserve(kThreadCount);
        for (uint32 Index = 0; Index < kThreadCount; ++Index)
        {
            Threads.emplace_back([&Mutex, &Counter]
            {
                for (uint32 Step = 0; Step < kIterations; ++Step)
                {
                    FScopeLock Lock(Mutex);
                    ++Counter;
                }
            });
        }

        for (FThread& Thread : Threads)
        {
            Thread.Join();
        }

        EXPECT_EQ(Counter, uint64(kThreadCount) * kIterations);
    }

    TEST(Mutex, TryLockFailsWhileHeldByAnotherThread)
    {
        FMutex Mutex;
        std::atomic<bool> bHeld{ false };
        std::atomic<bool> bRelease{ false };
        std::atomic<bool> bTrySucceeded{ true };

        FThread Holder([&]
        {
            Mutex.Lock();
            bHeld.store(true, std::memory_order_release);
            while (!bRelease.load(std::memory_order_acquire))
            {
                PlatformTime::YieldThread();
            }
            Mutex.Unlock();
        });

        while (!bHeld.load(std::memory_order_acquire))
        {
            PlatformTime::YieldThread();
        }

        bTrySucceeded.store(Mutex.TryLock(), std::memory_order_relaxed);
        bRelease.store(true, std::memory_order_release);
        Holder.Join();

        EXPECT_FALSE(bTrySucceeded.load(std::memory_order_relaxed));
        EXPECT_TRUE(Mutex.TryLock());
        Mutex.Unlock();
    }

    TEST(Mutex, IsOnePointerWide)
    {
        EXPECT_EQ(sizeof(FMutex), sizeof(void*));
        EXPECT_EQ(sizeof(FSharedMutex), sizeof(void*));
    }

    TEST(RecursiveMutex, ReentersOnTheOwningThread)
    {
        FRecursiveMutex Mutex;

        Mutex.Lock();
        Mutex.Lock();
        Mutex.Lock();
        EXPECT_TRUE(Mutex.TryLock());
        Mutex.Unlock();
        Mutex.Unlock();
        Mutex.Unlock();
        Mutex.Unlock();

        EXPECT_TRUE(Mutex.TryLock());
        Mutex.Unlock();
    }

    TEST(RecursiveMutex, StillExcludesOtherThreads)
    {
        FRecursiveMutex Mutex;
        uint64 Counter = 0;

        std::vector<FThread> Threads;
        Threads.reserve(4);
        for (uint32 Index = 0; Index < 4; ++Index)
        {
            Threads.emplace_back([&Mutex, &Counter]
            {
                for (uint32 Step = 0; Step < 5000; ++Step)
                {
                    FRecursiveScopeLock Outer(Mutex);
                    FRecursiveScopeLock Inner(Mutex);
                    ++Counter;
                }
            });
        }

        for (FThread& Thread : Threads)
        {
            Thread.Join();
        }

        EXPECT_EQ(Counter, 4ull * 5000);
    }

    TEST(SharedMutex, LetsReadersOverlapButNotWriters)
    {
        FSharedMutex Mutex;
        std::atomic<int32> Concurrent{ 0 };
        std::atomic<int32> HighWater{ 0 };
        std::atomic<int32> WriterOverlap{ 0 };

        std::vector<FThread> Threads;
        Threads.reserve(6);
        for (uint32 Index = 0; Index < 4; ++Index)
        {
            Threads.emplace_back([&]
            {
                for (uint32 Step = 0; Step < 500; ++Step)
                {
                    FReadScopeLock Lock(Mutex);
                    const int32 Now = Concurrent.fetch_add(1, std::memory_order_acq_rel) + 1;

                    int32 Seen = HighWater.load(std::memory_order_relaxed);
                    while (Now > Seen && !HighWater.compare_exchange_weak(Seen, Now, std::memory_order_relaxed))
                    {
                    }

                    PlatformTime::YieldThread();
                    Concurrent.fetch_sub(1, std::memory_order_acq_rel);
                }
            });
        }

        for (uint32 Index = 0; Index < 2; ++Index)
        {
            Threads.emplace_back([&]
            {
                for (uint32 Step = 0; Step < 500; ++Step)
                {
                    FWriteScopeLock Lock(Mutex);
                    if (Concurrent.load(std::memory_order_acquire) != 0)
                    {
                        WriterOverlap.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (FThread& Thread : Threads)
        {
            Thread.Join();
        }

        EXPECT_EQ(WriterOverlap.load(), 0);
        EXPECT_GT(HighWater.load(), 1);
    }

    TEST(SharedMutex, TryLockSharedReportsWhetherItTook)
    {
        FSharedMutex Mutex;

        {
            FReadScopeLock First(Mutex, Lumina::TryToLock);
            EXPECT_TRUE(First.OwnsLock());
        }

        Mutex.Lock();
        {
            FReadScopeLock Denied(Mutex, Lumina::TryToLock);
            EXPECT_FALSE(Denied.OwnsLock());
        }
        Mutex.Unlock();

        {
            FReadScopeLock Free(Mutex, Lumina::TryToLock);
            EXPECT_TRUE(Free.OwnsLock());
        }
    }

    TEST(UniqueLock, DefersAndReleasesOnDemand)
    {
        FMutex Mutex;

        FUniqueLock Lock(Mutex, Lumina::DeferLock);
        EXPECT_FALSE(Lock.OwnsLock());
        EXPECT_TRUE(Mutex.TryLock());
        Mutex.Unlock();

        Lock.Lock();
        EXPECT_TRUE(Lock.OwnsLock());
        EXPECT_FALSE(Mutex.TryLock());

        Lock.Unlock();
        EXPECT_FALSE(Lock.OwnsLock());
        EXPECT_TRUE(Mutex.TryLock());
        Mutex.Unlock();
    }

    TEST(ConditionVariable, WakesAWaiter)
    {
        FMutex Mutex;
        FConditionVariable Signal;
        bool bReady = false;
        std::atomic<bool> bWoke{ false };

        FThread Waiter([&]
        {
            FUniqueLock Lock(Mutex);
            Signal.Wait(Lock, [&bReady] { return bReady; });
            bWoke.store(true, std::memory_order_release);
        });

        PlatformTime::SleepMilliseconds(10);
        {
            FScopeLock Lock(Mutex);
            bReady = true;
        }
        Signal.NotifyOne();

        Waiter.Join();
        EXPECT_TRUE(bWoke.load(std::memory_order_acquire));
    }

    TEST(ConditionVariable, WakesEveryWaiter)
    {
        FMutex Mutex;
        FConditionVariable Signal;
        bool bReady = false;
        std::atomic<uint32> Woke{ 0 };

        std::vector<FThread> Threads;
        Threads.reserve(4);
        for (uint32 Index = 0; Index < 4; ++Index)
        {
            Threads.emplace_back([&]
            {
                FUniqueLock Lock(Mutex);
                Signal.Wait(Lock, [&bReady] { return bReady; });
                Woke.fetch_add(1, std::memory_order_acq_rel);
            });
        }

        PlatformTime::SleepMilliseconds(10);
        {
            FScopeLock Lock(Mutex);
            bReady = true;
        }
        Signal.NotifyAll();

        for (FThread& Thread : Threads)
        {
            Thread.Join();
        }

        EXPECT_EQ(Woke.load(), 4u);
    }

    TEST(ConditionVariable, TimesOutWhenNothingSignals)
    {
        FMutex Mutex;
        FConditionVariable Signal;

        const PlatformTime::FStopwatch Timer;
        FUniqueLock Lock(Mutex);
        const bool bSatisfied = Signal.WaitFor(Lock, 0.05, [] { return false; });

        EXPECT_FALSE(bSatisfied);
        EXPECT_GE(Timer.ElapsedMilliseconds(), 40.0);
        EXPECT_TRUE(Lock.OwnsLock());
    }

    TEST(OnceFlag, RunsExactlyOnceUnderContention)
    {
        FOnceFlag Flag;
        std::atomic<uint32> Runs{ 0 };
        std::atomic<uint32> Observed{ 0 };

        std::vector<FThread> Threads;
        Threads.reserve(kThreadCount);
        for (uint32 Index = 0; Index < kThreadCount; ++Index)
        {
            Threads.emplace_back([&]
            {
                Lumina::CallOnce(Flag, [&Runs]
                {
                    PlatformTime::SleepMilliseconds(2);
                    Runs.fetch_add(1, std::memory_order_acq_rel);
                });

                // Every caller must see the initializer finished, not merely started.
                Observed.fetch_add(Runs.load(std::memory_order_acquire), std::memory_order_acq_rel);
            });
        }

        for (FThread& Thread : Threads)
        {
            Thread.Join();
        }

        EXPECT_EQ(Runs.load(), 1u);
        EXPECT_EQ(Observed.load(), kThreadCount);
        EXPECT_TRUE(Flag.IsDone());
    }

    TEST(Thread, RunsJoinsAndReportsJoinable)
    {
        std::atomic<uint32> Ran{ 0 };

        FThread Worker([&Ran] { Ran.fetch_add(1, std::memory_order_acq_rel); });
        EXPECT_TRUE(Worker.Joinable());
        Worker.Join();

        EXPECT_FALSE(Worker.Joinable());
        EXPECT_EQ(Ran.load(), 1u);
    }

    TEST(Thread, ForwardsExtraArgumentsToTheEntryPoint)
    {
        std::atomic<uint32> Sum{ 0 };

        auto Body = [&Sum](uint32 Left, uint32 Right)
        {
            Sum.fetch_add(Left + Right, std::memory_order_acq_rel);
        };

        FThread Worker(Body, 3u, 4u);
        Worker.Join();

        EXPECT_EQ(Sum.load(), 7u);
    }

    TEST(Thread, MovesWithoutLosingTheHandle)
    {
        std::atomic<uint32> Ran{ 0 };

        FThread First([&Ran] { Ran.fetch_add(1, std::memory_order_acq_rel); });
        FThread Second(std::move(First));

        EXPECT_FALSE(First.Joinable());
        EXPECT_TRUE(Second.Joinable());

        Second.Join();
        EXPECT_EQ(Ran.load(), 1u);
    }

    TEST(Thread, ReportsADistinctIdPerThread)
    {
        std::atomic<uint64> Observed{ 0 };

        FThread Worker([&Observed]
        {
            Observed.store(Lumina::Threading::GetThreadID(), std::memory_order_release);
        });

        const uint64 Reported = Worker.GetId();
        Worker.Join();

        EXPECT_NE(Reported, 0ull);
        EXPECT_EQ(Observed.load(std::memory_order_acquire), Reported);
        EXPECT_NE(Reported, Lumina::Threading::GetThreadID());
    }
}
