#include "RHITestHarness.h"

#include "Renderer/RHITexture.h"
#include "Renderer/RHIUpload.h"

namespace Lumina::RHITests
{
    namespace
    {
        constexpr uint32 kRed     = 0xFF0000FFu;
        constexpr uint32 kGreen   = 0xFF00FF00u;
        constexpr uint32 kMagenta = 0xFFFF00FFu;

        RHI::FPipelineH CreateSlotProbe(FTestContext& Ctx)
        {
            const TVector<uint32> Spirv = Ctx.CompileShader(R"SLANG(
                struct FRHIRoot { uint64_t Args; };
                [[vk::push_constant]] FRHIRoot gRHI;
                [[vk::binding(1, 0)]] Texture2D<float4> gTextures[];

                struct FArgs { uint* Output; uint Slot; };

                [shader("compute")]
                [numthreads(1, 1, 1)]
                void main()
                {
                    FArgs* Pass = (FArgs*)gRHI.Args;
                    const uint4 Bytes = uint4(round(saturate(gTextures[NonUniformResourceIndex(Pass.Slot)].Load(int3(0, 0, 0))) * 255.0));
                    Pass.Output[0] = Bytes.r | (Bytes.g << 8) | (Bytes.b << 16) | (Bytes.a << 24);
                }
            )SLANG", "RHITests.SlotProbe");

            return Spirv.empty() ? RHI::FPipelineH{} : Ctx.TrackPipeline(RHI::CreateComputePipeline(ShaderSource(Spirv)));
        }

        RHI::FTextureH CreateSolidTexture(const float (&Color)[4], const char* DebugName)
        {
            const RHI::FTextureH Texture = RHI::CreateTexture(MakeSampledDesc(1));
            RHI::SetDebugName(Texture, DebugName);
            RHI::UploadClearTexture(Texture, Color);
            RHI::FlushUploadsAndWait();
            return Texture;
        }

        // What a dispatch recorded now reads through Slot, packed as RGBA8.
        uint32 ProbeSlot(FTestContext& Ctx, RHI::FPipelineH Probe, uint32 Slot)
        {
            const RHI::FGPUAllocation Output   = Ctx.Malloc(sizeof(uint32), RHI::EMemoryType::GPUOnly, "RHITests.ProbeOutput");
            const RHI::FGPUAllocation Readback = Ctx.Malloc(sizeof(uint32), RHI::EMemoryType::CPURead, "RHITests.ProbeReadback");

            struct FArgs { RHI::GPUPtr Output; uint32 Slot; uint32 _Pad; };
            const RHI::GPUPtr Args = RHI::CopyTransient(FArgs{ Output.Gpu, Slot, 0 });

            const RHI::FCmdListH CL = Ctx.OpenCL();
            RHI::CmdSetPipeline(CL, Probe);
            RHI::CmdDispatch(CL, Args, 1, 1, 1);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            RHI::CmdMemcpy(CL, { Readback.Gpu, sizeof(uint32) }, { Output.Gpu, sizeof(uint32) });
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Host,
                RHI::EAccessFlags::HostRead);
            Ctx.SubmitAndWait(CL);

