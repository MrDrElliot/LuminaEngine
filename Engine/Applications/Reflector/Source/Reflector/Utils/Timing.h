#pragma once

#include <chrono>
#include <cstdio>

namespace Lumina::Reflection
{
    inline bool GReportTimings = false;

    // Reports on destruction, or earlier at a Stop() when the phase does not end with a scope.
    class FScopedPhaseTimer
    {
    public:

        explicit FScopedPhaseTimer(const char* InName)
            : Name(InName)
            , Start(std::chrono::steady_clock::now())
        {}

        ~FScopedPhaseTimer() { Stop(); }

        FScopedPhaseTimer(const FScopedPhaseTimer&) = delete;
        FScopedPhaseTimer& operator=(const FScopedPhaseTimer&) = delete;

        void Stop()
        {
            if (Name == nullptr)
            {
                return;
            }

            if (GReportTimings)
            {
                const double Milliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - Start).count();
                std::printf("[timing] %-30s %8.1f ms\n", Name, Milliseconds);
                std::fflush(stdout);
            }

            Name = nullptr;
        }

    private:

        const char*                           Name;
        std::chrono::steady_clock::time_point Start;
    };
}
