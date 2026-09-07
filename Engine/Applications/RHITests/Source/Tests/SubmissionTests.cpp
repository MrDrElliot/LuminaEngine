#include "RHITestHarness.h"
#include "Log/Log.h"

namespace Lumina::RHITests
{
    RHI_TEST(Submission, TimelineSemaphoreStartsAtInitialValue)
    {
        const RHI::FSemaphoreH Semaphore = RHI::CreateTimelineSemaphore(7);
        RHI_REQUIRE(RHI::IsValid(Semaphore));

        RHI_CHECK_EQ(RHI::GetSemaphoreValue(Semaphore), 7ull);

        RHI::WaitDeviceIdle();
        RHI::Retire(Semaphore);
    }

    RHI_TEST(Submission, SubmitSignalsTimeline)
    {
        const RHI::FGPUAllocation Buffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.SignalTarget");
        RHI_REQUIRE(Buffer.Gpu != 0);

        const RHI::FCmdListH CL = Ctx.OpenCL();
        RHI::CmdMemzero(CL, { Buffer.Gpu, 256 });

        const RHI::FSemaphoreH Timeline = RHI::GetQueueTimeline(RHI::EQueueType::Graphics);
        const uint64 Value = RHI::Submit(RHI::EQueueType::Graphics, TSpan{ &CL, 1 });

        RHI::WaitSemaphore(Timeline, Value);
        RHI_CHECK(RHI::GetSemaphoreValue(Timeline) >= Value);
    }

    // Chained through a timeline value rather than a host wait, so the second must not start early.
    RHI_TEST(Submission, WaitThenSignalChain)
    {
        const RHI::FGPUAllocation Buffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.ChainTarget");
        RHI_REQUIRE(Buffer.Gpu != 0);

        const RHI::FSemaphoreH Timeline = RHI::GetQueueTimeline(RHI::EQueueType::Graphics);

        const RHI::FCmdListH First = Ctx.OpenCL();
        RHI::CmdMemset(First, { Buffer.Gpu, 256 }, 1u);
        const uint64 FirstValue = RHI::Submit(RHI::EQueueType::Graphics, TSpan{ &First, 1 });

        const RHI::FCmdListH Second = Ctx.OpenCL();
        RHI::CmdMemset(Second, { Buffer.Gpu, 256 }, 2u);
        const RHI::FSemaphoreInfo SecondWait{ Timeline, FirstValue };
        const uint64 SecondValue = RHI::Submit(RHI::EQueueType::Graphics, TSpan{ &Second, 1 }, TSpan{ &SecondWait, 1 });

        RHI_CHECK(SecondValue > FirstValue);
        RHI::WaitSemaphore(Timeline, SecondValue);
        RHI_CHECK(RHI::GetSemaphoreValue(Timeline) >= SecondValue);
    }

    RHI_TEST(Submission, SubmitReturnsRisingValues)
    {
        // Separate buffers on purpose, since this is about the returned timeline values, not hazards.
        const RHI::FGPUAllocation FirstBuffer  = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.SubmitA");
        const RHI::FGPUAllocation SecondBuffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.SubmitB");
        RHI_REQUIRE(FirstBuffer.Gpu != 0 && SecondBuffer.Gpu != 0);

        const RHI::FCmdListH First = Ctx.OpenCL();
        RHI::CmdMemzero(First, { FirstBuffer.Gpu, 256 });
        RHI::Barriers::TransferToAll(First);
        const uint64 FirstValue = RHI::Submit(RHI::EQueueType::Graphics, TSpan{ &First, 1 });

        const RHI::FCmdListH Second = Ctx.OpenCL();
        RHI::CmdMemzero(Second, { SecondBuffer.Gpu, 256 });
        RHI::Barriers::TransferToAll(Second);
        const uint64 SecondValue = RHI::Submit(RHI::EQueueType::Graphics, TSpan{ &Second, 1 });

        RHI_CHECK(SecondValue > FirstValue);

        RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Graphics), SecondValue);
    }

    RHI_TEST(Submission, AsyncComputeQueue)
    {
        if (!RHI::SupportsAsyncCompute())
        {
            LOG_INFO("             (no async compute queue on this device -- nothing to exercise)");
            return;
        }

        const RHI::FGPUAllocation Buffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.AsyncCompute");
        RHI_REQUIRE(Buffer.Gpu != 0);

        const RHI::FCmdListH CL = RHI::OpenCommandList(RHI::EQueueType::Compute);
        RHI::CmdMemzero(CL, { Buffer.Gpu, 256 });
        RHI::Barriers::TransferToCompute(CL);

        const uint64 Value = RHI::Submit(RHI::EQueueType::Compute, TSpan{ &CL, 1 });
        RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Compute), Value);
    }

    RHI_TEST(Submission, AsyncTransferQueue)
    {
        if (!RHI::SupportsAsyncTransfer())
        {
            LOG_INFO("             (no async transfer queue on this device -- nothing to exercise)");
            return;
        }

        const RHI::FGPUAllocation Source = Ctx.Malloc(1024, RHI::EMemoryType::GPUOnly, "RHITests.AsyncXferSrc");
        const RHI::FGPUAllocation Dest   = Ctx.Malloc(1024, RHI::EMemoryType::GPUOnly, "RHITests.AsyncXferDst");
        RHI_REQUIRE(Source.Gpu != 0 && Dest.Gpu != 0);

        const RHI::FCmdListH CL = RHI::OpenCommandList(RHI::EQueueType::Transfer);
        RHI::CmdMemcpy(CL, { Dest.Gpu, 1024 }, { Source.Gpu, 1024 });

        const uint64 Value = RHI::Submit(RHI::EQueueType::Transfer, TSpan{ &CL, 1 });
        RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Transfer), Value);
    }

    RHI_TEST(Submission, EmptyCommandListSubmits)
    {
        const RHI::FCmdListH CL = Ctx.OpenCL();
        Ctx.SubmitAndWait(CL);
    }

    // ResetCommandList recycles into the free pool, so recording without re-opening is a begin error.
    RHI_TEST(Submission, CommandListRecyclesThroughFreePool)
    {
        const RHI::FGPUAllocation Buffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.Recycle");
        RHI_REQUIRE(Buffer.Gpu != 0);

        for (uint32 i = 0; i < 8; ++i)
        {
            const RHI::FCmdListH CL = Ctx.OpenCL();
            RHI::CmdMemset(CL, { Buffer.Gpu, 256 }, i);
            RHI::Barriers::TransferToAll(CL);
            Ctx.SubmitAndWait(CL);
        }
    }
}
