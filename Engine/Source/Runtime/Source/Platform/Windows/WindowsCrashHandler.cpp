#include "RuntimePCH.h"
#ifdef LE_PLATFORM_WINDOWS

#include "Platform/CrashHandler.h"

#include "Log/Log.h"
#include "Platform/Process/PlatformProcess.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

namespace Lumina::CrashHandler
{
    namespace
    {
        std::atomic<bool> GInsideHandler{false};
        LPTOP_LEVEL_EXCEPTION_FILTER GPreviousFilter = nullptr;
        std::terminate_handler GPreviousTerminate = nullptr;
        bool GInstalled = false;

        struct FCrashPaths
        {
            wchar_t DumpPath[MAX_PATH];
            char    LogPath[MAX_PATH];
        };
        
        void BuildCrashPaths(FCrashPaths& Out)
        {
            wchar_t ExePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, ExePath, MAX_PATH);

            wchar_t* LastSlash = wcsrchr(ExePath, L'\\');
            if (LastSlash)
            {
                *LastSlash = 0;
            }

            wchar_t CrashDir[MAX_PATH];
            swprintf_s(CrashDir, L"%s\\Crashes", ExePath);
            CreateDirectoryW(CrashDir, nullptr);

            SYSTEMTIME T;
            GetLocalTime(&T);
            swprintf_s(Out.DumpPath, L"%s\\Lumina-%04d%02d%02d-%02d%02d%02d-%lu.dmp",
                CrashDir, T.wYear, T.wMonth, T.wDay, T.wHour, T.wMinute, T.wSecond,
                GetCurrentProcessId());

            // ANSI variant for the modal/log path display (no wide log formatter).
            int Written = WideCharToMultiByte(CP_UTF8, 0, Out.DumpPath, -1,
                Out.LogPath, MAX_PATH, nullptr, nullptr);
            if (Written <= 0)
            {
                Out.LogPath[0] = 0;
            }
        }

