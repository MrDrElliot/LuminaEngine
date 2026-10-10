#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace
    {
        FRenderTexture ToRenderTexture(const FSceneImage& Image)
        {
            FRenderTexture Out;
            if (!Image.IsValid())
            {
                return Out;
            }

            Out.Texture = Image.Texture;
            Out.Index   = Image.GetResourceID() < 0 ? Constants::kIndexNoneU32 : (uint32)Image.GetResourceID();
            Out.Format  = Image.Desc.Format;
            Out.Extent  = Image.GetExtent();
            return Out;
        }

        const char* StageMarker(ERenderStage Stage)
        {
            switch (Stage)
            {
            case ERenderStage::FrameStart:        return "Render Callbacks (Frame Start)";
            case ERenderStage::Depth:             return "Render Callbacks (Depth)";
            case ERenderStage::ShadowDepth:       return "Render Callbacks (Shadow Depth)";
            case ERenderStage::AfterOpaque:       return "Render Callbacks (After Opaque)";
            case ERenderStage::AfterTranslucency: return "Render Callbacks (After Translucency)";
            case ERenderStage::AfterPostProcess:  return "Render Callbacks (After Post Process)";
            case ERenderStage::Overlay:           return "Render Callbacks (Overlay)";
            default:                              return "Render Callbacks";
            }
        }

        // Callbacks may touch any resource from any stage, so nothing narrower is correct at either edge.
        void FullBarrier(RHI::FCmdListH CL)
        {
            RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EAccessFlags::Any,
                                RHI::EStageFlags::AllCommands, RHI::EAccessFlags::Any);
        }
    }

    RHI::FPipelineH FDefaultSceneRenderer::GetCallbackPipeline(const FRenderPipelineDesc& Desc, TSpan<const RHI::FColorTarget> ColorTargets, EFormat DepthFormat)
    {
        if (Desc.VertexShader == nullptr && Desc.MeshShader == nullptr)
        {
            LOG_WARN_ONCE("Render callback pipeline has neither a vertex nor a mesh shader; nothing will draw.");
            return {};
        }

        FGraphicsPipelineKey Key;
        Key.VS          = Desc.VertexShader;
        Key.PS          = Desc.PixelShader;
        Key.MS          = Desc.MeshShader;
        Key.Topology    = Desc.Topology;
        Key.DepthFormat = DepthFormat;
        for (const RHI::FColorTarget& Target : ColorTargets)
        {
            Key.ColorTargets.push_back(Target);
        }

        return GetOrCreatePipeline(Key);
    }

    RHI::FPipelineH FDefaultSceneRenderer::GetCallbackComputePipeline(FShaderH ComputeShader)
    {
        if (ComputeShader == nullptr)
        {
            return {};
        }

        return GetOrCreateComputePipeline(ComputeShader);
    }

    bool FDefaultSceneRenderer::HasRenderCallbacks(ERenderStage Stage) const
    {
        return World != nullptr && World->GetRenderCallbacks().HasAny(Stage);
    }

    FRenderContext FDefaultSceneRenderer::MakeRenderContext(RHI::FCmdListH CL, ERenderStage Stage)
    {
        const FFrameData& Frame        = *RenderFrame;
        const FSceneGlobalData& Global = Frame.SceneGlobalData;
        const FCameraData& CameraData  = Global.CameraData;
        const FSceneView& View         = SceneViews[0];

        FRenderContext Context(*this);
        Context.CmdList   = CL;
        Context.World     = World;
        Context.Stage     = Stage;
        Context.FrameSlot = CurrentFrameSlot;
        Context.Time      = Global.Time;
        Context.DeltaTime = Global.DeltaTime;

        Context.Camera.View              = CameraData.View;
        Context.Camera.InverseView       = CameraData.InverseView;
        Context.Camera.Projection        = CameraData.Projection;
        Context.Camera.InverseProjection = CameraData.InverseProjection;
        Context.Camera.Location          = FVector3(CameraData.Location);
        Context.Camera.NearPlane         = Global.NearPlane;
        Context.Camera.FarPlane          = Global.FarPlane;

        const FSceneImage& HDR = View.Images[(int)ENamedImage::HDR];

        Context.View.ViewProjection        = CameraData.Projection * CameraData.View;
        Context.View.InverseViewProjection = CameraData.InverseView * CameraData.InverseProjection;
        Context.View.Extent                = HDR.GetExtent();
        Context.View.bReverseZ             = true;

        // The display color lives in LDR until anti-aliasing resolves it into the view's output.
        const FSceneImage* Color = &HDR;
        if (Stage == ERenderStage::AfterPostProcess)
        {
            Color = GetViewSMAAMode(View) != ESMAAMode::Off ? &View.Images[(int)ENamedImage::LDR] : &View.Output;
        }
        else if (Stage == ERenderStage::Overlay)
        {
            Color = &View.Output;
        }

        // The stages after the upscale draw at display resolution, where the render resolution depth cannot attach.
        Context.View.Extent = Color->GetExtent();
        Context.SceneColor  = ToRenderTexture(*Color);
        const FSceneImage& Depth = View.Images[(int)ENamedImage::DepthAttachment];
        Context.SceneDepth  = Depth.GetExtent() == Color->GetExtent() ? ToRenderTexture(Depth) : FRenderTexture{};
        Context.Velocity   = ToRenderTexture(View.Images[(int)ENamedImage::Velocity]);
        return Context;
    }

    void FDefaultSceneRenderer::RestoreAfterRenderCallbacks(RHI::FCmdListH CL)
    {
        RHI::CmdSetTextureHeap(CL, RHI::GetGlobalHeap());
        RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CW);
        if (SceneBindings.Root != 0)
        {
            RHI::CmdSetSceneRoot(CL, SceneBindings);
        }
    }

    void FDefaultSceneRenderer::PrepareSceneColorForCallbacks(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        bSceneColorClearedForCallbacks = false;

        constexpr ERenderStage SceneColorStages = ERenderStage::FrameStart | ERenderStage::AfterOpaque | ERenderStage::AfterTranslucency;
        const bool bEngineWritesSceneColor = !Frame.Geometry.DrawCommands.empty() || FrameFlags.bHasEnvironment
                                          || !Frame.Extracts.TerrainExtracts.empty();
        if (bEngineWritesSceneColor || !HasRenderCallbacks(SceneColorStages))
        {
            return;
        }

        // Otherwise the first pass to touch HDR clears it, which may be after a callback has drawn into it.
        const float Black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Barriers::SceneToTransfer(CL);
        RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::HDR).Texture, Black);
        FullBarrier(CL);
        bSceneColorClearedForCallbacks = true;
    }

    void FDefaultSceneRenderer::RunRenderCallbacks(RHI::FCmdListH CL, ERenderStage Stage)
    {
        if (!HasRenderCallbacks(Stage))
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, StageMarker(Stage));

        FRenderContext Context = MakeRenderContext(CL, Stage);

        FullBarrier(CL);
        World->GetRenderCallbacks().Invoke(Context);
        FullBarrier(CL);

        RestoreAfterRenderCallbacks(CL);
    }

    void FDefaultSceneRenderer::CustomDepthPass(RHI::FCmdListH CL)
    {
        if (!HasRenderCallbacks(ERenderStage::Depth))
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, StageMarker(ERenderStage::Depth));

        // Stamping empty into the visibility buffer keeps deferred from shading a mesh the custom geometry covers.
        static const FShaderH StampPixelShader = FShaderLibrary::Get("TerrainDepthPixel.slang");

        const FSceneImage& VisRT   = GetNamedImage(ENamedImage::VisBuffer);
        const FSceneImage& DepthRT = GetNamedImage(ENamedImage::DepthAttachment);
        const FUIntVector2 Extent  = GetNamedImage(ENamedImage::HDR).GetExtent();

        RHI::FRenderAttachment Color;
        Color.Texture = VisRT.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = DepthRT.Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencil(CL, DepthDesc);

        FRenderContext Context = MakeRenderContext(CL, ERenderStage::Depth);
        const EFormat VisFormat = VisRT.Desc.Format;
        Context.AdoptEnginePass(TSpan<const EFormat>(&VisFormat, 1), DepthRT.Desc.Format, StampPixelShader);

        World->GetRenderCallbacks().Invoke(Context);

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
        RestoreAfterRenderCallbacks(CL);
    }

    void FDefaultSceneRenderer::RunShadowRenderCallbacks(RHI::FCmdListH CL, EShadowViewType Type, uint32 Index,
                                                         const FMatrix4& ViewProjection, const RHI::FRect& Tile, EFormat DepthFormat)
    {
        FRenderContext Context = MakeRenderContext(CL, ERenderStage::ShadowDepth);
        Context.View.ViewProjection        = ViewProjection;
        Context.View.InverseViewProjection = Math::Inverse(ViewProjection);
        Context.View.Extent                = FUIntVector2((uint32)(Tile.MaxX - Tile.MinX), (uint32)(Tile.MaxY - Tile.MinY));
        Context.View.bReverseZ             = false;
        Context.Shadow.Type                = Type;
        Context.Shadow.Index               = Index;
        Context.AdoptEnginePass({}, DepthFormat, FShaderH{});

        RHI::CmdSetViewport(CL, Tile);
        RHI::CmdSetScissor(CL, Tile);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        World->GetRenderCallbacks().Invoke(Context);
    }
}
