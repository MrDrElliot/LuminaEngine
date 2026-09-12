#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    void FDefaultSceneRenderer::TexturePaintPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.Extracts.PaintOps.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Texture Paint Pass", tracy::Color::Red);

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
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite,
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
                        RHI::EStageFlags::PixelShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
    }

    void FDefaultSceneRenderer::VisBufferPass(RHI::FCmdListH CL, uint32 ViewIndex, bool bClear,
                                            ECullPhase::Type Phase)
    {
        const FFrameData& Frame             = *RenderFrame;
        const auto& DrawCommands            = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList          = Frame.Geometry.OpaqueDrawList;

        if (ViewIndex == ~0u)
        {
            return;
        }

        // With nothing to draw the pass still opens when it clears, since the targets are read regardless.
        bool bHasDraws = !DrawCommands.empty();
        if (!bHasDraws && !bClear)
        {
            return;
        }

        // A single-phase view is drawn in full by Early, so skipping Late collapses the frame.
        if (Phase == ECullPhase::Late)
        {
            if (ViewIndex >= Frame.Views.CullViews.size() ||
                (GetCullViewFlags(Frame.Views.CullViews[ViewIndex]) & ECullViewFlags::MeshletHiZ) == 0u)
            {
                return;
            }
        }

        {
            const RHI::GPUPtr DrawListAddr  = GetMeshletDrawList().Gpu;
            const RHI::GPUPtr InstancesAddr = SceneBindings.Instances;
            const RHI::GPUPtr BucketsAddr   = GetRenderBuckets().Gpu;
            const char* MissingBuffer = DrawListAddr == 0  ? "MeshletDrawList"
                                      : InstancesAddr == 0 ? "Instances (visible-instance ring)"
                                      : BucketsAddr == 0   ? "RenderBuckets"
                                                           : nullptr;
            if (bHasDraws && MissingBuffer != nullptr)
            {
                static FName LastMissing;
                const FName Missing(MissingBuffer);
                if (Missing != LastMissing)
                {
                    LastMissing = Missing;
                    LOG_ERROR("VisBuffer: '{}' has no device address this frame; skipping the draws. "
                              "Drawing would page-fault the GPU inside the mesh shader.", MissingBuffer);
                }
                bHasDraws = false;
            }
        }

        LUMINA_PROFILE_SECTION_COLORED("VisBuffer Geometry Pass", tracy::Color::Red);

        static const FShaderH VisPixel = FShaderLibrary::Get("VisBufferPixel.slang");
        bHasDraws = bHasDraws && VisPixel != nullptr;
        if (!bHasDraws && !bClear)
        {
            return;
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::MeshShader | RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::PixelShader,
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

        FMeshletPassContext Ctx;
        Ctx.CullViewIndex     = ViewIndex;
        Ctx.ViewportW         = (float)Extent.x;
        Ctx.ViewportH         = (float)Extent.y;
        // The two phases append into the same bucket, so each rasterizes only what its own cull added.
        Ctx.Slice             = (Phase == ECullPhase::Late) ? EMeshletSlice::Late : EMeshletSlice::Early;

        if (bHasDraws)
        {
        ForEachMeshletBatch(CL, OpaqueDrawList, Ctx,
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
                Key.ColorTargets.push_back({ VisRT.Desc.Format, {} });
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
            });
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    bool FDefaultSceneRenderer::BindShadowBatchPipeline(RHI::FCmdListH CL, const FMeshDrawCommand& Batch,
                                                      FShaderH PixelShader)
    {
        // A masked caster needs the interpolant-carrying geometry and a PS that can clip, or its shadow is
        // the silhouette of the whole quad instead of the leaf.
        const bool bMaskedClip = Batch.bMasked
                              && Batch.MeshShaderShadowMasked  != nullptr
                              && Batch.ShadowMaskedPixelShader != nullptr;

        FGraphicsPipelineKey Key;
        Key.MS          = bMaskedClip ? Batch.MeshShaderShadowMasked  : Batch.MeshShaderShadow;
        Key.PS          = bMaskedClip ? Batch.ShadowMaskedPixelShader : PixelShader;
        Key.DepthFormat = EFormat::D32;
        // SPEC_SKINNED dead-strips the unused vertex-load path; a mixed batch takes a runtime branch.
        Key.SkinnedMode = SelectSkinnedMode(Batch);
        // A two-sided caster has no back face to reject, and culling one costs it half its shadow.
        Key.TriCullMode = (uint8)((Batch.bTwoSided ? 0u : (uint32)TriCull_Backface) | (uint32)TriCull_SmallPrim);
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        // Set here, not hoisted out of the caller's loop, so it cannot disagree with the bit above.
        RHI::CmdSetCullMode(CL, Batch.bTwoSided ? RHI::ECullMode::None : RHI::ECullMode::Back);
        return Key.MS != nullptr;
    }

    void FDefaultSceneRenderer::DrawShadowBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, bool bUseMesh,
        uint32 CullViewIndex, int32 ShadowDataIndex, int32 ShadowViewIndex,
        const FUIntVector2& ViewportExtent)
    {
        if (!bUseMesh)
        {
            return;
        }

        // Shadows are single-phase, culling at INSTANCE level against the cascade pyramid.
        FMeshletPassContext Ctx;
        Ctx.CullViewIndex   = CullViewIndex;
        Ctx.ShadowDataIndex = ShadowDataIndex;
        Ctx.ShadowViewIndex = ShadowViewIndex;
        Ctx.ViewportW       = (float)ViewportExtent.x;
        Ctx.ViewportH       = (float)ViewportExtent.y;

        DrawMeshletBatch(CL, Batch, Ctx);
    }

    void FDefaultSceneRenderer::PointShadowPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands             = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList           = Frame.Geometry.OpaqueDrawList;
        const auto& PackedShadows            = Frame.Lighting.PackedShadows;
        const auto& AtlasTiles               = Frame.Lighting.AtlasTiles;
        const auto& PointShadowCullViewBases = Frame.Views.PointShadowCullViewBases;

        if (PackedShadows[(uint32)ELightType::Point].empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Point Light Shadow Pass", tracy::Color::DeepPink2);

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

        for (uint32 OpaqueIdx : OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, PixelShader);

            for (uint32 LightIdx = 0; LightIdx < PointShadows.size(); ++LightIdx)
            {
                const FLightShadow& LightShadow = PointShadows[LightIdx];
                const uint32 ViewBase = PointShadowCullViewBases[LightIdx];
                if (ViewBase == ~0u)
                {
                    continue;
                }

                const FLightShadowData& ShadowData = Frame.Lighting.Shadows[LightShadow.ShadowDataIndex];

                for (int32 Face = 0; Face < 6; ++Face)
                {
                    const FLightShadow& FaceShadow = ShadowData.Shadow[Face];
                    const FShadowTile& Tile = AtlasTiles[FaceShadow.ShadowMapIndex];
                    const int32 TilePixelX = (int32)(Tile.UVOffset.x * GShadowAtlasResolution);
                    const int32 TilePixelY = (int32)(Tile.UVOffset.y * GShadowAtlasResolution);
                    const int32 TileSize   = (int32)(Tile.UVScale.x * GShadowAtlasResolution);

                    const RHI::FRect TileRect{ TilePixelX, TilePixelX + TileSize, TilePixelY, TilePixelY + TileSize };
                    RHI::CmdSetViewport(CL, TileRect);
                    RHI::CmdSetScissor(CL, TileRect);

                    DrawShadowBatch(CL, Batch, bUseMesh, ViewBase + (uint32)Face,
                                    LightShadow.ShadowDataIndex, Face,
                                    FUIntVector2((uint32)TileSize, (uint32)TileSize));
                }
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::SpotShadowPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands             = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList           = Frame.Geometry.OpaqueDrawList;
        const auto& PackedShadows            = Frame.Lighting.PackedShadows;
        const auto& AtlasTiles               = Frame.Lighting.AtlasTiles;
        const auto& SpotShadowCullViewBases  = Frame.Views.SpotShadowCullViewBases;

        if (PackedShadows[(uint32)ELightType::Spot].empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Spot Shadow Pass", tracy::Color::DeepPink4);

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

        // See PointShadowPass for why these bias values are lower than the CSM pass.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode            = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest            = RHI::EOp::Less;
        DepthDesc.DepthBias            = 1.0f;
        DepthDesc.DepthBiasSlopeFactor = 1.5f;
        RHI::CmdSetDepthStencil(CL, (DepthDesc));

        const TVector<FLightShadow>& SpotShadows = PackedShadows[(uint32)ELightType::Spot];

        // Batch-outer gives one pipeline bind per batch; per-spot tile viewports are dynamic state.
        for (uint32 OpaqueIdx : OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, PixelShader);

            for (uint32 SpotIdx = 0; SpotIdx < SpotShadows.size(); ++SpotIdx)
            {
                const FLightShadow& Shadow  = SpotShadows[SpotIdx];
                const uint32 ViewIndex      = SpotShadowCullViewBases[SpotIdx];
                if (ViewIndex == ~0u)
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

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::CascadedShowPass(RHI::FCmdListH CL, uint32 CascadeViewBase)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands     = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList   = Frame.Geometry.OpaqueDrawList;
        const auto& LightData        = Frame.Lighting.LightData;

        if (!LightData.bHasSun)
        {
            return;
        }
        if (Frame.Lighting.Lights[0].ShadowDataIndex == INDEX_NONE)
        {
            return;
        }

        if (CascadeViewBase == ~0u)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cascaded Shadow Map Pass", tracy::Color::DeepPink2);

        RHI::FRenderPassDesc Pass;
        Pass.DepthAttachment.Texture  = GetNamedImage(ENamedImage::Cascade).Texture;
        Pass.DepthAttachment.LoadOp   = TakeCascadeLoadOp();
        Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        Pass.DepthAttachment.Color[0] = 1.0f;
        Pass.RenderArea               = FUIntVector2(GCSMAtlasWidth, GCSMAtlasHeight);

        RHI::CmdBeginRenderPass(CL, Pass);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode            = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest            = RHI::EOp::Less;
        DepthDesc.DepthBias            = 25.0f;
        DepthDesc.DepthBiasSlopeFactor = 0.75f;
        RHI::CmdSetDepthStencil(CL, (DepthDesc));

        const int32 SunShadowDataIndex = Frame.Lighting.Lights[0].ShadowDataIndex;

        for (uint32 OpaqueIdx : OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, FShaderH{});

            for (uint32 c = 0; c < (uint32)NumCascades; ++c)
            {
                const int32 TileX = GCSMCascadeOriginX[c];
                const int32 TileY = GCSMCascadeOriginY[c];
                const int32 TileW = GCSMCascadeSizes[c];

                const RHI::FRect TileRect{ TileX, TileX + TileW, TileY, TileY + TileW };
                RHI::CmdSetViewport(CL, TileRect);
                RHI::CmdSetScissor(CL, TileRect);

                // Cascade is depth-only (no pixel shader); ViewIndex indexes the cascade's ViewProjection.
                DrawShadowBatch(CL, Batch, bUseMesh, CascadeViewBase + c, SunShadowDataIndex, (int32)c,
                                FUIntVector2((uint32)TileW, (uint32)TileW));
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::DecalPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const TVector<FGPUDecal>& Decals = Frame.Primitives.DecalExtracts;

        // With no decals the DBuffers do not exist and the base pass publishes the sentinel.
        if (Decals.empty())
        {
            return;
        }

        const FSceneImage& DBufferA = GetNamedImage(ENamedImage::DBufferA);
        const FSceneImage& DBufferB = GetNamedImage(ENamedImage::DBufferB);
        const FSceneImage& DBufferC = GetNamedImage(ENamedImage::DBufferC);

        // Cleared to transmittance = 1 (alpha) / zero color, so the base pass reads a no-op where no decal lands.
        RHI::FRenderAttachment Colors[3];
        const RHI::FTextureH Targets[3] = { DBufferA.Texture, DBufferB.Texture, DBufferC.Texture };
        for (int i = 0; i < 3; ++i)
        {
            Colors[i].Texture  = Targets[i];
            Colors[i].LoadOp   = RHI::ELoadOp::Clear;
            Colors[i].StoreOp  = RHI::EStoreOp::Store;
            Colors[i].Color[0] = Colors[i].Color[1] = Colors[i].Color[2] = 0.0f;
            Colors[i].Color[3] = 1.0f;
        }

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(Colors, 3);
        Pass.RenderArea       = DBufferA.GetExtent();

        LUMINA_PROFILE_SECTION_COLORED("Decal Pass", tracy::Color::Orange);

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

        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);

        struct FDecalPushConstants
        {
            RHI::TGPUSpan<FGPUDecal> Decals;
            uint32 DepthIndex;
            uint32 Pad;
        };
        static_assert(sizeof(FDecalPushConstants) == 24, "FDecalPushConstants must match the slang pass block.");

        FDecalPushConstants PC = {};
        PC.Decals = RHI::CopyTransientArray(Decals.data(), Decals.size());
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
            Key.ColorTargets.push_back({ DBufferA.Desc.Format, DecalBlend });
            Key.ColorTargets.push_back({ DBufferB.Desc.Format, DecalBlend });
            Key.ColorTargets.push_back({ DBufferC.Desc.Format, DecalBlend });
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            RHI::CmdDraw(CL, MakeArgs(PC), 36, Batch.Count, 0, Batch.FirstInstance);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
    namespace
    {
        static FSceneImage CreateTerrainImage(uint32 Size, uint16 ArraySize, EFormat Format, bool bUav, bool bArrayView = false,
            const char* DebugName = "Terrain.Image")
        {
            RHI::FTextureDesc Desc;
            // Shader samples weight maps as Sampler2DArray, so array textures get an array view.
            Desc.Type       = (bArrayView || ArraySize > 1) ? RHI::ETextureType::Tex2DArray : RHI::ETextureType::Tex2D;
            Desc.Dimension  = FUIntVector3(Size, Size, 1);
            Desc.LayerCount = ArraySize;
            Desc.Format     = Format;
            Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;
            if (bUav)
            {
                Desc.Usage = Desc.Usage | RHI::EImageUsageFlags::Storage;
            }
            FSceneImage Image = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ bUav);
            RHI::SetDebugName(Image.Texture, DebugName);
            return Image;
        }
    }

    void FDefaultSceneRenderer::TerrainUpdatePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Terrain Update", tracy::Color::SeaGreen);

        static const FShaderH NormalShader = FShaderLibrary::Get("TerrainNormalCompute.slang");

        const FFrameData& Frame = *RenderFrame;

        if (!TerrainGPUStates.empty())
        {
            const TVector<ECS::FEntity>& Live = Frame.Extracts.LiveTerrainEntities;
            auto IsLive = [&](ECS::FEntity E)
            {
                return Algo::Contains(Live, E);
            };

            for (auto It = TerrainGPUStates.begin(); It != TerrainGPUStates.end();)
            {
                if (!IsLive(It->first))
                {
                    FTerrainGPUState& Dead = It->second;
                    RetireSceneImage(Dead.HeightmapTexture);
                    RetireSceneImage(Dead.NormalTexture);
                    RetireSceneImage(Dead.LayerWeightTexture);
                    if (Dead.ChunkInfoBuffer)      { DeferFree(Dead.ChunkInfoBuffer); }
                    if (Dead.MeshletInfoBuffer)    { DeferFree(Dead.MeshletInfoBuffer); }
                    if (Dead.VisibleMeshletBuffer) { DeferFree(Dead.VisibleMeshletBuffer); }
                    if (Dead.IndirectDrawBuffer)   { DeferFree(Dead.IndirectDrawBuffer); }
                    It = TerrainGPUStates.erase(It);
                }
                else
                {
                    ++It;
                }
            }
        }

        bool bAnyUpload = false;

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            if (TerrainItem.Resolution < 2 || TerrainItem.ChunkResolution < 2)
            {
                continue;
            }

            FTerrainGPUState& State = TerrainGPUStates[TerrainItem.Entity];
            const uint32 Res        = (uint32)TerrainItem.Resolution;
            const uint32 LayerCount = (uint32)Math::Max(TerrainItem.LayerCount, 1);
            const size_t SlicePixels = size_t(Res) * size_t(Res);

            const bool bRealloc = State.AllocatedResolution != Res || State.AllocatedLayerCount != LayerCount;
            if (bRealloc)
            {
                RetireSceneImage(State.HeightmapTexture);
                RetireSceneImage(State.NormalTexture);
                RetireSceneImage(State.LayerWeightTexture);

                State.HeightmapTexture   = CreateTerrainImage(Res, 1u,          EFormat::R32_FLOAT, false, false, "Terrain.Heightmap");
                State.NormalTexture      = CreateTerrainImage(Res, 1u,          EFormat::RGBA8_UNORM, true, false, "Terrain.Normal");
                State.LayerWeightTexture = CreateTerrainImage(Res, (uint16)Math::Max(LayerCount, 1u), EFormat::R8_UNORM, false, true, "Terrain.LayerWeight");
                State.AllocatedResolution = Res;
                State.AllocatedLayerCount = LayerCount;

                if (TerrainItem.WeightUpload == 0)
                {
                    const float Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                    RHI::CmdClearTexture(CL, State.LayerWeightTexture.Texture, Zero);
                    bAnyUpload = true;
                }
            }

            // Height upload (from the snapshot)
            const int32 ResI = (int32)Res;
            const bool  bHeightDirty = TerrainItem.HeightUpload != 0;
            FIntVector2 RectMin = FIntVector2(0);
            FIntVector2 RectMax = FIntVector2(ResI - 1);

            if (TerrainItem.HeightUpload == 1 && TerrainItem.HeightBytes.size() == SlicePixels)
            {
                const RHI::FGPURange Src = RHI::CopyTransientArray(TerrainItem.HeightBytes.data(), TerrainItem.HeightBytes.size());
                RHI::CmdCopyMemoryToTexture(CL, Src, Res, State.HeightmapTexture.Texture);
                bAnyUpload = true;
            }
            else if (TerrainItem.HeightUpload == 2)
            {
                RectMin = TerrainItem.HeightRectMin;
                RectMax = TerrainItem.HeightRectMax;
                const uint32 RegionW = uint32(RectMax.x - RectMin.x + 1);
                const uint32 RegionH = uint32(RectMax.y - RectMin.y + 1);
                // Snapshot rect is tightly packed, so the source row pitch is RegionW, not Res.
                const RHI::FGPURange Src = RHI::CopyTransientArray(TerrainItem.HeightBytes.data(), TerrainItem.HeightBytes.size());

                RHI::FTextureSlice Slice;
                Slice.Offset = FUIntVector3((uint32)RectMin.x, (uint32)RectMin.y, 0);
                Slice.Extent = FUIntVector3(RegionW, RegionH, 1);
                RHI::CmdCopyMemoryToTexture(CL, Src, RegionW, State.HeightmapTexture.Texture, Slice);
                bAnyUpload = true;
            }

            // Weights upload as whole slices, packed ascending in the snapshot.
            if (TerrainItem.WeightUpload != 0 && !TerrainItem.WeightBytes.empty())
            {
                const uint8* Cursor = TerrainItem.WeightBytes.data();
                const uint8* End    = Cursor + TerrainItem.WeightBytes.size();
                for (uint32 L = 0; L < LayerCount; ++L)
                {
                    if ((TerrainItem.WeightSliceMask & (1u << L)) == 0u)
                    {
                        continue;
                    }
                    if (Cursor + SlicePixels > End)
                    {
                        break;
                    }
                    const RHI::FGPURange Src = RHI::CopyTransientArray(Cursor, SlicePixels);

                    RHI::FTextureSlice Slice;
                    Slice.Layer = L;
                    RHI::CmdCopyMemoryToTexture(CL, Src, Res, State.LayerWeightTexture.Texture, Slice);
                    Cursor += SlicePixels;
                    bAnyUpload = true;
                }
            }

            if (bHeightDirty && NormalShader)
            {
                // Heightmap upload must land before the normal recompute samples it.
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                    RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

                const int32 NMinX = Math::Max(RectMin.x - 1, 0);
                const int32 NMinY = Math::Max(RectMin.y - 1, 0);
                const int32 NMaxX = Math::Min(RectMax.x + 1, ResI - 1);
                const int32 NMaxY = Math::Min(RectMax.y + 1, ResI - 1);
                const int32 NW    = NMaxX - NMinX + 1;
                const int32 NH    = NMaxY - NMinY + 1;

                struct FTerrainNormalArgs
                {
                    FTerrainNormalParams Params;
                    uint32 HeightmapIndex;
                    uint32 NormalUAV;
                    uint32 _Pad0;
                    uint32 _Pad1;
                };

                FTerrainNormalArgs NormalArgs{};
                NormalArgs.Params.Resolution    = ResI;
                NormalArgs.Params.RegionMinX    = NMinX;
                NormalArgs.Params.RegionMinY    = NMinY;
                NormalArgs.Params.RegionSizeX   = NW;
                NormalArgs.Params.RegionSizeY   = NH;
                NormalArgs.Params.TileWorldSize = TerrainItem.TileWorldSize;
                NormalArgs.Params.MaxHeight     = TerrainItem.MaxHeight;
                NormalArgs.HeightmapIndex       = (uint32)State.HeightmapTexture.GetResourceID();
                NormalArgs.NormalUAV            = (uint32)State.NormalTexture.GetMipUAVIndex(0);

                DispatchCompute(CL, NormalShader, NormalArgs, RenderUtils::GetGroupCount((uint32)NW, 8u),
                                                   RenderUtils::GetGroupCount((uint32)NH, 8u), 1u);

                // Normals are sampled by the terrain VS/PS.
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                    RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
            }

            if (TerrainItem.bGeometryRebuilt)
            {
                const uint32 ChunkCount   = (uint32)TerrainItem.Chunks.size();
                const uint32 MeshletCount = (uint32)TerrainItem.Meshlets.size();

                if (ChunkCount > 0 && MeshletCount > 0)
                {
                    auto AllocSSBO = [&](RHI::FGPUAllocation& Buffer, uint64 SizeBytes, const char* DebugName)
                    {
                        if (!Buffer || Buffer.Size < SizeBytes)
                        {
                            if (Buffer)
                            {
                                DeferFree(Buffer);
                            }
                            Buffer = CreateSceneBuffer(std::max<uint64>(SizeBytes, 16ull), DebugName);
                        }
                    };

                    AllocSSBO(State.ChunkInfoBuffer,      uint64(ChunkCount)   * sizeof(FTerrainChunkInfo),      "Terrain.ChunkInfo");
                    AllocSSBO(State.MeshletInfoBuffer,    uint64(MeshletCount) * sizeof(FTerrainMeshletInfo),   "Terrain.MeshletInfo");
                    AllocSSBO(State.VisibleMeshletBuffer, uint64(MeshletCount) * sizeof(FTerrainVisibleMeshlet), "Terrain.VisibleMeshlets");
                    AllocSSBO(State.IndirectDrawBuffer,   sizeof(RHI::FDrawIndirectArguments),                   "Terrain.IndirectDraw");

                    WriteBuffer(CL, State.ChunkInfoBuffer.Gpu,   TerrainItem.Chunks.data(),   ChunkCount * sizeof(FTerrainChunkInfo));
                    WriteBuffer(CL, State.MeshletInfoBuffer.Gpu, TerrainItem.Meshlets.data(), MeshletCount * sizeof(FTerrainMeshletInfo));
                    bAnyUpload = true;

                    State.AllocatedChunkCount   = ChunkCount;
                    State.AllocatedMeshletCount = MeshletCount;
                }
            }
        }

        if (bAnyUpload)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
        }
    }

    void FDefaultSceneRenderer::TerrainCullPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.Extracts.TerrainExtracts.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Terrain Cull", tracy::Color::SeaGreen);

        static const FShaderH CullShader = FShaderLibrary::Get("TerrainCull.slang");
        if (!CullShader)
        {
            return;
        }

        bool bAnyDispatched = false;

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            auto TerrainStateIt = TerrainGPUStates.find(TerrainItem.Entity);
            if (TerrainStateIt == TerrainGPUStates.end())
            {
                continue;
            }
            FTerrainGPUState& State = TerrainStateIt->second;
            if (!State.ChunkInfoBuffer || !State.MeshletInfoBuffer || !State.VisibleMeshletBuffer || !State.IndirectDrawBuffer)
            {
                continue;
            }
            if (State.AllocatedChunkCount == 0u || State.AllocatedMeshletCount == 0u)
            {
                continue;
            }

            RHI::FDrawIndirectArguments InitialArgs{};
            InitialArgs.VertexCount   = (uint32)(GTerrainMeshletMaxQuads * 6);
            InitialArgs.InstanceCount = 0u;
            InitialArgs.FirstVertex   = 0u;
            InitialArgs.FirstInstance = 0u;
            WriteBuffer(CL, State.IndirectDrawBuffer.Gpu, &InitialArgs, sizeof(InitialArgs));
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);

            if (!bAnyDispatched)
            {
                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullShader));
            }

            FTerrainCullPushConstants Push{};
            Push.Chunks          = { State.ChunkInfoBuffer, State.AllocatedChunkCount };
            Push.Meshlets        = { State.MeshletInfoBuffer, State.AllocatedMeshletCount };
            Push.VisibleMeshlets = { State.VisibleMeshletBuffer, State.AllocatedMeshletCount };
            Push.TerrainIndirect = { State.IndirectDrawBuffer };

            RHI::CmdDispatch(CL, MakeArgs(Push), State.AllocatedChunkCount, 1u, 1u);
            bAnyDispatched = true;
        }

        if (bAnyDispatched)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::VertexShader | RHI::EStageFlags::IndirectArguments,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead | RHI::EAccessFlags::IndexRead);
        }
    }

    // Matches FGrassRetirePushConstants in GrassRetire.slang.
    struct FGrassRetirePushConstants
    {
        RHI::TGPUSpan<FInstanceCullEntry> CullEntries;
        RHI::TGPUSpan<uint32>             Cursor;
        RHI::TGPUSpan<uint32>             PrevCursor;
    };
    static_assert(sizeof(FGrassRetirePushConstants) == 48, "FGrassRetirePushConstants must match GrassRetire.slang.");

    // Matches FGrassScatterPushConstants in GrassScatter.slang.
    struct FGrassScatterPushConstants
    {
        RHI::TGPUSpan<FInstanceCullEntry> OutCullEntries;
        RHI::TGPUSpan<FTransform3x4>      OutTransforms;
        RHI::TGPUSpan<FInstanceStatic>    OutStatic;
        RHI::TGPUSpan<uint32>             OutCursor;

        uint32  HeightmapIndex    = 0;
        uint32  NormalIndex       = 0;
        uint32  LayerWeightsIndex = 0;
        uint32  LayerIndex        = 0;

        float   CameraX = 0.0f;
        float   CameraZ = 0.0f;
        float   Radius  = 0.0f;
        float   CellSize = 1.0f;

        float   MinWeight   = 0.0f;
        float   MaxSlopeCos = 0.0f;
        float   ScaleMin    = 1.0f;
        float   ScaleMax    = 1.0f;

        float   AlignToNormal = 0.0f;
        float   ZOffset       = 0.0f;
        uint32  bRandomYaw    = 1;
        uint32  Seed          = 0;

        uint32  GridSide     = 0;
        uint32  _PadMaxInstances = 0;

        uint32  _PadInstanceSlotBase = 0;
        uint32  DrawIDAndFlags    = 0;
        uint32  SurfaceDescIndex  = 0;
        uint32  MeshletHeaderSlot = 0;
        uint32  MaterialIndex     = 0;
        uint32  EntityID          = 0;
        float   MeshBoundsRadius  = 0.0f;
        float   MaxDrawDistance   = 0.0f;

        float   TerrainOriginX  = 0.0f;
        float   TerrainOriginZ  = 0.0f;
        float   TileWorldSize   = 0.0f;
        float   MaxHeight       = 0.0f;
        float   TerrainBaseY    = 0.0f;
        uint32  _Pad0 = 0;
        uint32  _Pad1 = 0;
        uint32  _Pad2 = 0;
    };
    static_assert(sizeof(FGrassScatterPushConstants) == 200,
        "FGrassScatterPushConstants must match GrassScatter.slang.");

    void FDefaultSceneRenderer::GrassScatterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.Extracts.TerrainExtracts.empty())
        {
            return;
        }

        // The blades land in the retained arrays, so this must run after their upload and before the cull
        // reads them. Without those buffers there is nowhere to write.
        if (!RetainedCullEntryBuffer || !RetainedTransformBuffer || !RetainedStaticBuffer)
        {
            return;
        }

        static const FShaderH ScatterShader = FShaderLibrary::Get("GrassScatter.slang");
        if (!ScatterShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Grass Scatter", tracy::Color::ForestGreen);

        const FVector3 CameraPos = FVector3(Frame.SceneGlobalData.CameraData.Location);
        bool bAnyDispatched = false;

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            if (TerrainItem.Grass.empty() || TerrainItem.GrassMaxInstances == 0u)
            {
                continue;
            }

            auto TerrainStateIt = TerrainGPUStates.find(TerrainItem.Entity);
            if (TerrainStateIt == TerrainGPUStates.end())
            {
                continue;
            }

            const FTerrainGPUState& Terrain = TerrainStateIt->second;
            if (!Terrain.HeightmapTexture || !Terrain.NormalTexture || !Terrain.LayerWeightTexture)
            {
                continue;
            }

            TVector<FGrassGPUState>& States = GrassGPUStates[TerrainItem.Entity];
            States.resize(TerrainItem.Grass.size());

            // One scratch cursor per species, in extract order, matching what BeginFrameScratch sized.
            const uint32 FirstCursor = GrassCursorCursor;
            GrassCursorCursor += (uint32)TerrainItem.Grass.size();

            const FVector3 Origin = FVector3(TerrainItem.WorldMatrix[3]);

            for (SIZE_T Index = 0; Index < TerrainItem.Grass.size(); ++Index)
            {
                const FFrameData::FGrassSpeciesExtract& Species = TerrainItem.Grass[Index];
                FGrassGPUState& State = States[Index];

                const FScenePrimitiveSet::FGrassSpeciesBinding Binding =
                    ScenePrimitives.AcquireGrassSpecies(Species.Mesh, TerrainItem.GrassMaxInstances);
                if (!Binding.bValid)
                {
                    continue;
                }

                // A species may cap its own draw distance below the component's radius.
                const float Radius = Species.CullDistance > 0.0f
                                   ? Math::Min(Species.CullDistance, TerrainItem.GrassMaxDrawDistance)
                                   : TerrainItem.GrassMaxDrawDistance;
                if (Radius <= 0.0f || Species.CellSize <= 0.0f)
                {
                    continue;
                }

                // Candidates cover the square inscribing the radius; the shader rejects the corners.
                const uint32 GridSide = (uint32)Math::Ceil((Radius * 2.0f) / Species.CellSize);
                if (GridSide == 0u)
                {
                    continue;
                }

                if (!State.PrevCursorBuffer)
                {
                    State.PrevCursorBuffer = CreateSceneBuffer(sizeof(uint32), "Grass.PrevCursor");
                    if (State.PrevCursorBuffer)
                    {
                        // A fresh block has no live tail, so the first retire has nothing to walk.
                        RHI::CmdMemset(CL, { State.PrevCursorBuffer.Gpu, sizeof(uint32) }, 0u);
                        Barriers::TransferToCompute(CL);
                    }
                }
                if (!State.PrevCursorBuffer)
                {
                    continue;
                }

                // The append cursor restarts every frame; the scratch block already zeroed it.
                const RHI::FGPURange Cursor = GetGrassCursor(FirstCursor + (uint32)Index);

                // Whatever the scatter leaves past its cursor is retired afterward, not zeroed in advance.
                FGrassRetireItem& Retire = GrassRetireScratch.emplace_back();
                Retire.SlotBase   = Binding.InstanceSlotBase;
                Retire.Capacity   = Binding.Capacity;
                Retire.Cursor     = Cursor;
                Retire.PrevCursor = State.PrevCursorBuffer;

                if (!bAnyDispatched)
                {
                    RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ScatterShader));
                }

                FGrassScatterPushConstants Push{};
                // Sliced to the reserved block, so the shader indexes it block-locally and cannot name a
                // retained slot that belongs to something else.
                Push.OutCullEntries = RHI::TGPUSpan<FInstanceCullEntry>::Slice(
                    RetainedCullEntryBuffer, Binding.InstanceSlotBase, Binding.Capacity);
                Push.OutTransforms  = RHI::TGPUSpan<FTransform3x4>::Slice(
                    RetainedTransformBuffer, Binding.InstanceSlotBase, Binding.Capacity);
                Push.OutStatic      = RHI::TGPUSpan<FInstanceStatic>::Slice(
                    RetainedStaticBuffer, Binding.InstanceSlotBase, Binding.Capacity);
                Push.OutCursor      = { Cursor };
                Push.HeightmapIndex     = (uint32)Terrain.HeightmapTexture.GetResourceID();
                Push.NormalIndex        = (uint32)Terrain.NormalTexture.GetResourceID();
                Push.LayerWeightsIndex  = (uint32)Terrain.LayerWeightTexture.GetResourceID();
                Push.LayerIndex         = Species.LayerIndex;
                Push.CameraX            = CameraPos.x;
                Push.CameraZ            = CameraPos.z;
                Push.Radius             = Radius;
                Push.CellSize           = Species.CellSize;
                Push.MinWeight          = Species.MinWeight;
                Push.MaxSlopeCos        = Species.MaxSlopeCos;
                Push.ScaleMin           = Species.ScaleMin;
                Push.ScaleMax           = Species.ScaleMax;
                Push.AlignToNormal      = Species.AlignToNormal;
                Push.ZOffset            = Species.ZOffset;
                Push.bRandomYaw         = Species.bRandomYaw ? 1u : 0u;
                Push.Seed               = Species.Seed;
                Push.GridSide           = GridSide;

                Push.DrawIDAndFlags     = Binding.DrawIDAndFlags;
                Push.SurfaceDescIndex   = Binding.SurfaceDescIndex;
                Push.MeshletHeaderSlot  = Binding.MeshletHeaderSlot;
                Push.MaterialIndex      = Binding.MaterialIndex;
                Push.EntityID           = (uint32)TerrainItem.Entity.GetPacked();
                Push.MeshBoundsRadius   = Binding.MeshBoundsRadius;
                Push.MaxDrawDistance    = Radius;
                Push.TerrainOriginX     = Origin.x;
                Push.TerrainOriginZ     = Origin.z;
                Push.TileWorldSize      = TerrainItem.TileWorldSize;
                Push.MaxHeight          = TerrainItem.MaxHeight;
                Push.TerrainBaseY       = Origin.y;

                const uint32 Groups = (GridSide + 7u) / 8u;
                RHI::CmdDispatch(CL, MakeArgs(Push), Groups, Groups, 1u);
                bAnyDispatched = true;
            }
        }

        if (bAnyDispatched)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }

        if (!GrassRetireScratch.empty())
        {
            static const FShaderH RetireShader = FShaderLibrary::Get("GrassRetire.slang");
            if (RetireShader)
            {
                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(RetireShader));
                for (const FGrassRetireItem& Item : GrassRetireScratch)
                {
                    FGrassRetirePushConstants Push{};
                    // Sliced like the scatter's span, so the clamp to the allocation is the same one.
                    Push.CullEntries = RHI::TGPUSpan<FInstanceCullEntry>::Slice(RetainedCullEntryBuffer, Item.SlotBase, Item.Capacity);
                    Push.Cursor      = { Item.Cursor };
                    Push.PrevCursor  = { Item.PrevCursor };
                    RHI::CmdDispatch(CL, MakeArgs(Push), (Item.Capacity + 63u) / 64u, 1u, 1u);
                }

                // Four bytes each, on the GPU; the count never comes back to the CPU.
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                    RHI::EStageFlags::Transfer,
                    RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
                for (const FGrassRetireItem& Item : GrassRetireScratch)
                {
                    RHI::CmdMemcpy(CL, { Item.PrevCursor.Gpu, sizeof(uint32) }, Item.Cursor);
                }
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                    RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            }
            GrassRetireScratch.clear();
        }
    }

    void FDefaultSceneRenderer::TerrainDepthPrePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (Frame.Extracts.TerrainExtracts.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Terrain Depth", tracy::Color::SeaGreen);

        static const FShaderH StampPS = FShaderLibrary::Get("TerrainDepthPixel.slang");
        const FSceneImage& VisRT = GetNamedImage(ENamedImage::VisBuffer);

        RHI::ELoadOp VisLoadOp = DrawCommands.empty() ? RHI::ELoadOp::Clear : RHI::ELoadOp::Load;

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            if (TerrainItem.Resolution < 2 || TerrainItem.ChunkResolution < 2)
            {
                continue;
            }

            auto TerrainStateIt = TerrainGPUStates.find(TerrainItem.Entity);
            if (TerrainStateIt == TerrainGPUStates.end())
            {
                continue;
            }
            FTerrainGPUState& State = TerrainStateIt->second;
            if (!State.HeightmapTexture || !State.NormalTexture || !State.LayerWeightTexture)
            {
                continue;
            }
            if (!State.ChunkInfoBuffer || !State.MeshletInfoBuffer || !State.VisibleMeshletBuffer || !State.IndirectDrawBuffer)
            {
                continue;
            }
            if (State.AllocatedMeshletCount == 0u)
            {
                continue;
            }

            // Shaders were resolved + ref-held at extract (Extract Terrain); null VS => skip.
            FShaderH VertexShader = TerrainItem.Shaders.VertexShader;
            if (!VertexShader)
            {
                continue;
            }

            const ECS::FEntity Entity = TerrainItem.Entity;
            const uint32 Res = (uint32)TerrainItem.Resolution;

            const FMatrix4 WorldMat    = TerrainItem.WorldMatrix;
            const FVector3 WorldOrigin = FVector3(WorldMat[3]);
            const float HalfSize        = TerrainItem.TileWorldSize * 0.5f;

            const int32 QuadsPerChunk        = Math::Max(1, TerrainItem.ChunkResolution - 1);
            const int32 ChunksPerSide        = Math::Max(1, ((int32)Res - 1) / QuadsPerChunk);
            const int32 MeshletsPerChunkSide = (QuadsPerChunk + GTerrainMeshletQuads - 1) / GTerrainMeshletQuads;

            FTerrainRenderParams RenderParams{};
            RenderParams.OriginXZ             = FVector2(WorldOrigin.x - HalfSize, WorldOrigin.z - HalfSize);
            RenderParams.TileWorldSize        = TerrainItem.TileWorldSize;
            RenderParams.MaxHeight            = TerrainItem.MaxHeight;
            RenderParams.Resolution           = (int32)Res;
            RenderParams.ChunkResolution      = TerrainItem.ChunkResolution;
            RenderParams.ChunksPerSide        = ChunksPerSide;
            RenderParams.LayerCount           = TerrainItem.LayerCount;
            RenderParams.WorldOriginY         = FVector3(WorldOrigin.y, 0.0f, 0.0f);
            RenderParams.EntityID             = (uint32)Entity;
            RenderParams.MaterialIndex        = TerrainItem.MaterialIndex;
            RenderParams.MeshletsPerChunkSide = MeshletsPerChunkSide;
            RenderParams.MeshletQuadSide      = GTerrainMeshletQuads;

            const FUIntVector2 Extent = GetNamedImage(ENamedImage::HDR).GetExtent();

            RHI::FRenderAttachment Color;
            Color.Texture = VisRT.Texture;
            Color.LoadOp  = VisLoadOp;
            Color.StoreOp = RHI::EStoreOp::Store;

            RHI::FRenderPassDesc Pass;
            if (StampPS != nullptr)
            {
                Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            }
            Pass.DepthAttachment.Texture  = GetNamedImage(ENamedImage::DepthAttachment).Texture;
            Pass.DepthAttachment.LoadOp   = RHI::ELoadOp::Load;   // cleared by VisBuffer phase 1 or ResetPass
            Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
            Pass.RenderArea               = Extent;

            RHI::CmdBeginRenderPass(CL, Pass);
            SetViewportScissor(CL, Extent);

            RHI::FDepthStencilDesc DepthDesc;
            DepthDesc.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
            DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
            RHI::CmdSetDepthStencil(CL, (DepthDesc));
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

            FGraphicsPipelineKey Key;
            Key.VS          = VertexShader;
            Key.PS          = StampPS;
            Key.DepthFormat = EFormat::D32;
            if (StampPS != nullptr)
            {
                Key.ColorTargets.push_back({ VisRT.Desc.Format, {} });
            }
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FTerrainPushConstants Push{};
            Push.ParamsAddr        = RHI::CopyTransient(RenderParams);
            Push.Chunks            = { State.ChunkInfoBuffer, State.AllocatedChunkCount };
            Push.Meshlets          = { State.MeshletInfoBuffer, State.AllocatedMeshletCount };
            Push.Visible           = { State.VisibleMeshletBuffer, State.AllocatedMeshletCount };
            Push.HeightmapIndex    = (uint32)State.HeightmapTexture.GetResourceID();
            Push.NormalIndex       = (uint32)State.NormalTexture.GetResourceID();
            Push.LayerWeightsIndex = (uint32)State.LayerWeightTexture.GetResourceID();

            RHI::CmdDrawIndirect(CL, MakeArgs(Push), State.IndirectDrawBuffer, 1u, sizeof(RHI::FDrawIndirectArguments));

            RHI::CmdEndRenderPass(CL);
            VisLoadOp = RHI::ELoadOp::Load;   // only the first terrain may own the clear
        }

        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::TerrainRenderPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (Frame.Extracts.TerrainExtracts.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Terrain Render", tracy::Color::SeaGreen);

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            const ECS::FEntity Entity  = TerrainItem.Entity;
            if (TerrainItem.Resolution < 2 || TerrainItem.ChunkResolution < 2)
            {
                continue;
            }

            auto TerrainStateIt = TerrainGPUStates.find(TerrainItem.Entity);
            if (TerrainStateIt == TerrainGPUStates.end())
            {
                continue;
            }
            FTerrainGPUState& State = TerrainStateIt->second;
            if (!State.HeightmapTexture || !State.NormalTexture || !State.LayerWeightTexture)
            {
                continue;
            }
            if (!State.ChunkInfoBuffer || !State.MeshletInfoBuffer || !State.VisibleMeshletBuffer || !State.IndirectDrawBuffer)
            {
                continue;
            }
            if (State.AllocatedMeshletCount == 0u)
            {
                continue;
            }

            FShaderH VertexShader = TerrainItem.Shaders.VertexShader;
            FShaderH PixelShader  = TerrainItem.Shaders.PixelShader;
            if (!VertexShader || !PixelShader)
            {
                continue;
            }

            const uint32 Res = (uint32)TerrainItem.Resolution;

            const FMatrix4 WorldMat = TerrainItem.WorldMatrix;
            const FVector3 WorldOrigin = FVector3(WorldMat[3]);
            const float HalfSize = TerrainItem.TileWorldSize * 0.5f;

            const int32 QuadsPerChunk        = Math::Max(1, TerrainItem.ChunkResolution - 1);
            const int32 ChunksPerSide        = Math::Max(1, ((int32)Res - 1) / QuadsPerChunk);
            const int32 MeshletsPerChunkSide = (QuadsPerChunk + GTerrainMeshletQuads - 1) / GTerrainMeshletQuads;

            FTerrainRenderParams RenderParams{};
            RenderParams.OriginXZ             = FVector2(WorldOrigin.x - HalfSize, WorldOrigin.z - HalfSize);
            RenderParams.TileWorldSize        = TerrainItem.TileWorldSize;
            RenderParams.MaxHeight            = TerrainItem.MaxHeight;
            RenderParams.Resolution           = (int32)Res;
            RenderParams.ChunkResolution      = TerrainItem.ChunkResolution;
            RenderParams.ChunksPerSide        = ChunksPerSide;
            RenderParams.LayerCount           = TerrainItem.LayerCount;
            RenderParams.WorldOriginY         = FVector3(WorldOrigin.y, 0.0f, 0.0f);
            RenderParams.EntityID             = (uint32)Entity;
            RenderParams.MaterialIndex        = TerrainItem.MaterialIndex;
            RenderParams.MeshletsPerChunkSide = MeshletsPerChunkSide;
            RenderParams.MeshletQuadSide      = GTerrainMeshletQuads;

            const bool bHDRWasWritten = !DrawCommands.empty() || FrameFlags.bHasEnvironment;
            const FSceneImage& ColorRT  = GetNamedImage(ENamedImage::HDR);
            const FUIntVector2 Extent   = GetNamedImage(ENamedImage::HDR).GetExtent();

            RHI::FRenderAttachment Colors[2];
            uint32 NumColors = 1;
            Colors[0].Texture        = ColorRT.Texture;
            Colors[0].LoadOp         = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
            Colors[0].StoreOp        = RHI::EStoreOp::Store;
            #if USING(WITH_EDITOR)
            const FSceneImage& PickerRT = GetNamedImage(ENamedImage::Picker);
            // Deferred clears the picker when meshes exist; terrain-only scenes never ran it (early-out).
            Colors[1].Texture        = PickerRT.Texture;
            Colors[1].LoadOp         = DrawCommands.empty() ? RHI::ELoadOp::Clear : RHI::ELoadOp::Load;
            Colors[1].StoreOp        = RHI::EStoreOp::Store;
            NumColors = 2;
            #endif

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments               = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
            Pass.DepthAttachment.Texture        = GetNamedImage(ENamedImage::DepthAttachment).Texture;
            Pass.DepthAttachment.LoadOp         = RHI::ELoadOp::Load;
            Pass.DepthAttachment.StoreOp        = RHI::EStoreOp::Store;
            Pass.RenderArea                     = Extent;

            RHI::CmdBeginRenderPass(CL, Pass);
            SetViewportScissor(CL, Extent);

            RHI::FDepthStencilDesc DepthDesc;
            DepthDesc.DepthMode = RHI::EDepthFlags::Read;
            DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
            RHI::CmdSetDepthStencil(CL, (DepthDesc));
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

            FGraphicsPipelineKey Key;
            Key.VS          = VertexShader;
            Key.PS          = PixelShader;
            Key.DepthFormat = EFormat::D32;
            Key.ShadingFeatures = SF_DebugViews | SF_GTAO |
                                  (FrameFlags.bShadowMaskValid ? (uint32)SF_ShadowMask : 0u);
            Key.ColorTargets.push_back({ ColorRT.Desc.Format, {} });
            #if USING(WITH_EDITOR)
            Key.ColorTargets.push_back({ PickerRT.Desc.Format, {} });
            #endif
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FTerrainPushConstants Push{};
            Push.ParamsAddr        = RHI::CopyTransient(RenderParams);
            Push.Chunks            = { State.ChunkInfoBuffer, State.AllocatedChunkCount };
            Push.Meshlets          = { State.MeshletInfoBuffer, State.AllocatedMeshletCount };
            Push.Visible           = { State.VisibleMeshletBuffer, State.AllocatedMeshletCount };
            Push.HeightmapIndex    = (uint32)State.HeightmapTexture.GetResourceID();
            Push.NormalIndex       = (uint32)State.NormalTexture.GetResourceID();
            Push.LayerWeightsIndex = (uint32)State.LayerWeightTexture.GetResourceID();

            RHI::CmdDrawIndirect(CL, MakeArgs(Push), State.IndirectDrawBuffer, 1u, sizeof(RHI::FDrawIndirectArguments));

            RHI::CmdEndRenderPass(CL);
        }

        Barriers::RasterToRead(CL);
    }
}
