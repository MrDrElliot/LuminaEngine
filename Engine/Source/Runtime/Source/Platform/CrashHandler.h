#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/String.h"

namespace Lumina::CrashHandler
{
    // Installs exception/terminate/signal handlers; on crash flushes the log, writes a minidump, pops a modal.
    // Call once, as early in main as possible.
    RUNTIME_API void Install();

    // Clear on a process with no interactive desktop, where nothing dismisses the modal and the crash hangs.
    RUNTIME_API void SetAllowModalDialog(bool bAllow);

    RUNTIME_API void Shutdown();

    // What kind of failure produced the crash. Only affects how the dump is named -- a GPU device
    // loss filed under CPUCrash_*.dmp sends triage down the wrong path before the log is even open.
    enum class EFatalKind : uint8
    {
        Cpu,
        Gpu,
    };

    // Reports a fatal condition that is not a CPU exception -- a lost GPU device, a failed
    // invariant -- through the same path a real crash takes: local dump, log, then the reporter's
    // send dialog. Returns after the report is dealt with; the caller still has to terminate.
    //
    // Without this a device loss reaches abort() and dies, and whether anything got reported
    // depends on the SIGABRT handler winning a race it was never meant to be in.
    RUNTIME_API void ReportFatal(const char* Reason, EFatalKind Kind = EFatalKind::Cpu);

    // Runs while the process is crashing, before the dump is written, to log subsystem state the
    // handler cannot reach on its own -- the RHI's GPU breadcrumbs being the reason this exists.
    //
    // Register at startup, never during a crash. The provider runs on a suspect heap inside the
    // handler's __try, so it must not allocate more than it has to and must not take a lock the
    // crashing thread might already hold.
    using FDiagnosticProvider = void (*)();

    RUNTIME_API void AddDiagnosticProvider(FDiagnosticProvider Provider);

    // Where minidumps and GPU crash dumps land. Install() seeds it with <exe dir>/CrashDumps because no
    // project is known that early; FEngine::LoadProject repoints it at <ProjectPath>/CrashDumps so a crash
    // lands with the project that caused it.
    RUNTIME_API void SetCrashDumpDirectory(FStringView Directory);

    RUNTIME_API FString GetCrashDumpDirectory();

    // Buffer variant for the crash path: copies into OutBuffer (always null-terminated) and returns the
    // length written. Allocates nothing and takes no lock, so it is safe once the heap is suspect.
    RUNTIME_API uint32 GetCrashDumpDirectory(char* OutBuffer, uint32 BufferSize);
}
