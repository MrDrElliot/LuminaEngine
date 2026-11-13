#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
#if !defined(LE_SHIPPING)
    // Kept out of the lighting shader so the production path carries no debug branch.
    void FDefaultSceneRenderer::SceneDebugViewPass(RHI::FCmdListH CL)
    {
        if (MaterialClassifyLayout.NumSlots == 0u ||
            RenderFrame->SceneGlobalData.CullData.DebugMode == 0u)
        {
            return;
        }

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SceneDebugViewPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Scene Debug View");

        const FSceneImage& ColorImg = GetNamedImage(ENamedImage::HDR);
        const FUIntVector2 Extent   = FUIntVector2(MaterialClassifyLayout.ScreenW, MaterialClassifyLayout.ScreenH);

        RHI::FRenderAttachment Color;
        Color.Texture        = ColorImg.Texture;
        Color.LoadOp         = RHI::ELoadOp::Load;   // pixels the deferred lane does not own are discarded
        Color.StoreOp        = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.ColorTargets.Add({ ColorImg.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FSceneDebugViewPC
        {
            uint32 GBufferAIndex;
            uint32 GBufferBIndex;
            uint32 GBufferCIndex;
            uint32 GBufferDIndex;
            uint32 VisBufferIndex;
            uint32 DepthIndex;
            uint32 DrawListCount;
            uint32 _Pad0;
        } PC = {};
        static_assert(sizeof(FSceneDebugViewPC) == 32, "FSceneDebugViewPC must match SceneDebugViewPixel.slang FSceneDebugViewArgs.");
        PC.GBufferAIndex  = (uint32)GetNamedImage(ENamedImage::GBufferA).GetResourceID();
        PC.GBufferBIndex  = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        PC.GBufferCIndex  = (uint32)GetNamedImage(ENamedImage::GBufferC).GetResourceID();
        PC.GBufferDIndex  = (uint32)GetNamedImage(ENamedImage::GBufferD).GetResourceID();
        PC.VisBufferIndex = (uint32)GetNamedImage(ENamedImage::VisBuffer).GetResourceID();
        PC.DepthIndex     = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.DrawListCount  = DrawListCapacity;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
#endif

    namespace
    {
        RHI::FBlendDesc MakeOverlayAlphaBlend()
        {
            RHI::FBlendDesc AlphaBlend;
            AlphaBlend.bBlendEnable   = true;
            AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
            AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
            AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
            AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;
            return AlphaBlend;
        }
    }

    RHI::ELoadOp FDefaultSceneRenderer::TakeHDRLoadOp(bool bAfterParticles)
    {
        const FFrameData& Frame = *RenderFrame;

        // These write HDR before any overlay pass and decide their own clear.
        const bool bWrittenUpstream = !Frame.Geometry.DrawCommands.IsEmpty() || FrameFlags.bHasEnvironment
            || !Frame.Extracts.TerrainExtracts.IsEmpty() || bSceneColorClearedForCallbacks
            || (bAfterParticles && !Frame.Extracts.ParticleExtracts.IsEmpty());

        // Stamped by tick and view, so a new frame or the next view starts uncleared without a reset.
        const bool bWrittenThisView = HDRWrittenTick == OptionalImageTick && HDRWrittenView == CurrentView;
        HDRWrittenTick = OptionalImageTick;
        HDRWrittenView = CurrentView;

        return (bWrittenUpstream || bWrittenThisView) ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
    }

    void FDefaultSceneRenderer::BeginHDROverlayPass(RHI::FCmdListH CL, RHI::ELoadOp HDRLoadOp, bool bWithPicker,
                                                    const RHI::FBlendDesc& HDRBlend, FGraphicsPipelineKey& Key)
    {
        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);

        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture = HDR.Texture;
        Colors[0].LoadOp  = HDRLoadOp;
        Colors[0].StoreOp = RHI::EStoreOp::Store;

        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.Emplace(HDR.Desc.Format, HDRBlend);

        // Entity picking is editor-only; a packaged game binds no Picker MRT (SV_Target1 writes discard).
        #if USING(WITH_EDITOR)
        if (bWithPicker)
        {
            const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
            Colors[1].Texture = Picker.Texture;
            Colors[1].LoadOp  = RHI::ELoadOp::Load;
            Colors[1].StoreOp = RHI::EStoreOp::Store;
            NumColors = 2;
            Key.ColorTargets.Add({ Picker.Desc.Format, {} });
        }
        #else
        (void)bWithPicker;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
    }

    void FDefaultSceneRenderer::BillboardPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& BillboardInstances = Frame.Primitives.BillboardInstances;

        if (BillboardInstances.IsEmpty() || !FrameSettings.ShouldDrawBillboards())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Billboard Pass", tracy::Color::Red);

        static const FShaderH VertexShader = FShaderLibrary::Get("BillboardVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("BillboardPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        BeginHDROverlayPass(CL, TakeHDRLoadOp(/*bAfterParticles*/ true), /*bWithPicker*/ true, MakeOverlayAlphaBlend(), Key);
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::CmdDraw(CL, MakeArgs(), 6, (uint32)BillboardInstances.Num(), 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::WidgetPickerPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& WidgetInstances = Frame.Primitives.WidgetInstances;

        if (WidgetInstances.IsEmpty())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Widget Picker");

        static const FShaderH VertexShader = FShaderLibrary::Get("WidgetVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("WidgetPickerPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);

        RHI::FRenderAttachment Color;
        Color.Texture = Picker.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Picker.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Picker.GetExtent());

        // Reversed-Z, so GreaterOrEqual keeps fragments at or in front of scene depth.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencil(CL, (DepthDesc));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.Add({ Picker.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::CmdDraw(CL, MakeArgs(), 6, (uint32)WidgetInstances.Num(), 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::WidgetPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& WidgetInstances = Frame.Primitives.WidgetInstances;

        if (WidgetInstances.IsEmpty() || !CurrentView->Output.IsValid())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Widgets");

        static const FShaderH VertexShader = FShaderLibrary::Get("WidgetVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("WidgetPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = CurrentView->Output;
        const FSceneImage& Depth  = GetWidgetDepth(CL, Output.GetExtent());
        if (!Depth.IsValid())
        {
            return;
        }

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = Depth.Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());

        // Reversed-Z, so GreaterOrEqual keeps fragments in front and discards occluded ones.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencil(CL, (DepthDesc));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.Emplace(Output.Desc.Format, MakeOverlayAlphaBlend());
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::CmdDraw(CL, MakeArgs(), 6, (uint32)WidgetInstances.Num(), 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    // The widgets draw at display resolution, so an upscaled view needs its render-resolution depth resampled to match.
    const FSceneImage& FDefaultSceneRenderer::GetWidgetDepth(RHI::FCmdListH CL, const FUIntVector2& Extent)
    {
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);
        if (!SceneDepth.IsValid() || SceneDepth.GetExtent() == Extent)
        {
            RetireSceneImage(WidgetDepth);
            return SceneDepth;
        }

        if (!WidgetDepth.IsValid() || WidgetDepth.GetExtent() != Extent)
        {
            RetireSceneImage(WidgetDepth);

            RHI::FTextureDesc Desc;
            Desc.Type      = RHI::ETextureType::Tex2D;
            Desc.Dimension = FUIntVector3(Extent.x, Extent.y, 1);
            Desc.Format    = EFormat::D32;
            Desc.Usage     = RHI::EImageUsageFlags::DepthAttachment | RHI::EImageUsageFlags::TransferDst;
            WidgetDepth    = CreateSceneImage(Desc, /*bSampled*/ false);
            if (!WidgetDepth.IsValid())
            {
                return WidgetDepth;
            }
        }

        // Nearest, since a filtered depth would place occluder edges between two surfaces.
        Barriers::SceneToTransfer(CL);
        RHI::CmdBlitTexture(CL, SceneDepth.Texture, RHI::FTextureSlice{}, WidgetDepth.Texture, RHI::FTextureSlice{}, RHI::EFilter::Nearest);
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::FragmentTests, RHI::EAccessFlags::DepthStencilRead | RHI::EAccessFlags::DepthStencilWrite);
        return WidgetDepth;
    }

    void FDefaultSceneRenderer::SpritePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame     = *RenderFrame;
        const auto&       Instances = Frame.Primitives.SpriteInstances;
        const auto&       Batches   = Frame.Primitives.SpriteBatches;

        if (Instances.IsEmpty() || Batches.IsEmpty())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Sprites");

        static const FShaderH VertexShader = FShaderLibrary::Get("SpriteVert.slang");
        static const FShaderH PixelShader  = FShaderLibrary::Get("SpritePixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        // Blend only the color target; the Picker (uint id) must not blend.
        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        BeginHDROverlayPass(CL, TakeHDRLoadOp(/*bAfterParticles*/ true), /*bWithPicker*/ true, MakeOverlayAlphaBlend(), Key);
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        // Blended sprites must not occlude the ones sorted behind them, so depth is read but never written.
        RHI::FDepthStencilDesc DepthTested;
        DepthTested.DepthMode = RHI::EDepthFlags::Read;
        DepthTested.DepthTest = RHI::EOp::GreaterEqual;

        const RHI::FGPURange InstancesRange = RHI::CopyTransientArray(Instances.GetData(), Instances.Num());

        struct FSpritePushConstants
        {
            RHI::TGPUSpan<FGPUSprite> Instances;
            uint32 TextureIndex;
            uint32 Pad0;
        };
        static_assert(sizeof(FSpritePushConstants) == 24, "FSpritePushConstants must match SpriteCommon.slang.");

        bool bDepthStateSet  = false;
        bool bLastDepthTest  = false;
        bool bCullStateSet   = false;
        bool bLastDoubleSided = false;

        for (const FFrameData::FSpriteBatch& Batch : Batches)
        {
            if (!bDepthStateSet || bLastDepthTest != Batch.bDepthTest)
            {
                RHI::CmdSetDepthStencil(CL, (Batch.bDepthTest ? DepthTested : RHI::FDepthStencilDesc{}));
                bLastDepthTest = Batch.bDepthTest;
                bDepthStateSet = true;
            }

            if (!bCullStateSet || bLastDoubleSided != Batch.bDoubleSided)
            {
                RHI::CmdSetCullMode(CL, Batch.bDoubleSided ? RHI::ECullMode::None : RHI::ECullMode::Back);
                bLastDoubleSided = Batch.bDoubleSided;
                bCullStateSet    = true;
            }

            FSpritePushConstants PC = {};
            PC.Instances = InstancesRange;
            PC.TextureIndex  = Batch.TextureIndex;

            RHI::CmdDraw(CL, MakeArgs(PC), 6, Batch.Count, 0, Batch.FirstInstance);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::TextPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame   = *RenderFrame;
        const auto&       Glyphs  = Frame.Primitives.GlyphInstances;
        const auto&       Batches = Frame.Primitives.TextBatches;

        if (Glyphs.IsEmpty() || Batches.IsEmpty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Text Pass", tracy::Color::Yellow);

        static const FShaderH VertexShader = FShaderLibrary::Get("TextVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("TextPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        // Blend only the color target; the Picker (uint id) must not blend.
        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        BeginHDROverlayPass(CL, TakeHDRLoadOp(/*bAfterParticles*/ true), /*bWithPicker*/ true, MakeOverlayAlphaBlend(), Key);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::FDepthStencilDesc DepthTested;
        DepthTested.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthTested.DepthTest = RHI::EOp::GreaterEqual;

        // All glyphs across every batch share one transient array; batches index it via FirstInstance.
        const RHI::FGPURange GlyphsRange = RHI::CopyTransientArray(Glyphs.GetData(), Glyphs.Num());

        struct FTextPushConstants
        {
            RHI::TGPUSpan<FGPUGlyph> Glyphs;
            uint32 AtlasIndex;
            uint32 AtlasWidth;
            uint32 AtlasHeight;
            float  DistanceRange;
            uint32 ScreenWidth;   // 0 for world text (only the debug screen-space pass uses these)
            uint32 ScreenHeight;
        };
        static_assert(sizeof(FTextPushConstants) == 40, "FTextPushConstants must match TextCommon.slang.");

        auto DrawBatch = [&](const FFrameData::FTextBatch& Batch)
        {
            FTextPushConstants PC = {};
            PC.Glyphs        = GlyphsRange;
            PC.AtlasIndex    = Batch.AtlasIndex;
            PC.AtlasWidth    = Batch.AtlasWidth;
            PC.AtlasHeight   = Batch.AtlasHeight;
            PC.DistanceRange = Batch.DistanceRange;

            RHI::CmdDraw(CL, MakeArgs(PC), 6, Batch.Count, 0, Batch.FirstInstance);
        };

        // Depth-tested text first (sorts against the scene), then always-on-top text last so it stays on top.
        RHI::CmdSetDepthStencil(CL, (DepthTested));
        for (const FFrameData::FTextBatch& Batch : Batches)
        {
            if (Batch.bDepthTest)
            {
                DrawBatch(Batch);
            }
        }

        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        for (const FFrameData::FTextBatch& Batch : Batches)
        {
            if (!Batch.bDepthTest)
            {
                DrawBatch(Batch);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::DebugTextPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame  = *RenderFrame;
        const auto&       Glyphs = Frame.Primitives.DebugTextGlyphs;
        const auto&       Batch  = Frame.Primitives.DebugTextBatch;

        if (Glyphs.IsEmpty() || Batch.Count == 0 || !CurrentView->Output.IsValid())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Debug Text");

        static const FShaderH VertexShader = FShaderLibrary::Get("DebugTextVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("DebugTextPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = CurrentView->Output;

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.Emplace(Output.Desc.Format, MakeOverlayAlphaBlend());
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FTextPushConstants
        {
            RHI::TGPUSpan<FGPUGlyph> Glyphs;
            uint32 AtlasIndex;
            uint32 AtlasWidth;
            uint32 AtlasHeight;
            float  DistanceRange;
            uint32 ScreenWidth;
            uint32 ScreenHeight;
        };
        static_assert(sizeof(FTextPushConstants) == 40, "FTextPushConstants must match TextCommon.slang.");

        const uint32   ScreenW   = Output.GetSizeX();
        const uint32   ScreenH   = Output.GetSizeY();

        FTextPushConstants PC = {};
        PC.Glyphs        = RHI::CopyTransientArray(Glyphs.GetData(), Glyphs.Num());
        PC.AtlasIndex    = Batch.AtlasIndex;
        PC.AtlasWidth    = Batch.AtlasWidth;
        PC.AtlasHeight   = Batch.AtlasHeight;
        PC.DistanceRange = Batch.DistanceRange;
        PC.ScreenWidth   = ScreenW;
        PC.ScreenHeight  = ScreenH;

        RHI::CmdDraw(CL, MakeArgs(PC), 6, Batch.Count, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    namespace
    {
        struct FSimpleElementPassData { RHI::TGPUSpan<FSimpleElementVertex> Vertices; };
    }

    void FDefaultSceneRenderer::BatchedLineDraw(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& SimpleVertices     = Frame.Primitives.SimpleVertices;
        const auto& LineBatches        = Frame.Primitives.LineBatches;
        const auto& ImmediateRanges    = Frame.Primitives.ImmediateLines;

        const bool bHasBatched = !SimpleVertices.IsEmpty() && !LineBatches.IsEmpty();

        bool bHasImmediate = false;
        for (const FImmediateLineRenderer::FDrawRange& Range : ImmediateRanges)
        {
            bHasImmediate |= (Range.Vertices != 0 && Range.VertexCount > 0);
        }

        if (!bHasBatched && !bHasImmediate)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Batched Line Draw", tracy::Color::Red2);

        static const FShaderH VertexShader = FShaderLibrary::Get("SimpleElementVertex.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SimpleElementPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        // No input layout, since the VS pulls vertices from PassAddr by SV_VertexID.
        FGraphicsPipelineKey Key;
        Key.VS       = VertexShader;
        Key.PS       = PixelShader;
        Key.Topology = RHI::ETopology::LineList;
        BeginHDROverlayPass(CL, TakeHDRLoadOp(/*bAfterParticles*/ false), /*bWithPicker*/ false, RHI::FBlendDesc{}, Key);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        // Depth-tested lines occlude (reversed-Z Greater + depth write); X-ray lines draw on top.
        RHI::FDepthStencilDesc DepthTested;
        DepthTested.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthTested.DepthTest = RHI::EOp::Greater;

        // Re-set only when the depth mode changes between consecutive batches.
        int CurrentDepthMode = -1;

        if (bHasBatched)
        {
            // Vertices live in the transient ring for this submission; the VS reads them by device address.
            const FSimpleElementPassData VertsPass
            {
                RHI::CopyTransientArray(SimpleVertices.GetData(), SimpleVertices.Num())
            };
            const RHI::GPUPtr Args = MakeArgs(VertsPass);

            for (const FLineBatch& Batch : LineBatches)
            {
                const int DepthMode = Batch.bDepthTest ? 1 : 0;
                if (DepthMode != CurrentDepthMode)
                {
                    RHI::CmdSetDepthStencil(CL, (Batch.bDepthTest ? DepthTested : RHI::FDepthStencilDesc{}));
                    CurrentDepthMode = DepthMode;
                }
                RHI::CmdSetLineWidth(CL, Batch.Thickness);

                // FirstVertex feeds SV_VertexID so the VS indexes into the full vertex array.
                RHI::CmdDraw(CL, Args, Batch.VertexCount, 1, Batch.StartVertex, 0);
            }
        }

        if (bHasImmediate)
        {
            RHI::CmdSetLineWidth(CL, 1.0f);

            for (uint32 Channel = 0; Channel < FImmediateLineRenderer::NumChannels; ++Channel)
            {
                const FImmediateLineRenderer::FDrawRange& Range = ImmediateRanges[Channel];
                if (Range.Vertices == 0 || Range.VertexCount == 0)
                {
                    continue;
                }

                const bool bDepthTest = (Channel == FImmediateLineRenderer::DepthTested);
                const int  DepthMode  = bDepthTest ? 1 : 0;
                if (DepthMode != CurrentDepthMode)
                {
                    RHI::CmdSetDepthStencil(CL, (bDepthTest ? DepthTested : RHI::FDepthStencilDesc{}));
                    CurrentDepthMode = DepthMode;
                }

                const RHI::GPUPtr ImmediateArgs = MakeArgs(FSimpleElementPassData{
                    RHI::TGPUSpan<FSimpleElementVertex>::FromAddress(Range.Vertices, Range.VertexCount) });
                RHI::CmdDraw(CL, ImmediateArgs, Range.VertexCount, 1, 0, 0);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::BatchedTriangleDraw(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& SolidVertices = Frame.Primitives.SolidVertices;
        const auto& SolidBatches  = Frame.Primitives.SolidBatches;

        if (SolidVertices.IsEmpty() || SolidBatches.IsEmpty())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Batched Solid Tris");

        static const FShaderH VertexShader = FShaderLibrary::Get("SimpleElementVertex.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SimpleElementPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        bool bWritesDepth = false;
        for (const FSolidBatch& Batch : SolidBatches)
        {
            if (Batch.Mode == ESolidDrawMode::Opaque)
            {
                bWritesDepth = true;
                break;
            }
        }

        if (bWritesDepth)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::FragmentTests,
                RHI::EAccessFlags::DepthStencilRead | RHI::EAccessFlags::DepthStencilWrite);
        }

        // No input layout, since the VS pulls vertices from PassAddr by SV_VertexID.
        FGraphicsPipelineKey BlendedKey;
        BlendedKey.VS = VertexShader;
        BlendedKey.PS = PixelShader;
        BeginHDROverlayPass(CL, TakeHDRLoadOp(/*bAfterParticles*/ false), /*bWithPicker*/ false, MakeOverlayAlphaBlend(), BlendedKey);
        // Two-sided so the surface reads from any angle.
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey OpaqueKey = BlendedKey;
        OpaqueKey.ColorTargets[0].Blend = RHI::FBlendDesc{};

        RHI::FDepthStencilDesc TranslucentDepth;
        TranslucentDepth.DepthMode = RHI::EDepthFlags::Read;
        TranslucentDepth.DepthTest = RHI::EOp::Greater;

        RHI::FDepthStencilDesc OpaqueDepth;
        OpaqueDepth.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        OpaqueDepth.DepthTest = RHI::EOp::Greater;

        const FSimpleElementPassData VertsPass{ RHI::CopyTransientArray(SolidVertices.GetData(), SolidVertices.Num()) };
        const RHI::GPUPtr Args = MakeArgs(VertsPass);

        struct FModeGroup
        {
            ESolidDrawMode                  Mode;
            const FGraphicsPipelineKey*     Pipeline;
            const RHI::FDepthStencilDesc*   Depth;
        };

        const RHI::FDepthStencilDesc XRayDepth{};
        const FModeGroup Groups[] =
        {
            { ESolidDrawMode::Opaque,      &OpaqueKey,  &OpaqueDepth      },
            { ESolidDrawMode::Translucent, &BlendedKey, &TranslucentDepth },
            { ESolidDrawMode::XRay,        &BlendedKey, &XRayDepth        },
        };

        for (const FModeGroup& Group : Groups)
        {
            bool bStateBound = false;
            for (const FSolidBatch& Batch : SolidBatches)
            {
                if (Batch.Mode != Group.Mode)
                {
                    continue;
                }

                if (!bStateBound)
                {
                    RHI::CmdSetPipeline(CL, GetOrCreatePipeline(*Group.Pipeline));
                    RHI::CmdSetDepthStencil(CL, (*Group.Depth));
                    bStateBound = true;
                }

                RHI::CmdDraw(CL, Args, Batch.VertexCount, 1, Batch.StartVertex, 0);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
}
