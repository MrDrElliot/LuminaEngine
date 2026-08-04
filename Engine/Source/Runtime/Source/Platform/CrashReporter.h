#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/String.h"

// Uploads crashes to a hosted crash service (BugSplat). Compiled out entirely unless WITH_BUGSPLAT
// is defined, in which case every function below is a no-op that still links.
//
// This sits on top of CrashHandler, it does not replace it: CrashHandler still writes the local
// dump, log and stack text into <Project>/CrashDumps so there is always an on-disk artifact even
// when the machine is offline or reporting is off.

namespace Lumina::CrashReporting
{
    // Must run before anything can crash, so this goes first in main, ahead of CrashHandler::Install.
    // The reporter installs its own unhandled-exception filter; installing CrashHandler afterwards
    // is what lets the local dump run first and then hand off to the uploader.
    RUNTIME_API void Initialize();

    RUNTIME_API void Shutdown();

    RUNTIME_API bool IsEnabled();

    // Reports whether the reporter came up, and why not if it did not. Call once logging is running:
    // Initialize() runs before the log system exists, so it cannot say anything itself.
    RUNTIME_API void LogStatus();

    // Builds and queues a report for a crash the reporter will NOT see by itself.
    //
    // Its monitor detects abnormal process death, which covers a genuine unhandled exception but not
    // a fatal the engine synthesizes and then swallows -- a lost GPU device, a failed assert, abort.
    // Those end in a controlled exit that looks like a clean shutdown from outside the process, so
    // without this call they upload nothing at all.
    //
    // ExceptionPointers must be the EXCEPTION_POINTERS the OS supplied to a filter; it is not a
    // value to construct. Does not terminate the process.
    RUNTIME_API void GenerateReport(void* ExceptionPointers);

    // Consent is the SDK's own job: its sender shows the user what is about to be uploaded and asks
    // before sending, so the engine does not gate this a second time.

    // Shown as a column on the crash list. Cheap enough to call whenever a value changes.
    RUNTIME_API void SetAttribute(FStringView Key, FStringView Value);

    // Uploaded alongside the next crash. The path is captured now and opened at crash time, so it
    // has to still exist then: pass the log file, not a scratch file you are about to delete.
    RUNTIME_API void AddAttachment(FStringView Path);

    // Clears the attachment list. Used when a project load repoints the log somewhere else.
    RUNTIME_API void ClearAttachments();

    RUNTIME_API void SetUser(FStringView Name, FStringView Email);
}
