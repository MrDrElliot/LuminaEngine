#include "RHITestHarness.h"

#include "Renderer/RHITexture.h"

namespace Lumina::RHITests
{
    RHI_TEST(Heap, WriteAndFreeTexture)
    {
        const RHI::FTextureHeapH Heap = RHI::Core::GetGlobalHeap();
        const RHI::FTextureH Texture = Ctx.CreateTexture(MakeSampledDesc(8), "RHITests.HeapWrite");
        RHI_REQUIRE(RHI::IsValid(Texture));

        const uint32 Slot = RHI::HeapWriteTexture(Heap, Texture);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        RHI::HeapFreeTexture(Heap, Slot);
    }

    RHI_TEST(Heap, SlotsAreUniqueWhileLive)
    {
        const RHI::FTextureHeapH Heap = RHI::Core::GetGlobalHeap();

        const RHI::FTextureH A = Ctx.CreateTexture(MakeSampledDesc(8), "RHITests.HeapUniqueA");
        const RHI::FTextureH B = Ctx.CreateTexture(MakeSampledDesc(8), "RHITests.HeapUniqueB");
        RHI_REQUIRE(RHI::IsValid(A) && RHI::IsValid(B));

        const uint32 SlotA = RHI::HeapWriteTexture(Heap, A);
        const uint32 SlotB = RHI::HeapWriteTexture(Heap, B);
        RHI_REQUIRE(SlotA != RHI::kInvalidHeapSlot && SlotB != RHI::kInvalidHeapSlot);
        RHI_CHECK(SlotA != SlotB);

        RHI::HeapFreeTexture(Heap, SlotA);
        RHI::HeapFreeTexture(Heap, SlotB);
    }

    // Repointing replaces a texture without invalidating the index every material already baked.
    RHI_TEST(Heap, RepointKeepsSlot)
    {
        const RHI::FTextureHeapH Heap = RHI::Core::GetGlobalHeap();

        const RHI::FTextureH First  = Ctx.CreateTexture(MakeSampledDesc(8),  "RHITests.RepointFirst");
        const RHI::FTextureH Second = Ctx.CreateTexture(MakeSampledDesc(16), "RHITests.RepointSecond");
        RHI_REQUIRE(RHI::IsValid(First) && RHI::IsValid(Second));

        const uint32 Slot = RHI::HeapWriteTexture(Heap, First);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        RHI::HeapRepointTexture(Heap, Slot, Second);

        // The only observable effect is that the next allocation must not hand this one back.
        const RHI::FTextureH Third = Ctx.CreateTexture(MakeSampledDesc(8), "RHITests.RepointThird");
        const uint32 OtherSlot = RHI::HeapWriteTexture(Heap, Third);
        RHI_CHECK(OtherSlot != Slot);

        RHI::HeapFreeTexture(Heap, OtherSlot);
        RHI::HeapFreeTexture(Heap, Slot);
    }

    // Unbind points the descriptor at the fallback without releasing the index.
    RHI_TEST(Heap, UnbindKeepsSlotReserved)
    {
        const RHI::FTextureHeapH Heap = RHI::Core::GetGlobalHeap();

        const RHI::FTextureH Texture = Ctx.CreateTexture(MakeSampledDesc(8), "RHITests.Unbind");
        RHI_REQUIRE(RHI::IsValid(Texture));

        const uint32 Slot = RHI::HeapWriteTexture(Heap, Texture);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        RHI::HeapUnbindTexture(Heap, Slot);

        // Still reserved, so a fresh registration must not be handed the unbound index.
        const RHI::FTextureH Other = Ctx.CreateTexture(MakeSampledDesc(8), "RHITests.UnbindOther");
        const uint32 OtherSlot = RHI::HeapWriteTexture(Heap, Other);
        RHI_CHECK(OtherSlot != Slot);

        // And freeing after an unbind must not double-free or fault.
        RHI::HeapFreeTexture(Heap, Slot);
        RHI::HeapFreeTexture(Heap, OtherSlot);
    }

    RHI_TEST(Heap, WriteAndFreeRWTexture)
    {
        const RHI::FTextureHeapH Heap = RHI::Core::GetGlobalHeap();

        const RHI::FTextureH Texture = Ctx.CreateTexture(MakeSampledDesc(8, EFormat::RGBA8_UNORM, RHI::EImageUsageFlags::Storage), "RHITests.HeapRW");
        RHI_REQUIRE(RHI::IsValid(Texture));

        const uint32 Slot = RHI::HeapWriteRWTexture(Heap, Texture, 0);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        RHI::HeapFreeRWTexture(Heap, Slot);

        // The RW view is heap-owned and destroyed on a slot drain, so give it the frames to get there.
        Ctx.PumpFrames(RHI::kFramesInFlight + 1);
    }

    RHI_TEST(Heap, WriteAndFreeSampler)
    {
        const RHI::FTextureHeapH Heap = RHI::Core::GetGlobalHeap();

        RHI::FSamplerDesc Desc;
        Desc.MaxAnisotropy = 4.0f;

        const uint32 Slot = RHI::HeapWriteSampler(Heap, Desc);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        RHI::HeapFreeSampler(Heap, Slot);
        Ctx.PumpFrames(RHI::kFramesInFlight + 1);
    }

    // A reordered AddSampler in Core::Initialize silently repoints every sampler in the engine.
    RHI_TEST(Heap, StockSamplerSlotsMatchEnum)
    {
        // Every value pinned, since GlobalRHI.slang hardcodes the indices and a count alone hides a swap.
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::LinearWrap,   0u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::LinearClamp,  1u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::LinearMirror, 2u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::PointWrap,    3u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::PointClamp,   4u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::AnisoWrap,    5u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::AnisoClamp,   6u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::Shadow,       7u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::MinReduction, 8u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::MaxReduction, 9u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::PointMirror,  10u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::AnisoMirror,  11u);
        RHI_CHECK_EQ((uint32)RHI::EStockSampler::Count,        12u);
    }

    RHI_TEST(Heap, FreeInvalidSlotIsIgnored)
    {
        const RHI::FTextureHeapH Heap = RHI::Core::GetGlobalHeap();

        // Out of range on every path must be a no-op, not an out-of-bounds write into the slot arrays.
        RHI::HeapFreeTexture(Heap, 0x7FFFFFFFu);
        RHI::HeapUnbindTexture(Heap, 0x7FFFFFFFu);
        RHI::HeapFreeRWTexture(Heap, 0x7FFFFFFFu);
        RHI::HeapFreeSampler(Heap, 0x7FFFFFFFu);
    }
}
