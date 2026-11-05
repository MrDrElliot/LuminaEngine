#include "RuntimePCH.h"
#include "NavBuildPool.h"

namespace Lumina
{
    FNavBuildPool::FNavBuildPool(uint32 NumThreads)
    {
        Threads.Reserve(NumThreads);
        for (uint32 i = 0; i < NumThreads; ++i)
        {
            Threads.Emplace(FThreadOptions{ "NavBuild", Threading::ThreadGroup_Other, EThreadPriority::Background }, [this] { ThreadLoop(); });
        }
    }

    FNavBuildPool::~FNavBuildPool()
    {
        {
            FScopeLock Lock(QueueLock);
            bStopping = true;
        }
        QueueWake.NotifyAll();
        for (FThread& Thread : Threads)
        {
            Thread.Join();
        }
    }

    void FNavBuildPool::Enqueue(TMoveOnlyFunction<void()>&& Job)
    {
        {
            FScopeLock Lock(QueueLock);
            Queue.PushLast(std::move(Job));
        }
        QueueWake.NotifyOne();
    }

    bool FNavBuildPool::RunOne()
    {
        TMoveOnlyFunction<void()> Job;
        {
            FScopeLock Lock(QueueLock);
            if (Queue.IsEmpty())
            {
                return false;
            }
            Job = std::move(Queue[0]);
            Queue.PopFirst();
        }
        Job();
        return true;
    }

    void FNavBuildPool::ThreadLoop()
    {
        while (true)
        {
            TMoveOnlyFunction<void()> Job;
            {
                FUniqueLock Lock(QueueLock);
                QueueWake.Wait(Lock, [this] { return bStopping || !Queue.IsEmpty(); });
                if (Queue.IsEmpty())
                {
                    return;
                }
                Job = std::move(Queue[0]);
                Queue.PopFirst();
            }
            Job();
        }
    }
}
