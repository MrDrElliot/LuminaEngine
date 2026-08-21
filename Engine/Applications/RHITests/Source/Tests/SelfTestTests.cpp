#include "RHITestHarness.h"

#include "Log/Log.h"

// Negative controls that PASS when validation reports them; CPU-side ONLY, a GPU-side fault poisons the device.

namespace Lumina::RHITests
{
    // A plain core-validation violation with no instrumentation, the control for the control.
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