            const uint32* Word = Readback.CpuAs<const uint32>();
            return Word != nullptr ? *Word : 0u;
        }
    }

    // Frames already recording keep the old texture, since rewriting a descriptor they read is undefined.
    RHI_TEST(Heap, RepointReachesOnlyLaterFrames)
    {
        const RHI::FPipelineH Probe = CreateSlotProbe(Ctx);
        RHI_REQUIRE(RHI::IsValid(Probe));

        const float Red[4]   = { 1.0f, 0.0f, 0.0f, 1.0f };
        const float Green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        const RHI::FTextureH First  = CreateSolidTexture(Red, "RHITests.RepointRed");
        const RHI::FTextureH Second = CreateSolidTexture(Green, "RHITests.RepointGreen");
        RHI_REQUIRE(RHI::IsValid(First) && RHI::IsValid(Second));

        const uint32 Slot = RHI::HeapWriteTexture(RHI::GetGlobalHeap(), First);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);
        RHI_CHECK_EQ(ProbeSlot(Ctx, Probe, Slot), kRed);

        RHI::HeapRepointTexture(RHI::GetGlobalHeap(), Slot, Second);
        RHI_CHECK_EQ(ProbeSlot(Ctx, Probe, Slot), kRed);

        for (uint32 Frame = 0; Frame < RHI::kFramesInFlight; ++Frame)
        {
            Ctx.PumpFrames(1);
            RHI_CHECK_EQ(ProbeSlot(Ctx, Probe, Slot), kGreen);
        }

        RHI::RetireSampledSlot(Slot);
        RHI::Retire(First);
        RHI::Retire(Second);
        Ctx.PumpFrames(RHI::kFramesInFlight * 2 + 1);
    }

    // A retired slot stays on its texture for the frame that retired it, then reads the fallback.
    RHI_TEST(Heap, RetiredSlotFallsBackNextFrame)
    {
        const RHI::FPipelineH Probe = CreateSlotProbe(Ctx);
        RHI_REQUIRE(RHI::IsValid(Probe));

        const float Red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        const RHI::FTextureH Texture = CreateSolidTexture(Red, "RHITests.RetiredRed");
        RHI_REQUIRE(RHI::IsValid(Texture));

        const uint32 Slot = RHI::HeapWriteTexture(RHI::GetGlobalHeap(), Texture);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        RHI::RetireSampledSlot(Slot);
        RHI::Retire(Texture);
        RHI_CHECK_EQ(ProbeSlot(Ctx, Probe, Slot), kRed);

        Ctx.PumpFrames(1);
        RHI_CHECK_EQ(ProbeSlot(Ctx, Probe, Slot), kMagenta);

        Ctx.PumpFrames(RHI::kFramesInFlight * 2 + 1);
    }

    RHI_TEST(Heap, WriteAndFreeTexture)
    {
        const RHI::FTextureHeapH Heap = RHI::GetGlobalHeap();
        const RHI::FTextureH Texture = Ctx.CreateTexture(MakeSampledDesc(8), "RHITests.HeapWrite");
        RHI_REQUIRE(RHI::IsValid(Texture));

        const uint32 Slot = RHI::HeapWriteTexture(Heap, Texture);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        RHI::HeapFreeTexture(Heap, Slot);
    }

    RHI_TEST(Heap, SlotsAreUniqueWhileLive)
    {
        const RHI::FTextureHeapH Heap = RHI::GetGlobalHeap();

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
        const RHI::FTextureHeapH Heap = RHI::GetGlobalHeap();

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
        const RHI::FTextureHeapH Heap = RHI::GetGlobalHeap();

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
        const RHI::FTextureHeapH Heap = RHI::GetGlobalHeap();

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
        const RHI::FTextureHeapH Heap = RHI::GetGlobalHeap();

        RHI::FSamplerDesc Desc;
        Desc.MaxAnisotropy = 4.0f;

        const uint32 Slot = RHI::HeapWriteSampler(Heap, Desc);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        RHI::HeapFreeSampler(Heap, Slot);
        Ctx.PumpFrames(RHI::kFramesInFlight + 1);
    }

    // A reordered AddSampler in InitializeCore silently repoints every sampler in the engine.
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
        const RHI::FTextureHeapH Heap = RHI::GetGlobalHeap();

        // Out of range on every path must be a no-op, not an out-of-bounds write into the slot arrays.
        RHI::HeapFreeTexture(Heap, 0x7FFFFFFFu);
        RHI::HeapUnbindTexture(Heap, 0x7FFFFFFFu);
        RHI::HeapFreeRWTexture(Heap, 0x7FFFFFFFu);
        RHI::HeapFreeSampler(Heap, 0x7FFFFFFFu);
    }
}
