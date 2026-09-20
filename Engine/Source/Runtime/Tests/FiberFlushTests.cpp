#include <gtest/gtest.h>

#include "Core/Threading/Atomic.h"
#include "TaskSystem/FiberSync.h"
#include "TaskSystem/TaskSystem.h"

using namespace Lumina;

namespace
{
    // The shape FSpirVShaderCompiler::Flush uses: a pending count, a fiber mutex and a signal.
    struct FPendingWork
    {
        TAtomic<uint32>         Pending{0};
        FFiberMutex             Mutex;
        FFiberConditionVariable Signal;

        void Add(uint32 Count)
        {
            FFiberScopeLock Lock(Mutex);
            Pending.fetch_add(Count, std::memory_order_relaxed);
        }

        void Complete(uint32 Count)
        {
            {
                FFiberScopeLock Lock(Mutex);
                Pending.fetch_sub(Count, std::memory_order_acq_rel);
            }
            Signal.NotifyAll();
        }

        void Flush()
        {
            FFiberScopeLock Lock(Mutex);
            Signal.Wait(Mutex, [this] { return Pending.load(std::memory_order_acquire) == 0; });
        }
    };
}

// A flush with nothing outstanding must not wait at all.
TEST(FiberFlush, FlushingWithNoPendingWorkReturnsImmediately)
{
    FPendingWork Work;
    Work.Flush();
    EXPECT_EQ(Work.Pending.load(), 0u);
}

// The lost-wakeup case: completions land while the caller is between its predicate check and its park.
TEST(FiberFlush, FlushFromAnExternalThreadSeesEveryCompletion)
{
    constexpr uint32 kTasks = 64;

    for (uint32 Attempt = 0; Attempt < 32; ++Attempt)
    {
        FPendingWork Work;
        Work.Add(kTasks);

        FTaskHandle Handle = Task::AsyncTask(kTasks, 1, [&Work](uint32 Start, uint32 End, uint32)
        {
            Work.Complete(End - Start);
        });

        Work.Flush();
        EXPECT_EQ(Work.Pending.load(), 0u) << "attempt " << Attempt;
        Handle->Wait();
    }
}

// The case the change exists for: the waiter is itself on a worker, so it must park its fiber and resume.
TEST(FiberFlush, FlushFromInsideAWorkerCompletes)
{
    constexpr uint32 kTasks = 32;

    TAtomic<uint32> Finished{0};
    FTaskHandle Outer = Task::AsyncTask(1, 1, [&Finished](uint32, uint32, uint32)
    {
        FPendingWork Work;
        Work.Add(kTasks);

        FTaskHandle Inner = Task::AsyncTask(kTasks, 1, [&Work](uint32 Start, uint32 End, uint32)
        {
            Work.Complete(End - Start);
        });

        Work.Flush();
        if (Work.Pending.load(std::memory_order_acquire) == 0)
        {
            Finished.store(1, std::memory_order_release);
        }
        Inner->Wait();
    });

    ASSERT_NE(Outer, nullptr);
    Outer->Wait();
    EXPECT_EQ(Finished.load(std::memory_order_acquire), 1u);
}
