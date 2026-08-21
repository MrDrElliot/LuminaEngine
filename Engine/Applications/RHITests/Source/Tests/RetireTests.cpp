#include "RHITestHarness.h"

#include "Renderer/RHITexture.h"

namespace Lumina::RHITests
{
    RHI_TEST(Retire, BufferSurvivesUntilDrained)
    {
        const RHI::GPUPtr Buffer = RHI::Malloc(4096, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
        RHI_REQUIRE(Buffer != 0);
        RHI::SetDebugName(Buffer, "RHITests.RetireBuffer");

        const RHI::FCmdListH CL = Ctx.OpenCL();
        RHI::CmdMemzero(CL, Buffer, 4096);
        Ctx.SubmitAndWait(CL);

        RHI::Core::Retire(Buffer);
        Ctx.PumpFrames(RHI::kFramesInFlight * 2 + 1);
    }

    RHI_TEST(Retire, TextureAndItsHeapSlot)
    {
        const RHI::FTextureH Texture = RHI::CreateTexture(MakeSampledDesc(16));
        RHI_REQUIRE(RHI::IsValid(Texture));
        RHI::SetDebugName(Texture, "RHITests.RetireTexture");

        const uint32 Slot = RHI::HeapWriteTexture(RHI::Core::GetGlobalHeap(), Texture);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        const RHI::FCmdListH CL = Ctx.OpenCL();
        const float Clear[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        RHI::Barriers::AllToTransfer(CL);
        RHI::CmdClearTexture(CL, Texture, Clear);
        RHI::Barriers::TransferToAll(CL);
        Ctx.SubmitAndWait(CL);

        RHI::Core::RetireSampledSlot(Slot);
        RHI::Core::Retire(Texture);

        Ctx.PumpFrames(RHI::kFramesInFlight * 2 + 1);
    }

    // Regression test for a resource retired while the frame referencing it is STILL EXECUTING.
    RHI_TEST(Retire, RetireWhileWorkInFlight)
    {
        const RHI::FTextureH Texture = RHI::CreateTexture(MakeSampledDesc(256));
        RHI_REQUIRE(RHI::IsValid(Texture));
        RHI::SetDebugName(Texture, "RHITests.InFlightTexture");

        const uint32 Slot = RHI::HeapWriteTexture(RHI::Core::GetGlobalHeap(), Texture);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        const RHI::GPUPtr Buffer = RHI::Malloc(4 * 1024 * 1024, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
        RHI_REQUIRE(Buffer != 0);
        RHI::SetDebugName(Buffer, "RHITests.InFlightBuffer");

        const RHI::FCmdListH CL = Ctx.OpenCL();

        // Enough work that the submission is unlikely to have retired by the time we return.
        const float Clear[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
        RHI::Barriers::AllToTransfer(CL);
        for (uint32 i = 0; i < 64; ++i)
        {
            RHI::CmdMemset(CL, Buffer, 4 * 1024 * 1024, i);
            RHI::Barriers::TransferToTransfer(CL);
        }
        RHI::CmdClearTexture(CL, Texture, Clear);
        RHI::Barriers::TransferToAll(CL);

        // No wait.
        RHI::Core::SubmitOn(RHI::EQueueType::Graphics, TSpan{ &CL, 1 });

        // ... and retire immediately, exactly as an asset delete or a thumbnail sweep does.
        RHI::Core::RetireSampledSlot(Slot);
        RHI::Core::Retire(Texture);
        RHI::Core::Retire(Buffer);

        // Drives the ring hard enough that a queue keyed only on the frame slot would have destroyed both.
        Ctx.PumpFrames(RHI::kFramesInFlight * 3);
    }

    // The descriptor must stop pointing at the texture when it is retired, not when the queue drains.
    RHI_TEST(Retire, SampledSlotUnbindsBeforeDrain)
    {
        const RHI::FTextureH Texture = RHI::CreateTexture(MakeSampledDesc(64));
        RHI_REQUIRE(RHI::IsValid(Texture));
        RHI::SetDebugName(Texture, "RHITests.UnbindBeforeDrain");

        const uint32 Slot = RHI::HeapWriteTexture(RHI::Core::GetGlobalHeap(), Texture);
        RHI_REQUIRE(Slot != RHI::kInvalidHeapSlot);

        RHI::Core::RetireSampledSlot(Slot);
        RHI::Core::Retire(Texture);

        // With the unbind deferred, this list would bind a retired texture and still be in flight.
        for (uint32 i = 0; i < RHI::kFramesInFlight * 2; ++i)
        {
            const RHI::FCmdListH CL = Ctx.OpenCL();
            RHI::Barriers::AllToTransfer(CL);
            RHI::Core::SubmitOn(RHI::EQueueType::Graphics, TSpan{ &CL, 1 });
            Ctx.PumpFrames(1);
        }

        Ctx.PumpFrames(RHI::kFramesInFlight * 2);
    }

    RHI_TEST(Retire, RetireOfInvalidHandlesIsIgnored)
    {
        RHI::Core::Retire(RHI::GPUPtr{ 0 });
        RHI::Core::Retire(RHI::FTextureH{});
        RHI::Core::RetireSampledSlot(RHI::kInvalidHeapSlot);
        RHI::Core::RetireStorageSlot(RHI::kInvalidHeapSlot);

        Ctx.PumpFrames(RHI::kFramesInFlight + 1);
    }

    RHI_TEST(Retire, ManyRetiresInOneSlot)
    {
        constexpr uint32 Count = 256;

        for (uint32 i = 0; i < Count; ++i)
        {
            const RHI::GPUPtr Ptr = RHI::Malloc(1024, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            if (Ptr == 0)
            {
                Ctx.Failf("allocation %u of %u failed", i, Count);
                break;
            }
            RHI::Core::Retire(Ptr);
        }

        Ctx.PumpFrames(RHI::kFramesInFlight * 2 + 1);
    }
}
