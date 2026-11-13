#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <vector>

#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"
#include "Platform/Time/PlatformTime.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaMutexTests
{
    namespace PlatformTime = Lumina::PlatformTime;
    using Lumina::TMutex;
    using Lumina::TMutexGuard;
    using Lumina::TRwLock;
    using Lumina::TRwLockReadGuard;
    using Lumina::TRwLockWriteGuard;
    using Lumina::FMutex;
    using Lumina::FScopeLock;
    using Lumina::FThread;
    using Lumina::int32;
    using Lumina::uint32;
    using Lumina::uint64;

    constexpr uint32 kThreadCount = 8;
    constexpr uint32 kIterations = 20000;

    struct FNoDefault
    {
        FNoDefault(int32 InLeft, int32 InRight) : Left(InLeft), Right(InRight) {}

        int32 Left;
        int32 Right;
    };

    static_assert(!std::is_copy_constructible_v<TMutex<int32>> && !std::is_move_constructible_v<TMutex<int32>>);
    static_assert(!std::is_copy_constructible_v<TRwLock<int32>> && !std::is_move_constructible_v<TRwLock<int32>>);
    static_assert(std::is_nothrow_move_constructible_v<TMutexGuard<int32>> && std::is_nothrow_move_assignable_v<TMutexGuard<int32>>);
    static_assert(!std::is_copy_constructible_v<TMutexGuard<int32>> && !std::is_copy_assignable_v<TMutexGuard<int32>>);
    static_assert(!std::is_convertible_v<TMutexGuard<int32>, int32*> && !std::is_convertible_v<TMutexGuard<int32>&, int32&>);
    static_assert(!std::is_copy_constructible_v<TRwLockReadGuard<int32>> && std::is_move_constructible_v<TRwLockReadGuard<int32>>);
    static_assert(!std::is_copy_constructible_v<TRwLockWriteGuard<int32>> && std::is_move_constructible_v<TRwLockWriteGuard<int32>>);
    static_assert(std::is_same_v<decltype(std::declval<const TMutex<int32>&>().Lock()), TMutexGuard<const int32>>);
    static_assert(std::is_same_v<decltype(*std::declval<TMutexGuard<const int32>&>()), const int32&>);
    static_assert(std::is_same_v<decltype(std::declval<const TMutexGuard<int32>&>().Get()), const int32&>);
    static_assert(std::is_same_v<decltype(std::declval<TRwLockReadGuard<int32>&>().Get()), const int32&>);
    static_assert(std::is_same_v<decltype(std::declval<TRwLockWriteGuard<int32>&>().Get()), int32&>);
    static_assert(sizeof(TMutexGuard<int32>) == sizeof(void*));

    TEST(TMutex, SerializesConcurrentIncrements)
    {
        TMutex<uint64> Counter(0ull);

        std::vector<FThread> Threads;
        Threads.reserve(kThreadCount);
        for (uint32 Index = 0; Index < kThreadCount; ++Index)
        {
            Threads.emplace_back([&Counter]
            {
                for (uint32 Step = 0; Step < kIterations; ++Step)
                {
                    TMutexGuard<uint64> Guard = Counter.Lock();
                    ++*Guard;
                }
            });
        }

        for (FThread& Thread : Threads)
        {
            Thread.Join();
        }

        EXPECT_EQ(*Counter.Lock(), uint64(kThreadCount) * kIterations);
    }

    TEST(TMutex, BuildsMoveOnlyAndNonDefaultValuesInPlace)
    {
        TMutex<std::unique_ptr<int32>> Owned(std::make_unique<int32>(7));
        EXPECT_EQ(**Owned.Lock(), 7);

        TMutex<FNoDefault> Pair(3, 4);
        TMutexGuard<FNoDefault> Guard = Pair.Lock();
        EXPECT_EQ(Guard->Left, 3);
        EXPECT_EQ(Guard.Get().Right, 4);
    }

    TEST(TMutex, TryLockFailsWhileHeldByAnotherThread)
    {
        TMutex<int32> Value(1);
        std::atomic<bool> bHeld{ false };
        std::atomic<bool> bRelease{ false };

        FThread Holder([&]
        {
            TMutexGuard<int32> Guard = Value.Lock();
            bHeld.store(true, std::memory_order_release);
            while (!bRelease.load(std::memory_order_acquire))
            {
                PlatformTime::YieldThread();
            }
        });

        while (!bHeld.load(std::memory_order_acquire))
        {
            PlatformTime::YieldThread();
        }

        const bool bTookWhileHeld = Value.TryLock().IsSet();
        bRelease.store(true, std::memory_order_release);
        Holder.Join();

        EXPECT_FALSE(bTookWhileHeld);
        Lumina::TOptional<TMutexGuard<int32>> Guard = Value.TryLock();
        ASSERT_TRUE(Guard.IsSet());
        EXPECT_EQ(*Guard.GetValue(), 1);
    }

    TEST(TMutex, GuardMovesCarryTheLock)
    {
        TMutex<int32> First(1);
        TMutex<int32> Second(2);

        TMutexGuard<int32> Original = First.Lock();
        TMutexGuard<int32> Moved(std::move(Original));
        EXPECT_FALSE(First.TryLock().IsSet());
        EXPECT_EQ(*Moved, 1);

        TMutexGuard<int32> Other = Second.Lock();
        Moved = std::move(Other);
        EXPECT_TRUE(First.TryLock().IsSet()) << "assigning over a guard releases the lock it held";
        EXPECT_FALSE(Second.TryLock().IsSet());
        EXPECT_EQ(*Moved, 2);

        Lumina::TOptional<TMutexGuard<int32>> Optional = First.TryLock();
        Lumina::TOptional<TMutexGuard<int32>> MovedOptional(std::move(Optional));
        EXPECT_FALSE(Optional.IsSet());
        ASSERT_TRUE(MovedOptional.IsSet());
        EXPECT_FALSE(First.TryLock().IsSet());

        MovedOptional.Reset();
        EXPECT_TRUE(First.TryLock().IsSet());
    }

    TEST(TMutex, ConstLockReadsThroughAConstGuard)
    {
        const TMutex<int32> Value(5);
        TMutexGuard<const int32> Guard = Value.Lock();
        EXPECT_EQ(*Guard, 5);
        EXPECT_FALSE(Value.TryLock().IsSet());
    }

    TEST(TMutex, WithLockReturnsTheFunctionResult)
    {
        TMutex<int32> Value(10);

        const int32 Doubled = Value.WithLock([](int32& Locked) { Locked *= 2; return Locked; });
        EXPECT_EQ(Doubled, 20);

        int32& Inside = Value.WithLock([](int32& Locked) -> int32& { return Locked; });
        EXPECT_EQ(Inside, 20);

        const TMutex<int32>& ConstValue = Value;
        EXPECT_TRUE((ConstValue.WithLock([](auto& Locked) { return std::is_const_v<std::remove_reference_t<decltype(Locked)>>; })));
        EXPECT_TRUE(Value.TryLock().IsSet()) << "WithLock unlocks before it returns";
    }

    TEST(TRwLock, NeverLetsAWriterInWhileAReaderHoldsIt)
    {
        TRwLock<int32> Value(0);
        std::atomic<int32> Concurrent{ 0 };
        std::atomic<int32> WriterOverlap{ 0 };

        std::vector<FThread> Threads;
        Threads.reserve(6);
        for (uint32 Index = 0; Index < 4; ++Index)
        {
            Threads.emplace_back([&]
            {
                for (uint32 Step = 0; Step < 2000; ++Step)
                {
                    TRwLockReadGuard<int32> Guard = Value.Read();
                    Concurrent.fetch_add(1, std::memory_order_acq_rel);
                    PlatformTime::YieldThread();
                    Concurrent.fetch_sub(1, std::memory_order_acq_rel);
                }
            });
        }

        for (uint32 Index = 0; Index < 2; ++Index)
        {
            Threads.emplace_back([&]
            {
                for (uint32 Step = 0; Step < 2000; ++Step)
                {
                    TRwLockWriteGuard<int32> Guard = Value.Write();
                    ++*Guard;
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
        EXPECT_EQ(*Value.Read(), 4000);
    }

    TEST(TRwLock, LetsEveryReaderInAtOnce)
    {
        constexpr int32 kReaders = 4;

        TRwLock<int32> Value(9);
        std::atomic<int32> Inside{ 0 };
        std::atomic<int32> Seen{ 0 };

        // No writer runs here, so every reader can hold the lock at once and the barrier cannot deadlock.
        std::vector<FThread> Threads;
        Threads.reserve(kReaders);
        for (int32 Index = 0; Index < kReaders; ++Index)
        {
            Threads.emplace_back([&]
            {
                TRwLockReadGuard<int32> Guard = Value.Read();
                Inside.fetch_add(1, std::memory_order_acq_rel);
                while (Inside.load(std::memory_order_acquire) < kReaders)
                {
                    PlatformTime::YieldThread();
                }
                Seen.fetch_add(*Guard, std::memory_order_relaxed);
            });
        }

        for (FThread& Thread : Threads)
        {
            Thread.Join();
        }

        EXPECT_EQ(Seen.load(), 9 * kReaders);
    }

    TEST(TRwLock, GuardMovesCarryTheLock)
    {
        TRwLock<int32> Value(1);

        {
            TRwLockWriteGuard<int32> Writer = Value.Write();
            TRwLockWriteGuard<int32> Moved(std::move(Writer));
            *Moved = 2;
        }

        {
            TRwLockReadGuard<int32> Reader = Value.Read();
            TRwLockReadGuard<int32> Moved(std::move(Reader));
            EXPECT_EQ(*Moved, 2);
        }

        // A guard that unlocked twice or leaked its lock would fail or hang here.
        EXPECT_EQ(*Value.Write(), 2);
    }

    // Prints both paths for comparison, never asserting on time because a shared machine makes it noisy.
    TEST(TMutex, UncontendedCostMatchesScopeLock)
    {
        constexpr uint32 kOps = 2000000;
        constexpr int32 kRounds = 5;

        TMutex<uint64> Wrapped(0ull);
        FMutex Raw;
        uint64 RawCounter = 0;

        double BestWrapped = 1e30;
        double BestRaw = 1e30;
        for (int32 Round = 0; Round < kRounds; ++Round)
        {
            PlatformTime::FStopwatch WrappedTimer;
            for (uint32 Op = 0; Op < kOps; ++Op)
            {
                TMutexGuard<uint64> Guard = Wrapped.Lock();
                ++*Guard;
            }
            BestWrapped = std::min(BestWrapped, WrappedTimer.ElapsedSeconds());

            PlatformTime::FStopwatch RawTimer;
            for (uint32 Op = 0; Op < kOps; ++Op)
            {
                FScopeLock Lock(Raw);
                ++RawCounter;
            }
            BestRaw = std::min(BestRaw, RawTimer.ElapsedSeconds());
        }

        printf("[Bench] TMutex lock+increment+unlock %6.2f ns\n", BestWrapped * 1e9 / kOps);
        printf("[Bench] FMutex+FScopeLock            %6.2f ns\n", BestRaw * 1e9 / kOps);
        EXPECT_EQ(*Wrapped.Lock(), RawCounter);
    }
}
