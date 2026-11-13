#include <gtest/gtest.h>

#include "Memory/MemoryTracking.h"

#if LUMINA_MEMORY_TRACKING

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaMemoryTrackingTests
{
    using namespace Lumina;

    uint64 TotalAllocs(uint32 Category)
    {
        Memory::FMemoryCategoryStats Stats[256];
        return Category < Memory::GetCategoryStats(Stats, 256) ? Stats[Category].TotalAllocs : 0;
    }

    TEST(MemoryTracking, ScopesNestedPastTheStackCapStillAttributeToTheInnermostRecordedCategory)
    {
        constexpr uint32 kNesting = 64;
        const uint32 Category = Memory::RegisterCategory("MemoryTrackingTests.Nested");
        const bool bWasEnabled = Memory::IsTrackingEnabled();
        Memory::SetTrackingEnabled(true);

        for (uint32 Depth = 0; Depth < kNesting; ++Depth)
        {
            Memory::Hooks::PushCategory(Category);
        }

        const uint64 Before = TotalAllocs(Category);
        int Allocation = 0;
        Memory::Hooks::OnAlloc(&Allocation, sizeof(Allocation));
        const uint64 After = TotalAllocs(Category);
        Memory::Hooks::OnFree(&Allocation);

        for (uint32 Depth = 0; Depth < kNesting; ++Depth)
        {
            Memory::Hooks::PopCategory();
        }
        Memory::SetTrackingEnabled(bWasEnabled);

        EXPECT_EQ(After, Before + 1) << "an allocation under over-nested scopes lands in the pushed category, not Default";
    }
}

#endif
