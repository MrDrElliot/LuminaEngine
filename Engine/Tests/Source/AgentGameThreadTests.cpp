#include <gtest/gtest.h>

#include "Agent/AgentGameThread.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/ThreadedCallback.h"

using namespace Lumina;
using namespace Lumina::Agent;

namespace
{
    // The test thread is the main thread, so it stands in for the frame loop that normally drains this.
    void PumpFor(double Seconds)
    {
        const double Deadline = PlatformTime::Seconds() + Seconds;
        while (PlatformTime::Seconds() < Deadline)
        {
            MainThread::ProcessQueue();
            Threading::Sleep(1);
        }
    }

    // Runs Body on a worker and pumps the main thread queue meanwhile, which is the real arrangement.
    void RunOffThreadWhilePumping(TFunction<void()> Body, double PumpSeconds)
    {
        TAtomic<bool> bDone { false };

        FThread Worker([&Body, &bDone]()
        {
            Body();
            bDone.store(true, std::memory_order_release);
        });

        const double Deadline = PlatformTime::Seconds() + PumpSeconds;
        while (!bDone.load(std::memory_order_acquire) && PlatformTime::Seconds() < Deadline)
        {
            MainThread::ProcessQueue();
            Threading::Sleep(1);
        }

        Worker.Join();
    }
}

// Called from the thread that owns the world, a hop would wait on a queue only this thread drains.
TEST(AgentGameThread, WorkOnTheMainThreadRunsInline)
{
    ASSERT_TRUE(Threading::IsMainThread());

    bool bRan = false;
    const EGameThreadResult Result = FGameThreadGate::Run([&bRan]() { bRan = true; }, 1000);

    EXPECT_EQ(Result, EGameThreadResult::Ran);
    EXPECT_TRUE(bRan);
}

TEST(AgentGameThread, EmptyWorkIsHarmless)
{
    const EGameThreadResult Result = FGameThreadGate::Run(TMoveOnlyFunction<void()>(), 1000);
    EXPECT_EQ(Result, EGameThreadResult::Ran);
}

TEST(AgentGameThread, WorkFromAnotherThreadRunsOnTheMainThread)
{
    TAtomic<bool> bRanOnMain { false };
    EGameThreadResult Result = EGameThreadResult::TimedOut;

    RunOffThreadWhilePumping([&]()
    {
        Result = FGameThreadGate::Run([&bRanOnMain]()
        {
            bRanOnMain.store(Threading::IsMainThread(), std::memory_order_release);
        }, 4000);
    }, 5.0);

    EXPECT_EQ(Result, EGameThreadResult::Ran);
    EXPECT_TRUE(bRanOnMain.load(std::memory_order_acquire));
}

TEST(AgentGameThread, AValueWrittenByTheWorkIsVisibleAfterwards)
{
    int32 Written = 0;
    EGameThreadResult Result = EGameThreadResult::TimedOut;

    RunOffThreadWhilePumping([&]()
    {
        Result = FGameThreadGate::Run([&Written]() { Written = 42; }, 4000);
    }, 5.0);

    ASSERT_EQ(Result, EGameThreadResult::Ran);
    EXPECT_EQ(Written, 42);
}

// A wedged frame loop must not hang the caller forever.
TEST(AgentGameThread, WorkThatNeverGetsPumpedTimesOut)
{
    TAtomic<bool> bRan { false };
    EGameThreadResult Result = EGameThreadResult::Ran;

    FThread Worker([&]()
    {
        Result = FGameThreadGate::Run([&bRan]() { bRan.store(true, std::memory_order_release); }, 150);
    });

    Worker.Join();

    EXPECT_EQ(Result, EGameThreadResult::TimedOut);
    EXPECT_FALSE(bRan.load(std::memory_order_acquire));
}

// Abandoned work must stay abandoned, or it would touch caller memory that is already gone.
TEST(AgentGameThread, AbandonedWorkNeverRunsEvenIfPumpedLater)
{
    TAtomic<bool> bRan { false };
    EGameThreadResult Result = EGameThreadResult::Ran;

    FThread Worker([&]()
    {
        Result = FGameThreadGate::Run([&bRan]() { bRan.store(true, std::memory_order_release); }, 100);
    });

    Worker.Join();
    ASSERT_EQ(Result, EGameThreadResult::TimedOut);

    PumpFor(0.25);

    EXPECT_FALSE(bRan.load(std::memory_order_acquire));
}

// Work already running when the deadline passes is waited out rather than abandoned mid-flight.
TEST(AgentGameThread, WorkAlreadyRunningIsWaitedOutPastTheTimeout)
{
    TAtomic<bool> bFinished { false };
    EGameThreadResult Result = EGameThreadResult::TimedOut;

    RunOffThreadWhilePumping([&]()
    {
        Result = FGameThreadGate::Run([&bFinished]()
        {
            Threading::Sleep(250);
            bFinished.store(true, std::memory_order_release);
        }, 50);
    }, 5.0);

    EXPECT_EQ(Result, EGameThreadResult::Ran);
    EXPECT_TRUE(bFinished.load(std::memory_order_acquire));
}

TEST(AgentGameThread, SeveralCallsAllRunInOrder)
{
    TVector<int32> Order;
    EGameThreadResult Results[3] = { EGameThreadResult::TimedOut, EGameThreadResult::TimedOut, EGameThreadResult::TimedOut };

    RunOffThreadWhilePumping([&]()
    {
        for (int32 Index = 0; Index < 3; ++Index)
        {
            Results[Index] = FGameThreadGate::Run([&Order, Index]() { Order.push_back(Index); }, 4000);
        }
    }, 6.0);

    for (const EGameThreadResult Result : Results)
    {
        EXPECT_EQ(Result, EGameThreadResult::Ran);
    }

    ASSERT_EQ(Order.size(), 3u);
    EXPECT_EQ(Order[0], 0);
    EXPECT_EQ(Order[1], 1);
    EXPECT_EQ(Order[2], 2);
}

TEST(AgentGameThread, TheDefaultTimeoutIsPositive)
{
    EXPECT_GT(FGameThreadGate::GetDefaultTimeoutMilliseconds(), 0);
}
