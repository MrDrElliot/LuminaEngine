#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "Core/Threading/Atomic.h"
#include "Core/Threading/Sync.h"
#include "Core/Threading/Thread.h"
#include "TaskSystem/FiberSync.h"
#include "TaskSystem/TaskSystem.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaRecursiveMutexFiberTests
{
    using namespace Lumina;

    // A holder that parks frees its worker for the next fiber, which shares the thread but not the ownership.
    TEST(RecursiveMutex, ExcludesAnotherFiberWhileTheHolderIsParked)
    {
        FRecursiveMutex Mutex;
        FFiberMutex     Gate;
        TAtomic<int32>  Inside{0};
        TAtomic<int32>  MaxInside{0};
        TAtomic<uint32> Holders{0};

        // Held here, so every fiber that takes the mutex parks on the gate while still holding it.
        Gate.Lock();

        constexpr uint32 kFibers = 256;
        FTaskHandle Handle = Task::AsyncTask(kFibers, 1, [&](uint32, uint32, uint32)
        {
            if (!Mutex.TryLock())
            {
                return;
            }

            const int32 Now = Inside.fetch_add(1, std::memory_order_acq_rel) + 1;
            for (int32 Seen = MaxInside.load(); Now > Seen && !MaxInside.compare_exchange_weak(Seen, Now);) {}
            Holders.fetch_add(1, std::memory_order_relaxed);

            Gate.Lock();
            Gate.Unlock();

            Inside.fetch_sub(1, std::memory_order_acq_rel);
            Mutex.Unlock();
        });

        // Long enough for every worker to park its holder and run the fibers queued behind it.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Gate.Unlock();
        Handle->Wait();

        EXPECT_GE(Holders.load(), 1u);
        EXPECT_EQ(MaxInside.load(), 1) << "two fibers held the recursive mutex at once";
    }

    // Recursion is per logical execution, so a fiber that parks and resumes elsewhere still re-enters.
    TEST(RecursiveMutex, ReentersOnTheSameFiberAcrossAPark)
    {
        FRecursiveMutex Mutex;
        FFiberMutex     Gate;
        TAtomic<uint32> Reentered{0};

        Gate.Lock();
        FTaskHandle Handle = Task::AsyncTask(1, 1, [&](uint32, uint32, uint32)
        {
            FRecursiveScopeLock Outer(Mutex);
            Gate.Lock();
            Gate.Unlock();
            if (Mutex.TryLock())
            {
                Reentered.store(1, std::memory_order_release);
                Mutex.Unlock();
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Gate.Unlock();
        Handle->Wait();

        EXPECT_EQ(Reentered.load(), 1u);
    }
}
