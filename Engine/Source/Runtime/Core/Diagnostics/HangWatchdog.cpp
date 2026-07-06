#include "pch.h"
#include "Core/Diagnostics/HangWatchdog.h"

#if defined(LE_PLATFORM_WINDOWS) && !defined(LE_SHIPPING)

#include "Core/Threading/Thread.h"
#include "Log/Log.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#pragma comment(lib, "dbghelp.lib")

namespace Lumina::HangWatchdog
{
    namespace
    {
        constexpr uint32 kMaxFrames = 48;

        std::atomic<uint64> GHeartbeat{0};
        std::atomic<bool>   GRunning{false};
        FThread             GThread;
        float               GTimeoutSeconds   = 8.0f;
        DWORD               GMainThreadId     = 0;
        DWORD               GWatchdogThreadId = 0;

        constexpr uint32          kMaxReporters = 8;
        std::atomic<uint32>       GNumReporters{0};
        void (*GReporters[kMaxReporters])() = {};
        
        void EnsureSymbols()
        {
            static std::atomic<bool> Initialized{false};
            bool Expected = false;
            if (Initialized.compare_exchange_strong(Expected, true))
            {
                SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
                SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            }
        }

#if defined(_M_X64)
        // Walk a suspended thread's stack using the in-memory unwind tables.
        uint32 WalkSuspended(CONTEXT Context, DWORD64* OutAddrs)
        {
            uint32 Count = 0;
            while (Count < kMaxFrames && Context.Rip != 0)
            {
                OutAddrs[Count++] = Context.Rip;

                DWORD64           ImageBase = 0;
                PRUNTIME_FUNCTION Function  = RtlLookupFunctionEntry(Context.Rip, &ImageBase, nullptr);
                if (Function == nullptr)
                {
                    // Leaf function (no unwind data): the return address sits at the top of the stack.
                    if (Context.Rsp == 0)
                    {
                        break;
                    }
                    Context.Rip  = *reinterpret_cast<DWORD64*>(Context.Rsp);
                    Context.Rsp += sizeof(DWORD64);
                    continue;
                }

                PVOID   HandlerData     = nullptr;
                DWORD64 EstablisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, ImageBase, Context.Rip, Function,
                    &Context, &HandlerData, &EstablisherFrame, nullptr);
            }
            return Count;
        }
#endif

        // Suspend ThreadId, capture up to kMaxFrames return addresses, resume. No heap/log/dbghelp use
        // while suspended (symbolization is deferred), so the watchdog can't deadlock on a lock the
        // suspended thread holds. Returns frame count (0 if the thread couldn't be walked).
        uint32 CaptureThreadFrames(DWORD ThreadId, DWORD64* OutAddrs)
        {
            HANDLE Thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, ThreadId);
            if (Thread == nullptr)
            {
                return 0;
            }

