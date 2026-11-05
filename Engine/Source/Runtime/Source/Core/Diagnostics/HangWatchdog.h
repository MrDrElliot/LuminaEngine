#pragma once
#include "Platform/GenericPlatform.h"

namespace Lumina::HangWatchdog
{
    // Background main-thread stall detector.
    RUNTIME_API void Start(float TimeoutSeconds = 8.0f);
    RUNTIME_API void Stop();

    RUNTIME_API void Heartbeat();

    // Runs at the end of a stall dump on the watchdog thread, threads resumed; reporters may only read atomics and log. Registration is permanent.
    RUNTIME_API void RegisterReporter(void (*Reporter)());
}
