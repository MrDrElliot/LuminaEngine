#include "RuntimePCH.h"
#ifdef LE_PLATFORM_WINDOWS

#include "Thread.h"
#include <windows.h>
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

    bool SetThreadPerformanceHint()
    {
        // ControlMask selects EXECUTION_SPEED; StateMask = 0 means "do not throttle", i.e. opt out of
        // EcoQoS so the Thread Director schedules this thread on performance cores.
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
