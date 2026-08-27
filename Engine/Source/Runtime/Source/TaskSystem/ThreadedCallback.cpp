#include "RuntimePCH.h"
#include "ThreadedCallback.h"
#include "Containers/ConcurrentQueue.h"
#include "Core/Templates/LuminaTemplate.h"

namespace Lumina::MainThread
{
    static TConcurrentQueue<TMoveOnlyFunction<void()>> Callbacks;
    
    void ProcessQueue()
    {
        TMoveOnlyFunction<void()> Callback;
        while (Callbacks.TryDequeue(Callback))
        {
            Callback();
        }
    }

    void Enqueue(TMoveOnlyFunction<void()>&& Callback)
    {
        Callbacks.Enqueue(Move(Callback));
    }
}
