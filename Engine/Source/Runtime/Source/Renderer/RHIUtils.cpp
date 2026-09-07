#include "RuntimePCH.h"
#include "RHIUtils.h"

namespace Lumina::RHI::Utils
{
    void BeginScreenPass(FCmdListH CL, const FScreenPassDesc& Desc)
    {
        const FRenderAttachment Color
        {
            .Texture = Desc.Target,
            .LoadOp  = Desc.LoadOp,
            .StoreOp = EStoreOp::Store,
            .Color   = { Desc.Clear[0], Desc.Clear[1], Desc.Clear[2], Desc.Clear[3] },
        };

        const FRenderPassDesc Pass
        {
            .ColorAttachments = TSpan<const FRenderAttachment>(&Color, 1),
            .RenderArea       = Desc.Extent,
        };

        const FRect Rect { 0, int32(Desc.Extent.x), 0, int32(Desc.Extent.y) };

        CmdBeginRenderPass(CL, Pass);

        CmdSetDepthStencil(CL, Desc.DepthState);
        CmdSetCullMode(CL, ECullMode::None);
        CmdSetFrontFace(CL, EFrontFace::CCW);
        CmdSetViewport(CL, Rect);
        CmdSetScissor(CL, Rect);
    }

    void EndScreenPass(FCmdListH CL)
    {
        CmdEndRenderPass(CL);
    }

    void DrawFullscreen(FCmdListH CL, FPipelineH Pipeline, GPUPtr Args)
    {
        CmdSetPipeline(CL, Pipeline);
        CmdDraw(CL, Args, 3, 1, 0, 0);
    }

    void FMipChain::Initialize(const FUIntVector2& BaseExtent, uint32 LevelCount, EFormat Format,
        const char* DebugName)
    {
        Shutdown();

        Count = Math::Min(LevelCount, kMaxLevels);

        for (uint32 Level = 0; Level < Count; ++Level)
        {
            Extents[Level] =
            {
                Math::Max(1u, BaseExtent.x >> (Level + 1u)),
                Math::Max(1u, BaseExtent.y >> (Level + 1u)),
            };

            Levels[Level] = Textures::Create(FTexture2DDesc
            {
                .Width         = Extents[Level].x,
                .Height        = Extents[Level].y,
                .Format        = Format,
                .bRenderTarget = true,
                .DebugName     = DebugName,
            });
        }
    }

    void FMipChain::Shutdown()
    {
        for (uint32 Level = 0; Level < Count; ++Level)
        {
            if (Levels[Level].IsValid())
            {
                Textures::Release(Levels[Level]);
            }
        }
        Count = 0;
    }
}
