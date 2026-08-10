#include "RHITestHarness.h"

#include "Renderer/RHIUpload.h"

namespace Lumina::RHITests
{
    namespace
    {
        // Copies Source into a fresh CPURead allocation and blocks until it lands, so a test can assert
        // on what the GPU actually wrote rather than on what it asked for.
        const uint32* ReadBack(FTestContext& Ctx, RHI::GPUPtr Source, uint64 Size)
        {
            const RHI::GPUPtr Readback = Ctx.Malloc(Size, RHI::EMemoryType::CPURead, "RHITests.Readback");
            if (Readback == 0)
            {
                return nullptr;
            }

            const RHI::FCmdListH CL = Ctx.OpenCL();
            RHI::CmdMemcpy(CL, Readback, Source, Size);
            RHI::Barriers::TransferToAll(CL);
            Ctx.SubmitAndWait(CL);

            return static_cast<const uint32*>(RHI::ToHost(Readback));
        }
    }

    RHI_TEST(Commands, Memset)
    {
        constexpr uint64 Size = 4096;
        const RHI::GPUPtr Buffer = Ctx.Malloc(Size, RHI::EMemoryType::GPUOnly, "RHITests.Memset");
        RHI_REQUIRE(Buffer != 0);

        const RHI::FCmdListH CL = Ctx.OpenCL();
        RHI::CmdMemset(CL, Buffer, Size, 0xABCDEF01u);
        RHI::Barriers::TransferToAll(CL);
        Ctx.SubmitAndWait(CL);

        const uint32* Words = ReadBack(Ctx, Buffer, Size);
        RHI_REQUIRE(Words != nullptr);
        RHI_CHECK_EQ(Words[0], 0xABCDEF01u);
        RHI_CHECK_EQ(Words[(Size / sizeof(uint32)) - 1], 0xABCDEF01u);
    }

    RHI_TEST(Commands, Memzero)
    {
        constexpr uint64 Size = 1024;
        const RHI::GPUPtr Buffer = Ctx.Malloc(Size, RHI::EMemoryType::GPUOnly, "RHITests.Memzero");
        RHI_REQUIRE(Buffer != 0);

        {
            const RHI::FCmdListH CL = Ctx.OpenCL();
            RHI::CmdMemset(CL, Buffer, Size, 0xFFFFFFFFu);
            RHI::Barriers::TransferToTransfer(CL);
            RHI::CmdMemzero(CL, Buffer, Size);
            RHI::Barriers::TransferToAll(CL);
            Ctx.SubmitAndWait(CL);
        }

        const uint32* Words = ReadBack(Ctx, Buffer, Size);
        RHI_REQUIRE(Words != nullptr);
        RHI_CHECK_EQ(Words[0], 0u);
        RHI_CHECK_EQ(Words[(Size / sizeof(uint32)) - 1], 0u);
    }

    RHI_TEST(Commands, MemcpyDeviceToDevice)
    {
        constexpr uint64 Size = 2048;
        const RHI::GPUPtr Source = Ctx.Malloc(Size, RHI::EMemoryType::GPUOnly, "RHITests.CopySrc");
        const RHI::GPUPtr Dest   = Ctx.Malloc(Size, RHI::EMemoryType::GPUOnly, "RHITests.CopyDst");
        RHI_REQUIRE(Source != 0 && Dest != 0);

        {
            const RHI::FCmdListH CL = Ctx.OpenCL();
            RHI::CmdMemset(CL, Source, Size, 0x11223344u);
            RHI::Barriers::TransferToTransfer(CL);
            RHI::CmdMemcpy(CL, Dest, Source, Size);
            RHI::Barriers::TransferToAll(CL);
            Ctx.SubmitAndWait(CL);
        }

        const uint32* Words = ReadBack(Ctx, Dest, Size);
        RHI_REQUIRE(Words != nullptr);
        RHI_CHECK_EQ(Words[0], 0x11223344u);
        RHI_CHECK_EQ(Words[(Size / sizeof(uint32)) - 1], 0x11223344u);
    }

    RHI_TEST(Commands, WriteMemory)
    {
        uint32 Source[64];
        for (uint32 i = 0; i < 64; ++i)
        {
            Source[i] = i * 7u + 1u;
        }

        const RHI::GPUPtr Buffer = Ctx.Malloc(sizeof(Source), RHI::EMemoryType::GPUOnly, "RHITests.WriteMemory");
        RHI_REQUIRE(Buffer != 0);

        {
            const RHI::FCmdListH CL = Ctx.OpenCL();
            RHI::CmdWriteMemory(CL, Buffer, Source, sizeof(Source));
            RHI::Barriers::TransferToAll(CL);
            Ctx.SubmitAndWait(CL);
        }

        const uint32* Words = ReadBack(Ctx, Buffer, sizeof(Source));
        RHI_REQUIRE(Words != nullptr);
        RHI_CHECK_EQ(Words[0], 1u);
        RHI_CHECK_EQ(Words[63], 63u * 7u + 1u);
    }

    // Every barrier helper, back to back in one list. Catches a stage mask that the queue does not
    // support, which is a validation error at record time rather than a wrong result.
    RHI_TEST(Commands, BarrierHelpers)
    {
        const RHI::FCmdListH CL = Ctx.OpenCL();

        RHI::Barriers::ComputeToAll(CL);
        RHI::Barriers::RasterToRead(CL);
        RHI::Barriers::RasterToRaster(CL);
        RHI::Barriers::TransferToAll(CL);
        RHI::Barriers::TransferToTransfer(CL);
        RHI::Barriers::AllToTransfer(CL);
        RHI::Barriers::TransferToCompute(CL);
        RHI::Barriers::ComputeToGeometry(CL);
        RHI::Barriers::ComputeToIndirect(CL);

        Ctx.SubmitAndWait(CL);
    }

    RHI_TEST(Commands, DebugMarkersAreBalanced)
    {
        const RHI::FCmdListH CL = Ctx.OpenCL();

        RHI::CmdBeginMarker(CL, "RHITests.Outer");
        RHI::CmdBeginMarker(CL, "RHITests.Inner");
        RHI::CmdEndMarker(CL);
        RHI::CmdEndMarker(CL);

        Ctx.SubmitAndWait(CL);
    }

    RHI_TEST(Commands, TransientAllocation)
    {
        struct FPayload { uint32 Values[16]; };

        FPayload Payload{};
        for (uint32 i = 0; i < 16; ++i)
        {
            Payload.Values[i] = i + 100u;
        }

        const RHI::GPUPtr Gpu = RHI::Core::CopyTransient(Payload);
        RHI_REQUIRE(Gpu != 0);

        const RHI::GPUPtr Dest = Ctx.Malloc(sizeof(FPayload), RHI::EMemoryType::GPUOnly, "RHITests.TransientDst");
        RHI_REQUIRE(Dest != 0);

        {
            const RHI::FCmdListH CL = Ctx.OpenCL();
            RHI::CmdMemcpy(CL, Dest, Gpu, sizeof(FPayload));
            RHI::Barriers::TransferToAll(CL);
            Ctx.SubmitAndWait(CL);
        }

        const uint32* Words = ReadBack(Ctx, Dest, sizeof(FPayload));
        RHI_REQUIRE(Words != nullptr);
        RHI_CHECK_EQ(Words[0], 100u);
        RHI_CHECK_EQ(Words[15], 115u);
    }
}
