#include "RuntimePCH.h"
#ifdef LE_PLATFORM_WINDOWS

#include "Platform/Time/PlatformTime.h"

#include <ctime>
#include <windows.h>
#include <timeapi.h>

namespace Lumina::PlatformTime
{
    namespace
    {
        // FILETIME counts 100ns ticks from 1601-01-01; this is the gap to the Unix epoch.
        constexpr int64 kFileTimeToUnixTicks = 116444736000000000ll;

        struct FClockState
        {
            double SecondsPerCycleValue = 0.0;
            uint64 Origin = 0;

            FClockState() noexcept
            {
                LARGE_INTEGER Frequency{};
                QueryPerformanceFrequency(&Frequency);
                SecondsPerCycleValue = 1.0 / static_cast<double>(Frequency.QuadPart);

                LARGE_INTEGER Counter{};
                QueryPerformanceCounter(&Counter);
                Origin = static_cast<uint64>(Counter.QuadPart);
            }
        };

        const FClockState& Clock() noexcept
        {
            static const FClockState State;
            return State;
        }

        FDateTime FromBrokenDown(const tm& Parts, int64 UnixNanoseconds) noexcept
        {
            FDateTime Result;
            Result.Year        = Parts.tm_year + 1900;
            Result.Month       = Parts.tm_mon + 1;
            Result.Day         = Parts.tm_mday;
            Result.Hour        = Parts.tm_hour;
            Result.Minute      = Parts.tm_min;
            Result.Second      = Parts.tm_sec;
            Result.DayOfWeek   = Parts.tm_wday;
            Result.Millisecond = static_cast<int32>((UnixNanoseconds % 1000000000ll) / 1000000ll);
            return Result;
        }
    }

    uint64 Cycles() noexcept
    {
        LARGE_INTEGER Counter{};
        QueryPerformanceCounter(&Counter);
        return static_cast<uint64>(Counter.QuadPart);
    }

    double SecondsPerCycle() noexcept
    {
        return Clock().SecondsPerCycleValue;
    }

    double Seconds() noexcept
    {
        const FClockState& State = Clock();
        return static_cast<double>(Cycles() - State.Origin) * State.SecondsPerCycleValue;
    }

    double ToSeconds(uint64 CycleDelta) noexcept
    {
        return static_cast<double>(CycleDelta) * Clock().SecondsPerCycleValue;
    }

    double ToMilliseconds(uint64 CycleDelta) noexcept
    {
        return ToSeconds(CycleDelta) * 1000.0;
    }

    double ToMicroseconds(uint64 CycleDelta) noexcept
    {
        return ToSeconds(CycleDelta) * 1000000.0;
    }

    int64 UtcNanoseconds() noexcept
    {
        FILETIME FileTime{};
        GetSystemTimePreciseAsFileTime(&FileTime);

        const int64 Ticks = (static_cast<int64>(FileTime.dwHighDateTime) << 32) |
                             static_cast<int64>(FileTime.dwLowDateTime);

        return (Ticks - kFileTimeToUnixTicks) * 100ll;
    }

    int64 UtcSeconds() noexcept
    {
        return UtcNanoseconds() / 1000000000ll;
    }

    FDateTime LocalTime(int64 UnixNanoseconds) noexcept
    {
        const time_t Raw = static_cast<time_t>(UnixNanoseconds / 1000000000ll);
        tm Parts{};
        localtime_s(&Parts, &Raw);
        return FromBrokenDown(Parts, UnixNanoseconds);
    }

    FDateTime UtcTime(int64 UnixNanoseconds) noexcept
    {
        const time_t Raw = static_cast<time_t>(UnixNanoseconds / 1000000000ll);
        tm Parts{};
        gmtime_s(&Parts, &Raw);
        return FromBrokenDown(Parts, UnixNanoseconds);
    }

    FDateTime LocalNow() noexcept
    {
        return LocalTime(UtcNanoseconds());
    }

    void Sleep(double InSeconds) noexcept
    {
        if (InSeconds <= 0.0)
        {
            YieldThread();
            return;
        }

        // Rounded up, because a sleep that returns early is a busier spin loop than the caller asked for.
        const double Milliseconds = InSeconds * 1000.0;
        ::Sleep(static_cast<DWORD>(Milliseconds + 0.999));
    }

    void SleepMilliseconds(uint32 Milliseconds) noexcept
    {
        ::Sleep(static_cast<DWORD>(Milliseconds));
    }

    void SleepMicroseconds(uint32 Microseconds) noexcept
    {
        if (Microseconds == 0)
        {
            YieldThread();
            return;
        }

        ::Sleep(static_cast<DWORD>((Microseconds + 999u) / 1000u));
    }

    // The Sleep(0) fallback hands over the whole quantum and cost the job system roughly 4x.
    void YieldThread() noexcept
    {
        SwitchToThread();
    }

    void EnableHighResolutionTiming() noexcept
    {
        timeBeginPeriod(1);
    }

    void DisableHighResolutionTiming() noexcept
    {
        timeEndPeriod(1);
    }
}

#endif
