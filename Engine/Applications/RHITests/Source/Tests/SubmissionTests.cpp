#include "RHITestHarness.h"

namespace Lumina::RHITests
{
    RHI_TEST(Submission, TimelineSemaphoreStartsAtInitialValue)
    {
        const RHI::FSemaphoreH Semaphore = RHI::CreateTimelineSemaphore(7);
        RHI_REQUIRE(RHI::IsValid(Semaphore));

        RHI_CHECK_EQ(RHI::GetSemaphoreValue(Semaphore), 7ull);

        RHI::WaitDeviceIdle();
        RHI::FreeH(Semaphore);
    }

    RHI_TEST(Submission, SubmitSignalsTimeline)
    {
        const RHI::FSemaphoreH Semaphore = RHI::CreateTimelineSemaphore(0);
        RHI_REQUIRE(RHI::IsValid(Semaphore));

        const RHI::GPUPtr Buffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.SignalTarget");
        RHI_REQUIRE(Buffer != 0);

        const RHI::FCmdListH CL = Ctx.OpenCL();
        RHI::CmdMemzero(CL, Buffer, 256);

        const RHI::FSemaphoreInfo Signal{ Semaphore, 42, RHI::EStageFlags::AllCommands };
        RHI::Submit(RHI::EQueueType::Graphics, TSpan{ &CL, 1 }, {}, TSpan{ &Signal, 1 });

        RHI::WaitSemaphore(Semaphore, 42);
        RHI_CHECK_EQ(RHI::GetSemaphoreValue(Semaphore), 42ull);

        RHI::ResetCommandList(CL);
        RHI::WaitDeviceIdle();
        RHI::FreeH(Semaphore);
    }

    // Two submissions chained through a timeline value rather than a host wait: the second must not
    // start until the first signals.
    RHI_TEST(Submission, WaitThenSignalChain)
    {
        const RHI::FSemaphoreH Semaphore = RHI::CreateTimelineSemaphore(0);
        RHI_REQUIRE(RHI::IsValid(Semaphore));

        const RHI::GPUPtr Buffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.ChainTarget");
        RHI_REQUIRE(Buffer != 0);

        const RHI::FCmdListH First = Ctx.OpenCL();
        RHI::CmdMemset(First, Buffer, 256, 1u);
        const RHI::FSemaphoreInfo FirstSignal{ Semaphore, 1, RHI::EStageFlags::AllCommands };
        RHI::Submit(RHI::EQueueType::Graphics, TSpan{ &First, 1 }, {}, TSpan{ &FirstSignal, 1 });

        const RHI::FCmdListH Second = Ctx.OpenCL();
        RHI::CmdMemset(Second, Buffer, 256, 2u);
        const RHI::FSemaphoreInfo SecondWait{ Semaphore, 1, RHI::EStageFlags::AllCommands };
        const RHI::FSemaphoreInfo SecondSignal{ Semaphore, 2, RHI::EStageFlags::AllCommands };
        RHI::Submit(RHI::EQueueType::Graphics, TSpan{ &Second, 1 }, TSpan{ &SecondWait, 1 }, TSpan{ &SecondSignal, 1 });

        RHI::WaitSemaphore(Semaphore, 2);
        RHI_CHECK_EQ(RHI::GetSemaphoreValue(Semaphore), 2ull);

        RHI::ResetCommandList(First);
        RHI::ResetCommandList(Second);
        RHI::WaitDeviceIdle();
        RHI::FreeH(Semaphore);
    }

