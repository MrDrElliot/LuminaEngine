#pragma once

#include "Platform/GenericPlatform.h"

// Thin user-mode fiber wrapper (cooperative stacks that migrate between OS threads). Windows-backed
// today (Win32 fibers); a POSIX/asm backend can slot in behind the same surface later. Method names
// differ from the WinAPI on purpose so the implementation can call the unqualified Win32 functions.
namespace Lumina::Fibers
{
    using FFiber      = void*;
    using FFiberEntry = void (*)(void* Arg);

    // Convert the calling OS thread into a fiber; returns its fiber handle (the "thread fiber").
    RUNTIME_API FFiber ThreadToFiber();
    
    // Convert the calling thread's fiber back to a normal thread. Call before the thread exits.
    RUNTIME_API void   FiberToThread();

    // Create a fiber with StackSize bytes of reserved stack. Entry runs on the first Switch into it
    // and is expected never to return (loop + Switch away); returning ends the hosting thread.
    RUNTIME_API FFiber Create(size_t StackSize, FFiberEntry Entry, void* Arg);
    
    // Free a fiber. Must NOT be the running fiber, and no thread may currently have it switched in.
    RUNTIME_API void   Destroy(FFiber Fiber);

    // Switch the calling thread to Fiber. Execution resumes in Fiber where it last switched away.
    RUNTIME_API void   Switch(FFiber Fiber);

    // The fiber currently executing on the calling thread (nullptr if the thread isn't a fiber).
    RUNTIME_API FFiber Current();
}
