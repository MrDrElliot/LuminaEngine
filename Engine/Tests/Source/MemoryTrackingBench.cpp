#include <gtest/gtest.h>
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include <chrono>
#include <cstdio>

using namespace Lumina;

namespace
{
    using FClock = std::chrono::steady_clock;

    constexpr size_t kOps = 200000;

    // Sizes spread across rpmalloc's small/medium classes so the measurement is not one hot bucket.
    constexpr size_t kSizes[] = { 24, 64, 200, 512, 1400, 4096 };

    double RunAllocFreePass()
    {
        void* Ptrs[64];
        const FClock::time_point Start = FClock::now();

        for (size_t Op = 0; Op < kOps; Op += 64)
        {
            for (size_t i = 0; i < 64; ++i)
            {
                Ptrs[i] = Memory::Malloc(kSizes[(Op + i) % (sizeof(kSizes) / sizeof(kSizes[0]))]);
            }
            for (size_t i = 0; i < 64; ++i)
            {
                Memory::Free(Ptrs[i]);
            }
        }

        const FClock::time_point End = FClock::now();
        const double Total = std::chrono::duration<double, std::nano>(End - Start).count();
        return Total / (double)kOps;
    }

    double MeasureWith(bool bTracking, bool bCallstacks)
    {
#if LUMINA_MEMORY_TRACKING
        Memory::SetTrackingEnabled(bTracking);
        Memory::SetCaptureCallstacks(bCallstacks);
#else
        (void)bTracking; (void)bCallstacks;
#endif
        RunAllocFreePass();                 // warm the allocator and the tables
        double Best = RunAllocFreePass();
        for (int i = 0; i < 3; ++i)
        {
            Best = std::min(Best, RunAllocFreePass());
        }
        return Best;
    }
}

TEST(MemoryTrackingBench, AllocFreeCostByTrackingState)
{
#if !LUMINA_MEMORY_TRACKING
    GTEST_SKIP() << "Tracking compiled out; the hooks do not exist in this configuration.";
#else
    const bool bWasTracking   = Memory::IsTrackingEnabled();
    const bool bWasCapturing  = Memory::IsCapturingCallstacks();

    const double Off      = MeasureWith(false, false);
    const double On       = MeasureWith(true,  false);
    const double Stacks   = MeasureWith(true,  true);

    Memory::SetTrackingEnabled(bWasTracking);
    Memory::SetCaptureCallstacks(bWasCapturing);

    std::printf("\n[ MEMORY TRACKING ] ns per malloc+free pair, best of 4 x %zu ops\n", kOps);
    std::printf("  tracking off              %7.1f ns   (baseline: hook call + atomic load + branch)\n", Off);
    std::printf("  tracking on, no stacks    %7.1f ns   (+%.1f ns, %.2fx)\n", On, On - Off, On / Off);
    std::printf("  tracking on, stacks on    %7.1f ns   (+%.1f ns, %.2fx)\n", Stacks, Stacks - Off, Stacks / Off);

    EXPECT_GT(Off, 0.0);
#endif
}
