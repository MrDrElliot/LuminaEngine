#include "RuntimePCH.h"
#ifdef _WIN32

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Process/PlatformPty.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace Lumina::Platform
{
    namespace
    {
        // Declared locally against void* so the build does not depend on an SDK new enough for HPCON.
        using FCreatePseudoConsoleFn = HRESULT (WINAPI*)(COORD, HANDLE, HANDLE, DWORD, void**);
        using FResizePseudoConsoleFn = HRESULT (WINAPI*)(void*, COORD);
        using FClosePseudoConsoleFn  = void (WINAPI*)(void*);

        FCreatePseudoConsoleFn GCreatePseudoConsole = nullptr;
        FResizePseudoConsoleFn GResizePseudoConsole = nullptr;
        FClosePseudoConsoleFn  GClosePseudoConsole  = nullptr;

        bool GConPtyResolved  = false;
        bool GConPtyAvailable = false;

        bool ResolveConPty()
        {
            if (GConPtyResolved)
            {
                return GConPtyAvailable;
            }

            GConPtyResolved = true;

            HMODULE Kernel = GetModuleHandleW(L"kernel32.dll");
            if (Kernel == nullptr)
            {
                return false;
            }

            GCreatePseudoConsole = reinterpret_cast<FCreatePseudoConsoleFn>(reinterpret_cast<void*>(GetProcAddress(Kernel, "CreatePseudoConsole")));
            GResizePseudoConsole = reinterpret_cast<FResizePseudoConsoleFn>(reinterpret_cast<void*>(GetProcAddress(Kernel, "ResizePseudoConsole")));
            GClosePseudoConsole  = reinterpret_cast<FClosePseudoConsoleFn>(reinterpret_cast<void*>(GetProcAddress(Kernel, "ClosePseudoConsole")));

            GConPtyAvailable = GCreatePseudoConsole != nullptr
                            && GResizePseudoConsole != nullptr
                            && GClosePseudoConsole  != nullptr;

            if (!GConPtyAvailable)
            {
                LOG_WARN("[Pty] ConPTY is unavailable; Windows 10 1809 or newer is required.");
            }

            return GConPtyAvailable;
        }

        void CloseIfValid(HANDLE& Handle)
        {
            if (Handle != nullptr && Handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(Handle);
            }
            Handle = INVALID_HANDLE_VALUE;
        }

        class FWindowsPtySession final : public IPtySession
        {
        public:

            ~FWindowsPtySession() override { Close(); }

            bool Start(const FPtyLaunchParams& Params);

            bool IsRunning() override;
            int32 Read(TVector<uint8>& OutBytes, int32 MaxBytes) override;
            bool Write(const uint8* Bytes, int32 Count) override;
            void Resize(uint16 Columns, uint16 Rows) override;
            void Close() override;

            int32 GetExitCode() const override { return ExitCode; }

        private:

            void ReleaseAttributeList();

            void*   PseudoConsole = nullptr;
            HANDLE  OutputRead    = INVALID_HANDLE_VALUE;
            HANDLE  InputWrite    = INVALID_HANDLE_VALUE;
            HANDLE  ProcessHandle = INVALID_HANDLE_VALUE;
            HANDLE  ThreadHandle  = INVALID_HANDLE_VALUE;

            LPPROC_THREAD_ATTRIBUTE_LIST AttributeList = nullptr;

            int32 ExitCode  = 0;
            bool  bExited   = false;

        };

        bool FWindowsPtySession::Start(const FPtyLaunchParams& Params)
        {
            if (!ResolveConPty())
            {
                return false;
            }

            HANDLE InputRead  = INVALID_HANDLE_VALUE;
            HANDLE OutputWrite = INVALID_HANDLE_VALUE;

            // ConPTY hands the inner ends to its own conhost, which only receives inheritable handles.
            SECURITY_ATTRIBUTES PipeSecurity = {};
            PipeSecurity.nLength = sizeof(PipeSecurity);
            PipeSecurity.bInheritHandle = TRUE;
            PipeSecurity.lpSecurityDescriptor = nullptr;

            if (!CreatePipe(&InputRead, &InputWrite, &PipeSecurity, 0))
            {
                LOG_ERROR("[Pty] Failed to create the input pipe.");
                return false;
            }

            if (!CreatePipe(&OutputRead, &OutputWrite, &PipeSecurity, 0))
            {
                LOG_ERROR("[Pty] Failed to create the output pipe.");
                CloseIfValid(InputRead);
                CloseIfValid(InputWrite);
                return false;
            }

            const COORD Size = { static_cast<SHORT>(Params.Columns), static_cast<SHORT>(Params.Rows) };
            const HRESULT Created = GCreatePseudoConsole(Size, InputRead, OutputWrite, 0, &PseudoConsole);

            // ConPTY duplicates both ends, and the read never reports the child exiting while ours stay open.
            CloseIfValid(InputRead);
            CloseIfValid(OutputWrite);

            if (FAILED(Created))
            {
                LOG_ERROR("[Pty] CreatePseudoConsole failed (0x{:08X}).", static_cast<uint32>(Created));
                Close();
                return false;
            }

            SIZE_T AttributeSize = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeSize);

            // A Win32 structure sized by a Win32 API stays on the Win32 heap, not the engine allocator.
            AttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, AttributeSize));
            if (AttributeList == nullptr)
            {
                Close();
                return false;
            }

            if (!InitializeProcThreadAttributeList(AttributeList, 1, 0, &AttributeSize))
            {
                LOG_ERROR("[Pty] InitializeProcThreadAttributeList failed ({}).", GetLastError());
                Close();
                return false;
            }

            if (!UpdateProcThreadAttribute(AttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                    PseudoConsole, sizeof(PseudoConsole), nullptr, nullptr))
            {
                LOG_ERROR("[Pty] UpdateProcThreadAttribute failed ({}).", GetLastError());
                Close();
                return false;
            }

            STARTUPINFOEXW StartupInfo = {};
            StartupInfo.StartupInfo.cb = sizeof(STARTUPINFOEXW);
            StartupInfo.lpAttributeList = AttributeList;

            // Only null standard handles make the console layer hand the child the pseudoconsole ones.
            StartupInfo.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
            StartupInfo.StartupInfo.hStdInput  = nullptr;
            StartupInfo.StartupInfo.hStdOutput = nullptr;
            StartupInfo.StartupInfo.hStdError  = nullptr;

            const FString Shell = Params.Shell.empty() ? GetDefaultShell() : Params.Shell;
            FWString WideCommand = StringUtils::ToWideString(Shell);

            TVector<wchar_t> CommandBuffer(WideCommand.begin(), WideCommand.end());
            CommandBuffer.push_back(L'\0');

            FWString WideWorkingDirectory;
            const wchar_t* WorkingDirectory = nullptr;
            if (!Params.WorkingDirectory.empty())
            {
                WideWorkingDirectory = StringUtils::ToWideString(Params.WorkingDirectory);
                WorkingDirectory = WideWorkingDirectory.c_str();
            }

            PROCESS_INFORMATION ProcessInfo = {};

            // Handle inheritance stays off; the pseudoconsole attribute is what hands the child its ends.
            const BOOL bSpawned = CreateProcessW(
                nullptr,
                CommandBuffer.data(),
                nullptr,
                nullptr,
                FALSE,
                EXTENDED_STARTUPINFO_PRESENT,
                nullptr,
                WorkingDirectory,
                &StartupInfo.StartupInfo,
                &ProcessInfo);

            if (!bSpawned)
            {
                LOG_ERROR("[Pty] Failed to spawn '{}' ({}).", Shell, GetLastError());
                Close();
                return false;
            }

            ProcessHandle = ProcessInfo.hProcess;
            ThreadHandle  = ProcessInfo.hThread;

            LOG_INFO("[Pty] Spawned '{}' pid={}, {}x{}.",
                Shell, ProcessInfo.dwProcessId, Params.Columns, Params.Rows);

            return true;
        }

        bool FWindowsPtySession::IsRunning()
        {
            if (bExited)
            {
                return false;
            }

            if (ProcessHandle == INVALID_HANDLE_VALUE)
            {
                LOG_WARN("[Pty] The process handle was never valid.");
                bExited = true;
                return false;
            }

            DWORD Code = 0;
            const BOOL bQueried = GetExitCodeProcess(ProcessHandle, &Code);

            if (!bQueried)
            {
                LOG_WARN("[Pty] GetExitCodeProcess failed ({}), handle=0x{:X}.",
                    GetLastError(), reinterpret_cast<uintptr_t>(ProcessHandle));
                bExited = true;
                return false;
            }

            if (Code != STILL_ACTIVE)
            {
                ExitCode = static_cast<int32>(Code);
                bExited  = true;
                return false;
            }

            return true;
        }

        int32 FWindowsPtySession::Read(TVector<uint8>& OutBytes, int32 MaxBytes)
        {
            if (OutputRead == INVALID_HANDLE_VALUE)
            {
                return 0;
            }

            int32 Total = 0;
            while (Total < MaxBytes)
            {
                DWORD Available = 0;
                if (!PeekNamedPipe(OutputRead, nullptr, 0, nullptr, &Available, nullptr))
                {
                    static bool bPeekFailureLogged = false;
                    if (!bPeekFailureLogged)
                    {
                        bPeekFailureLogged = true;
                        LOG_WARN("[Pty] PeekNamedPipe failed ({}).", GetLastError());
                    }
                    break;
                }

                if (Available == 0)
                {
                    break;
                }

                const DWORD Wanted = Math::Min(static_cast<DWORD>(MaxBytes - Total), Available);
                const size_t Offset = OutBytes.size();
                OutBytes.resize(Offset + Wanted);

                DWORD BytesRead = 0;
                if (!ReadFile(OutputRead, OutBytes.data() + Offset, Wanted, &BytesRead, nullptr) || BytesRead == 0)
                {
                    OutBytes.resize(Offset);
                    break;
                }

                OutBytes.resize(Offset + BytesRead);
                Total += static_cast<int32>(BytesRead);
            }

            return Total;
        }

        bool FWindowsPtySession::Write(const uint8* Bytes, int32 Count)
        {
            if (InputWrite == INVALID_HANDLE_VALUE || Bytes == nullptr || Count <= 0)
            {
                return false;
            }

            int32 Written = 0;
            while (Written < Count)
            {
                DWORD Chunk = 0;
                if (!WriteFile(InputWrite, Bytes + Written, static_cast<DWORD>(Count - Written), &Chunk, nullptr) || Chunk == 0)
                {
                    return false;
                }
                Written += static_cast<int32>(Chunk);
            }

            return true;
        }

        void FWindowsPtySession::Resize(uint16 Columns, uint16 Rows)
        {
            if (PseudoConsole == nullptr || GResizePseudoConsole == nullptr)
            {
                return;
            }

            const COORD Size = { static_cast<SHORT>(Columns), static_cast<SHORT>(Rows) };
            GResizePseudoConsole(PseudoConsole, Size);
        }

        void FWindowsPtySession::ReleaseAttributeList()
        {
            if (AttributeList != nullptr)
            {
                DeleteProcThreadAttributeList(AttributeList);
                HeapFree(GetProcessHeap(), 0, AttributeList);
                AttributeList = nullptr;
            }
        }

        void FWindowsPtySession::Close()
        {
            if (ProcessHandle != INVALID_HANDLE_VALUE && IsRunning())
            {
                TerminateProcess(ProcessHandle, 0);
                WaitForSingleObject(ProcessHandle, 2000);
            }

            // Closing the console before the client is gone can block, so it follows the terminate.
            if (PseudoConsole != nullptr && GClosePseudoConsole != nullptr)
            {
                GClosePseudoConsole(PseudoConsole);
                PseudoConsole = nullptr;
            }

            CloseIfValid(InputWrite);
            CloseIfValid(OutputRead);
            CloseIfValid(ThreadHandle);
            CloseIfValid(ProcessHandle);

            ReleaseAttributeList();

            bExited = true;
        }
    }

    bool IsPtySupported()
    {
        return ResolveConPty();
    }

    FString GetDefaultShell()
    {
        const FString Preferred = GetEnvVariable("COMSPEC");
        return Preferred.empty() ? FString("cmd.exe") : Preferred;
    }

    FPtySessionPtr CreatePtySession(const FPtyLaunchParams& Params)
    {
        FWindowsPtySession* Session = Memory::New<FWindowsPtySession>();
        if (!Session->Start(Params))
        {
            Memory::Delete(Session);
            return FPtySessionPtr();
        }

        return FPtySessionPtr(Session);
    }
}

#endif
