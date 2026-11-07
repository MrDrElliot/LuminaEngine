#pragma once

#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina
{
    class FThreadStop;
}

namespace Lumina::Platform
{
    struct FFileDialogResult
    {
        bool    bSuccess = false;
        FString FilePath;
    };
    
    RUNTIME_API void* GetDLLHandle(const TCHAR* Filename);
    RUNTIME_API bool FreeDLLHandle(void* DLLHandle);
    RUNTIME_API void AddDLLDirectory(const FString& Directory);

    RUNTIME_API void PushDLLDirectory(const TCHAR* Directory);

    RUNTIME_API FString GetCurrentProcessPath();

    // The binary holding the Runtime module, which is the executable itself in a monolithic build.
    RUNTIME_API FString GetRuntimeModulePath();

    // Empty when the variable is unset, which this API cannot distinguish from an empty value.
    RUNTIME_API FString GetEnvVariable(FStringView Variable);

    // Applies to this process and children spawned afterward, and an empty value removes the variable.
    RUNTIME_API bool SetEnvVariable(const FString& Name, const FString& Value);

	RUNTIME_API int LaunchProcess(const TCHAR* URL, const TCHAR* Params = nullptr, bool bLaunchDetached = true);
    RUNTIME_API void LaunchURL(const TCHAR* URL);

    // Synchronously run a command line in its own console window; blocks, returns the exit code (-1 if spawn failed).
    RUNTIME_API int RunProcessAndWait(const TCHAR* Executable, const TCHAR* Params, const TCHAR* WorkingDirectory = nullptr);

    // No console, each merged output line goes to LineCallback on the calling thread, and a timeout (ProcessTimedOutExitCode) or a raised Stop kills the whole process tree.
    RUNTIME_API int RunProcessAndWaitCapture(const TCHAR* Executable, const TCHAR* Params, const TCHAR* WorkingDirectory, const TFunction<void(FStringView)>& LineCallback,
                                             uint32 TimeoutMilliseconds = 0, const FThreadStop* Stop = nullptr);

    inline constexpr int ProcessTimedOutExitCode = 124;

    // A child the caller keeps watching, unlike LaunchProcess, which forgets it once started.
    struct FProcessHandle
    {
        void*  Handle    = nullptr;
        uint32 ProcessId = 0;
        int32  ExitCode  = 0;
        bool   bExited   = false;

        bool IsValid() const { return ProcessId != 0; }
    };

    // Starts Executable in WorkingDirectory, or the caller's when null. Returns an invalid handle when it cannot start.
    RUNTIME_API FProcessHandle SpawnProcess(const TCHAR* Executable, const TCHAR* Params, const TCHAR* WorkingDirectory = nullptr);

    // False once the child has exited, after which ExitCode holds its status.
    RUNTIME_API bool IsProcessRunning(FProcessHandle& Process);

    RUNTIME_API void KillProcess(FProcessHandle& Process, int32 ExitCode = 1);

    // Releases the handle without touching the child, which keeps running if it still is.
    RUNTIME_API void CloseProcess(FProcessHandle& Process);

    // Released by the OS when its holder exits, so a crashed holder never strands it.
    struct FInstanceLock
    {
        void* Handle = nullptr;

        bool IsHeld() const { return Handle != nullptr; }
    };

    // False only when another process holds Path, whose id then lands in OutOwnerId, or 0 when unreadable.
    RUNTIME_API bool TryAcquireInstanceLock(const FString& Path, FInstanceLock& OutLock, uint32& OutOwnerId);

    RUNTIME_API void ReleaseInstanceLock(FInstanceLock& Lock);

    // Restores and raises the process's main window. False when it has none or the OS refused the focus change.
    RUNTIME_API bool FocusProcessWindow(uint32 ProcessId);

    // Whether the OS focus is on any window of this process, a floating tool window or a dialog included. True where the platform cannot tell.
    RUNTIME_API bool IsProcessForeground();

