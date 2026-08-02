#pragma once
#include "Containers/Function.h"

namespace Lumina::MainThread
{
    void ProcessQueue();

    /** Thread-safe; runs once on the main thread next frame in FIFO order. */
    RUNTIME_API void Enqueue(TMoveOnlyFunction<void()>&& Callback);
    
}
