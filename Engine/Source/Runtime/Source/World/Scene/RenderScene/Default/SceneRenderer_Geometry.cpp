#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    void FDefaultSceneRenderer::TexturePaintPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.Extracts.PaintOps.IsEmpty())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Texture Paint");

        static const FShaderH PaintShader = FShaderLibrary::Get("TexturePaint.slang");
        if (!PaintShader)
        {
            return;
        }

        RHI::FPipelineH Pipeline = GetOrCreateComputePipeline(PaintShader);

        struct FPaintPC
        {
            uint32      TargetIndex;
            int32       BrushIndex;
            uint32      TargetSize[2];
            uint32      RectMin[2];
            uint32      RectMax[2];
            float       CenterPx[2];
            float       RadiusPx;
            float       Strength;
            float       Hardness;
            float       ColorR;
            float       ColorG;
            float       ColorB;
            float       ColorA;
        };

        bool bBoundPipeline = false;

        for (const FTexturePaintOp& Op : Frame.Extracts.PaintOps)
        {
            if (!RHI::IsValid(Op.Target))
            {
                continue;
            }

            if (Op.Mode == FTexturePaintOp::EMode::Clear)
            {
                // Transfer in the source too, so two clears of one target in a frame stay ordered.
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute | RHI::EStageFlags::Transfer,
                    RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::TransferWrite,
                    RHI::EStageFlags::Transfer,
                    RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
                const float Clear[4] = { Op.Color.r, Op.Color.g, Op.Color.b, Op.Color.a };
                RHI::CmdClearTexture(CL, Op.Target, Clear);
                continue;
            }

            if (Op.TargetUAV == RHI::kInvalidHeapSlot)
            {
                continue;
            }

            const uint32 W = Op.TargetExtent.x;
            const uint32 H = Op.TargetExtent.y;
            const float  CenterX = Op.CenterUV.x * (float)W;
            const float  CenterY = Op.CenterUV.y * (float)H;
            // Radius is relative to the longer side so the brush stays circular in pixels.
            const float  RadiusPx = Math::Max(Op.RadiusUV * (float)Math::Max(W, H), 1.0f);

            const int32 MinX = Math::Clamp((int32)std::floor(CenterX - RadiusPx), 0, (int32)W);
            const int32 MinY = Math::Clamp((int32)std::floor(CenterY - RadiusPx), 0, (int32)H);
            const int32 MaxX = Math::Clamp((int32)std::ceil (CenterX + RadiusPx), 0, (int32)W);
            const int32 MaxY = Math::Clamp((int32)std::ceil (CenterY + RadiusPx), 0, (int32)H);
            if (MaxX <= MinX || MaxY <= MinY)
            {
                continue;
            }

            if (!bBoundPipeline)
            {
                RHI::CmdSetPipeline(CL, Pipeline);
                bBoundPipeline = true;
            }

            // Order against any prior clear/paint of the same target.
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer | RHI::EStageFlags::Compute, RHI::EAccessFlags::TransferWrite | RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

            FPaintPC PC = {};
            PC.TargetIndex   = Op.TargetUAV;
            PC.BrushIndex    = Op.BrushIndex;
            PC.TargetSize[0] = W;             PC.TargetSize[1] = H;
            PC.RectMin[0]    = (uint32)MinX;  PC.RectMin[1]    = (uint32)MinY;
            PC.RectMax[0]    = (uint32)MaxX;  PC.RectMax[1]    = (uint32)MaxY;
            PC.CenterPx[0]   = CenterX;       PC.CenterPx[1]   = CenterY;
            PC.RadiusPx      = RadiusPx;
            PC.Strength      = Op.Strength;
            PC.Hardness      = Op.Hardness;
            PC.ColorR        = Op.Color.r;
            PC.ColorG        = Op.Color.g;
            PC.ColorB        = Op.Color.b;
            PC.ColorA        = Op.Color.a;

            const uint32 DispatchX = RenderUtils::GetGroupCount((uint32)(MaxX - MinX), 8u);
            const uint32 DispatchY = RenderUtils::GetGroupCount((uint32)(MaxY - MinY), 8u);
            RHI::CmdDispatch(CL, MakeArgs(PC), DispatchX, DispatchY, 1u);
        }

        // Painted texels are sampled by materials in every shader stage.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer | RHI::EStageFlags::Compute, RHI::EAccessFlags::TransferWrite | RHI::EAccessFlags::ShaderWrite,
                        RHI::EStageFlags::PixelShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::MeshShader | RHI::EStageFlags::Compute,
                        RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
    }

    const char* FDefaultSceneRenderer::FindMissingMeshletDrawBuffer() const
    {
        return GetMeshletDrawList().Gpu == 0 ? "MeshletDrawList"
             : SceneBindings.Instances == 0  ? "Instances (visible-instance ring)"
             : GetRenderBuckets().Gpu == 0   ? "RenderBuckets"
                                             : nullptr;
    }

    FShaderH FDefaultSceneRenderer::GetVisBufferPixelShader()
    {
        static const FString  VertexPathDefine("MESHLET_VERTEX_PATH");
        static const FShaderH VisPixel = FShaderLibrary::Get("VisBufferPixel.slang",
            RHI::SupportsMeshShaders() ? TSpan<const FString>() : TSpan<const FString>(&VertexPathDefine, 1));
        return VisPixel;
    }

    void FDefaultSceneRenderer::DrawVisBufferBatches(RHI::FCmdListH CL, uint32 ViewIndex, EMeshletSlice Slice,
                                                     const FUIntVector2& Extent, EFormat VisFormat)
    {
        const FShaderH VisPixel = GetVisBufferPixelShader();

        FMeshletPassContext Ctx;
        Ctx.CullViewIndex = ViewIndex;
        Ctx.ViewportW     = (float)Extent.x;
        Ctx.ViewportH     = (float)Extent.y;
        Ctx.Slice         = Slice;

        ForEachMeshletBatch(CL, RenderFrame->Geometry.OpaqueDrawList, Ctx,
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand& Batch)
            {
                const bool bMaskedClip = Batch.bMasked
                                      && Batch.MaskedVisBufferPixelShader != nullptr
                                      && Batch.VisBufferMeshShaderMasked != nullptr;

                Key.MS               = bMaskedClip ? Batch.VisBufferMeshShaderMasked : Batch.VisBufferMeshShader;
                Key.PS               = bMaskedClip ? Batch.MaskedVisBufferPixelShader : VisPixel;
                Key.bVisBufferMasked = bMaskedClip;   // interpolants only when actually masked-clipping
                Key.bWireframe       = FrameSettings.bWireframe;
                // The backface bit must agree with the CmdSetCullMode below or two-sided geometry vanishes.
                Key.TriCullMode      = (uint8)((Batch.bTwoSided ? 0u : (uint32)TriCull_Backface)
                                             | ((FrameSettings.bWireframe || Key.SampleCount > 1) ? 0u : (uint32)TriCull_SmallPrim));
                Key.DepthFormat      = EFormat::D32;
                Key.ColorTargets.Add({ VisFormat, {} });
                return true;
            },
            [&](const FMeshDrawCommand& Batch)
            {
                if (FrameSettings.bWireframe)
                {
                    RHI::CmdSetLineWidth(CL, 1.5f);
                }

                // Two-sided materials must rasterize both faces into the VisBuffer.
                RHI::CmdSetCullMode(CL, Batch.bTwoSided ? RHI::ECullMode::None : RHI::ECullMode::Back);
            },
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand&)
            {
                Key.MS               = DefaultMaterialStage(EMaterialShaderStage::VisBufferMesh);
                Key.PS               = VisPixel;
                Key.bVisBufferMasked = false;
                return Key.MS != nullptr;
            });
    }

    void FDefaultSceneRenderer::VisBufferPass(RHI::FCmdListH CL, uint32 ViewIndex, bool bClear,
                                            ECullPhase::Type Phase)
    {
        const FFrameData& Frame             = *RenderFrame;
        const auto& DrawCommands            = Frame.Geometry.DrawCommands;

        if (ViewIndex == Constants::kIndexNoneU32)
        {
            return;
        }

        // Terrain culls by frustum alone, so it draws in full with the early phase and is the first occluder the pyramid sees.
        const bool bDrawTerrain = Phase == ECullPhase::Early && !Frame.Extracts.TerrainExtracts.IsEmpty();

        // With nothing to draw the pass still opens when it clears, since the targets are read regardless.
        bool bHasDraws = !DrawCommands.IsEmpty();
        if (!bHasDraws && !bDrawTerrain && !bClear)
        {
            return;
        }

        // A single-phase view is drawn in full by Early, so skipping Late collapses the frame.
        const bool bTwoPhaseView = IsTwoPhaseView(Frame, ViewIndex);
        if (Phase == ECullPhase::Late && !bTwoPhaseView)
        {
            return;
        }

        if (const char* MissingBuffer = FindMissingMeshletDrawBuffer(); bHasDraws && MissingBuffer != nullptr)
        {
            const FName Missing(MissingBuffer);
            if (Missing != LastMissingMeshletDrawBuffer)
            {
                LastMissingMeshletDrawBuffer = Missing;
                LOG_ERROR("VisBuffer: '{}' has no device address this frame; skipping the draws. "
                          "Drawing would page-fault the GPU inside the mesh shader.", MissingBuffer);
            }
            bHasDraws = false;
        }

        LUMINA_PROFILE_SECTION_COLORED("VisBuffer Geometry Pass", tracy::Color::Red);

        bHasDraws = bHasDraws && GetVisBufferPixelShader() != nullptr;
        if (!bHasDraws && !bDrawTerrain && !bClear)
        {
            return;
        }

        // Once per frame, in the camera's last geometry phase, so a two-phase camera splats only what its late pyramid leaves visible.
        const bool bProxyView   = ViewIndex == 0u && HasFarProxies();
        const bool bDrawProxies = bProxyView && (Phase == ECullPhase::Late || !bTwoPhaseView);
        if (bProxyView && bTwoPhaseView)
        {
            OcclusionCullFarProxies(CL, Phase);
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::PixelShader,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);

        const FSceneImage& VisRT   = GetNamedImage(ENamedImage::VisBuffer);
        const FSceneImage& DepthRT = GetNamedImage(ENamedImage::DepthAttachment);
        const FUIntVector2 Extent  = GetNamedImage(ENamedImage::HDR).GetExtent();

        const RHI::ELoadOp GeomLoadOp = bClear ? RHI::ELoadOp::Clear : RHI::ELoadOp::Load;

        RHI::FRenderAttachment Color;
        Color.Texture = VisRT.Texture;
        Color.LoadOp  = GeomLoadOp;            // clears to 0 = empty (geometry stores VisID + 1)
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments               = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture        = DepthRT.Texture;
        Pass.DepthAttachment.LoadOp         = GeomLoadOp;
        Pass.DepthAttachment.StoreOp        = RHI::EStoreOp::Store;
        Pass.DepthAttachment.Color[0]       = 0.0f;   // reverse-Z clear
        Pass.RenderArea                     = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest = RHI::EOp::Greater;
        RHI::CmdSetDepthStencil(CL, (DepthDesc));

        if (bDrawTerrain)
        {
            DrawTerrainVisBuffer(CL, VisRT.Desc.Format);
        }

        if (bHasDraws)
        {
            // The two phases append into the same bucket, so each rasterizes only what its own cull added.
            DrawVisBufferBatches(CL, ViewIndex, (Phase == ECullPhase::Late) ? EMeshletSlice::Late : EMeshletSlice::Early,
                                 Extent, VisRT.Desc.Format);
        }

        if (bDrawProxies)
        {
            DrawFarProxies(CL, Extent, VisRT.Desc.Format, bTwoPhaseView);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::VisBufferComputeRasterPass(RHI::FCmdListH CL, bool bLate)
    {
        static const FShaderH RasterShader = FShaderLibrary::Get("VisBufferComputeRaster.slang");
        static const FShaderH FullscreenVS = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH ResolvePS    = FShaderLibrary::Get("VisBufferComputeResolve.slang");
        if (!bCameraComputeRaster || !bCullBucketsValid || !RasterShader || !FullscreenVS || !ResolvePS)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, bLate ? "VisBuffer Compute Raster Late" : "VisBuffer Compute Raster Early");

        // The cull's list and draw entries, and the hardware phase's depth the resolve tests against.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                        RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        struct FRasterPC
        {
            RHI::TGPUSpan<uint32> List;
            RHI::TGPUSpan<uint32> Cursors;
            RHI::TGPUSpan<uint64> Target;
            uint32                TargetSize[2];
            uint32                _Pad0;
            uint32                _Pad1;
        } RasterPC = {};
        static_assert(sizeof(FRasterPC) == 64, "FRasterPC must match VisBufferComputeRaster.slang.");
        RasterPC.List          = { CameraRasterList, VISBUFFER_COMPUTE_RASTER_CAPACITY };
        RasterPC.Cursors       = { CameraRasterCursors, 2u };
        RasterPC.Target        = { CameraRasterTarget, CameraRasterSize.x * CameraRasterSize.y };
        RasterPC.TargetSize[0] = CameraRasterSize.x;
        RasterPC.TargetSize[1] = CameraRasterSize.y;
        {
            SCENE_GPU_SCOPE(CL, "Raster");
            DispatchCompute(CL, RasterShader, RasterPC, VISBUFFER_COMPUTE_RASTER_GROUPS, 1u, 1u);
        }

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                        RHI::EStageFlags::PixelShader | RHI::EStageFlags::Transfer,
                        RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::TransferRead);

        // The late cull appends after what the early raster drew, so the late raster starts where this one ended.
        if (!bLate)
        {
            RHI::CmdMemcpy(CL, { CameraRasterCursors.Gpu + sizeof(uint32), sizeof(uint32) }, { CameraRasterCursors.Gpu, sizeof(uint32) });
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }

        const FSceneImage& VisRT   = GetNamedImage(ENamedImage::VisBuffer);
        const FSceneImage& DepthRT = GetNamedImage(ENamedImage::DepthAttachment);

        RHI::FRenderAttachment Color;
        Color.Texture = VisRT.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments         = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture  = DepthRT.Texture;
        Pass.DepthAttachment.LoadOp   = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        Pass.RenderArea               = CameraRasterSize;

        SCENE_GPU_SCOPE(CL, "Resolve");
        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, CameraRasterSize);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // The hardware draws' own test, so a compute pixel lands only where it is nearer than what they wrote.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest = RHI::EOp::Greater;
        RHI::CmdSetDepthStencil(CL, DepthDesc);

        FGraphicsPipelineKey Key;
        Key.VS          = FullscreenVS;
        Key.PS          = ResolvePS;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.Add({ VisRT.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FResolvePC
        {
            RHI::TGPUSpan<uint64> Target;
            uint32                TargetSize[2];
            uint32                _Pad0;
            uint32                _Pad1;
        } ResolvePC = {};
        static_assert(sizeof(FResolvePC) == 32, "FResolvePC must match VisBufferComputeResolve.slang.");
        ResolvePC.Target        = RasterPC.Target;
        ResolvePC.TargetSize[0] = CameraRasterSize.x;
        ResolvePC.TargetSize[1] = CameraRasterSize.y;
        RHI::CmdDraw(CL, MakeArgs(ResolvePC), 3, 1, 0, 0);

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    bool FDefaultSceneRenderer::IsTwoPhaseView(const FFrameData& Frame, uint32 ViewIndex)
    {
        return ViewIndex < static_cast<uint32>(Frame.Views.CullViews.Num())
            && (GetCullViewFlags(Frame.Views.CullViews[ViewIndex]) & ECullViewFlags::MeshletHiZ) != 0u;
    }

    bool FDefaultSceneRenderer::HasFarProxies() const
    {
        // Empty unless this frame's cull wrote the list, so a stale ring slot is never drawn.
        if (!bCullBucketsValid || SceneRootShared.ProxyList.IsEmpty())
        {
            return false;
        }

        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        return Settings == nullptr || (Settings->bFarProxies && Settings->ProxyTransitionPixels > 0.0f);
    }

    void FDefaultSceneRenderer::OcclusionCullFarProxies(RHI::FCmdListH CL, ECullPhase::Type Phase)
    {
        static const FString  ArgsDefine = "PROXY_CULL_ARGS";
        static const FShaderH CullShader = FShaderLibrary::Get("ProxyOcclusionCull.slang");
        static const FShaderH ArgsShader = FShaderLibrary::Get("ProxyOcclusionCull.slang", TSpan<const FString>(&ArgsDefine, 1));
        if (CullShader == nullptr || ArgsShader == nullptr)
        {
            return;
        }

        struct FProxyOcclusionCullPC
        {
            RHI::TGPUSpan<uint32> ProxyList;
            RHI::TGPUSpan<uint32> ProxyDrawArgs;
            RHI::TGPUSpan<uint32> OutProxyList;
            RHI::TGPUSpan<uint32> OutProxyDrawArgs;
            RHI::TGPUSpan<uint32> OutDispatchArgs;
        };
        static_assert(sizeof(FProxyOcclusionCullPC) == 80, "FProxyOcclusionCullPC must match ProxyOcclusionCull.slang.");

        constexpr uint32 DrawArgsWords     = sizeof(RHI::FDrawIndirectArguments) / sizeof(uint32);
        constexpr uint32 DispatchArgsWords = sizeof(RHI::FDispatchIndirectArguments) / sizeof(uint32);
        const uint8 Slot = CurrentFrameSlot;
        FProxyOcclusionCullPC PC;
        PC.ProxyList        = RHI::TGPUSpan<uint32>::Slice(ProxyListRing[Slot], 0u, ProxyListCapacity);
        PC.ProxyDrawArgs    = RHI::TGPUSpan<uint32>::Slice(ProxyDrawArgsRing[Slot], 0u, DrawArgsWords);
        PC.OutProxyList     = RHI::TGPUSpan<uint32>::Slice(ProxyListRing[Slot], ProxyListCapacity, ProxyListCapacity);
        PC.OutProxyDrawArgs = RHI::TGPUSpan<uint32>::Slice(ProxyDrawArgsRing[Slot], DrawArgsWords, DrawArgsWords);
        PC.OutDispatchArgs  = RHI::TGPUSpan<uint32>::Slice(ProxyDrawArgsRing[Slot], DrawArgsWords * 2u, DispatchArgsWords);

        // Early sizes the late cull from the listed count, and the geometry pass's own barrier hands those args to indirect fetch.
        if (Phase == ECullPhase::Early)
        {
            SCENE_GPU_SCOPE(CL, "Proxy Occlusion Cull Args");
            DispatchCompute(CL, ArgsShader, PC, 1u);
            return;
        }

        SCENE_GPU_SCOPE(CL, "Proxy Occlusion Cull");
        DispatchComputeIndirect(CL, CullShader, PC,
                                ProxyDrawArgsRing[Slot].Sub(sizeof(RHI::FDrawIndirectArguments) * 2u, sizeof(RHI::FDispatchIndirectArguments)));
    }

    void FDefaultSceneRenderer::DrawFarProxies(RHI::FCmdListH CL, const FUIntVector2& Extent, EFormat VisFormat, bool bOcclusionCulled)
    {
        static const FShaderH ProxyVertex = FShaderLibrary::Get("ProxyVisBufferVert.slang");
        static const FShaderH ProxyPixel  = FShaderLibrary::Get("ProxyVisBufferPixel.slang");
        if (ProxyVertex == nullptr || ProxyPixel == nullptr)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Far Proxies");

        FGraphicsPipelineKey Key;
        Key.VS          = ProxyVertex;
        Key.PS          = ProxyPixel;
        Key.DepthFormat = EFormat::D32;
        Key.Topology    = RHI::ETopology::TriangleStrip;
        Key.ColorTargets.Add({ VisFormat, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // The occlusion cull's args follow the full list's, matching the list half SceneRootShared.ProxyList points at.
        const uint64 ArgsOffset = bOcclusionCulled ? sizeof(RHI::FDrawIndirectArguments) : 0u;
        RHI::CmdDrawIndirect(CL, MakeArgs(), ProxyDrawArgsRing[CurrentFrameSlot].Sub(ArgsOffset, sizeof(RHI::FDrawIndirectArguments)),
                             1u, sizeof(RHI::FDrawIndirectArguments));
    }

    void FDefaultSceneRenderer::CustomDepthStencilPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const uint32 ViewIndex  = Frame.Views.CustomDepthCullView;
        if (ViewIndex == Constants::kIndexNoneU32 || !CurrentView->bIsPrimary)
        {
            return;
        }

        const FSceneImage& DepthRT   = GetNamedImage(ENamedImage::CustomDepth);
        const FSceneImage& VisRT     = GetNamedImage(ENamedImage::CustomDepthVis);
        const FSceneImage& StencilRT = GetNamedImage(ENamedImage::CustomStencil);
        if (!DepthRT.IsValid() || !VisRT.IsValid() || !StencilRT.IsValid())
        {
            return;
        }

        static const FShaderH FullscreenVS = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH ResolvePS    = FShaderLibrary::Get("CustomStencilResolve.slang");
        if (!FullscreenVS || !ResolvePS)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Custom Depth");

        const bool bHasDraws = !Frame.Geometry.DrawCommands.IsEmpty() && FindMissingMeshletDrawBuffer() == nullptr
                            && GetVisBufferPixelShader() != nullptr;

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::PixelShader,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);

        const FUIntVector2 Extent = DepthRT.GetExtent();

        {
            RHI::FRenderAttachment Color;
            Color.Texture = VisRT.Texture;
            Color.LoadOp  = RHI::ELoadOp::Clear;
            Color.StoreOp = RHI::EStoreOp::Store;

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments         = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            Pass.DepthAttachment.Texture  = DepthRT.Texture;
            Pass.DepthAttachment.LoadOp   = RHI::ELoadOp::Clear;
            Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
            Pass.DepthAttachment.Color[0] = 0.0f;
            Pass.RenderArea               = Extent;

            RHI::CmdBeginRenderPass(CL, Pass);
            SetViewportScissor(CL, Extent);

            RHI::FDepthStencilDesc DepthDesc;
            DepthDesc.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
            DepthDesc.DepthTest = RHI::EOp::Greater;
            RHI::CmdSetDepthStencil(CL, DepthDesc);

            if (bHasDraws)
            {
                DrawVisBufferBatches(CL, ViewIndex, EMeshletSlice::All, Extent, VisRT.Desc.Format);
            }

            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);
        }

        RHI::FRenderAttachment Stencil;
        Stencil.Texture = StencilRT.Texture;
        Stencil.LoadOp  = RHI::ELoadOp::Undefined;   // every pixel is written, background included
        Stencil.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Stencil, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = FullscreenVS;
        Key.PS = ResolvePS;
        Key.ColorTargets.Add({ StencilRT.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FCustomStencilResolvePC
        {
            uint32 VisBufferIndex;
            uint32 DrawListCount;
        } PC = {};
        static_assert(sizeof(FCustomStencilResolvePC) == 8, "FCustomStencilResolvePC must match CustomStencilResolve.slang.");
        PC.VisBufferIndex = (uint32)VisRT.GetResourceID();
        PC.DrawListCount  = DrawListCapacity;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    bool FDefaultSceneRenderer::BindShadowBatchPipeline(RHI::FCmdListH CL, const FMeshDrawCommand& Batch,
                                                      FShaderH PixelShader, bool* bOutStandIn, EFormat DepthFormat,
                                                      bool bBothFaces)
    {
        const bool bTwoSided = Batch.bTwoSided || bBothFaces;

        // A masked caster needs interpolant-carrying geometry and a clipping PS, or its shadow is the whole quad.
        const bool bMaskedClip = Batch.bMasked
                              && Batch.MeshShaderShadowMasked  != nullptr
                              && Batch.ShadowMaskedPixelShader != nullptr;

        FGraphicsPipelineKey Key;
        Key.MS          = bMaskedClip ? Batch.MeshShaderShadowMasked  : Batch.MeshShaderShadow;
        Key.PS          = bMaskedClip ? Batch.ShadowMaskedPixelShader : PixelShader;
        Key.DepthFormat = DepthFormat;
        // SPEC_SKINNED dead-strips the unused vertex-load path; a mixed batch takes a runtime branch.
        Key.SkinnedMode = SelectSkinnedMode(Batch);
        // A two-sided caster has no back face to reject, and culling one costs it half its shadow.
        Key.TriCullMode = (uint8)((bTwoSided ? 0u : (uint32)TriCull_Backface) | (uint32)TriCull_SmallPrim);
        if (Key.MS == nullptr)
        {
            return false;
        }

        RHI::FPipelineH Pipeline = FindPipeline(Key);
        if (!Pipeline)
        {
            // Casts the default material's shadow until this one's pipeline is ready.
            Key.MS = DefaultMaterialStage(EMaterialShaderStage::MeshShadow);
            Key.PS = PixelShader;
            if (Key.MS == nullptr || !(Pipeline = GetOrCreatePipeline(Key)))
            {
                return false;
            }
            if (bOutStandIn != nullptr)
            {
                *bOutStandIn = true;
            }
        }
        RHI::CmdSetPipeline(CL, Pipeline);

        // Set here, not hoisted out of the caller's loop, so it cannot disagree with the bit above.
        RHI::CmdSetCullMode(CL, bTwoSided ? RHI::ECullMode::None : RHI::ECullMode::Back);
        return true;
    }

    namespace
    {
        TConsoleVar<bool> CVarMultiViewShadowDraws(
            "r.Shadows.MultiViewDraws",
            true,
            "Draw the views of a shadow pass with one indirect draw per batch per group of viewports, instead of one draw per view.");
    }

    bool FDefaultSceneRenderer::DrawShadowViewsTogether(RHI::FCmdListH CL, const FShadowViewRun& Run, FShaderH PixelShader, EFormat DepthFormat)
    {
        // Consecutive views sit one view of indirect slots apart only while each slice holds a single sub-draw.
        const uint32 ViewsPerDraw = Math::Min(RHI::GetMaxViewports(), kMaxShadowViewportsPerDraw);
        // The vertex path writes no viewport index, so it draws its views one at a time.
        if (!CVarMultiViewShadowDraws.GetValue() || !RHI::SupportsMeshShaders() || Run.Views.IsEmpty() || !Run.bContiguous
            || MeshSubDrawsPerSlice != 1u || ViewsPerDraw < 2u)
        {
            return false;
        }

        const FFrameData& Frame = *RenderFrame;
        const uint32 NumViews  = (uint32)Run.Views.Num();
        const uint32 NumGroups = RenderUtils::GetGroupCount(NumViews, ViewsPerDraw);
        const RHI::TGPUSpan<FShadowRasterViewGPU> AllViews = RHI::CopyTransientArray(Run.Views.GetData(), Run.Views.Num());

        const auto SetGroupViewports = [&](uint32 First)
        {
            const uint32 Count = Math::Min(ViewsPerDraw, NumViews - First);
            RHI::CmdSetViewportArray(CL, TSpan<const RHI::FRect>(Run.Tiles.GetData() + First, Count));
        };

        // Batch-outer keeps one pipeline bind per batch, and the viewports only change between groups when there are several.
        if (NumGroups == 1u)
        {
            SetGroupViewports(0u);
        }

        for (uint32 OpaqueIdx : Frame.Geometry.OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = Frame.Geometry.DrawCommands[OpaqueIdx];
            if (!BindShadowBatchPipeline(CL, Batch, PixelShader, nullptr, DepthFormat))
            {
                continue;
            }

            for (uint32 First = 0; First < NumViews; First += ViewsPerDraw)
            {
                if (NumGroups > 1u)
                {
                    SetGroupViewports(First);
                }

                FMeshletPassContext Ctx;
                Ctx.CullViewIndex       = Run.FirstCullView + First;
                Ctx.ShadowViews.Address = AllViews.Address + (RHI::GPUPtr)First * sizeof(FShadowRasterViewGPU);
                Ctx.ShadowViews.Count   = Math::Min(ViewsPerDraw, NumViews - First);
                DrawMeshletBatch(CL, Batch, Ctx);
            }
        }
        return true;
    }

    void FDefaultSceneRenderer::DrawShadowBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, bool bUseMesh,
        uint32 CullViewIndex, int32 ShadowDataIndex, int32 ShadowViewIndex,
        const FUIntVector2& ViewportExtent)
    {
        if (!bUseMesh)
        {
            return;
        }

        // Shadows are single-phase, culling at instance level against the light's cull view.
        FMeshletPassContext Ctx;
        Ctx.CullViewIndex   = CullViewIndex;
        Ctx.ShadowDataIndex = ShadowDataIndex;
        Ctx.ShadowViewIndex = ShadowViewIndex;
        Ctx.ViewportW       = (float)ViewportExtent.x;
        Ctx.ViewportH       = (float)ViewportExtent.y;

        DrawMeshletBatch(CL, Batch, Ctx);
    }

    namespace
    {
        RHI::FRect AtlasTileRect(const FShadowTile& Tile)
        {
            const int32 TilePixelX = (int32)(Tile.UVOffset.x * GShadowAtlasResolution);
            const int32 TilePixelY = (int32)(Tile.UVOffset.y * GShadowAtlasResolution);
            const int32 TileSize   = (int32)(Tile.UVScale.x * GShadowAtlasResolution);
            return RHI::FRect{ TilePixelX, TilePixelX + TileSize, TilePixelY, TilePixelY + TileSize };
        }
    }

    void FDefaultSceneRenderer::PointShadowPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands             = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList           = Frame.Geometry.OpaqueDrawList;
        const auto& PackedShadows            = Frame.Lighting.PackedShadows;
        const auto& AtlasTiles               = Frame.Lighting.AtlasTiles;
        const auto& PointShadowCullViewBases = Frame.Views.PointShadowCullViewBases;

        if (PackedShadows[(uint32)ELightType::Point].IsEmpty())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Point Shadows");

        static const FShaderH PixelShader = FShaderLibrary::Get("ShadowMappingPixel.slang");
        if (!PixelShader)
        {
            return;
        }

        const TVector<FLightShadow>& PointShadows = PackedShadows[(uint32)ELightType::Point];

        RHI::FRenderPassDesc Pass;
        Pass.DepthAttachment.Texture  = ShadowAtlas.GetImage().Texture;
        Pass.DepthAttachment.LoadOp   = TakeLocalAtlasLoadOp();
        Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        Pass.DepthAttachment.Color[0] = 1.0f;
        Pass.RenderArea               = FUIntVector2(GShadowAtlasResolution, GShadowAtlasResolution);

        RHI::CmdBeginRenderPass(CL, Pass);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode            = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest            = RHI::EOp::Less;
        DepthDesc.DepthBias            = 1.0f;
        DepthDesc.DepthBiasSlopeFactor = 1.5f;
        RHI::CmdSetDepthStencil(CL, (DepthDesc));

        FShadowViewRun& Run = ShadowViewRunScratch;
        Run.Reset();
        for (int32 LightIdx = 0; LightIdx < PointShadows.Num(); ++LightIdx)
        {
            uint32 FaceView = PointShadowCullViewBases[LightIdx];
            if (FaceView == Constants::kIndexNoneU32)
            {
                continue;
            }

            const FLightShadowData& ShadowData = Frame.Lighting.Shadows[PointShadows[LightIdx].ShadowDataIndex];
            for (int32 Face = 0; Face < 6; ++Face)
            {
                const int32 ShadowMapIndex = ShadowData.Shadow[Face].ShadowMapIndex;
                if (ShadowMapIndex != Constants::kIndexNone)
                {
                    Run.Add(FaceView++, PointShadows[LightIdx].ShadowDataIndex, Face, AtlasTileRect(AtlasTiles[ShadowMapIndex]));
                }
            }
        }

        // Empty when one draw per batch already covered every view.
        const TSpan<const uint32> PerViewBatches = DrawShadowViewsTogether(CL, Run, PixelShader)
            ? TSpan<const uint32>() : TSpan<const uint32>(OpaqueDrawList.GetData(), OpaqueDrawList.Num());
        for (uint32 OpaqueIdx : PerViewBatches)
        {
            const FMeshDrawCommand& Batch = DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, PixelShader);

            for (int32 LightIdx = 0; LightIdx < PointShadows.Num(); ++LightIdx)
            {
                const FLightShadow& LightShadow = PointShadows[LightIdx];
                const uint32 ViewBase = PointShadowCullViewBases[LightIdx];
                if (ViewBase == Constants::kIndexNoneU32)
                {
                    continue;
                }

                const FLightShadowData& ShadowData = Frame.Lighting.Shadows[LightShadow.ShadowDataIndex];

                uint32 FaceView = ViewBase;
                for (int32 Face = 0; Face < 6; ++Face)
                {
                    const FLightShadow& FaceShadow = ShadowData.Shadow[Face];
                    if (FaceShadow.ShadowMapIndex == Constants::kIndexNone)
                    {
                        continue;
                    }
                    const FShadowTile& Tile = AtlasTiles[FaceShadow.ShadowMapIndex];
                    const int32 TilePixelX = (int32)(Tile.UVOffset.x * GShadowAtlasResolution);
                    const int32 TilePixelY = (int32)(Tile.UVOffset.y * GShadowAtlasResolution);
                    const int32 TileSize   = (int32)(Tile.UVScale.x * GShadowAtlasResolution);

                    const RHI::FRect TileRect{ TilePixelX, TilePixelX + TileSize, TilePixelY, TilePixelY + TileSize };
                    RHI::CmdSetViewport(CL, TileRect);
                    RHI::CmdSetScissor(CL, TileRect);

                    DrawShadowBatch(CL, Batch, bUseMesh, FaceView++,
                                    LightShadow.ShadowDataIndex, Face,
                                    FUIntVector2((uint32)TileSize, (uint32)TileSize));
                }
            }
        }

        const bool bShadowCallbacks = HasRenderCallbacks(ERenderStage::ShadowDepth);
        if (bShadowCallbacks)
        {
            const EFormat AtlasFormat = ShadowAtlas.GetImage().Desc.Format;
            for (int32 LightIdx = 0; LightIdx < PointShadows.Num(); ++LightIdx)
            {
                if (PointShadowCullViewBases[LightIdx] == Constants::kIndexNoneU32)
                {
                    continue;
                }

                const FLightShadowData& ShadowData = Frame.Lighting.Shadows[PointShadows[LightIdx].ShadowDataIndex];
                for (int32 Face = 0; Face < 6; ++Face)
                {
                    const int32 ShadowMapIndex = ShadowData.Shadow[Face].ShadowMapIndex;
                    if (ShadowMapIndex == Constants::kIndexNone)
                    {
                        continue;
                    }

                    RunShadowRenderCallbacks(CL, EShadowViewType::PointFace, (uint32)Face, ShadowData.ViewProjection[Face],
                                             AtlasTileRect(AtlasTiles[ShadowMapIndex]), AtlasFormat);
                }
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
        if (bShadowCallbacks)
        {
            RestoreAfterRenderCallbacks(CL);
        }
    }

    void FDefaultSceneRenderer::SpotShadowPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands             = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList           = Frame.Geometry.OpaqueDrawList;
        const auto& PackedShadows            = Frame.Lighting.PackedShadows;
        const auto& AtlasTiles               = Frame.Lighting.AtlasTiles;
        const auto& SpotShadowCullViewBases  = Frame.Views.SpotShadowCullViewBases;

        if (PackedShadows[(uint32)ELightType::Spot].IsEmpty())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Spot Shadows");

        static const FShaderH PixelShader = FShaderLibrary::Get("ShadowMappingPixel.slang");
        if (!PixelShader)
        {
            return;
        }

        RHI::FRenderPassDesc Pass;
        Pass.DepthAttachment.Texture  = ShadowAtlas.GetImage().Texture;
        Pass.DepthAttachment.LoadOp   = TakeLocalAtlasLoadOp();
        Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        Pass.DepthAttachment.Color[0] = 1.0f;
        Pass.RenderArea               = FUIntVector2(GShadowAtlasResolution, GShadowAtlasResolution);

        RHI::CmdBeginRenderPass(CL, Pass);

        // See PointShadowPass for why these bias values are low.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode            = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest            = RHI::EOp::Less;
        DepthDesc.DepthBias            = 1.0f;
        DepthDesc.DepthBiasSlopeFactor = 1.5f;
        RHI::CmdSetDepthStencil(CL, (DepthDesc));

        const TVector<FLightShadow>& SpotShadows = PackedShadows[(uint32)ELightType::Spot];

        FShadowViewRun& Run = ShadowViewRunScratch;
        Run.Reset();
        for (int32 SpotIdx = 0; SpotIdx < SpotShadows.Num(); ++SpotIdx)
        {
            if (SpotShadowCullViewBases[SpotIdx] != Constants::kIndexNoneU32)
            {
                const FLightShadow& Shadow = SpotShadows[SpotIdx];
                Run.Add(SpotShadowCullViewBases[SpotIdx], Shadow.ShadowDataIndex, 0, AtlasTileRect(AtlasTiles[Shadow.ShadowMapIndex]));
            }
        }

        // Empty when one draw per batch already covered every view.
        const TSpan<const uint32> PerViewBatches = DrawShadowViewsTogether(CL, Run, PixelShader)
            ? TSpan<const uint32>() : TSpan<const uint32>(OpaqueDrawList.GetData(), OpaqueDrawList.Num());
        for (uint32 OpaqueIdx : PerViewBatches)
        {
            const FMeshDrawCommand& Batch = DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, PixelShader);

            for (int32 SpotIdx = 0; SpotIdx < SpotShadows.Num(); ++SpotIdx)
            {
                const FLightShadow& Shadow  = SpotShadows[SpotIdx];
                const uint32 ViewIndex      = SpotShadowCullViewBases[SpotIdx];
                if (ViewIndex == Constants::kIndexNoneU32)
                {
                    continue;
                }

                const FShadowTile& Tile = AtlasTiles[Shadow.ShadowMapIndex];
                const int32 TilePixelX = (int32)(Tile.UVOffset.x * GShadowAtlasResolution);
                const int32 TilePixelY = (int32)(Tile.UVOffset.y * GShadowAtlasResolution);
                const int32 TileSize   = (int32)(Tile.UVScale.x * GShadowAtlasResolution);

                const RHI::FRect TileRect{ TilePixelX, TilePixelX + TileSize, TilePixelY, TilePixelY + TileSize };
                RHI::CmdSetViewport(CL, TileRect);
                RHI::CmdSetScissor(CL, TileRect);

                // Spotlights only use ViewProjection[0], so the shadow ViewIndex is 0.
                DrawShadowBatch(CL, Batch, bUseMesh, ViewIndex, Shadow.ShadowDataIndex, 0,
                                FUIntVector2((uint32)TileSize, (uint32)TileSize));
            }
        }

        const bool bShadowCallbacks = HasRenderCallbacks(ERenderStage::ShadowDepth);
        if (bShadowCallbacks)
        {
            const EFormat AtlasFormat = ShadowAtlas.GetImage().Desc.Format;
            for (int32 SpotIdx = 0; SpotIdx < SpotShadows.Num(); ++SpotIdx)
            {
                if (SpotShadowCullViewBases[SpotIdx] == Constants::kIndexNoneU32)
                {
                    continue;
                }

                const FLightShadow& Shadow = SpotShadows[SpotIdx];
                RunShadowRenderCallbacks(CL, EShadowViewType::Spot, SpotIdx,
                                         Frame.Lighting.Shadows[Shadow.ShadowDataIndex].ViewProjection[0],
                                         AtlasTileRect(AtlasTiles[Shadow.ShadowMapIndex]), AtlasFormat);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
        if (bShadowCallbacks)
        {
            RestoreAfterRenderCallbacks(CL);
        }
    }

    void FDefaultSceneRenderer::DecalPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const TVector<FGPUDecal>& Decals = Frame.Primitives.DecalExtracts;

        // With no decals the DBuffers do not exist and the base pass publishes the sentinel.
        if (Decals.IsEmpty())
        {
            return;
        }

        const FSceneImage& DBufferA = GetNamedImage(ENamedImage::DBufferA);
        const FSceneImage& DBufferB = GetNamedImage(ENamedImage::DBufferB);
        const FSceneImage& DBufferC = GetNamedImage(ENamedImage::DBufferC);
        const FSceneImage& DBufferD = GetNamedImage(ENamedImage::DBufferD);

        // Cleared to transmittance = 1 (alpha) / zero color, so the base pass reads a no-op where no decal lands.
        RHI::FRenderAttachment Colors[4];
        const RHI::FTextureH Targets[4] = { DBufferA.Texture, DBufferB.Texture, DBufferC.Texture, DBufferD.Texture };
        for (int i = 0; i < 4; ++i)
        {
            Colors[i].Texture  = Targets[i];
            Colors[i].LoadOp   = RHI::ELoadOp::Clear;
            Colors[i].StoreOp  = RHI::EStoreOp::Store;
            Colors[i].Color[0] = Colors[i].Color[1] = Colors[i].Color[2] = 0.0f;
            Colors[i].Color[3] = 1.0f;
        }

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(Colors, 4);
        Pass.RenderArea       = DBufferA.GetExtent();

        SCENE_GPU_SCOPE(CL, "Decals");

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, DBufferA.GetExtent());

        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::Front);

        // Transmittance compositing where RGB is SrcAlpha over and alpha accumulates (1 - coverage).
        RHI::FBlendDesc DecalBlend;
        DecalBlend.bBlendEnable   = true;
        DecalBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        DecalBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        DecalBlend.ColorOp        = RHI::EBlend::Add;
        DecalBlend.SrcAlphaFactor = RHI::EFactor::Zero;
        DecalBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;
        DecalBlend.AlphaOp        = RHI::EBlend::Add;

        RHI::FBlendDesc EmissionBlend;
        EmissionBlend.bBlendEnable   = true;
        EmissionBlend.SrcColorFactor = RHI::EFactor::One;
        EmissionBlend.DstColorFactor = RHI::EFactor::One;
        EmissionBlend.ColorOp        = RHI::EBlend::Add;
        EmissionBlend.SrcAlphaFactor = RHI::EFactor::Zero;
        EmissionBlend.DstAlphaFactor = RHI::EFactor::One;
        EmissionBlend.AlphaOp        = RHI::EBlend::Add;

        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);

        struct FDecalPushConstants
        {
            RHI::TGPUSpan<FGPUDecal> Decals;
            uint32 DepthIndex;
            uint32 Pad;
        };
        static_assert(sizeof(FDecalPushConstants) == 24, "FDecalPushConstants must match the slang pass block.");

        FDecalPushConstants PC = {};
        PC.Decals = RHI::CopyTransientArray(Decals.GetData(), Decals.Num());
        PC.DepthIndex = (uint32)SceneDepth.GetResourceID();

        // One instanced draw per shader batch.
        for (const FFrameData::FDecalBatch& Batch : Frame.Primitives.DecalBatches)
        {
            FShaderH VS = Batch.Shaders.VertexShader;
            FShaderH PS = Batch.Shaders.PixelShader;
            if (VS == nullptr || PS == nullptr)
            {
                continue;
            }

            FGraphicsPipelineKey Key;
            Key.VS = VS;
            Key.PS = PS;
            Key.ColorTargets.Emplace(DBufferA.Desc.Format, DecalBlend);
            Key.ColorTargets.Emplace(DBufferB.Desc.Format, DecalBlend);
            Key.ColorTargets.Emplace(DBufferC.Desc.Format, DecalBlend);
            Key.ColorTargets.Emplace(DBufferD.Desc.Format, EmissionBlend);
            const RHI::FPipelineH Pipeline = FindPipeline(Key);
            if (!Pipeline)
            {
                continue;
            }
            RHI::CmdSetPipeline(CL, Pipeline);

            RHI::CmdDraw(CL, MakeArgs(PC), 36, Batch.Count, 0, Batch.FirstInstance);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
}
