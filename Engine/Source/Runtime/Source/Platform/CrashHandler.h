#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina::CrashHandler
{
    // Installs exception/terminate/signal handlers; on crash flushes the log, writes a minidump, pops a modal.
    // Call once, as early in main as possible.
    RUNTIME_API void Install();

    RUNTIME_API void Shutdown();
}
