#pragma once
#include "Containers/Function.h"

namespace Lumina::MainThread
{
    // Drained by the frame loop. Exported so a test can stand in for that loop.
    RUNTIME_API void ProcessQueue();

    /** Thread-safe; runs once on the main thread next frame in FIFO order. */
    RUNTIME_API void Enqueue(TMoveOnlyFunction<void()>&& Callback);

    // Advances as callbacks start and finish and is odd while one runs, so a waiter can tell busy from stuck.
    RUNTIME_API uint64 GetProgressStamp();
    
}
