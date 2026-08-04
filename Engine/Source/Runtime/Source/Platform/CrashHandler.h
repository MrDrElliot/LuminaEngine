#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/String.h"

namespace Lumina::CrashHandler
{
    // Installs exception/terminate/signal handlers; on crash flushes the log, writes a minidump, pops a modal.
    // Call once, as early in main as possible.
    RUNTIME_API void Install();

    RUNTIME_API void Shutdown();

    // Where minidumps and GPU crash dumps land. Install() seeds it with <exe dir>/CrashDumps because no
    // project is known that early; FEngine::LoadProject repoints it at <ProjectPath>/CrashDumps so a crash
    // lands with the project that caused it.
    RUNTIME_API void SetCrashDumpDirectory(FStringView Directory);

    RUNTIME_API FString GetCrashDumpDirectory();

    // Buffer variant for the crash path: copies into OutBuffer (always null-terminated) and returns the
    // length written. Allocates nothing and takes no lock, so it is safe once the heap is suspect.
    RUNTIME_API uint32 GetCrashDumpDirectory(char* OutBuffer, uint32 BufferSize);
}
