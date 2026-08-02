#pragma once
#include "Platform/GenericPlatform.h"

namespace Lumina::HangWatchdog
{
    // Background main-thread stall detector.
    RUNTIME_API void Start(float TimeoutSeconds = 8.0f);
    RUNTIME_API void Stop();

    RUNTIME_API void Heartbeat();

    // Register a callback the watchdog invokes at the end of a stall dump (on the watchdog thread,
    // with all suspended threads already resumed). Reporters should only read atomics and log: their
    // job is to surface state thread stacks can't show (e.g. a parked-fiber pump like the render
    // drain). Registration is permanent (subsystem Start), capacity is small and fixed.
    RUNTIME_API void RegisterReporter(void (*Reporter)());
}