            uint32 Count = 0;
            if (SuspendThread(Thread) != (DWORD)-1)
            {
                CONTEXT Context{};
                Context.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(Thread, &Context))
                {
#if defined(_M_X64)
                    Count = WalkSuspended(Context, OutAddrs);
#else
                    OutAddrs[0] = (DWORD64)Context.Pc;
                    Count = 1;
#endif
                }
                ResumeThread(Thread);
            }

            CloseHandle(Thread);
            return Count;
        }

        void GetThreadLabel(DWORD ThreadId, char* OutName, size_t OutSize)
        {
            OutName[0] = 0;
            HANDLE Handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, ThreadId);
            if (Handle == nullptr)
            {
                return;
            }

            PWSTR Desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(Handle, &Desc)) && Desc != nullptr)
            {
                WideCharToMultiByte(CP_UTF8, 0, Desc, -1, OutName, (int)OutSize, nullptr, nullptr);
                LocalFree(Desc);
            }
            CloseHandle(Handle);
        }

        void SymbolizeFrame(DWORD64 Addr, char* Out, size_t OutSize)
        {
            char         SymStorage[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* Symbol = reinterpret_cast<SYMBOL_INFO*>(SymStorage);
            std::memset(Symbol, 0, sizeof(SymStorage));
            Symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            Symbol->MaxNameLen   = 255;

            HANDLE  Process      = GetCurrentProcess();
            DWORD64 Displacement = 0;
            if (SymFromAddr(Process, Addr, &Displacement, Symbol))
            {
                IMAGEHLP_LINE64 LineInfo{};
                LineInfo.SizeOfStruct = sizeof(LineInfo);
                DWORD LineDisp = 0;
                if (SymGetLineFromAddr64(Process, Addr, &LineDisp, &LineInfo))
                {
                    _snprintf_s(Out, OutSize, _TRUNCATE, "  [0x%016llX] %s + 0x%llX  (%s:%lu)",
                        (unsigned long long)Addr, Symbol->Name, (unsigned long long)Displacement,
                        LineInfo.FileName ? LineInfo.FileName : "?", LineInfo.LineNumber);
                }
                else
                {
                    _snprintf_s(Out, OutSize, _TRUNCATE, "  [0x%016llX] %s + 0x%llX",
                        (unsigned long long)Addr, Symbol->Name, (unsigned long long)Displacement);
                }
            }
            else
            {
                _snprintf_s(Out, OutSize, _TRUNCATE, "  [0x%016llX] <no symbol>", (unsigned long long)Addr);
            }
        }

        void DumpThread(DWORD ThreadId)
        {
            DWORD64 Addrs[kMaxFrames];
            const uint32 Frames = CaptureThreadFrames(ThreadId, Addrs);

            char Name[160];
            GetThreadLabel(ThreadId, Name, sizeof(Name));
            const char* RoleTag = (ThreadId == GMainThreadId) ? " [MAIN]" : "";

            if (Frames == 0)
            {
                LOG_ERROR("-- Thread {} \"{}\"{} (stack unavailable)", (uint32)ThreadId, Name, RoleTag);
                return;
            }

            // Symbolize now that the thread is running again.
            char   Block[kMaxFrames * 512];
            size_t Offset = 0;
            Block[0] = 0;
            for (uint32 i = 0; i < Frames; ++i)
            {
                char Line[512];
                SymbolizeFrame(Addrs[i], Line, sizeof(Line));
                const size_t Len = std::strlen(Line);
                if (Offset + Len + 2 >= sizeof(Block))
                {
                    break;
                }
                std::memcpy(Block + Offset, Line, Len);
                Offset += Len;
                Block[Offset++] = '\n';
                Block[Offset]   = 0;
            }

            LOG_ERROR("-- Thread {} \"{}\"{}:\n{}", (uint32)ThreadId, Name, RoleTag, Block);
        }

        void DumpAllThreads()
        {
            EnsureSymbols();

            LOG_ERROR("==================== HANG WATCHDOG ====================");
            LOG_ERROR("Main thread (tid {}) stalled for >{:.1f}s. Dumping all thread stacks:",
                (uint32)GMainThreadId, GTimeoutSeconds);

            // The main thread is the most interesting stack; dump it first so we still get it even if
            // walking a later (e.g. managed/JIT) thread misbehaves.
            if (GMainThreadId != 0)
            {
                DumpThread(GMainThreadId);
            }

            const DWORD Pid      = GetCurrentProcessId();
            HANDLE      Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (Snapshot != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 Entry{};
                Entry.dwSize = sizeof(Entry);
                if (Thread32First(Snapshot, &Entry))
                {
                    do
                    {
                        if (Entry.th32OwnerProcessID != Pid)
                        {
                            continue;
                        }
                        const DWORD Tid = Entry.th32ThreadID;
                        if (Tid == GWatchdogThreadId || Tid == GMainThreadId)
                        {
                            continue; // never walk ourselves; main already dumped above
                        }
                        DumpThread(Tid);
                    }
                    while (Thread32Next(Snapshot, &Entry));
                }
                CloseHandle(Snapshot);
            }

            // Subsystem reporters last: they explain state the stacks can't (parked fibers, counters).
            const uint32 NumReporters = GNumReporters.load(std::memory_order_acquire);
            for (uint32 i = 0; i < NumReporters; ++i)
            {
                GReporters[i]();
            }

            LOG_ERROR("================== END HANG WATCHDOG ==================");
            if (Logging::IsInitialized())
            {
                Logging::GetLogger()->flush();
            }
        }

        void WatchdogMain()
        {
            Threading::SetThreadName("HangWatchdog", Threading::ThreadGroup_Other);
            GWatchdogThreadId = GetCurrentThreadId();

            uint64 LastBeat   = GHeartbeat.load(std::memory_order_relaxed);
            auto   LastChange = std::chrono::steady_clock::now();
            bool   bDumped    = false;

            while (GRunning.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));

                const uint64 Beat = GHeartbeat.load(std::memory_order_relaxed);
                const auto   Now  = std::chrono::steady_clock::now();

                if (Beat != LastBeat)
                {
                    LastBeat   = Beat;
                    LastChange = Now;
                    bDumped    = false;
                    continue;
                }

                // Beat == 0 means the frame loop hasn't started yet (boot/init can legitimately block
                // the main thread for seconds); only arm once we've seen real forward progress.
                if (Beat == 0 || bDumped)
                {
                    continue;
                }

                const float Elapsed = std::chrono::duration<float>(Now - LastChange).count();
                if (Elapsed >= GTimeoutSeconds)
                {
                    DumpAllThreads();
                    bDumped = true; // one dump per stall; re-arms if the main thread ever advances
                }
            }
        }
    }

    void Start(float TimeoutSeconds)
    {
        bool Expected = false;
        if (!GRunning.compare_exchange_strong(Expected, true))
        {
            return;
        }

        GTimeoutSeconds = TimeoutSeconds;
        GMainThreadId   = GetCurrentThreadId();
        GThread         = FThread(&WatchdogMain);
    }

    void Stop()
    {
        if (!GRunning.exchange(false))
        {
            return;
        }
        if (GThread.joinable())
        {
            GThread.join();
        }
    }

    void Heartbeat()
    {
        GHeartbeat.fetch_add(1, std::memory_order_relaxed);
    }

    void RegisterReporter(void (*Reporter)())
    {
        if (Reporter == nullptr)
        {
            return;
        }
        const uint32 Slot = GNumReporters.load(std::memory_order_relaxed);
        if (Slot >= kMaxReporters)
        {
            return;
        }
        GReporters[Slot] = Reporter;
        GNumReporters.store(Slot + 1, std::memory_order_release);
    }
}

#else

namespace Lumina::HangWatchdog
{
    void Start(float) {}
    void Stop() {}
    void Heartbeat() {}
    void RegisterReporter(void (*)()) {}
}

#endif
