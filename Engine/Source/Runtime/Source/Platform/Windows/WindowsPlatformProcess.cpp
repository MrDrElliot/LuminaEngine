#include "RuntimePCH.h"
#ifdef _WIN32

#include <string>
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Core/Threading/Thread.h"
// The lean-and-mean macro is already defined workspace-wide, so guarding on it would skip the include.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <tchar.h>
#include <PathCch.h>  // For PathFindFileName
#include <psapi.h>
#include <Shlwapi.h>

#include <timeapi.h>
#include "Log/Log.h"

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "PathCch.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "Advapi32.lib")

namespace Lumina::Platform
{
    namespace
    {
        TVector<FString> GDLLSearchPaths;

        struct FScopedComApartment
        {
            const HRESULT Result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

            ~FScopedComApartment()
            {
                if (SUCCEEDED(Result))
                {
                    CoUninitialize();
                }
            }

            FScopedComApartment() = default;
            FScopedComApartment(const FScopedComApartment&) = delete;
            FScopedComApartment& operator=(const FScopedComApartment&) = delete;
        };

        void BuildDialogFilters(const char* Filter, TVector<FWString>& Storage, TVector<COMDLG_FILTERSPEC>& FileTypes)
        {
            if (Filter == nullptr)
            {
                return;
            }

            const char* Cursor = Filter;
            while (*Cursor != '\0')
            {
                Storage.Emplace(UTF8_TO_TCHAR(Cursor));
                Cursor += strlen(Cursor) + 1;
                Storage.Emplace(UTF8_TO_TCHAR(Cursor));
                Cursor += strlen(Cursor) + 1;
            }

            FileTypes.Reserve(Storage.Num() / 2);
            for (int32 Index = 0; Index < Storage.Num(); Index += 2)
            {
                FileTypes.Emplace(*Storage[Index], *Storage[Index + 1]);
            }
        }

        // GDLLSearchPaths only locates the file asked for, not the imports the loader resolves first.
        void RegisterLoaderDirectory(const FString& Directory)
        {
            FWString Wide = StringUtils::ToWideString(Directory);

            if (::AddDllDirectory(*Wide) == nullptr)
            {
                LOG_WARN("AddDllDirectory failed for '{0}' (error {1})", Directory, (uint32)GetLastError());
            }
        }

        // Falls back to the default order, since that flag set drops PATH and vendored deps rely on it.
        void* LoadModuleImage(const FWString& Wide)
        {
            constexpr DWORD SearchFlags =
                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                | LOAD_LIBRARY_SEARCH_APPLICATION_DIR
                | LOAD_LIBRARY_SEARCH_USER_DIRS
                | LOAD_LIBRARY_SEARCH_SYSTEM32;

            if (HMODULE Handle = LoadLibraryExW(*Wide, nullptr, SearchFlags))
            {
                return Handle;
            }

            const DWORD SearchError = GetLastError();

            if (HMODULE Handle = LoadLibraryW(*Wide))
            {
                return Handle;
            }

            // Error 126 means a dependency is missing rather than this file, which we resolved a path to.
            LOG_ERROR("LoadLibrary failed for '{0}' (error {1}, {2} with default search order)",
                StringUtils::FromWideString(Wide), (uint32)SearchError, (uint32)GetLastError());

            return nullptr;
        }
    }

    void* GetDLLHandle(const TCHAR* Filename)
    {
        FWString WideString = Filename;
        TVector<FString> SearchPaths = GDLLSearchPaths;

        
        return LoadLibraryWithSearchPaths(StringUtils::FromWideString(WideString), SearchPaths);
    }

    bool FreeDLLHandle(void* DLLHandle)
    {
        return FreeLibrary((HMODULE)DLLHandle);
    }

    void* GetDLLExport(void* DLLHandle, const char* ProcName)
    {
        return (void*)::GetProcAddress((HMODULE)DLLHandle, ProcName);
    }

    void AddDLLDirectory(const FString& Directory)
    {
        for (const FString& Existing : GDLLSearchPaths)
        {
            if (Existing == Directory)
            {
                return;
            }
        }

        GDLLSearchPaths.Add(Directory);
        RegisterLoaderDirectory(Directory);
    }

    void PushDLLDirectory(const TCHAR* Directory)
    {
        SetDllDirectory(Directory);

        FString Narrow = StringUtils::FromWideString(Directory);

        // SetDllDirectory is ignored once search flags are passed, so both have to agree.
        RegisterLoaderDirectory(Narrow);

        GDLLSearchPaths.Add(Narrow);

        LOG_INFO("Pushing DLL Search Path: {0}", Narrow);
    }

    void PopDLLDirectory()
    {
        GDLLSearchPaths.Pop();

        if (GDLLSearchPaths.IsEmpty())
        {
            SetDllDirectory(L"");
        }
        else
        {
            SetDllDirectory(*StringUtils::ToWideString(GDLLSearchPaths.Last()));
        }
    }

