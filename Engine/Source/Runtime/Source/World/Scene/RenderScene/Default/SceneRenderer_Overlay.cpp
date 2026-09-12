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

        LUMINA_PROFILE_SECTION_COLORED("Scene Debug View", tracy::Color::Magenta);
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
        Key.ColorTargets.push_back({ ColorImg.Desc.Format, {} });
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

    void FDefaultSceneRenderer::BillboardPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& BillboardInstances = Frame.Primitives.BillboardInstances;
        const auto& DrawCommands       = Frame.Geometry.DrawCommands;

        if (BillboardInstances.empty() || !FrameSettings.bDrawBillboards)
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

        const FSceneImage& HDR    = GetNamedImage(ENamedImage::HDR);

        const bool bHDRWasWritten = !DrawCommands.empty() || FrameFlags.bHasEnvironment
            || !Frame.Extracts.TerrainExtracts.empty() || !Frame.Primitives.SolidBatches.empty()
            || !Frame.Primitives.LineBatches.empty() || !Frame.Extracts.ParticleExtracts.empty();

        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture = HDR.Texture;
        Colors[0].LoadOp  = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Colors[0].StoreOp = RHI::EStoreOp::Store;
        // Entity picking is editor-only; a packaged game binds no Picker MRT (SV_Target1 writes discard).
        #if USING(WITH_EDITOR)
        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
        Colors[1].Texture = Picker.Texture;
        Colors[1].LoadOp  = RHI::ELoadOp::Load;
        Colors[1].StoreOp = RHI::EStoreOp::Store;
        NumColors = 2;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.push_back({ HDR.Desc.Format, AlphaBlend });
        #if USING(WITH_EDITOR)
        Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
        #endif
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::CmdDraw(CL, MakeArgs(), 6, (uint32)BillboardInstances.size(), 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::WidgetPickerPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& WidgetInstances = Frame.Primitives.WidgetInstances;

        if (WidgetInstances.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Widget Picker Pass", tracy::Color::Magenta);

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
        Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::CmdDraw(CL, MakeArgs(), 6, (uint32)WidgetInstances.size(), 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::WidgetPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& WidgetInstances = Frame.Primitives.WidgetInstances;

        if (WidgetInstances.empty() || !CurrentView->Output.IsValid())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Widget Pass", tracy::Color::Magenta);

        static const FShaderH VertexShader = FShaderLibrary::Get("WidgetVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("WidgetPixel.slang");
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
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
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

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.push_back({ Output.Desc.Format, AlphaBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::CmdDraw(CL, MakeArgs(), 6, (uint32)WidgetInstances.size(), 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::SpritePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame     = *RenderFrame;
        const auto&       Instances = Frame.Primitives.SpriteInstances;
        const auto&       Batches   = Frame.Primitives.SpriteBatches;

        if (Instances.empty() || Batches.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Sprite Pass", tracy::Color::Magenta);

        static const FShaderH VertexShader = FShaderLibrary::Get("SpriteVert.slang");
        static const FShaderH PixelShader  = FShaderLibrary::Get("SpritePixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);

        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture = HDR.Texture;
        Colors[0].LoadOp  = RHI::ELoadOp::Load;
        Colors[0].StoreOp = RHI::EStoreOp::Store;
        #if USING(WITH_EDITOR)
        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
        Colors[1].Texture = Picker.Texture;
        Colors[1].LoadOp  = RHI::ELoadOp::Load;
        Colors[1].StoreOp = RHI::EStoreOp::Store;
        NumColors = 2;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());

        // Blend only the color target; the Picker (uint id) must not blend.
        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.push_back({ HDR.Desc.Format, AlphaBlend });
        #if USING(WITH_EDITOR)
        Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
        #endif
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        // Blended sprites must not occlude the ones sorted behind them, so depth is read but never written.
        RHI::FDepthStencilDesc DepthTested;
        DepthTested.DepthMode = RHI::EDepthFlags::Read;
        DepthTested.DepthTest = RHI::EOp::GreaterEqual;

        const RHI::FGPURange InstancesRange = RHI::CopyTransientArray(Instances.data(), Instances.size());

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

        if (Glyphs.empty() || Batches.empty())
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

        const FSceneImage& HDR    = GetNamedImage(ENamedImage::HDR);

        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture = HDR.Texture;
        Colors[0].LoadOp  = RHI::ELoadOp::Load;
        Colors[0].StoreOp = RHI::EStoreOp::Store;
        #if USING(WITH_EDITOR)
        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
        Colors[1].Texture = Picker.Texture;
        Colors[1].LoadOp  = RHI::ELoadOp::Load;
        Colors[1].StoreOp = RHI::EStoreOp::Store;
        NumColors = 2;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // Blend only the color target; the Picker (uint id) must not blend.
        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.push_back({ HDR.Desc.Format, AlphaBlend });
        #if USING(WITH_EDITOR)
        Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
        #endif
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::FDepthStencilDesc DepthTested;
        DepthTested.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthTested.DepthTest = RHI::EOp::GreaterEqual;

        // All glyphs across every batch share one transient array; batches index it via FirstInstance.
        const RHI::FGPURange GlyphsRange = RHI::CopyTransientArray(Glyphs.data(), Glyphs.size());

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

        if (Glyphs.empty() || Batch.Count == 0 || !CurrentView->Output.IsValid())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Debug Text Pass", tracy::Color::Yellow);

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

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, AlphaBlend });
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

        const FUIntVector4 PanelSize = Frame.SceneGlobalData.ScreenSize;
        const uint32   ScreenW   = PanelSize.x > 1u ? PanelSize.x : Output.GetSizeX();
        const uint32   ScreenH   = PanelSize.y > 1u ? PanelSize.y : Output.GetSizeY();

        FTextPushConstants PC = {};
        PC.Glyphs        = RHI::CopyTransientArray(Glyphs.data(), Glyphs.size());
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
        const auto& DrawCommands       = Frame.Geometry.DrawCommands;

        const bool bHasBatched = !SimpleVertices.empty() && !LineBatches.empty();

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

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);

        const bool bHDRWasWritten = !DrawCommands.empty() || FrameFlags.bHasEnvironment
            || !Frame.Extracts.TerrainExtracts.empty() || !Frame.Primitives.SolidBatches.empty();

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // No input layout, since the VS pulls vertices from PassAddr by SV_VertexID.
        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.Topology    = RHI::ETopology::LineList;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.push_back({ HDR.Desc.Format, {} });
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
                RHI::CopyTransientArray(SimpleVertices.data(), SimpleVertices.size())
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
        const auto& DrawCommands  = Frame.Geometry.DrawCommands;

        if (SolidVertices.empty() || SolidBatches.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Batched Triangle Draw", tracy::Color::Green2);

        static const FShaderH VertexShader = FShaderLibrary::Get("SimpleElementVertex.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SimpleElementPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);

        // First HDR writer in the frame clears and later ones load; base pass and terrain come first.
        const bool bHDRWasWritten = !DrawCommands.empty() || FrameFlags.bHasEnvironment || !Frame.Extracts.TerrainExtracts.empty();

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

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

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        // Two-sided so the surface reads from any angle.
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        // No input layout, since the VS pulls vertices from PassAddr by SV_VertexID.
        FGraphicsPipelineKey BlendedKey;
        BlendedKey.VS          = VertexShader;
        BlendedKey.PS          = PixelShader;
        BlendedKey.DepthFormat = EFormat::D32;
        BlendedKey.ColorTargets.push_back({ HDR.Desc.Format, AlphaBlend });

        FGraphicsPipelineKey OpaqueKey = BlendedKey;
        OpaqueKey.ColorTargets[0].Blend = RHI::FBlendDesc{};

        RHI::FDepthStencilDesc TranslucentDepth;
        TranslucentDepth.DepthMode = RHI::EDepthFlags::Read;
        TranslucentDepth.DepthTest = RHI::EOp::Greater;

        RHI::FDepthStencilDesc OpaqueDepth;
        OpaqueDepth.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        OpaqueDepth.DepthTest = RHI::EOp::Greater;

        const FSimpleElementPassData VertsPass{ RHI::CopyTransientArray(SolidVertices.data(), SolidVertices.size()) };
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
