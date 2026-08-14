// No-op fallbacks for the OS-shell helpers in PlatformProcess.h; compiled only where there's no native impl.

#include "RuntimePCH.h"

#if !defined(_WIN32) && !defined(LE_PLATFORM_LINUX)

#include "Platform/Process/PlatformProcess.h"
#include "Log/Log.h"

namespace Lumina::Platform
{
    // Per-callsite once-flag; a single shared static would swallow every call after the first.
    #define LUMINA_WARN_ONCE(What) \
        do { \
            static bool bWarned_ = false; \
            if (!bWarned_) { bWarned_ = true; \
                LOG_WARN("{0}: no implementation on this platform; ignoring.", What); } \
        } while (0)

    void ShowFileInExplorer(const TCHAR* /*Path*/)
    {
        LUMINA_WARN_ONCE("Platform::ShowFileInExplorer");
    }

    void ShowFolderInExplorer(const TCHAR* /*Directory*/)
    {
        LUMINA_WARN_ONCE("Platform::ShowFolderInExplorer");
    }

    void OpenTerminalAt(const TCHAR* /*Directory*/)
    {
        LUMINA_WARN_ONCE("Platform::OpenTerminalAt");
    }

    bool SetEnvVariable(const FString& Name, const FString& Value)
    {
        if (setenv(Name.c_str(), Value.c_str(), 1) == 0)
        {
            return true;
        }

        LOG_WARN("Failed to set environment variable {}", Name);
        return false;
    }

    bool PersistUserEnvVariable(const FString& /*Name*/, const FString& /*Value*/)
    {
        LUMINA_WARN_ONCE("Platform::PersistUserEnvVariable");
        return false;
    }

    #undef LUMINA_WARN_ONCE
}

#endif
