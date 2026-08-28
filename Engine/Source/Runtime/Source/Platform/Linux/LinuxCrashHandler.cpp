#include "RuntimePCH.h"
#ifdef LE_PLATFORM_LINUX

#include "Platform/CrashHandler.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <exception>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "Log/Log.h"
#include "Platform/CrashReporter.h"

namespace Lumina::CrashHandler
{
    namespace
    {
        constexpr uint32 kMaxFrames = 64;
        constexpr uint32 kMaxPath = 512;
        constexpr uint32 kMaxDiagnosticProviders = 8;

        std::atomic<bool>   GInsideHandler{ false };
        std::atomic<bool>   GInstalled{ false };

        FDiagnosticProvider GDiagnosticProviders[kMaxDiagnosticProviders] = {};
        std::atomic<uint32> GDiagnosticProviderCount{ 0 };

        constexpr size_t kSignalStackSize = 256 * 1024;
        char*            GSignalStack = nullptr;

        struct FHandledSignal
        {
            int         Number;
            const char* Name;
        };

        constexpr FHandledSignal kHandledSignals[] =
        {
            { SIGSEGV, "SIGSEGV (invalid memory access)" },
            { SIGBUS,  "SIGBUS (bad memory alignment or access)" },
            { SIGFPE,  "SIGFPE (arithmetic exception)" },
            { SIGILL,  "SIGILL (illegal instruction)" },
            { SIGABRT, "SIGABRT (abort)" },
        };

        struct sigaction GPreviousActions[sizeof(kHandledSignals) / sizeof(kHandledSignals[0])] = {};

        const char* DescribeSignal(int Number)
        {
            for (const FHandledSignal& Signal : kHandledSignals)
            {
                if (Signal.Number == Number)
                {
                    return Signal.Name;
                }
            }

            return "unknown signal";
        }


        void WriteAll(int Descriptor, const char* Text, size_t Length)
        {
            while (Length > 0)
            {
                const ssize_t Written = ::write(Descriptor, Text, Length);

                if (Written <= 0)
                {
                    if (Written < 0 && errno == EINTR)
                    {
                        continue;
                    }

                    return;
                }

                Text += Written;
                Length -= static_cast<size_t>(Written);
            }
        }

        void WriteText(int Descriptor, const char* Text)
        {
            WriteAll(Descriptor, Text, ::strlen(Text));
        }

        char* AppendUnsigned(char* Out, char* End, uint64 Value, uint32 Base, uint32 MinDigits)
        {
            char Digits[32];
            uint32 Count = 0;

            do
            {
                const uint32 Digit = static_cast<uint32>(Value % Base);
                Digits[Count++] = static_cast<char>(Digit < 10 ? '0' + Digit : 'a' + (Digit - 10));
                Value /= Base;
            }
            while (Value != 0 && Count < sizeof(Digits));

            while (Count < MinDigits && Count < sizeof(Digits))
            {
                Digits[Count++] = '0';
            }

            while (Count > 0 && Out < End)
            {
                *Out++ = Digits[--Count];
            }

            return Out;
        }

