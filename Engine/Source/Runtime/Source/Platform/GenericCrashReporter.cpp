#include "RuntimePCH.h"


#if !defined(LE_PLATFORM_WINDOWS)

#include "Platform/CrashReporter.h"
#include "Platform/CrashHandler.h"
#include "Log/Log.h"

namespace Lumina::CrashReporting
{
    void Initialize()
    {
    }

    void Shutdown()
    {
    }

    bool IsEnabled()
    {
        return false;
    }

    void LogStatus()
    {
        LOG_INFO("Crash reporting: disabled (no hosted reporter on this platform). Local crash "
                 "reports are still written to '{0}'.", CrashHandler::GetCrashDumpDirectory());
    }

    void GenerateReport(void*)
    {
    }

    void SetAttribute(FStringView, FStringView)
    {
    }

    void AddAttachment(FStringView)
    {
    }

    void ClearAttachments()
    {
    }

    void SetUser(FStringView, FStringView)
    {
    }
}

#endif