    RUNTIME_API const TCHAR* ExecutableName(bool bRemoveExtension = true);

    RUNTIME_API size_t GetProcessMemoryUsageBytes();
    RUNTIME_API size_t GetProcessMemoryUsageMegaBytes();

    // A scan of the process's own address space, straight from the OS. Unlike the category tracker
    // this sees memory no engine allocator ever touched -- the GPU driver, foreign-DLL CRT heaps,
    // code pages -- so it's what turns the profiler's opaque "external" number into named buckets.
    struct FAddressSpaceStats
    {
        // Committed bytes by page type. Private is the interesting one: rpmalloc's spans, the
        // driver's raw VirtualAlloc, thread stacks. Image/Mapped are largely shared, not private.
        uint64 PrivateCommitted = 0;
        uint64 ImageCommitted   = 0;
        uint64 MappedCommitted  = 0;

        // Address space claimed with no RAM behind it (fiber stack reservations, allocator arenas).
        // Costs no memory; listed so a large number here isn't mistaken for a leak.
        uint64 Reserved         = 0;
        uint32 RegionCount      = 0;

        // NT/CRT heaps -- a subset of PrivateCommitted. This is every allocation made by a DLL that
        // does NOT route through Memory::Malloc: slang.dll, the GPU driver, basisu, ucrtbase.
        uint64 HeapCommitted    = 0;   // committed by the heap manager
        uint64 HeapAllocated    = 0;   // of that, handed out to callers
        uint64 HeapOverhead     = 0;   // block headers and alignment slack
        uint32 HeapCount        = 0;
        bool   bHeapWalkValid   = false;
    };

    // Fills Out by walking the address space with VirtualQuery. Cheap (a few ms) but not free --
    // call it on demand, not per frame.
    //
    // bIncludeHeaps additionally walks every NT heap, which is the only way to size the CRT-heap
    // bucket. That walk takes a process-wide heap lock and is O(number of live blocks), so on a
    // multi-GB heap it can stall every other thread for seconds. Strictly opt-in.
    RUNTIME_API void GetAddressSpaceStats(FAddressSpaceStats& Out, bool bIncludeHeaps);


    RUNTIME_API const TCHAR* BaseDir();
    
    RUNTIME_API FVoidFuncPtr LumGetProcAddress(void* Handle, const char* Procedure);
    RUNTIME_API void* LoadLibraryWithSearchPaths(const FString& Filename, const TVector<FString>& SearchPaths);

    RUNTIME_API bool OpenFileDialogue(FFixedString& OutFile, const char* Title = "Open File", const char* Filter = nullptr, const char* InitialDir = nullptr);

    // Multi-select. Empty selection reports false, so callers need not check the vector as well.
    RUNTIME_API bool OpenFileDialogueMulti(TVector<FFixedString>& OutFiles, const char* Title = "Open Files", const char* Filter = nullptr, const char* InitialDir = nullptr);

    // OS shell integration: real impl on the host platform, quiet no-op fallback elsewhere.

    // Open the file manager with this file selected (explorer /select, open -R, etc.).
    RUNTIME_API void ShowFileInExplorer(const TCHAR* Path);

    // Open the file manager at the given directory.
    RUNTIME_API void ShowFolderInExplorer(const TCHAR* Directory);

    // Open the preferred terminal at the directory; no-op (one-shot warning) if none found.
    RUNTIME_API void OpenTerminalAt(const TCHAR* Directory);

    // Open a source file in the user's code editor. Line is honored only by editors that take one, and 0 means the file alone.
    RUNTIME_API void OpenSourceFile(const TCHAR* Path, int32 Line);

    template<typename TCall>
    requires(std::is_pointer_v<TCall> && std::is_function_v<std::remove_pointer_t<TCall>>)
    TCall LumGetProcAddress(void* Handle, const char* Procedure)
    {
        return reinterpret_cast<TCall>(LumGetProcAddress(Handle, Procedure));
    }
}
