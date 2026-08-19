#include "RuntimePCH.h"
#ifdef LE_PLATFORM_LINUX

#include "Platform/Time/PlatformTime.h"

#include <ctime>
#include <sched.h>

namespace Lumina::PlatformTime
{
    namespace
    {
        // The monotonic counter is read straight in nanoseconds, so a cycle is a nanosecond here.
        constexpr double kSecondsPerCycle = 1e-9;

        uint64 MonotonicNanoseconds() noexcept
        {
            timespec Now{};
            clock_gettime(CLOCK_MONOTONIC, &Now);
            return static_cast<uint64>(Now.tv_sec) * 1000000000ull + static_cast<uint64>(Now.tv_nsec);
        }

        uint64 Origin() noexcept
        {
            static const uint64 Start = MonotonicNanoseconds();
            return Start;
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

        void SleepNanoseconds(int64 Nanoseconds) noexcept
        {
            timespec Request{};
            Request.tv_sec  = static_cast<time_t>(Nanoseconds / 1000000000ll);
            Request.tv_nsec = static_cast<long>(Nanoseconds % 1000000000ll);

            timespec Remaining{};
            while (nanosleep(&Request, &Remaining) == -1)
            {
                Request = Remaining;
            }
        }
    }

    uint64 Cycles() noexcept
    {
        return MonotonicNanoseconds();
    }

    double SecondsPerCycle() noexcept
    {
        return kSecondsPerCycle;
    }

    double Seconds() noexcept
    {
        return static_cast<double>(MonotonicNanoseconds() - Origin()) * kSecondsPerCycle;
    }

    double ToSeconds(uint64 CycleDelta) noexcept
    {
        return static_cast<double>(CycleDelta) * kSecondsPerCycle;
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
        timespec Now{};
        clock_gettime(CLOCK_REALTIME, &Now);
        return static_cast<int64>(Now.tv_sec) * 1000000000ll + static_cast<int64>(Now.tv_nsec);
    }

    int64 UtcSeconds() noexcept
    {
        return UtcNanoseconds() / 1000000000ll;
    }

    FDateTime LocalTime(int64 UnixNanoseconds) noexcept
    {
        const time_t Raw = static_cast<time_t>(UnixNanoseconds / 1000000000ll);
        tm Parts{};
        localtime_r(&Raw, &Parts);
        return FromBrokenDown(Parts, UnixNanoseconds);
    }

    FDateTime UtcTime(int64 UnixNanoseconds) noexcept
    {
        const time_t Raw = static_cast<time_t>(UnixNanoseconds / 1000000000ll);
        tm Parts{};
        gmtime_r(&Raw, &Parts);
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

        SleepNanoseconds(static_cast<int64>(InSeconds * 1e9));
    }

    void SleepMilliseconds(uint32 Milliseconds) noexcept
    {
        SleepNanoseconds(static_cast<int64>(Milliseconds) * 1000000ll);
    }

    void SleepMicroseconds(uint32 Microseconds) noexcept
    {
        if (Microseconds == 0)
        {
            YieldThread();
            return;
        }

        SleepNanoseconds(static_cast<int64>(Microseconds) * 1000ll);
    }

    void YieldThread() noexcept
    {
        sched_yield();
    }

    void EnableHighResolutionTiming() noexcept
    {
    }

    void DisableHighResolutionTiming() noexcept
    {
    }
}

#endif
