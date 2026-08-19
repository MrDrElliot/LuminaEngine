#pragma once
#include "Containers/String.h"
#include "Core/LuminaMacros.h"
#include "Log/Log.h"
#include "Platform/Time/PlatformTime.h"


namespace Lumina
{
    enum class ETimeUnit : uint8
    {
        Nanoseconds,
        Microseconds,
        Milliseconds,
        Seconds,
    };

    /** Logs how long its scope took, at the unit you ask for. */
    template<ETimeUnit Unit = ETimeUnit::Milliseconds>
    class TTimedEvent
    {
    public:

        LE_NO_COPYMOVE(TTimedEvent<Unit>);

        TTimedEvent(FStringView InName)
            : Name(InName)
        {
        }

        ~TTimedEvent()
        {
            LOG_INFO("Timed Event {} - Took {:.3f}{}", Name, Elapsed(), GetUnitSuffix());
        }

    private:

        NODISCARD double Elapsed() const
        {
            const uint64 Cycles = Timer.ElapsedCycles();

            if constexpr (Unit == ETimeUnit::Nanoseconds)  { return PlatformTime::ToSeconds(Cycles) * 1e9; }
            else if constexpr (Unit == ETimeUnit::Microseconds) { return PlatformTime::ToMicroseconds(Cycles); }
            else if constexpr (Unit == ETimeUnit::Milliseconds) { return PlatformTime::ToMilliseconds(Cycles); }
            else { return PlatformTime::ToSeconds(Cycles); }
        }

        NODISCARD static constexpr const char* GetUnitSuffix()
        {
            if constexpr (Unit == ETimeUnit::Nanoseconds)  { return "ns"; }
            else if constexpr (Unit == ETimeUnit::Microseconds) { return "us"; }
            else if constexpr (Unit == ETimeUnit::Milliseconds) { return "ms"; }
            else { return "s"; }
        }

        FStringView              Name;
        PlatformTime::FStopwatch Timer;
    };

}
