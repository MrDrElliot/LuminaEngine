#include "RuntimePCH.h"
#ifdef LE_PLATFORM_WINDOWS

#include "Thread.h"
#include <windows.h>
#include <tracy/Tracy.hpp>

namespace Lumina::Threading
{
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

#endif