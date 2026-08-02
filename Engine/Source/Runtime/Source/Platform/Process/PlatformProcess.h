#pragma once

#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"


namespace Lumina::Platform
{
    struct FFileDialogResult
    {
        bool    bSuccess = false;
        FString FilePath;
    };
    
    RUNTIME_API void* GetDLLHandle(const TCHAR* Filename);
    RUNTIME_API bool FreeDLLHandle(void* DLLHandle);
    RUNTIME_API void* GetDLLExport(void* DLLHandle, const char* ProcName);
    RUNTIME_API void AddDLLDirectory(const FString& Directory);

    RUNTIME_API void PushDLLDirectory(const TCHAR* Directory);
    RUNTIME_API void PopDLLDirectory();

    RUNTIME_API uint32 GetCurrentCoreNumber();
    RUNTIME_API FString GetCurrentProcessPath();

    enum class ECpuCoreType : uint8 { Performance, Efficiency, Unknown };

    // Process-wide CPU core layout, detected once and cached. Platform-agnostic: where the OS can't
    // report per-core efficiency classes, every core reports Performance and bHybrid stays false.
    struct FCpuTopology
    {
        uint32       NumLogicalCores = 0;   // total logical processors
        uint32       NumPerformance  = 0;   // P-cores (top efficiency class)
        uint32       NumEfficiency   = 0;   // E-cores (below the top class)
        bool         bHybrid         = false;
        ECpuCoreType CoreTypes[256]  = {};  // indexed by logical processor number; Unknown where absent
    };

    RUNTIME_API const FCpuTopology& GetCpuTopology();

    // Set an environment variable for THIS process (and any child processes it
    // spawns afterward). Does not persist beyond the process lifetime.
    RUNTIME_API bool SetEnvVariable(const FString& Name, const FString& Value);

    // Persist an env var to the user environment so future processes inherit it; idempotent, returns true
    // only on an actual change. No-op returning false where there's no user-environment store.
    RUNTIME_API bool PersistUserEnvVariable(const FString& Name, const FString& Value);

    // Request the OS use its highest-resolution scheduler tick so std::this_thread::sleep_for
    // honors sub-millisecond durations. Paired calls; Disable must follow Enable.
    RUNTIME_API void EnableHighResolutionTiming();
    RUNTIME_API void DisableHighResolutionTiming();

    // Monotonic high-resolution time in seconds since first call; for frame timing and deltas.
    RUNTIME_API double GetTime();

	RUNTIME_API int LaunchProcess(const TCHAR* URL, const TCHAR* Params = nullptr, bool bLaunchDetached = true);
    RUNTIME_API void LaunchURL(const TCHAR* URL);

    // Synchronously run a command line in its own console window; blocks, returns the exit code (-1 if spawn failed).
    RUNTIME_API int RunProcessAndWait(const TCHAR* Executable, const TCHAR* Params, const TCHAR* WorkingDirectory = nullptr);

    // Like RunProcessAndWait but no console; merged stdout+stderr stream back via LineCallback, one line per call.
    // Callback runs on the calling thread (safe for lockless UI updates); returns the exit code (-1 if spawn failed).
    RUNTIME_API int RunProcessAndWaitCapture(const TCHAR* Executable, const TCHAR* Params, const TCHAR* WorkingDirectory, const TFunction<void(FStringView)>& LineCallback);

    RUNTIME_API const TCHAR* ExecutableName(bool bRemoveExtension = true);

    RUNTIME_API size_t GetProcessMemoryUsageBytes();
    RUNTIME_API size_t GetProcessMemoryUsageMegaBytes();
    
    RUNTIME_API const TCHAR* BaseDir();
    
    RUNTIME_API FVoidFuncPtr LumGetProcAddress(void* Handle, const char* Procedure);
    RUNTIME_API void* LoadLibraryWithSearchPaths(const FString& Filename, const TVector<FString>& SearchPaths);

    RUNTIME_API bool OpenFileDialogue(FFixedString& OutFile, const char* Title = "Open File", const char* Filter = nullptr, const char* InitialDir = nullptr);

    // OS shell integration: real impl on the host platform, quiet no-op fallback elsewhere.

    // Open the file manager with this file selected (explorer /select, open -R, etc.).
    RUNTIME_API void ShowFileInExplorer(const TCHAR* Path);

    // Open the file manager at the given directory.
    RUNTIME_API void ShowFolderInExplorer(const TCHAR* Directory);

    // Open the preferred terminal at the directory; no-op (one-shot warning) if none found.
    RUNTIME_API void OpenTerminalAt(const TCHAR* Directory);

    template<typename TCall>
    requires(std::is_pointer_v<TCall> && std::is_function_v<std::remove_pointer_t<TCall>>)
    TCall LumGetProcAddress(void* Handle, const char* Procedure)
    {
        return reinterpret_cast<TCall>(LumGetProcAddress(Handle, Procedure));
    }
}
