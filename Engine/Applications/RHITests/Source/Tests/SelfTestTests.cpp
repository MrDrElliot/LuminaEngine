#include "RHITestHarness.h"

#include "Log/Log.h"

// Negative controls. Every other test in this harness is correct by construction, so a clean validation
// run proves only that nothing WRONG was done -- not that a wrong thing would have been noticed. These
// deliberately break a rule and PASS when the validation layer reports it.
//
//   RHITests.exe --group=SelfTest
//
// STRICTLY CPU-SIDE VIOLATIONS ONLY. A previous version provoked an out-of-bounds WRITE from a shader to
// prove GPU-AV's buffer-address instrumentation was live. It proved that -- and GPU-AV reported the
// access as "4 bytes written", i.e. it diagnosed the write rather than suppressing it. Every subsequent
// GPU-AV run in that session page-faulted (Error_DMA_PageFault, MMU fault, stable low address bits across
// runs) until the machine was reset, while the same tests had passed cleanly minutes earlier. A negative
// control that corrupts the GPU for every later run is worse than no negative control: what belongs here
// is anything the validation layer rejects BEFORE it reaches the device.

namespace Lumina::RHITests
{
    /** A plain core-validation violation, no instrumentation involved: a transfer write recorded inside
     *  a render pass. Proves the capture path itself works even with GPU-AV off, which is the control
     *  for the control. */
    RHI_TEST_EXPECT_VALIDATION(SelfTest, TransferInsideRenderPass)
    {
        RHI::FTextureDesc Desc;
        Desc.Type      = RHI::ETextureType::Tex2D;
        Desc.Dimension = FUIntVector3(16, 16, 1);
        Desc.Format    = EFormat::RGBA8_UNORM;
        Desc.Usage     = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::TransferSrc;

        const RHI::FTextureH Target = Ctx.CreateTexture(Desc, "RHITests.SelfTestTarget");
        RHI_REQUIRE(RHI::IsValid(Target));

        const RHI::GPUPtr Buffer = Ctx.Malloc(256, RHI::EMemoryType::GPUOnly, "RHITests.SelfTestBuffer");
        RHI_REQUIRE(Buffer != 0);

        const RHI::FRenderAttachment Color
        {
            .Texture = Target,
            .LoadOp  = RHI::ELoadOp::Clear,
            .StoreOp = RHI::EStoreOp::Store,
        };
        const RHI::FRenderPassDesc Pass
        {
            .ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1),
            .RenderArea       = FUIntVector2(16, 16),
        };

        const RHI::FCmdListH CL = Ctx.OpenCL();
        RHI::CmdBeginRenderPass(CL, Pass);
        RHI::CmdMemzero(CL, Buffer, 256);   // illegal here
        RHI::CmdEndRenderPass(CL);
        Ctx.SubmitAndWait(CL);
    }
}
