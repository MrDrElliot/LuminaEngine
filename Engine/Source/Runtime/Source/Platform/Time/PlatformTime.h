#pragma once

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::PlatformTime
{
    /** Broken-down calendar time, for stamps and file names. */
    struct FDateTime
    {
        int32 Year        = 0;
        int32 Month       = 0;
        int32 Day         = 0;
        int32 Hour        = 0;
        int32 Minute      = 0;
        int32 Second      = 0;
        int32 Millisecond = 0;
        int32 DayOfWeek   = 0;
    };

    /** Monotonic counter in platform-defined units; turn a delta into time through ToSeconds. */
    NODISCARD RUNTIME_API uint64 Cycles() noexcept;

    NODISCARD RUNTIME_API double SecondsPerCycle() noexcept;

    /** Monotonic seconds since process start; unaffected by the system clock being set. */
    NODISCARD RUNTIME_API double Seconds() noexcept;

    NODISCARD RUNTIME_API double ToSeconds(uint64 CycleDelta) noexcept;
    NODISCARD RUNTIME_API double ToMilliseconds(uint64 CycleDelta) noexcept;
    NODISCARD RUNTIME_API double ToMicroseconds(uint64 CycleDelta) noexcept;

    /** Wall clock since the Unix epoch; it jumps when the clock is set, so never measure a span with it. */
    NODISCARD RUNTIME_API int64 UtcNanoseconds() noexcept;
    NODISCARD RUNTIME_API int64 UtcSeconds() noexcept;

    NODISCARD RUNTIME_API FDateTime LocalTime(int64 UnixNanoseconds) noexcept;
    NODISCARD RUNTIME_API FDateTime UtcTime(int64 UnixNanoseconds) noexcept;
    NODISCARD RUNTIME_API FDateTime LocalNow() noexcept;

    RUNTIME_API void Sleep(double InSeconds) noexcept;

    /** Waits the requested span rather than rounding it up to a scheduler tick. For frame pacing. */
    RUNTIME_API void SleepPrecise(double InSeconds) noexcept;
    RUNTIME_API void SleepMilliseconds(uint32 Milliseconds) noexcept;
    RUNTIME_API void SleepMicroseconds(uint32 Microseconds) noexcept;

    /** Gives the rest of this time slice to another runnable thread on the same core. */
    RUNTIME_API void YieldThread() noexcept;

    /** Asks the OS for its finest scheduler tick so Sleep honors sub-millisecond waits; pair the two. */
    RUNTIME_API void EnableHighResolutionTiming() noexcept;
    RUNTIME_API void DisableHighResolutionTiming() noexcept;

    /** Measures a span in cycles, so the arithmetic stays integral until someone asks for a unit. */
    class FStopwatch
    {
    public:

        FStopwatch() noexcept : Start(Cycles()) {}

        void Restart() noexcept { Start = Cycles(); }

        NODISCARD uint64 ElapsedCycles() const noexcept { return Cycles() - Start; }
        NODISCARD double ElapsedSeconds() const noexcept { return ToSeconds(ElapsedCycles()); }
        NODISCARD double ElapsedMilliseconds() const noexcept { return ToMilliseconds(ElapsedCycles()); }
        NODISCARD double ElapsedMicroseconds() const noexcept { return ToMicroseconds(ElapsedCycles()); }

    private:

        uint64 Start;
    };
}