    uint32 GetCurrentProcessID()
    {
        return 0;
    }

    uint32 GetCurrentCoreNumber()
    {
        return ::GetCurrentProcessorNumber();
    }

    const FCpuTopology& GetCpuTopology()
    {
        static const FCpuTopology Topo = []
        {
            FCpuTopology T;
            for (ECpuCoreType& C : T.CoreTypes) C = ECpuCoreType::Unknown;

            DWORD Len = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &Len);

            TVector<uint8> Buffer(Len);
            const bool bOk = Len != 0
                && GetLogicalProcessorInformationEx(RelationProcessorCore,
                       reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(Buffer.GetData()), &Len);

            if (!bOk)
            {
                // With no topology, treat every logical core as a performance core.
                const DWORD N = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
                T.NumLogicalCores = N < 256 ? N : 256;
                T.NumPerformance  = T.NumLogicalCores;
                for (uint32 i = 0; i < T.NumLogicalCores; ++i) T.CoreTypes[i] = ECpuCoreType::Performance;
                return T;
            }

            // A higher efficiency class is the more performant core, so the max class is the P-cores.
            uint8 EffClass[256];
            memset(EffClass, 0xFF, sizeof(EffClass));
            uint8 MaxClass = 0;

            uint8* Cursor = Buffer.GetData();
            uint8* End    = Buffer.GetData() + Len;
            while (Cursor < End)
            {
                auto* Info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(Cursor);
                if (Info->Relationship == RelationProcessorCore)
                {
                    const PROCESSOR_RELATIONSHIP& PR = Info->Processor;
                    if (PR.EfficiencyClass > MaxClass) MaxClass = PR.EfficiencyClass;
                    for (WORD g = 0; g < PR.GroupCount; ++g)
                    {
                        const KAFFINITY Mask = PR.GroupMask[g].Mask;
                        for (uint32 Bit = 0; Bit < sizeof(KAFFINITY) * 8; ++Bit)
                        {
                            if ((Mask & (static_cast<KAFFINITY>(1) << Bit)) == 0) continue;
                            const uint32 Logical = static_cast<uint32>(PR.GroupMask[g].Group) * 64u + Bit;
                            if (Logical < 256) EffClass[Logical] = PR.EfficiencyClass;
                        }
                    }
                }
                Cursor += Info->Size;
            }

            // The top class is the P-cores and anything below it is an E-core.
            for (uint32 i = 0; i < 256; ++i)
            {
                if (EffClass[i] == 0xFF) continue;
                ++T.NumLogicalCores;
                if (EffClass[i] >= MaxClass) { T.CoreTypes[i] = ECpuCoreType::Performance; ++T.NumPerformance; }
                else                         { T.CoreTypes[i] = ECpuCoreType::Efficiency;  ++T.NumEfficiency;  }
            }
            T.bHybrid = T.NumEfficiency > 0;
            return T;
        }();
        return Topo;
    }

    FString GetEnvVariable(FStringView Variable)
    {
        const auto WideName = StringCast<WIDECHAR>(Variable.GetData(), Variable.Len());

        TVector<wchar_t, 512> Buffer(512);
        for (;;)
        {
            const DWORD Length = ::GetEnvironmentVariableW(WideName.Get(), Buffer.GetData(), static_cast<DWORD>(Buffer.Num()));
            if (Length == 0)
            {
                return {};
            }

            if (Length < static_cast<DWORD>(Buffer.Num()))
            {
                const auto Narrow = StringCast<ANSICHAR>(Buffer.GetData(), static_cast<int32>(Length));
                return FString(Narrow.Get(), Narrow.Length());
            }

            // An overflowed call reports the size including the terminator, so resizing to it always fits.
            Buffer.SetNum(Length);
        }
    }

    FString GetRuntimeModulePath()
    {
        HMODULE Module = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&GetRuntimeModulePath), &Module))
        {
            return "";
        }

        char Buffer[MAX_PATH];
        const DWORD Length = GetModuleFileNameA(Module, Buffer, MAX_PATH);
        return Length == 0 ? FString() : FString(Buffer, Length);
    }

    FString GetCurrentProcessPath()
    {
        char Buffer[MAX_PATH];
        DWORD Length = GetModuleFileNameA(nullptr, Buffer, MAX_PATH);
        if (Length == 0)
        {
            return "";
        }
        
        return FString(Buffer, Length);
    }

    bool SetEnvVariable(const FString& Name, const FString& Value)
    {
        // An empty or '='-bearing name trips the CRT invalid-parameter handler, which aborts in debug.
        if (Name.IsEmpty() || Name.Find('=') != Constants::kIndexNone)
        {
            LOG_WARN("Refusing to set environment variable with invalid name '{}'", Name);
            return false;
        }

        const auto WideName = StringCast<WIDECHAR>(*Name, Name.Len());
        const auto WideValue = StringCast<WIDECHAR>(*Value, Value.Len());

        // The CRT block backs getenv in third-party code, the Win32 block is what a spawned child inherits.
        const bool bCrtOk = _wputenv_s(WideName.Get(), WideValue.Get()) == 0;
        const bool bWin32Ok = ::SetEnvironmentVariableW(WideName.Get(), WideValue.Get()) != 0;

        if (!bCrtOk || !bWin32Ok)
        {
            LOG_WARN("Failed to set environment variable {} (crt={}, win32={})", Name, bCrtOk, bWin32Ok);
            return false;
        }

        LOG_TRACE("Environment variable {} set to {}", Name, Value);
        return true;
    }

    int LaunchProcess(const TCHAR* URL, const TCHAR* Params, bool bLaunchDetached)
    {
        if (!URL || URL[0] == 0)
        {
            return -1;
        }

        FWString URLString(URL);
        if (URLString.Len() >= 2 && URLString[0] == L'"' && URLString.Last() == L'"')
        {
            URLString = URLString.Mid(1, URLString.Len() - 2);
        }
        if (URLString.IsEmpty())
        {
            return ERROR_INVALID_PARAMETER;
        }
        Algo::Replace(URLString, '/', '\\');

        if (URLString.Find(L'\\') == Constants::kIndexNone)
        {
            const DWORD Required = SearchPathW(nullptr, *URLString, L".exe", 0, nullptr, nullptr);
            if (Required == 0)
            {
                return static_cast<int>(GetLastError());
            }
            TVector<wchar_t> Resolved(Required);
            const DWORD Written = SearchPathW(nullptr, *URLString, L".exe", Required, Resolved.GetData(), nullptr);
            if (Written == 0)
            {
                return static_cast<int>(GetLastError());
            }
            if (Written >= Required)
            {
                return ERROR_INSUFFICIENT_BUFFER;
            }
            URLString = FWString(Resolved.GetData(), static_cast<int32>(Written));
        }

        FWString CommandLine = L"\"";
        CommandLine += URLString;
        CommandLine += L"\"";
        
        STARTUPINFOW si{};
        PROCESS_INFORMATION pi{};

        si.cb = sizeof(si);

        if (Params)
        {
            CommandLine += L" ";
            CommandLine += Params;
        }

        // Detached here means the child owns its own console and outlives the parent.
        DWORD creationFlags = 0;
        if (bLaunchDetached)
        {
            creationFlags |= CREATE_NEW_CONSOLE;
        }

        // CreateProcessW modifies the command line string, so make a writable buffer
        TVector<wchar_t> cmdBuffer(CommandLine.begin(), CommandLine.end());
        cmdBuffer.Add(L'\0');

        BOOL result = CreateProcessW(
            *URLString,
            cmdBuffer.GetData(),         // lpCommandLine
            nullptr,                  // lpProcessAttributes
            nullptr,                  // lpThreadAttributes
            FALSE,                    // bInheritHandles
            creationFlags,            // dwCreationFlags
            nullptr,                  // lpEnvironment
            nullptr,                  // lpCurrentDirectory
            &si,                      // lpStartupInfo
            &pi                       // lpProcessInformation
        );

        if (!result)
        {
            return static_cast<int>(GetLastError());
        }

        // Optionally detach from our process
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        return 0; // Success
    }
    
    void LaunchURL(const TCHAR* URL)
    {
        ShellExecuteW(nullptr, TEXT("open"), URL, nullptr, nullptr, SW_SHOWNORMAL);
    }

    FProcessHandle SpawnProcess(const TCHAR* Executable, const TCHAR* Params, const TCHAR* WorkingDirectory)
    {
        FProcessHandle Result;
        if (Executable == nullptr)
        {
            return Result;
        }

        FWString CmdLine = TEXT("\"");
        CmdLine += Executable;
        CmdLine += TEXT("\"");
        if (Params != nullptr && Params[0] != 0)
        {
            CmdLine += TEXT(" ");
            CmdLine += Params;
        }

        TVector<wchar_t> CmdBuffer(CmdLine.begin(), CmdLine.end());
        CmdBuffer.Add(L'\0');

        STARTUPINFOW StartupInfo{};
        StartupInfo.cb = sizeof(StartupInfo);
        PROCESS_INFORMATION ProcessInfo{};

        if (!CreateProcessW(nullptr, CmdBuffer.GetData(), nullptr, nullptr, FALSE, 0, nullptr, WorkingDirectory, &StartupInfo, &ProcessInfo))
        {
            return Result;
        }

        CloseHandle(ProcessInfo.hThread);
        Result.Handle    = ProcessInfo.hProcess;
        Result.ProcessId = (uint32)ProcessInfo.dwProcessId;
        return Result;
    }

    bool IsProcessRunning(FProcessHandle& Process)
    {
        if (Process.bExited || Process.Handle == nullptr)
        {
            return false;
        }

        if (WaitForSingleObject((HANDLE)Process.Handle, 0) == WAIT_TIMEOUT)
        {
            return true;
        }

        DWORD Code = 0;
        GetExitCodeProcess((HANDLE)Process.Handle, &Code);
        Process.ExitCode = (int32)Code;
        Process.bExited  = true;
        return false;
    }

    void KillProcess(FProcessHandle& Process, int32 ExitCode)
    {
        if (IsProcessRunning(Process))
        {
            ::TerminateProcess((HANDLE)Process.Handle, (UINT)ExitCode);
            WaitForSingleObject((HANDLE)Process.Handle, 5000);
            IsProcessRunning(Process);
        }
    }

    void CloseProcess(FProcessHandle& Process)
    {
        if (Process.Handle != nullptr)
        {
            CloseHandle((HANDLE)Process.Handle);
        }
        Process = FProcessHandle();
    }

    bool TryAcquireInstanceLock(const FString& Path, FInstanceLock& OutLock, uint32& OutOwnerId)
    {
        OutOwnerId = 0;
        const FWString WidePath = StringUtils::ToWideString(Path);

        // Shared for reading, which is how a second process learns who holds it.
        const HANDLE File = CreateFileW(*WidePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (File == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() != ERROR_SHARING_VIOLATION)
            {
                LOG_WARN("Could not create the instance lock '{}' (system error {}), so it is not enforced.", Path, GetLastError());
                return true;
            }

            const HANDLE Reader = CreateFileW(*WidePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (Reader != INVALID_HANDLE_VALUE)
            {
                char Buffer[16] = {};
                DWORD Read = 0;
                (void)ReadFile(Reader, Buffer, sizeof(Buffer) - 1, &Read, nullptr);
                CloseHandle(Reader);
                OutOwnerId = (uint32)strtoul(Buffer, nullptr, 10);
            }
            return false;
        }

        char Buffer[16] = {};
        const int Length = snprintf(Buffer, sizeof(Buffer), "%lu", GetCurrentProcessId());
        DWORD Written = 0;
        (void)WriteFile(File, Buffer, (DWORD)Length, &Written, nullptr);
        (void)SetEndOfFile(File);

        OutLock.Handle = File;
        return true;
    }

    void ReleaseInstanceLock(FInstanceLock& Lock)
    {
        if (Lock.Handle != nullptr)
        {
            CloseHandle((HANDLE)Lock.Handle);
            Lock.Handle = nullptr;
        }
    }

    bool IsProcessForeground()
    {
        const HWND Foreground = GetForegroundWindow();
        if (Foreground == nullptr)
        {
            return false;
        }
        DWORD Owner = 0;
        GetWindowThreadProcessId(Foreground, &Owner);
        return Owner == GetCurrentProcessId();
    }

    bool FocusProcessWindow(uint32 ProcessId)
    {
        struct FSearch
        {
            DWORD ProcessId;
            HWND  Found;
        };
        FSearch Search{ ProcessId, nullptr };

        // Top-level windows come in z-order, so the first visible unowned one is what the user last saw.
        EnumWindows([](HWND Window, LPARAM Param) -> BOOL
        {
            FSearch& Search = *reinterpret_cast<FSearch*>(Param);
            DWORD Owner = 0;
            GetWindowThreadProcessId(Window, &Owner);
            if (Owner != Search.ProcessId || !IsWindowVisible(Window) || GetWindow(Window, GW_OWNER) != nullptr)
            {
                return TRUE;
            }
            Search.Found = Window;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&Search));

        if (Search.Found == nullptr)
        {
            return false;
        }

        if (IsIconic(Search.Found))
        {
            ShowWindow(Search.Found, SW_RESTORE);
        }

        if (SetForegroundWindow(Search.Found))
        {
            return true;
        }

        // Windows only lets the foreground process move focus, so a refusal flashes the taskbar button instead.
        FLASHWINFO Flash{ sizeof(FLASHWINFO), Search.Found, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&Flash);
        return false;
    }

    int RunProcessAndWaitCapture(const TCHAR* Executable, const TCHAR* Params, const TCHAR* WorkingDirectory, const TFunction<void(FStringView)>& LineCallback,
                                 uint32 TimeoutMilliseconds)
    {
        if (!Executable)
        {
            return -1;
        }

        // Build command line.
        FWString CmdLine = TEXT("\"");
        CmdLine += Executable;
        CmdLine += TEXT("\"");
        if (Params && Params[0])
        {
            CmdLine += TEXT(" ");
            CmdLine += Params;
        }

        TVector<wchar_t> CmdBuffer(CmdLine.begin(), CmdLine.end());
        CmdBuffer.Add(L'\0');

        // Write end is inheritable for both stdout+stderr; stderr merged inline for build log.
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE ReadEnd = nullptr;
        HANDLE WriteEnd = nullptr;
        // Sized so a chatty child rarely blocks on a full pipe while the loop below sleeps on an empty one.
        constexpr DWORD CapturePipeBytes = 1u << 20;
        if (!CreatePipe(&ReadEnd, &WriteEnd, &sa, CapturePipeBytes))
        {
            return -1;
        }
        // The parent's read end must NOT be inherited by the child.
        SetHandleInformation(ReadEnd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput  = nullptr;
        si.StartupInfo.hStdOutput = WriteEnd;
        si.StartupInfo.hStdError  = WriteEnd;

        // Only the pipe is handed down, since a child that inherits a server's listening socket keeps its port bound after we exit.
        SIZE_T AttributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeBytes);
        TVector<uint8> AttributeStorage(static_cast<int32>(AttributeBytes));
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeStorage.GetData());
        HANDLE InheritedHandles[] = { WriteEnd };
        const bool bAttributeList = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &AttributeBytes) != FALSE;
        const bool bHandleList = bAttributeList
            && UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         InheritedHandles, sizeof(InheritedHandles), nullptr, nullptr);
        if (bAttributeList && !bHandleList)
        {
            DeleteProcThreadAttributeList(si.lpAttributeList);
        }
        if (!bHandleList)
        {
            si.StartupInfo.cb = sizeof(STARTUPINFOW);
            si.lpAttributeList = nullptr;
        }

        PROCESS_INFORMATION pi{};

        // The editor is not a console app and output is captured, so suppress the popup.
        const DWORD CreationFlags = CREATE_NO_WINDOW | (bHandleList ? EXTENDED_STARTUPINFO_PRESENT : 0);

        BOOL ok = CreateProcessW(
            nullptr,
            CmdBuffer.GetData(),
            nullptr,
            nullptr,
            TRUE,                     // bInheritHandles must be TRUE for pipe
            CreationFlags,
            nullptr,
            WorkingDirectory,
            &si.StartupInfo,
            &pi);

        if (bHandleList)
        {
            DeleteProcThreadAttributeList(si.lpAttributeList);
        }

        // Release parent's write end; without this ReadFile blocks forever after child exits.
        CloseHandle(WriteEnd);

        if (!ok)
        {
            CloseHandle(ReadEnd);
            return -1;
        }

        // Killing a stuck child closes its end of the pipe, which is what lets the blocking read below return.
        FThread Watchdog;
        if (TimeoutMilliseconds > 0)
        {
            const HANDLE Process = pi.hProcess;
            Watchdog = FThread([Process, TimeoutMilliseconds]()
            {
                if (WaitForSingleObject(Process, TimeoutMilliseconds) == WAIT_TIMEOUT)
                {
                    TerminateProcess(Process, (UINT)ProcessTimedOutExitCode);
                }
            });
        }

        FString Pending;
        TVector<char> ReadBuf(64 * 1024);
        DWORD BytesRead = 0;

        // Done once the child exits and the pipe drains, since a grandchild like mspdbsrv can hold the write end open for minutes.
        for (;;)
        {
            DWORD Available = 0;
            if (!PeekNamedPipe(ReadEnd, nullptr, 0, nullptr, &Available, nullptr))
            {
                break;
            }
            if (Available == 0)
            {
                if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0
                    && PeekNamedPipe(ReadEnd, nullptr, 0, nullptr, &Available, nullptr) && Available == 0)
                {
                    break;
                }
                continue;
            }
            const DWORD ToRead = Available < (DWORD)ReadBuf.Num() ? Available : (DWORD)ReadBuf.Num();
            if (!ReadFile(ReadEnd, ReadBuf.GetData(), ToRead, &BytesRead, nullptr) || BytesRead == 0)
            {
                break;
            }
            Pending.Append(ReadBuf.GetData(), static_cast<int32>(BytesRead));

            int32 Cursor = 0;
            for (;;)
            {
                const int32 NewlineIdx = Pending.Find('\n', Cursor);
                if (NewlineIdx == Constants::kIndexNone)
                {
                    break;
                }

                int32 LineEnd = NewlineIdx;
                // Strip trailing CR for CRLF lines.
                if (LineEnd > Cursor && Pending[LineEnd - 1] == '\r')
                {
                    --LineEnd;
                }

                if (LineCallback)
                {
                    LineCallback(FStringView(Pending.GetData() + Cursor, LineEnd - Cursor));
                }
                Cursor = NewlineIdx + 1;
            }

            if (Cursor > 0)
            {
                Pending.RemoveAt(0, Cursor);
            }
        }

        // Final partial line (no trailing newline).
        if (!Pending.IsEmpty() && LineCallback)
        {
            LineCallback(Pending.View());
        }

        CloseHandle(ReadEnd);

        WaitForSingleObject(pi.hProcess, INFINITE);
        if (Watchdog.Joinable())
        {
            Watchdog.Join();
        }
        DWORD ExitCode = 1;
        GetExitCodeProcess(pi.hProcess, &ExitCode);

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        return (int)ExitCode;
    }

    int RunProcessAndWait(const TCHAR* Executable, const TCHAR* Params, const TCHAR* WorkingDirectory)
    {
        if (!Executable)
        {
            return -1;
        }

        FWString CmdLine = TEXT("\"");
        CmdLine += Executable;
        CmdLine += TEXT("\"");
        if (Params && Params[0])
        {
            CmdLine += TEXT(" ");
            CmdLine += Params;
        }

        TVector<wchar_t> CmdBuffer(CmdLine.begin(), CmdLine.end());
        CmdBuffer.Add(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        // CREATE_NEW_CONSOLE keeps child stdout out of the editor window; visible in spawned console.
        const DWORD CreationFlags = CREATE_NEW_CONSOLE;

        BOOL ok = CreateProcessW(
            nullptr,
            CmdBuffer.GetData(),
            nullptr,
            nullptr,
            FALSE,
            CreationFlags,
            nullptr,
            WorkingDirectory,
            &si,
            &pi);

        if (!ok)
        {
            return -1;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD ExitCode = 1;
        GetExitCodeProcess(pi.hProcess, &ExitCode);

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        return (int)ExitCode;
    }

    const TCHAR* ExecutableName(bool bRemoveExtension)
    {
        static TCHAR ExecutablePath[MAX_PATH];
    
        if (GetModuleFileName(NULL, ExecutablePath, MAX_PATH) == 0)
        {
            return nullptr;
        }

        TCHAR* ExecutableName = PathFindFileName(ExecutablePath);

        // If bRemoveExtension is true, remove the file extension
        if (bRemoveExtension)
        {
            PathCchRemoveExtension(ExecutableName, MAX_PATH);
        }

        return ExecutableName;
    }

    size_t GetProcessMemoryUsageBytes()
    {
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        {
            return pmc.PrivateUsage;
        }
        return 0;
    }

    void GetAddressSpaceStats(FAddressSpaceStats& Out, bool bIncludeHeaps)
    {
        Out = FAddressSpaceStats();

        SYSTEM_INFO SystemInfo;
        GetSystemInfo(&SystemInfo);

        const uint8* Address = static_cast<const uint8*>(SystemInfo.lpMinimumApplicationAddress);
        const uint8* const MaxAddress = static_cast<const uint8*>(SystemInfo.lpMaximumApplicationAddress);

        MEMORY_BASIC_INFORMATION Info;
        while (Address < MaxAddress && VirtualQuery(Address, &Info, sizeof(Info)) == sizeof(Info))
        {
            // A zero-length region can't be stepped over; bail rather than spin forever.
            if (Info.RegionSize == 0)
            {
                break;
            }

            const uint64 RegionSize = static_cast<uint64>(Info.RegionSize);
            ++Out.RegionCount;

            switch (Info.State)
            {
            case MEM_COMMIT:
                switch (Info.Type)
                {
                case MEM_IMAGE:   Out.ImageCommitted   += RegionSize; break;
                case MEM_MAPPED:  Out.MappedCommitted  += RegionSize; break;
                default:          Out.PrivateCommitted += RegionSize; break;
                }
                break;

            case MEM_RESERVE:
                Out.Reserved += RegionSize;
                break;

            default:
                break;
            }

            Address += RegionSize;
        }

        if (!bIncludeHeaps)
        {
            return;
        }

        // 256 is far beyond any real process, and overflowing means missing heaps, not reading out of bounds.
        constexpr DWORD MaxHeaps = 256;
        HANDLE Heaps[MaxHeaps];
        const DWORD NumHeaps = GetProcessHeaps(MaxHeaps, Heaps);
        Out.HeapCount = static_cast<uint32>(NumHeaps < MaxHeaps ? NumHeaps : MaxHeaps);
        Out.bHeapWalkValid = Out.HeapCount > 0;

        for (uint32 HeapIndex = 0; HeapIndex < Out.HeapCount; ++HeapIndex)
        {
            const HANDLE Heap = Heaps[HeapIndex];

            // The lock is recursive on this thread, so our own incidental allocations are safe. Do not add any.
            if (!HeapLock(Heap))
            {
                Out.bHeapWalkValid = false;
                continue;
            }

            PROCESS_HEAP_ENTRY Entry;
            Entry.lpData = nullptr;

            while (HeapWalk(Heap, &Entry))
            {
                if (Entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
                {
                    Out.HeapAllocated += Entry.cbData;
                    Out.HeapOverhead  += Entry.cbOverhead;
                }
                else if (Entry.wFlags & PROCESS_HEAP_REGION)
                {
                    Out.HeapCommitted += Entry.Region.dwCommittedSize;
                }
            }

            HeapUnlock(Heap);
        }

        // The heap manager's own reservations live outside any region, so never under-report committed.
        const uint64 HeapInUse = Out.HeapAllocated + Out.HeapOverhead;
        if (HeapInUse > Out.HeapCommitted)
        {
            Out.HeapCommitted = HeapInUse;
        }
    }

    size_t GetProcessMemoryUsageMegaBytes()
    {
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        {
            return pmc.PrivateUsage / Constants::kMiB;
        }
        return 0;
    }


    const TCHAR* BaseDir()
    {
        static TCHAR Buffer[MAX_PATH] = {};
        if (Buffer[0] == 0)
        {
            GetModuleFileNameW(nullptr, Buffer, MAX_PATH);
        }
        return Buffer;
    }

    FVoidFuncPtr LumGetProcAddress(void* Handle, const char* Procedure)
    {
        return reinterpret_cast<FVoidFuncPtr>(GetProcAddress((HMODULE)Handle, Procedure));
    }

    void* LoadLibraryWithSearchPaths(const FString& Filename, const TVector<FString>& SearchPaths)
    {
        FWString Wide = StringUtils::ToWideString(Filename);
        if (void* Handle = GetModuleHandleW(*Wide))
        {
            return Handle;
        }

        if (void* Handle = LoadModuleImage(Wide))
        {
            return Handle;
        }

        for (const FString& Path : SearchPaths)
        {
            FFixedString FullPath = Paths::Combine(Path, Filename);
            if (Paths::Exists(FullPath))
            {
                if (void* Handle = LoadModuleImage(StringUtils::ToWideString(FullPath)))
                {
                    return Handle;
                }
            }
        }

        return nullptr;
    }

    bool OpenFileDialogue(FFixedString& OutFile, const char* Title, const char* Filter, const char* InitialDir)
    {
        FScopedComApartment Apartment;
        if (FAILED(Apartment.Result))
        {
            LOG_ERROR("Could not initialize COM for the file dialog (HRESULT {}).", static_cast<uint32>(Apartment.Result));
            return false;
        }

        IFileDialog* FileDialog = nullptr;
        HRESULT Result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileDialog, reinterpret_cast<void**>(&FileDialog));

        if (FAILED(Result))
        {
            LOG_ERROR("Could not create the file dialog (HRESULT {}).", static_cast<uint32>(Result));
            return false;
        }
        
        DWORD Options = 0;
        FileDialog->GetOptions(&Options);

        if (!Filter)
        {
            FileDialog->SetOptions(Options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        }
        else
        {
            FileDialog->SetOptions(Options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        }

        if (Title)
        {
            FileDialog->SetTitle(UTF8_TO_TCHAR(Title));
        }

        if (InitialDir)
        {
            IShellItem* pFolder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(UTF8_TO_TCHAR(InitialDir), nullptr, IID_PPV_ARGS(&pFolder))))
            {
                FileDialog->SetFolder(pFolder);
                pFolder->Release();
            }
        }
        
        TVector<COMDLG_FILTERSPEC> fileTypes;
        TVector<FWString> StringStorage;

        BuildDialogFilters(Filter, StringStorage, fileTypes);
        if (!fileTypes.IsEmpty())
        {
            FileDialog->SetFileTypes(static_cast<UINT>(fileTypes.Num()), fileTypes.GetData());
        }
        bool bResult = false;
        if (SUCCEEDED(FileDialog->Show(nullptr)))
        {
            IShellItem* Item = nullptr;
            if (SUCCEEDED(FileDialog->GetResult(&Item)))
            {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)))
                {
                    FWString wPath = pszPath;

                    OutFile = TCHAR_TO_UTF8(*wPath);
                    Algo::Replace(OutFile, '\\', '/');

                    CoTaskMemFree(pszPath);
                    bResult = true;
                }
                Item->Release();
            }
        }

        FileDialog->Release();
        return bResult;
    }

    // â”€â”€â”€ OS shell integration â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    bool OpenFileDialogueMulti(TVector<FFixedString>& OutFiles, const char* Title, const char* Filter, const char* InitialDir)
    {
        OutFiles.Reset();
        FScopedComApartment Apartment;
        if (FAILED(Apartment.Result))
        {
            LOG_ERROR("Could not initialize COM for the file dialog (HRESULT {}).", static_cast<uint32>(Apartment.Result));
            return false;
        }

        IFileOpenDialog* Dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&Dialog))))
        {
            return false;
        }

        DWORD Options = 0;
        Dialog->GetOptions(&Options);
        Dialog->SetOptions(Options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);

        if (Title)
        {
            Dialog->SetTitle(UTF8_TO_TCHAR(Title));
        }

        if (InitialDir)
        {
            IShellItem* Folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(UTF8_TO_TCHAR(InitialDir), nullptr, IID_PPV_ARGS(&Folder))))
            {
                Dialog->SetFolder(Folder);
                Folder->Release();
            }
        }

        // Same double-null-terminated "Name\0Spec\0..." form the single-file dialogue takes.
        TVector<COMDLG_FILTERSPEC> FileTypes;
        TVector<FWString>          Storage;

        BuildDialogFilters(Filter, Storage, FileTypes);

        if (!FileTypes.IsEmpty())
        {
            Dialog->SetFileTypes(static_cast<UINT>(FileTypes.Num()), FileTypes.GetData());
        }

        if (SUCCEEDED(Dialog->Show(nullptr)))
        {
            IShellItemArray* Items = nullptr;
            if (SUCCEEDED(Dialog->GetResults(&Items)))
            {
                DWORD Count = 0;
                Items->GetCount(&Count);

                for (DWORD Index = 0; Index < Count; ++Index)
                {
                    IShellItem* Item = nullptr;
                    if (!SUCCEEDED(Items->GetItemAt(Index, &Item)))
                    {
                        continue;
                    }

                    PWSTR Raw = nullptr;
                    if (SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Raw)))
                    {
                        FFixedString Path = TCHAR_TO_UTF8(*FWString(Raw));
                        Algo::Replace(Path, '\\', '/');
                        OutFiles.Add(Path);
                        CoTaskMemFree(Raw);
                    }

                    Item->Release();
                }

                Items->Release();
            }
        }

        Dialog->Release();

        return !OutFiles.IsEmpty();
    }

    void ShowFileInExplorer(const TCHAR* Path)
    {
        if (!Path || !Path[0])
        {
            return;
        }

        // Normalize to backslashes and quote, explorer is picky about both.
        FWString Normalized(Path);
        Algo::Replace(Normalized, L'/', L'\\');

        FWString Args = L"/select,\"";
        Args += Normalized;
        Args += L"\"";

        ShellExecuteW(nullptr, L"open", L"explorer.exe", *Args, nullptr, SW_SHOWNORMAL);
    }

    void ShowFolderInExplorer(const TCHAR* Directory)
    {
        if (!Directory || !Directory[0])
        {
            return;
        }

        FWString Normalized(Directory);
        Algo::Replace(Normalized, L'/', L'\\');

        ShellExecuteW(nullptr, L"open", *Normalized, nullptr, nullptr, SW_SHOWNORMAL);
    }

    namespace
    {
        // ShellExecute reports anything above 32 as launched, which is how a missing editor is detected.
        bool LaunchCodeEditor(const wchar_t* Program, const FWString& Arguments)
        {
            const HINSTANCE Result = ShellExecuteW(nullptr, L"open", Program, *Arguments, nullptr, SW_SHOWNORMAL);
            return reinterpret_cast<INT_PTR>(Result) > 32;
        }
    }

    void OpenSourceFile(const TCHAR* Path, int32 Line)
    {
        if (!Path || !Path[0])
        {
            return;
        }

        FWString Normalized(Path);
        Algo::Replace(Normalized, L'/', L'\\');

        if (Line > 0)
        {
            wchar_t LineText[16];
            _snwprintf_s(LineText, _TRUNCATE, L"%d", Line);

            if (LaunchCodeEditor(L"rider64.exe", FWString(L"--line ") + LineText + L" \"" + Normalized + L"\""))
            {
                return;
            }
            if (LaunchCodeEditor(L"code.cmd", FWString(L"--goto \"") + Normalized + L":" + LineText + L"\""))
            {
                return;
            }
        }

        // Whatever is registered for the extension, which lands on the file but not the line.
        ShellExecuteW(nullptr, L"open", *Normalized, nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenTerminalAt(const TCHAR* Directory)
    {
        if (!Directory || !Directory[0])
        {
            return;
        }

        FWString Normalized(Directory);
        Algo::Replace(Normalized, L'/', L'\\');

        // The -d flag sets the starting directory and the new tab gets the user's default profile.
        const HINSTANCE WtResult = ShellExecuteW(
            nullptr,
            L"open",
            L"wt.exe",
            *(FWString(L"-d \"") + Normalized + L"\""),
            nullptr,
            SW_SHOWNORMAL);

        // ShellExecute returns above 32 on success, so a not-found result falls back to cmd.exe.
        if (reinterpret_cast<INT_PTR>(WtResult) <= 32)
        {
            ShellExecuteW(
                nullptr,
                L"open",
                L"cmd.exe",
                nullptr,
                *Normalized,
                SW_SHOWNORMAL);
        }
    }
}


#endif