        bool WriteMiniDump(const wchar_t* Path, EXCEPTION_POINTERS* ExceptionPointers)
        {
            HANDLE File = CreateFileW(Path, GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (File == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            MINIDUMP_EXCEPTION_INFORMATION ExInfo{};
            ExInfo.ThreadId = GetCurrentThreadId();
            ExInfo.ExceptionPointers = ExceptionPointers;
            ExInfo.ClientPointers = FALSE;

            const MINIDUMP_TYPE DumpType = static_cast<MINIDUMP_TYPE>(
                MiniDumpWithDataSegs |
                MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpWithThreadInfo |
                MiniDumpWithProcessThreadData |
                MiniDumpWithHandleData);

            BOOL Ok = MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                File,
                DumpType,
                ExceptionPointers ? &ExInfo : nullptr,
                nullptr,
                nullptr);

            CloseHandle(File);
            return Ok != FALSE;
        }

        const char* ExceptionCodeToString(DWORD Code)
        {
            switch (Code)
            {
                case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
                case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
                case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
                case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
                case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
                case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
                case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT";
                case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
                case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
                case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
                case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
                case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
                case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
                case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
                case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
                case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
                case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
                case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
                case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
                case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
                default:                                  return "UNKNOWN";
            }
        }

        // Best-effort: bails on first failure since we are already in a crashed process.
        void CaptureStackTraceFromContext(CONTEXT* ContextRecord, char* OutBuffer, size_t BufferSize)
        {
            if (!ContextRecord || !OutBuffer || BufferSize == 0)
            {
                return;
            }

            HANDLE Process = GetCurrentProcess();
            HANDLE Thread  = GetCurrentThread();
            
            static std::atomic<bool> SymsInitialized{false};
            bool Expected = false;
            if (SymsInitialized.compare_exchange_strong(Expected, true))
            {
                SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
                SymInitialize(Process, nullptr, TRUE);
            }

            STACKFRAME64 Frame{};
            DWORD MachineType;
#if defined(_M_X64)
            MachineType         = IMAGE_FILE_MACHINE_AMD64;
            Frame.AddrPC.Offset = ContextRecord->Rip;
            Frame.AddrPC.Mode   = AddrModeFlat;
            Frame.AddrFrame.Offset = ContextRecord->Rbp;
            Frame.AddrFrame.Mode   = AddrModeFlat;
            Frame.AddrStack.Offset = ContextRecord->Rsp;
            Frame.AddrStack.Mode   = AddrModeFlat;
#elif defined(_M_ARM64)
            MachineType         = IMAGE_FILE_MACHINE_ARM64;
            Frame.AddrPC.Offset = ContextRecord->Pc;
            Frame.AddrPC.Mode   = AddrModeFlat;
            Frame.AddrFrame.Offset = ContextRecord->Fp;
            Frame.AddrFrame.Mode   = AddrModeFlat;
            Frame.AddrStack.Offset = ContextRecord->Sp;
            Frame.AddrStack.Mode   = AddrModeFlat;
#else
            return;
#endif

            size_t Offset = 0;
            auto AppendLine = [&](const char* Line)
            {
                size_t Len = strlen(Line);
                if (Offset + Len + 1 >= BufferSize)
                {
                    return;
                }
                memcpy(OutBuffer + Offset, Line, Len);
                Offset += Len;
                OutBuffer[Offset++] = '\n';
                OutBuffer[Offset]   = 0;
            };

            constexpr int MaxFrames = 64;
            char SymBuf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* Symbol = reinterpret_cast<SYMBOL_INFO*>(SymBuf);

            for (int i = 0; i < MaxFrames; ++i)
            {
                if (!StackWalk64(MachineType, Process, Thread, &Frame, ContextRecord,
                                 nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                {
                    break;
                }
                if (Frame.AddrPC.Offset == 0)
                {
                    break;
                }

                memset(Symbol, 0, sizeof(SymBuf));
                Symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                Symbol->MaxNameLen   = 255;

                char Line[512];
                DWORD64 Displacement = 0;
                if (SymFromAddr(Process, Frame.AddrPC.Offset, &Displacement, Symbol))
                {
                    IMAGEHLP_LINE64 LineInfo{};
                    LineInfo.SizeOfStruct = sizeof(LineInfo);
                    DWORD LineDisp = 0;
                    if (SymGetLineFromAddr64(Process, Frame.AddrPC.Offset, &LineDisp, &LineInfo))
                    {
                        _snprintf_s(Line, sizeof(Line), _TRUNCATE,
                            "  [0x%016llX] %s + 0x%llX  (%s:%lu)",
                            (unsigned long long)Frame.AddrPC.Offset, Symbol->Name,
                            (unsigned long long)Displacement,
                            LineInfo.FileName ? LineInfo.FileName : "?", LineInfo.LineNumber);
                    }
                    else
                    {
                        _snprintf_s(Line, sizeof(Line), _TRUNCATE,
                            "  [0x%016llX] %s + 0x%llX",
                            (unsigned long long)Frame.AddrPC.Offset, Symbol->Name,
                            (unsigned long long)Displacement);
                    }
                }
                else
                {
                    _snprintf_s(Line, sizeof(Line), _TRUNCATE,
                        "  [0x%016llX] <no symbol>", (unsigned long long)Frame.AddrPC.Offset);
                }

                AppendLine(Line);
            }
        }

        // The filter can re-enter if our own logging/dumping faults. The guard
        // ensures we fall through to the previous filter / WER on second entry.
        LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* ExceptionInfo)
        {
            bool Expected = false;
            if (!GInsideHandler.compare_exchange_strong(Expected, true))
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            FCrashPaths Paths;
            BuildCrashPaths(Paths);

            DWORD Code = ExceptionInfo && ExceptionInfo->ExceptionRecord
                            ? ExceptionInfo->ExceptionRecord->ExceptionCode : 0;
            void* Addr = ExceptionInfo && ExceptionInfo->ExceptionRecord
                            ? ExceptionInfo->ExceptionRecord->ExceptionAddress : nullptr;

            char StackBuffer[8192] = {};
            if (ExceptionInfo && ExceptionInfo->ContextRecord)
            {
                CaptureStackTraceFromContext(ExceptionInfo->ContextRecord,
                    StackBuffer, sizeof(StackBuffer));
            }

            // Best-effort log write, guarded because the logger sits on top of
            // STL/heap that may itself be corrupted.
            __try
            {
                if (Logging::IsInitialized())
                {
                    LOG_CRITICAL("======== CRASH ========");
                    LOG_CRITICAL("Code: 0x{:08X} ({})", (uint32)Code, ExceptionCodeToString(Code));
                    LOG_CRITICAL("Address: 0x{:016X}", (uintptr_t)Addr);
                    if (StackBuffer[0])
                    {
                        LOG_CRITICAL("Stack trace:\n{}", StackBuffer);
                    }
                    LOG_CRITICAL("Minidump: {}", Paths.LogPath);
                    Logging::Flush();
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }

            bool DumpOk = false;
            __try
            {
                DumpOk = WriteMiniDump(Paths.DumpPath, ExceptionInfo);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                DumpOk = false;
            }

            char ModalBody[16384];
            if (DumpOk)
            {
                _snprintf_s(ModalBody, sizeof(ModalBody), _TRUNCATE,
                    "Lumina has crashed.\n\n"
                    "Exception: 0x%08lX (%s)\n"
                    "Address:   0x%p\n\n"
                    "Minidump written to:\n%s\n\n"
                    "Stack trace:\n%s",
                    Code, ExceptionCodeToString(Code), Addr, Paths.LogPath,
                    StackBuffer[0] ? StackBuffer : "  <unavailable>");
            }
            else
            {
                _snprintf_s(ModalBody, sizeof(ModalBody), _TRUNCATE,
                    "Lumina has crashed.\n\n"
                    "Exception: 0x%08lX (%s)\n"
                    "Address:   0x%p\n\n"
                    "(Failed to write minidump to %s)\n\n"
                    "Stack trace:\n%s",
                    Code, ExceptionCodeToString(Code), Addr, Paths.LogPath,
                    StackBuffer[0] ? StackBuffer : "  <unavailable>");
            }

            MessageBoxA(nullptr, ModalBody, "Lumina - Fatal Error",
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SYSTEMMODAL);

            // Let WER / attached debugger see it too.
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Synthesize an EXCEPTION_POINTERS so terminate/abort paths share the
        // same dump pipeline as SEH crashes.
        void HandleNonSehCrash(const char* Reason)
        {
            __try
            {
                RaiseException(0xE0000001, 0, 0, nullptr);
            }
            __except (TopLevelFilter(GetExceptionInformation()))
            {
                (void)Reason;
            }
        }

        [[noreturn]] void TerminateHandler()
        {
            HandleNonSehCrash("std::terminate");
            if (GPreviousTerminate)
            {
                GPreviousTerminate();
            }
            std::abort();
        }

        [[noreturn]] void SignalAbortHandler(int)
        {
            HandleNonSehCrash("SIGABRT");
            std::_Exit(3);
        }
    }

    void Install()
    {
        if (GInstalled)
        {
            return;
        }
        GInstalled = true;

        GPreviousFilter    = SetUnhandledExceptionFilter(&TopLevelFilter);
        GPreviousTerminate = std::set_terminate(&TerminateHandler);
        std::signal(SIGABRT, &SignalAbortHandler);
        
        SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    }

    void Shutdown()
    {
        if (!GInstalled) return;
        SetUnhandledExceptionFilter(GPreviousFilter);
        std::set_terminate(GPreviousTerminate);
        std::signal(SIGABRT, SIG_DFL);
        GInstalled = false;
    }
}

#endif