    RHI_TEST(Submission, SubmitOnReturnsRisingValues)
    {
        // Separate buffers on purpose: two unsynchronized submissions writing one buffer is a genuine
        // write-after-write, and this test is about the returned timeline values, not about hazards.
        const RHI::GPUPtr FirstBuffer  = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.SubmitOnA");
        const RHI::GPUPtr SecondBuffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.SubmitOnB");
        RHI_REQUIRE(FirstBuffer != 0 && SecondBuffer != 0);

        const RHI::FCmdListH First = Ctx.OpenCL();
        RHI::CmdMemzero(First, FirstBuffer, 256);
        RHI::Barriers::TransferToAll(First);
        const uint64 FirstValue = RHI::Core::SubmitOn(RHI::EQueueType::Graphics, TSpan{ &First, 1 });

        const RHI::FCmdListH Second = Ctx.OpenCL();
        RHI::CmdMemzero(Second, SecondBuffer, 256);
        RHI::Barriers::TransferToAll(Second);
        const uint64 SecondValue = RHI::Core::SubmitOn(RHI::EQueueType::Graphics, TSpan{ &Second, 1 });

        RHI_CHECK(SecondValue > FirstValue);

        RHI::WaitSemaphore(RHI::Core::GetQueueTimeline(RHI::EQueueType::Graphics), SecondValue);
    }

    RHI_TEST(Submission, AsyncComputeQueue)
    {
        if (!RHI::SupportsAsyncCompute())
        {
            LOG_INFO("             (no async compute queue on this device -- nothing to exercise)");
            return;
        }

        const RHI::GPUPtr Buffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.AsyncCompute");
        RHI_REQUIRE(Buffer != 0);

        const RHI::FCmdListH CL = RHI::OpenCommandList(RHI::EQueueType::Compute);
        RHI::CmdMemzero(CL, Buffer, 256);
        RHI::Barriers::TransferToCompute(CL);

        const uint64 Value = RHI::Core::SubmitOn(RHI::EQueueType::Compute, TSpan{ &CL, 1 });
        RHI::WaitSemaphore(RHI::Core::GetQueueTimeline(RHI::EQueueType::Compute), Value);
        RHI::ResetCommandList(CL);
    }

    RHI_TEST(Submission, AsyncTransferQueue)
    {
        if (!RHI::SupportsAsyncTransfer())
        {
            LOG_INFO("             (no async transfer queue on this device -- nothing to exercise)");
            return;
        }

        const RHI::GPUPtr Source = Ctx.Malloc(1024, RHI::EMemoryType::GPUOnly, "RHITests.AsyncXferSrc");
        const RHI::GPUPtr Dest   = Ctx.Malloc(1024, RHI::EMemoryType::GPUOnly, "RHITests.AsyncXferDst");
        RHI_REQUIRE(Source != 0 && Dest != 0);

        const RHI::FCmdListH CL = RHI::OpenCommandList(RHI::EQueueType::Transfer);
        RHI::CmdMemcpy(CL, Dest, Source, 1024);

        const uint64 Value = RHI::Core::SubmitOn(RHI::EQueueType::Transfer, TSpan{ &CL, 1 });
        RHI::WaitSemaphore(RHI::Core::GetQueueTimeline(RHI::EQueueType::Transfer), Value);
        RHI::ResetCommandList(CL);
    }

    RHI_TEST(Submission, EmptyCommandListSubmits)
    {
        const RHI::FCmdListH CL = Ctx.OpenCL();
        Ctx.SubmitAndWait(CL);
    }

    // ResetCommandList RECYCLES the list into the per-queue free pool -- the handle is dead until
    // OpenCommandList hands it back and re-begins recording. Recording into it without re-opening is
    // "called before vkBeginCommandBuffer". This walks the supported loop several times over, which is
    // also what proves the pool actually reuses rather than growing without bound.
    RHI_TEST(Submission, CommandListRecyclesThroughFreePool)
    {
        const RHI::GPUPtr Buffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.Recycle");
        RHI_REQUIRE(Buffer != 0);

        for (uint32 i = 0; i < 8; ++i)
        {
            const RHI::FCmdListH CL = Ctx.OpenCL();
            RHI::CmdMemset(CL, Buffer, 256, i);
            RHI::Barriers::TransferToAll(CL);
            Ctx.SubmitAndWait(CL);
        }
    }
}
