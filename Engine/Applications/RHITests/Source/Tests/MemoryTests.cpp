#include "RHITestHarness.h"

#include "Renderer/RHIUpload.h"

namespace Lumina::RHITests
{
    RHI_TEST(Memory, MallocGPUOnly)
    {
        const RHI::GPUPtr Ptr = Ctx.Malloc(64 * 1024, RHI::EMemoryType::GPUOnly, "RHITests.GPUOnly");
        RHI_REQUIRE(Ptr != 0);

        // Device-local memory has no host mapping, so asking for one must not hand back a stray pointer.
        RHI_CHECK(RHI::ToHost(Ptr) == nullptr);
    }

    RHI_TEST(Memory, MallocCPUWriteIsMapped)
    {
        const RHI::GPUPtr Ptr = Ctx.Malloc(4096, RHI::EMemoryType::CPUWrite, "RHITests.CPUWrite");
        RHI_REQUIRE(Ptr != 0);

        void* Host = RHI::ToHost(Ptr);
        RHI_REQUIRE(Host != nullptr);

        // Persistently mapped, so a plain store is visible to the GPU without an explicit flush.
        auto* Words = static_cast<uint32*>(Host);
        for (uint32 i = 0; i < 1024; ++i)
        {
            Words[i] = i;
        }
        RHI_CHECK_EQ(Words[1023], 1023u);
    }

    RHI_TEST(Memory, MallocCPUReadIsMapped)
    {
        const RHI::GPUPtr Ptr = Ctx.Malloc(4096, RHI::EMemoryType::CPURead, "RHITests.CPURead");
        RHI_REQUIRE(Ptr != 0);
        RHI_CHECK(RHI::ToHost(Ptr) != nullptr);
    }

    RHI_TEST(Memory, DistinctAllocationsDoNotOverlap)
    {
        const uint64 Size = 16 * 1024;

        const RHI::GPUPtr A = Ctx.Malloc(Size, RHI::EMemoryType::GPUOnly, "RHITests.OverlapA");
        const RHI::GPUPtr B = Ctx.Malloc(Size, RHI::EMemoryType::GPUOnly, "RHITests.OverlapB");
        RHI_REQUIRE(A != 0 && B != 0);

        const bool bDisjoint = (A + Size <= B) || (B + Size <= A);
        RHI_CHECK(bDisjoint);
    }

    // Above kDedicatedMemoryThreshold, so this takes the VMA dedicated path a large mesh lands on.
    RHI_TEST(Memory, LargeDedicatedAllocation)
    {
        const RHI::GPUPtr Ptr = Ctx.Malloc(96ull * 1024 * 1024, RHI::EMemoryType::GPUOnly, "RHITests.Large");
        RHI_CHECK(Ptr != 0);
    }

    RHI_TEST(Memory, AlignmentIsHonored)
    {
        const RHI::GPUPtr Ptr = RHI::Malloc(4096, 256, RHI::EMemoryType::GPUOnly);
        RHI_REQUIRE(Ptr != 0);
        RHI_CHECK_EQ(Ptr % 256ull, 0ull);
        RHI::Core::Retire(Ptr);
    }

    RHI_TEST(Memory, ZeroSizedMallocReturnsNull)
    {
        RHI_CHECK(RHI::Malloc(0, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly) == 0);
    }

    RHI_TEST(Memory, GPUMemoryStatsAreSane)
    {
        RHI::FGPUMemoryStats Stats;
        RHI::GetGPUMemoryStats(Stats);

        RHI_CHECK(Stats.TotalBudget > 0);
        RHI_CHECK(!Stats.Heaps.empty());
        RHI_CHECK(Stats.TotalUsage <= Stats.TotalBudget);
    }
}
