#include "BenchCommon.h"

#include <cstdlib>
#include <cstdio>

namespace ECSBench
{
    namespace
    {
        constexpr int32 EntityCount = 200000;
        constexpr int32 Passes      = 5;
    }

    void ReportHeader()
    {
        std::printf("%-38s %10s %14s\n", "case", "ops", "ns per op");
        std::printf("%s\n", "----------------------------------------------------------------");
    }

    void ReportCase(const char* Name, int32 OpsPerPass, const FResult& Result)
    {
        std::printf("%-38s %10d %14.3f\n", Name, OpsPerPass, Result.Nanos);
        std::fflush(stdout);
    }

    void ReportFooter()
    {
        std::printf("%s\n", "----------------------------------------------------------------");
    }
}

namespace ECSBench
{
    void RunLayoutSweepCases(int32 EntityCount, int32 Passes);
    void RunExcludeCostCases(int32 EntityCount, int32 Passes);
    void RunEntityMapCases(int32 EntityCount, int32 Passes);
    void RunGroupHeadroomCases(int32 EntityCount, int32 Passes);

    bool RunSelfCheck();
}

int main(int Argc, char** Argv)
{
    using namespace ECSBench;

    // Unbuffered, so a crash mid-case still shows which case it died in.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    int32 Count = EntityCount;
    if (Argc > 1)
    {
        Count = static_cast<int32>(std::atoll(Argv[1]));
        if (Count == 0)
        {
            Count = EntityCount;
        }
    }

    std::printf("Lumina ECS, %d entities, best of %d passes\n\n", Count, Passes);

    if (!RunSelfCheck())
    {
        std::printf("self check failed, skipping the benchmark\n");
        return 1;
    }

    RunExcludeCostCases(Count, Passes);
    RunEntityMapCases(Count, Passes);
    RunLayoutSweepCases(Count, Passes);
    RunGroupHeadroomCases(Count, Passes);
    return 0;
}
