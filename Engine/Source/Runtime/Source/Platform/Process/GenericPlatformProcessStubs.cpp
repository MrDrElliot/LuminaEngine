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

    void OpenSourceFile(const TCHAR* /*Path*/, int32 /*Line*/)
    {
        LUMINA_WARN_ONCE("Platform::OpenSourceFile");
    }

    FString GetEnvVariable(FStringView Variable)
    {
        const FString Name(Variable);
        const char* Value = getenv(*Name);
        return Value ? FString(Value) : FString();
    }

    bool SetEnvVariable(const FString& Name, const FString& Value)
    {
        if (Name.IsEmpty() || Name.Find('=') != Constants::kIndexNone)
        {
            LOG_WARN("Refusing to set environment variable with invalid name '{}'", Name);
            return false;
        }

        // Windows cannot store an empty variable, so removing on empty keeps the platforms observably equal.
        const int Result = Value.IsEmpty() ? unsetenv(*Name) : setenv(*Name, *Value, 1);
        if (Result == 0)
        {
            return true;
        }

        LOG_WARN("Failed to set environment variable {}", Name);
        return false;
    }

    #undef LUMINA_WARN_ONCE
}

#endif
