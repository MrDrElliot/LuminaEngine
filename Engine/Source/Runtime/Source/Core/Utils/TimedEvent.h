#pragma once
#include "Containers/String.h"
#include "Core/LuminaMacros.h"
#include "Log/Log.h"


namespace Lumina
{
    template<typename T>
    class TTimedEvent
    {
    public:
        
        LE_NO_COPYMOVE(TTimedEvent<T>);
        
        TTimedEvent(FStringView InName)
            :Name(InName)
        {
            Start = std::chrono::high_resolution_clock::now();
        }
        
        ~TTimedEvent()
        {
            auto End = std::chrono::high_resolution_clock::now();
            auto Duration = std::chrono::duration_cast<T>(End - Start).count();
            LOG_INFO("Timed Event {} - Took {}{}", Name, Duration, GetUnitSuffix<T>());
        }
    
    private:
        
        
        template<typename Dur>
        static constexpr const char* GetUnitSuffix()
        {
            if constexpr (eastl::is_same_v<Dur, std::chrono::nanoseconds>) return "ns";
            else if constexpr (eastl::is_same_v<Dur, std::chrono::microseconds>) return "µs";
            else if constexpr (eastl::is_same_v<Dur, std::chrono::milliseconds>) return "ms";
            else if constexpr (eastl::is_same_v<Dur, std::chrono::seconds>) return "s";
            else if constexpr (eastl::is_same_v<Dur, std::chrono::minutes>) return "min";
            else if constexpr (eastl::is_same_v<Dur, std::chrono::hours>) return "h";
            else return "";
        }
        
        FStringView                                         Name;
        std::chrono::time_point<std::chrono::steady_clock>  Start;
    };
    
}
