#include "RuntimePCH.h"
#ifdef LE_PLATFORM_WINDOWS

#include "Thread.h"
#include <windows.h>
#pragma comment(lib, "Synchronization.lib")
#include <tracy/Tracy.hpp>

namespace Lumina::Threading
{
    uint64 GetThreadID()
    {
        return static_cast<uint64>(::GetCurrentThreadId());
    }

    uint32 GetNumThreads()
    {
        const DWORD Count = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        return Count != 0 ? static_cast<uint32>(Count) : 1u;
    }

    bool SetThreadName(const char* Name)
    {
        return SetThreadName(Name, ThreadGroup_Other);
    }

    bool SetThreadName(const char* Name, int32 GroupHint)
    {
        wchar_t WThreadName[255];
        auto pNativeThreadHandle = GetCurrentThread();
        MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, Name, -1, WThreadName, 255);
        HRESULT Result = SetThreadDescription(pNativeThreadHandle, WThreadName);
#ifdef TRACY_ENABLE
        tracy::SetThreadNameWithHint(Name, GroupHint);
#endif
        return Result != 0;
    }

    namespace
    {
        // Groups beyond the first are not described, since a partial map is worse than none.
        uint32 IndexOfBit(const GROUP_AFFINITY& Affinity, uint32 Bit)
        {
            return Affinity.Group * 64u + Bit;
        }
    }

    uint32 GetCpuTopology(FCpuTopology* Out, uint32 MaxCount)
    {
        if (Out == nullptr || MaxCount == 0)
        {
            return 0;
        }

        DWORD Bytes = 0;
        ::GetLogicalProcessorInformationEx(RelationAll, nullptr, &Bytes);
        if (Bytes == 0)
        {
            return 0;
        }

        auto* Buffer = static_cast<uint8*>(::malloc(Bytes));
        if (Buffer == nullptr)
        {
            return 0;
        }
        auto* Info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(Buffer);
        if (!::GetLogicalProcessorInformationEx(RelationAll, Info, &Bytes))
        {
            ::free(Buffer);
            return 0;
        }

        for (uint32 i = 0; i < MaxCount; ++i)
        {
            Out[i] = FCpuTopology{};
        }

        uint32 Described  = 0;
        uint16 NextCore   = 0;
        uint16 NextCache  = 0;
        uint8  BestLevel  = 0;

        // An L2-only machine still groups by its own last level instead of falling back to all-equidistant.
        for (uint8 Pass = 0; Pass < 2; ++Pass)
        {
            uint8* Cursor = Buffer;
            while (Cursor < Buffer + Bytes)
            {
                auto* Entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(Cursor);
                Cursor += Entry->Size;

                if (Pass == 0 && Entry->Relationship == RelationProcessorCore)
                {
                    const GROUP_AFFINITY& Affinity = Entry->Processor.GroupMask[0];
                    for (uint32 Bit = 0; Bit < 64; ++Bit)
                    {
                        if ((Affinity.Mask & (1ull << Bit)) == 0)
                        {
                            continue;
                        }
                        const uint32 Index = IndexOfBit(Affinity, Bit);
                        if (Index < MaxCount)
                        {
                            Out[Index].PhysicalCore = NextCore;
                            Described = Index + 1 > Described ? Index + 1 : Described;
                        }
                    }
                    ++NextCore;
                }
                else if (Entry->Relationship == RelationCache)
                {
                    const CACHE_RELATIONSHIP& Cache = Entry->Cache;
                    if (Cache.Type != CacheUnified && Cache.Type != CacheData)
                    {
                        continue;
                    }
                    if (Pass == 0)
                    {
                        BestLevel = Cache.Level > BestLevel ? Cache.Level : BestLevel;
                        continue;
                    }
                    if (Cache.Level != BestLevel)
                    {
                        continue;
                    }
                    const GROUP_AFFINITY& Affinity = Cache.GroupMask;
                    for (uint32 Bit = 0; Bit < 64; ++Bit)
                    {
                        if ((Affinity.Mask & (1ull << Bit)) == 0)
                        {
                            continue;
                        }
                        const uint32 Index = IndexOfBit(Affinity, Bit);
                        if (Index < MaxCount)
                        {
                            Out[Index].CacheGroup = NextCache;
                        }
                    }
                    ++NextCache;
                }
            }
        }

        ::free(Buffer);
        return Described;
    }

    bool WaitOnAddress32(const volatile uint32* Address, uint32 Compare, uint32 TimeoutMs)
    {
        uint32 Expected = Compare;
        return ::WaitOnAddress(const_cast<volatile VOID*>(reinterpret_cast<const volatile VOID*>(Address)),
                               &Expected, sizeof(uint32), TimeoutMs) != FALSE;
    }

    void WakeAllOnAddress32(const volatile uint32* Address)
    {
        ::WakeByAddressAll(const_cast<PVOID>(reinterpret_cast<const volatile void*>(Address)));
    }

    void SetThreadIdealProcessor(uint32 LogicalProcessor)
    {
        PROCESSOR_NUMBER Ideal = {};
        Ideal.Group  = static_cast<WORD>(LogicalProcessor / 64u);
        Ideal.Number = static_cast<BYTE>(LogicalProcessor % 64u);
        ::SetThreadIdealProcessorEx(::GetCurrentThread(), &Ideal, nullptr);
    }

    bool SetThreadPerformanceHint()
    {
        // A zero state mask opts out of EcoQoS so the Thread Director picks performance cores.
        THREAD_POWER_THROTTLING_STATE Throttling = {};
        Throttling.Version     = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        Throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        Throttling.StateMask   = 0;
        return SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &Throttling, sizeof(Throttling)) != 0;
    }

}


namespace Lumina
{
    namespace
    {
        DWORD WINAPI ThreadEntry(LPVOID Parameter)
        {
            TMoveOnlyFunction<void()>* Body = static_cast<TMoveOnlyFunction<void()>*>(Parameter);
            (*Body)();
            delete Body;
            return 0;
        }
    }

    void FThread::Start(TMoveOnlyFunction<void()>* Body)
    {
        DWORD ThreadId = 0;
        Handle = ::CreateThread(nullptr, 0, &ThreadEntry, Body, 0, &ThreadId);
        Id = static_cast<uint64>(ThreadId);

        if (Handle == nullptr)
        {
            delete Body;
            Id = 0;
        }
    }

    void FThread::Join() noexcept
    {
        if (Handle == nullptr)
        {
            return;
        }

        ::WaitForSingleObject(Handle, INFINITE);
        ::CloseHandle(Handle);
        Handle = nullptr;
        Id = 0;
    }

    void FThread::Detach() noexcept
    {
        if (Handle == nullptr)
        {
            return;
        }

        ::CloseHandle(Handle);
        Handle = nullptr;
        Id = 0;
    }

    FThread& FThread::operator=(FThread&& Other) noexcept
    {
        if (this != &Other)
        {
            Detach();
            Handle = Other.Handle;
            Id = Other.Id;
            Other.Handle = nullptr;
            Other.Id = 0;
        }

        return *this;
    }

    FThread::~FThread()
    {
        Detach();
    }
}

#endif
