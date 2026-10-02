#include "RuntimePCH.h"
#include "ThreadedCallback.h"
#include "Containers/ConcurrentQueue.h"
#include "Core/Templates/LuminaTemplate.h"

namespace Lumina::MainThread
{
    static TConcurrentQueue<TMoveOnlyFunction<void()>> Callbacks;
    static std::atomic<uint64> ProgressStamp{0};

    void ProcessQueue()
    {
        TMoveOnlyFunction<void()> Callback;
        while (Callbacks.TryDequeue(Callback))
        {
            ProgressStamp.fetch_add(1, std::memory_order_release);
            Callback();
            ProgressStamp.fetch_add(1, std::memory_order_release);
        }
    }

    uint64 GetProgressStamp()
    {
        return ProgressStamp.load(std::memory_order_acquire);
    }

    void Enqueue(TMoveOnlyFunction<void()>&& Callback)
    {
        Callbacks.Enqueue(Move(Callback));
    }
}
