#pragma once
#include "Containers/Function.h"

namespace Lumina::MainThread
{
    // Drained by the frame loop. Exported so a test can stand in for that loop.
    RUNTIME_API void ProcessQueue();

    /** Thread-safe; runs once on the main thread next frame in FIFO order. */
    RUNTIME_API void Enqueue(TMoveOnlyFunction<void()>&& Callback);
    
}
