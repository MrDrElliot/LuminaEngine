#include "BenchCommon.h"

#include <cstdlib>
#include <cstdio>

namespace ECSBench
{
    namespace
    {
        constexpr size_t EntityCount = 200000;
        constexpr size_t Passes      = 5;
    }

    void ReportHeader()
    {
        std::printf("%-38s %10s %14s\n", "case", "ops", "ns per op");
        std::printf("%s\n", "----------------------------------------------------------------");
    }

    void ReportCase(const char* Name, size_t OpsPerPass, const FResult& Result)
    {
        std::printf("%-38s %10zu %14.3f\n", Name, OpsPerPass, Result.Nanos);
        std::fflush(stdout);
    }

    void ReportFooter()
    {
        std::printf("%s\n", "----------------------------------------------------------------");
    }
}

namespace ECSBench
{
    void RunLayoutSweepCases(size_t EntityCount, size_t Passes);
    void RunExcludeCostCases(size_t EntityCount, size_t Passes);
    void RunEntityMapCases(size_t EntityCount, size_t Passes);

    bool RunSelfCheck();
}

int main(int Argc, char** Argv)
{
    using namespace ECSBench;

    // Unbuffered, so a crash mid-case still shows which case it died in.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    size_t Count = EntityCount;
    if (Argc > 1)
    {
        Count = static_cast<size_t>(std::atoll(Argv[1]));
        if (Count == 0)
        {
            Count = EntityCount;
        }
    }

    std::printf("Lumina ECS, %zu entities, best of %zu passes\n\n", Count, Passes);

    if (!RunSelfCheck())
    {
        std::printf("self check failed, skipping the benchmark\n");
        return 1;
    }

    RunExcludeCostCases(Count, Passes);
    RunEntityMapCases(Count, Passes);
    RunLayoutSweepCases(Count, Passes);
    return 0;
}
