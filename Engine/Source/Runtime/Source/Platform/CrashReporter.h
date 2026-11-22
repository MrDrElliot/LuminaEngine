#pragma once

#include "Platform/GenericPlatform.h"
#include "Containers/String.h"

// Uploads crashes to BugSplat, and every function is a no-op unless WITH_BUGSPLAT is defined.

namespace Lumina::CrashReporting
{
    // Must run first in main, ahead of CrashHandler::Install, so the local dump runs before the uploader.
    RUNTIME_API void Initialize();

    RUNTIME_API void Shutdown();

    RUNTIME_API bool IsEnabled();

    // Call once logging is running, since Initialize() runs before the log system exists.
    RUNTIME_API void LogStatus();

    // Covers fatals the engine swallows, such as a lost GPU device, which look like a clean exit to the monitor.
    RUNTIME_API void GenerateReport(void* ExceptionPointers);

    // The SDK's sender asks for consent, so the engine does not gate this a second time.

    // Shown as a column on the crash list. Cheap enough to call whenever a value changes.
    RUNTIME_API void SetAttribute(FStringView Key, FStringView Value);

    // The path is opened at crash time, so pass the log file rather than a scratch file you will delete.
    RUNTIME_API void AddAttachment(FStringView Path);

    // Clears the attachment list. Used when a project load repoints the log somewhere else.
    RUNTIME_API void ClearAttachments();

    RUNTIME_API void SetUser(FStringView Name, FStringView Email);
}
