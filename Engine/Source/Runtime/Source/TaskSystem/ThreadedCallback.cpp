#include "RuntimePCH.h"
#include "ThreadedCallback.h"
#include "Memory/MemoryConcurrentQueue.h"
#include "Core/Templates/LuminaTemplate.h"

namespace Lumina::MainThread
{
    static TConcurrentQueue<TMoveOnlyFunction<void()>> Callbacks;
    
    void ProcessQueue()
    {
        TMoveOnlyFunction<void()> Callback;
        while (Callbacks.try_dequeue(Callback))
        {
            Callback();
        }
    }

    void Enqueue(TMoveOnlyFunction<void()>&& Callback)
    {
        Callbacks.enqueue(Move(Callback));
    }
}
