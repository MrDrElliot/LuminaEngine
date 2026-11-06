// No-op fallbacks for the OS-shell helpers in PlatformProcess.h; compiled only where there's no native impl.

#include "RuntimePCH.h"

#if !defined(_WIN32) && !defined(LE_PLATFORM_LINUX)

#include "Platform/Process/PlatformProcess.h"
#include "Log/Log.h"

namespace Lumina::Platform
{
    void ShowFileInExplorer(const TCHAR* /*Path*/)
    {
        LOG_WARN_ONCE("Platform::ShowFileInExplorer: no implementation on this platform; ignoring.");
    }

    void ShowFolderInExplorer(const TCHAR* /*Directory*/)
    {
        LOG_WARN_ONCE("Platform::ShowFolderInExplorer: no implementation on this platform; ignoring.");
    }

    void OpenTerminalAt(const TCHAR* /*Directory*/)
    {
        LOG_WARN_ONCE("Platform::OpenTerminalAt: no implementation on this platform; ignoring.");
    }

    void OpenSourceFile(const TCHAR* /*Path*/, int32 /*Line*/)
    {
        LOG_WARN_ONCE("Platform::OpenSourceFile: no implementation on this platform; ignoring.");
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
}

#endif
