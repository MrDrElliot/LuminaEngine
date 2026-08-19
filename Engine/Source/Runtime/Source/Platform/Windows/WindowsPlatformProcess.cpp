#include "RuntimePCH.h"
#ifdef _WIN32

#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
// windows.h must be included unconditionally: WIN32_LEAN_AND_MEAN is already defined
// workspace-wide, so guarding the include on it would skip it entirely.
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

        // Hands the directory to the loader itself, which is the only way it participates in
        // resolving the *imports* of a DLL we load. GDLLSearchPaths alone only ever helped locate
        // the file we asked for, and a plugin binary a project module statically links is never
        // the file we ask for: the loader resolves that import before LoadLibrary returns.
        void RegisterLoaderDirectory(const FString& Directory)
        {
            FWString Wide = StringUtils::ToWideString(Directory);

            if (::AddDllDirectory(Wide.c_str()) == nullptr)
            {
                LOG_WARN("AddDllDirectory failed for '{0}' (error {1})", Directory, (uint32)GetLastError());
            }
        }

        // Searches the loading module's own directory and everything AddDLLDirectory registered,
        // so a project DLL under <Project>/Binaries resolves an import of a plugin DLL under
        // <Plugin>/Binaries. Falls back to the default order on failure, because that flag set
        // drops PATH and the current directory, which vendored dependencies still rely on.
        void* LoadModuleImage(const FWString& Wide)
        {
            constexpr DWORD SearchFlags =
                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                | LOAD_LIBRARY_SEARCH_APPLICATION_DIR
                | LOAD_LIBRARY_SEARCH_USER_DIRS
                | LOAD_LIBRARY_SEARCH_SYSTEM32;

            if (HMODULE Handle = LoadLibraryExW(Wide.c_str(), nullptr, SearchFlags))
            {
                return Handle;
            }

            const DWORD SearchError = GetLastError();

            if (HMODULE Handle = LoadLibraryW(Wide.c_str()))
            {
                return Handle;
            }

            // 126 (ERROR_MOD_NOT_FOUND) here means a dependency is missing, not this file: it
            // exists, we just resolved a path to it. 193 (ERROR_BAD_EXE_FORMAT) is a bitness or
            // corruption problem. Logged because the loader gives the caller no way to ask.
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

        GDLLSearchPaths.push_back(Directory);
        RegisterLoaderDirectory(Directory);
    }

    void PushDLLDirectory(const TCHAR* Directory)
    {
        SetDllDirectory(Directory);

        FString Narrow = StringUtils::FromWideString(Directory);

        // SetDllDirectory is ignored once a load passes LOAD_LIBRARY_SEARCH flags, so register
        // the same directory the other way too; the pair have to agree about what is searched.
        RegisterLoaderDirectory(Narrow);

        GDLLSearchPaths.push_back(Narrow);

        LOG_INFO("Pushing DLL Search Path: {0}", Narrow);
    }

    void PopDLLDirectory()
    {
        GDLLSearchPaths.pop_back();

        if (GDLLSearchPaths.empty())
        {
            SetDllDirectory(L"");
        }
        else
        {
            SetDllDirectory(StringUtils::ToWideString(GDLLSearchPaths.back()).c_str());
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
                       reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(Buffer.data()), &Len);

            if (!bOk)
            {
                // Topology unavailable: treat every logical core as a performance core.
                const DWORD N = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
                T.NumLogicalCores = N < 256 ? N : 256;
                T.NumPerformance  = T.NumLogicalCores;
                for (uint32 i = 0; i < T.NumLogicalCores; ++i) T.CoreTypes[i] = ECpuCoreType::Performance;
                return T;
            }

            // First pass: record each logical processor's efficiency class and find the top class.
            // Per MSDN, a higher EfficiencyClass is the more performant core, so the max class is the P-cores.
            uint8 EffClass[256];
            memset(EffClass, 0xFF, sizeof(EffClass));
            uint8 MaxClass = 0;

            uint8* Cursor = Buffer.data();
            uint8* End    = Buffer.data() + Len;
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

            // Second pass: classify against the top class. Top class = P-cores; anything below = E-cores.
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

    void EnableHighResolutionTiming()
    {
        timeBeginPeriod(1);
    }

    void DisableHighResolutionTiming()
    {
        timeEndPeriod(1);
    }

    double GetTime()
    {
        static const LARGE_INTEGER Frequency = []{ LARGE_INTEGER F; QueryPerformanceFrequency(&F); return F; }();
        static const LARGE_INTEGER Start     = []{ LARGE_INTEGER S; QueryPerformanceCounter(&S);   return S; }();

        LARGE_INTEGER Now;
        QueryPerformanceCounter(&Now);
        return double(Now.QuadPart - Start.QuadPart) / double(Frequency.QuadPart);
    }

    FString GetCurrentProcessPath()
    {
        char buffer[MAX_PATH];
        DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        if (length == 0)
        {
            return "";
        }
        
        return FString(buffer, length);
    }

    bool SetEnvVariable(const FString& Name, const FString& Value)
    {
        if (_putenv_s(Name.c_str(), Value.c_str()) == 0)
        {
            LOG_TRACE("Environment variable {} set to {}", Name, Value);
            return true;
        }

        LOG_WARN("Failed to set environment variable {}", Name);
        return false;
    }

    bool PersistUserEnvVariable(const FString& Name, const FString& Value)
    {
        HKEY Key = nullptr;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, KEY_READ | KEY_WRITE, &Key) != ERROR_SUCCESS)
        {
            LOG_WARN("Failed to open HKCU\\Environment to persist {}", Name);
            return false;
        }

        // Skip the write (and the broadcast) when the stored value already matches,
        // so a normal launch doesn't churn the registry or spam WM_SETTINGCHANGE.
        char Existing[1024] = {};
        DWORD ExistingSize = sizeof(Existing);
        DWORD Type = 0;
        if (RegQueryValueExA(Key, Name.c_str(), nullptr, &Type, reinterpret_cast<LPBYTE>(Existing), &ExistingSize) == ERROR_SUCCESS
            && (Type == REG_SZ || Type == REG_EXPAND_SZ)
            && Paths::PathsEqual(Existing, Value.c_str()))
        {
            RegCloseKey(Key);
            return false;
        }

        const LONG WriteResult = RegSetValueExA(Key, Name.c_str(), 0, REG_SZ,
            reinterpret_cast<const BYTE*>(Value.c_str()), static_cast<DWORD>(Value.size() + 1));
        RegCloseKey(Key);

        if (WriteResult != ERROR_SUCCESS)
        {
            LOG_WARN("Failed to persist environment variable {}", Name);
            return false;
        }

        // Tell already-running shells/Explorer to reload the environment block so
        // newly-spawned processes pick up the change without a reboot.
        SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
            reinterpret_cast<LPARAM>("Environment"), SMTO_ABORTIFHUNG, 5000, nullptr);

        return true;
    }

    int LaunchProcess(const TCHAR* URL, const TCHAR* Params, bool bLaunchDetached)
    {
        if (!URL)
        {
            return -1;
        }

        FWString URLString(URL);
        std::replace(URLString.begin(), URLString.end(), '/', '\\');
        
        STARTUPINFOW si{};
        PROCESS_INFORMATION pi{};

        si.cb = sizeof(si);

        if (Params)
        {
            URLString += L" ";
            URLString += Params;
        }

        // DETACHED_PROCESS + CREATE_NEW_CONSOLE are mutually exclusive (ERROR_INVALID_PARAMETER);
        // "detached" here means the child owns its own console and outlives the parent: CREATE_NEW_CONSOLE.
        DWORD creationFlags = 0;
        if (bLaunchDetached)
        {
            creationFlags |= CREATE_NEW_CONSOLE;
        }

        // CreateProcessW modifies the command line string, so make a writable buffer
        TVector<wchar_t> cmdBuffer(URLString.begin(), URLString.end());
        cmdBuffer.push_back(L'\0');

        BOOL result = CreateProcessW(
            nullptr,                  // lpApplicationName
            cmdBuffer.data(),         // lpCommandLine
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

    int RunProcessAndWaitCapture(const TCHAR* Executable, const TCHAR* Params, const TCHAR* WorkingDirectory, const TFunction<void(FStringView)>& LineCallback)
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
        CmdBuffer.push_back(L'\0');

        // Write end is inheritable for both stdout+stderr; stderr merged inline for build log.
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE ReadEnd = nullptr;
        HANDLE WriteEnd = nullptr;
        if (!CreatePipe(&ReadEnd, &WriteEnd, &sa, 0))
        {
            return -1;
        }
        // The parent's read end must NOT be inherited by the child.
        SetHandleInformation(ReadEnd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = nullptr;
        si.hStdOutput = WriteEnd;
        si.hStdError  = WriteEnd;

        PROCESS_INFORMATION pi{};

        // CREATE_NO_WINDOW: the editor isn't a console app, and we're capturing
        // output anyway; suppressing the popup keeps the experience clean.
        const DWORD CreationFlags = CREATE_NO_WINDOW;

        BOOL ok = CreateProcessW(
            nullptr,
            CmdBuffer.data(),
            nullptr,
            nullptr,
            TRUE,                     // bInheritHandles must be TRUE for pipe
            CreationFlags,
            nullptr,
            WorkingDirectory,
            &si,
            &pi);

        // Release parent's write end; without this ReadFile blocks forever after child exits.
        CloseHandle(WriteEnd);

        if (!ok)
        {
            CloseHandle(ReadEnd);
            return -1;
        }

        FString Pending;
        char ReadBuf[4096];
        DWORD BytesRead = 0;

        while (ReadFile(ReadEnd, ReadBuf, sizeof(ReadBuf), &BytesRead, nullptr) && BytesRead > 0)
        {
            Pending.append(ReadBuf, BytesRead);

            size_t Cursor = 0;
            for (;;)
            {
                const size_t NewlineIdx = Pending.find('\n', Cursor);
                if (NewlineIdx == FString::npos)
                {
                    break;
                }

                size_t LineEnd = NewlineIdx;
                // Strip trailing CR for CRLF lines.
                if (LineEnd > Cursor && Pending[LineEnd - 1] == '\r')
                {
                    --LineEnd;
                }

                if (LineCallback)
                {
                    LineCallback(FStringView(Pending.data() + Cursor, LineEnd - Cursor));
                }
                Cursor = NewlineIdx + 1;
            }

            if (Cursor > 0)
            {
                Pending.erase(0, Cursor);
            }
        }

        // Final partial line (no trailing newline).
        if (!Pending.empty() && LineCallback)
        {
            LineCallback(FStringView(Pending.data(), Pending.size()));
        }

        CloseHandle(ReadEnd);

        WaitForSingleObject(pi.hProcess, INFINITE);
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
        CmdBuffer.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        // CREATE_NEW_CONSOLE keeps child stdout out of the editor window; visible in spawned console.
        const DWORD CreationFlags = CREATE_NEW_CONSOLE;

        BOOL ok = CreateProcessW(
            nullptr,
            CmdBuffer.data(),
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

        // GetProcessHeaps needs the count up front; 256 is far beyond what any real process has,
        // and an overflowing return just means we'd miss heaps, not that we'd read out of bounds.
        constexpr DWORD MaxHeaps = 256;
        HANDLE Heaps[MaxHeaps];
        const DWORD NumHeaps = GetProcessHeaps(MaxHeaps, Heaps);
        Out.HeapCount = static_cast<uint32>(NumHeaps < MaxHeaps ? NumHeaps : MaxHeaps);
        Out.bHeapWalkValid = Out.HeapCount > 0;

        for (uint32 HeapIndex = 0; HeapIndex < Out.HeapCount; ++HeapIndex)
        {
            const HANDLE Heap = Heaps[HeapIndex];

            // HeapWalk requires exclusive access; without the lock it can trip over another thread
            // mid-allocation and return garbage. The lock is recursive on this thread, so our own
            // incidental allocations during the walk are safe -- but don't add any.
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

        // Blocks the heap manager services with its own VirtualAlloc live outside any region, so
        // the region total can under-count. Never report less committed than we know is handed out.
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
            return pmc.PrivateUsage / (1024 * 1024);
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
        if (void* Handle = GetModuleHandleW(Wide.c_str()))
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

        IFileDialog* FileDialog = nullptr;
        HRESULT Result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileDialog, reinterpret_cast<void**>(&FileDialog));

        if (FAILED(Result))
        {
            CoUninitialize();
            PANIC("Failed to create File Open Dialog");
        }
        
        DWORD Options;
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

        if (Filter && strlen(Filter) > 0)
        {
            const char* p = Filter;
            while (*p)
            {
                size_t NameLen = strlen(p);
                FWString wideName = UTF8_TO_TCHAR(std::string(p, NameLen).c_str());
                p += NameLen + 1;

                size_t specLen = strlen(p);
                FWString wideSpec = UTF8_TO_TCHAR(std::string(p, specLen).c_str());
                p += specLen + 1;

                StringStorage.push_back(wideName);
                StringStorage.push_back(wideSpec);

                fileTypes.push_back({ 
                    StringStorage[StringStorage.size() - 2].c_str(),
                    StringStorage[StringStorage.size() - 1].c_str() 
                });
            }
        }

        if (!fileTypes.empty())
        {
            FileDialog->SetFileTypes(static_cast<UINT>(fileTypes.size()), fileTypes.data());
        }

        if (!fileTypes.empty())
        {
            FileDialog->SetFileTypes(static_cast<UINT>(fileTypes.size()), fileTypes.data());
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

                    OutFile = TCHAR_TO_UTF8(wPath.c_str());
                    std::replace(OutFile.begin(), OutFile.end(), '\\', '/');

                    CoTaskMemFree(pszPath);
                    bResult = true;
                }
                Item->Release();
            }
        }

        FileDialog->Release();
        CoUninitialize();
        return bResult;
    }

    // â”€â”€â”€ OS shell integration â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    bool OpenFileDialogueMulti(TVector<FFixedString>& OutFiles, const char* Title, const char* Filter, const char* InitialDir)
    {
        OutFiles.clear();

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

        if (Filter && strlen(Filter) > 0)
        {
            const char* Cursor = Filter;
            while (*Cursor)
            {
                const size_t NameLen = strlen(Cursor);
                Storage.push_back(UTF8_TO_TCHAR(std::string(Cursor, NameLen).c_str()));
                Cursor += NameLen + 1;

                const size_t SpecLen = strlen(Cursor);
                Storage.push_back(UTF8_TO_TCHAR(std::string(Cursor, SpecLen).c_str()));
                Cursor += SpecLen + 1;

                FileTypes.push_back({ Storage[Storage.size() - 2].c_str(), Storage[Storage.size() - 1].c_str() });
            }
        }

        if (!FileTypes.empty())
        {
            Dialog->SetFileTypes(static_cast<UINT>(FileTypes.size()), FileTypes.data());
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
                        FFixedString Path = TCHAR_TO_UTF8(FWString(Raw).c_str());
                        std::replace(Path.begin(), Path.end(), '\\', '/');
                        OutFiles.push_back(Path);
                        CoTaskMemFree(Raw);
                    }

                    Item->Release();
                }

                Items->Release();
            }
        }

        Dialog->Release();
        CoUninitialize();

        return !OutFiles.empty();
    }

    void ShowFileInExplorer(const TCHAR* Path)
    {
        if (!Path || !Path[0])
        {
            return;
        }

        // Normalize to backslashes and quote, explorer is picky about both.
        FWString Normalized(Path);
        std::replace(Normalized.begin(), Normalized.end(), L'/', L'\\');

        FWString Args = L"/select,\"";
        Args += Normalized;
        Args += L"\"";

        ShellExecuteW(nullptr, L"open", L"explorer.exe", Args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void ShowFolderInExplorer(const TCHAR* Directory)
    {
        if (!Directory || !Directory[0])
        {
            return;
        }

        FWString Normalized(Directory);
        std::replace(Normalized.begin(), Normalized.end(), L'/', L'\\');

        ShellExecuteW(nullptr, L"open", Normalized.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenTerminalAt(const TCHAR* Directory)
    {
        if (!Directory || !Directory[0])
        {
            return;
        }

        FWString Normalized(Directory);
        std::replace(Normalized.begin(), Normalized.end(), L'/', L'\\');

        // Prefer Windows Terminal when present; the -d flag sets the starting
        // directory and the new tab gets the user's default profile.
        const HINSTANCE WtResult = ShellExecuteW(
            nullptr,
            L"open",
            L"wt.exe",
            (FWString(L"-d \"") + Normalized + L"\"").c_str(),
            nullptr,
            SW_SHOWNORMAL);

        // ShellExecute returns >32 on success. If wt isn't installed we get
        // SE_ERR_FNF (2), fall back to plain cmd.exe.
        if (reinterpret_cast<INT_PTR>(WtResult) <= 32)
        {
            ShellExecuteW(
                nullptr,
                L"open",
                L"cmd.exe",
                nullptr,
                Normalized.c_str(),
                SW_SHOWNORMAL);
        }
    }
}


#endif
