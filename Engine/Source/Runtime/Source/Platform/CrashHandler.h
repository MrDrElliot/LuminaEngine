#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/String.h"

namespace Lumina::CrashHandler
{
    // Call once, as early in main as possible.
    RUNTIME_API void Install();

    // Clear on a process with no interactive desktop, where nothing dismisses the modal and the crash hangs.
    RUNTIME_API void SetAllowModalDialog(bool bAllow);

    RUNTIME_API void Shutdown();

    // Only affects how the dump is named, since a GPU device loss filed under CPUCrash_*.dmp misleads triage.
    enum class EFatalKind : uint8
    {
        Cpu,
        Gpu,
    };

    // Reports a non-CPU fatal such as a lost GPU device through the real crash path, and the caller still has to terminate.
    RUNTIME_API void ReportFatal(const char* Reason, EFatalKind Kind = EFatalKind::Cpu);

    // Register at startup. The provider runs on a suspect heap, so it must not take a lock the crashing thread might hold.
    using FDiagnosticProvider = void (*)();

    RUNTIME_API void AddDiagnosticProvider(FDiagnosticProvider Provider);

    // Install() seeds this beside the exe since no project is known yet, and FEngine::LoadProject repoints it.
    RUNTIME_API void SetCrashDumpDirectory(FStringView Directory);

    RUNTIME_API FString GetCrashDumpDirectory();

    // Allocates nothing and takes no lock, so it is safe once the heap is suspect.
    RUNTIME_API uint32 GetCrashDumpDirectory(char* OutBuffer, uint32 BufferSize);
}