        void BuildReportPath(char* Out, size_t OutSize, const char* Prefix)
        {
            char Directory[kMaxPath] = {};
            const uint32 Length = GetCrashDumpDirectory(Directory, kMaxPath);

            char* Cursor = Out;
            char* End = Out + OutSize - 1;

            if (Length > 0)
            {
                for (uint32 Index = 0; Index < Length && Cursor < End; ++Index)
                {
                    *Cursor++ = Directory[Index];
                }
            }
            else
            {
                const char* Fallback = "CrashDumps";
                for (const char* Character = Fallback; *Character && Cursor < End; ++Character)
                {
                    *Cursor++ = *Character;
                }
            }

            *Cursor = 0;
            ::mkdir(Out, 0755);

            if (Cursor < End) { *Cursor++ = '/'; }

            for (const char* Character = Prefix; *Character && Cursor < End; ++Character)
            {
                *Cursor++ = *Character;
            }

            if (Cursor < End) { *Cursor++ = '_'; }

            timespec Now = {};
            ::clock_gettime(CLOCK_REALTIME, &Now);

            int64 Days = Now.tv_sec / 86400;
            int64 SecondsOfDay = Now.tv_sec % 86400;

            if (SecondsOfDay < 0)
            {
                SecondsOfDay += 86400;
                --Days;
            }

            int64 Era = (Days >= -719468 ? Days + 719468 : Days + 719468 - 146096) / 146097;
            int64 DayOfEra = (Days + 719468) - Era * 146097;
            int64 YearOfEra = (DayOfEra - DayOfEra / 1460 + DayOfEra / 36524 - DayOfEra / 146096) / 365;
            int64 Year = YearOfEra + Era * 400;
            int64 DayOfYear = DayOfEra - (365 * YearOfEra + YearOfEra / 4 - YearOfEra / 100);
            int64 MonthPrime = (5 * DayOfYear + 2) / 153;
            int64 Day = DayOfYear - (153 * MonthPrime + 2) / 5 + 1;
            int64 Month = MonthPrime + (MonthPrime < 10 ? 3 : -9);
            Year += (Month <= 2);

            Cursor = AppendUnsigned(Cursor, End, static_cast<uint64>(Year), 10, 4);
            Cursor = AppendUnsigned(Cursor, End, static_cast<uint64>(Month), 10, 2);
            Cursor = AppendUnsigned(Cursor, End, static_cast<uint64>(Day), 10, 2);
            if (Cursor < End) { *Cursor++ = '-'; }
            Cursor = AppendUnsigned(Cursor, End, static_cast<uint64>(SecondsOfDay / 3600), 10, 2);
            Cursor = AppendUnsigned(Cursor, End, static_cast<uint64>((SecondsOfDay / 60) % 60), 10, 2);
            Cursor = AppendUnsigned(Cursor, End, static_cast<uint64>(SecondsOfDay % 60), 10, 2);
            if (Cursor < End) { *Cursor++ = '-'; }
            Cursor = AppendUnsigned(Cursor, End, static_cast<uint64>(::getpid()), 10, 1);

            const char* Extension = ".txt";
            for (const char* Character = Extension; *Character && Cursor < End; ++Character)
            {
                *Cursor++ = *Character;
            }

            *Cursor = 0;
        }

        void WriteReport(int Descriptor, int SignalNumber, const void* FaultAddress, const char* Reason)
        {
            WriteText(Descriptor, "======== LUMINA CRASH ========\n");

            if (Reason != nullptr)
            {
                WriteText(Descriptor, "Reason: ");
                WriteText(Descriptor, Reason);
                WriteText(Descriptor, "\n");
            }

            if (SignalNumber != 0)
            {
                char Line[128];
                char* Cursor = Line;
                char* End = Line + sizeof(Line) - 1;

                WriteText(Descriptor, "Signal: ");
                WriteText(Descriptor, DescribeSignal(SignalNumber));
                WriteText(Descriptor, "\n");

                WriteText(Descriptor, "Fault address: 0x");
                Cursor = AppendUnsigned(Cursor, End, reinterpret_cast<uint64>(FaultAddress), 16, 16);
                *Cursor = 0;
                WriteText(Descriptor, Line);
                WriteText(Descriptor, "\n");
            }

            WriteText(Descriptor, "\nStack trace:\n");

            void* Frames[kMaxFrames];
            const int Count = ::backtrace(Frames, kMaxFrames);

            if (Count > 0)
            {
                ::backtrace_symbols_fd(Frames, Count, Descriptor);
            }
            else
            {
                WriteText(Descriptor, "  (unavailable)\n");
            }

            WriteText(Descriptor, "==============================\n");
        }
        
        void RunDiagnosticsBestEffort()
        {
            const uint32 Count = GDiagnosticProviderCount.load(std::memory_order_acquire);

            for (uint32 Index = 0; Index < Count; ++Index)
            {
                if (GDiagnosticProviders[Index] != nullptr)
                {
                    GDiagnosticProviders[Index]();
                }
            }

            Logging::Flush();
        }

        void RestoreDefaultHandlers()
        {
            for (size_t Index = 0; Index < sizeof(kHandledSignals) / sizeof(kHandledSignals[0]); ++Index)
            {
                ::sigaction(kHandledSignals[Index].Number, &GPreviousActions[Index], nullptr);
            }
        }

        extern "C" void HandleSignal(int SignalNumber, siginfo_t* Info, void*)
        {
            bool Expected = false;

            if (!GInsideHandler.compare_exchange_strong(Expected, true))
            {
                RestoreDefaultHandlers();
                ::raise(SignalNumber);
                return;
            }

            char Path[kMaxPath] = {};
            BuildReportPath(Path, sizeof(Path), "CPUCrash");

            const int Descriptor = ::open(Path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

            if (Descriptor >= 0)
            {
                WriteReport(Descriptor, SignalNumber, Info != nullptr ? Info->si_addr : nullptr, nullptr);
                ::close(Descriptor);
            }

            WriteReport(STDERR_FILENO, SignalNumber, Info != nullptr ? Info->si_addr : nullptr, nullptr);

            if (Path[0] != 0)
            {
                WriteText(STDERR_FILENO, "Crash report: ");
                WriteText(STDERR_FILENO, Path);
                WriteText(STDERR_FILENO, "\n");
            }

            RunDiagnosticsBestEffort();

            RestoreDefaultHandlers();
            ::raise(SignalNumber);
        }
    }

    // Nothing here is modal; the report already goes to stderr and the log.
    void SetAllowModalDialog(bool bAllow)
    {
        (void)bAllow;
    }

    void AddDiagnosticProvider(FDiagnosticProvider Provider)
    {
        if (Provider == nullptr)
        {
            return;
        }

        const uint32 Slot = GDiagnosticProviderCount.load(std::memory_order_relaxed);

        if (Slot >= kMaxDiagnosticProviders)
        {
            return;
        }

        GDiagnosticProviders[Slot] = Provider;
        GDiagnosticProviderCount.store(Slot + 1, std::memory_order_release);
    }

    void ReportFatal(const char* Reason, EFatalKind Kind)
    {
        bool Expected = false;

        if (!GInsideHandler.compare_exchange_strong(Expected, true))
        {
            return;
        }

        char Path[kMaxPath] = {};
        BuildReportPath(Path, sizeof(Path), Kind == EFatalKind::Gpu ? "GPUCrash" : "CPUCrash");

        const int Descriptor = ::open(Path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

        if (Descriptor >= 0)
        {
            WriteReport(Descriptor, 0, nullptr, Reason);
            ::close(Descriptor);
        }

        if (Logging::IsInitialized())
        {
            LOG_CRITICAL("======== FATAL ========");
            LOG_CRITICAL("Reason: {0}", Reason != nullptr ? Reason : "(unspecified)");
            LOG_CRITICAL("Crash report: {0}", Path);
        }

        RunDiagnosticsBestEffort();

        CrashReporting::GenerateReport(nullptr);

    }

    void Install()
    {
        bool Expected = false;

        if (!GInstalled.compare_exchange_strong(Expected, true))
        {
            return;
        }

        (void)GetCrashDumpDirectory();

        GSignalStack = new char[kSignalStackSize];

        stack_t SignalStack = {};
        SignalStack.ss_sp = GSignalStack;
        SignalStack.ss_size = kSignalStackSize;
        SignalStack.ss_flags = 0;

        if (::sigaltstack(&SignalStack, nullptr) != 0)
        {
            LOG_WARN("CrashHandler: sigaltstack failed; a stack overflow will not produce a report.");
        }

        struct sigaction Action = {};
        Action.sa_sigaction = &HandleSignal;
        sigemptyset(&Action.sa_mask);

        Action.sa_flags = SA_SIGINFO | SA_ONSTACK;

        for (size_t Index = 0; Index < sizeof(kHandledSignals) / sizeof(kHandledSignals[0]); ++Index)
        {
            ::sigaction(kHandledSignals[Index].Number, &Action, &GPreviousActions[Index]);
        }

        std::set_terminate([]
        {
            ReportFatal("std::terminate (uncaught exception)", EFatalKind::Cpu);
            RestoreDefaultHandlers();
            ::abort();
        });
    }

    void Shutdown()
    {
        bool Expected = true;

        if (!GInstalled.compare_exchange_strong(Expected, false))
        {
            return;
        }

        RestoreDefaultHandlers();

        stack_t Disable = {};
        Disable.ss_flags = SS_DISABLE;
        ::sigaltstack(&Disable, nullptr);

        delete[] GSignalStack;
        GSignalStack = nullptr;
    }
}

#endif
