#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    bool FDefaultSceneRenderer::GTAOPass(RHI::FCmdListH CL, EGTAOStage Stage)
    {
        FFrameData& Frame = *RenderFrame;
        if (Frame.Geometry.DrawCommands.IsEmpty() || !IsGTAOEnabled())
        {
            return false;
        }

        static const FShaderH PrefilterCS = FShaderLibrary::Get("GTAOPrefilterDepth.slang");
        static const FShaderH MainCS      = FShaderLibrary::Get("GTAOMain.slang");
        static const FShaderH DenoiseCS   = FShaderLibrary::Get("GTAODenoise.slang");
        static const FShaderH UpsampleCS  = FShaderLibrary::Get("GTAOUpsample.slang");
        if (!PrefilterCS || !MainCS || !DenoiseCS || !UpsampleCS)
        {
            return false;
        }

        FGTAOParams Params;
        if (!MakeGTAOParams(Params))
        {
            return false;
        }

        SCENE_GPU_SCOPE(CL, Stage == EGTAOStage::Prefilter ? "GTAO Prefilter" : "GTAO");
        if (Stage == EGTAOStage::Prefilter)
        {
            return GTAOPrefilterDepth(CL, Params, PrefilterCS);
        }
        if (!GTAOMain(CL, Params, MainCS) || !GTAODenoise(CL, Params, DenoiseCS))
        {
            return false;
        }
        if (!Params.bHalfTrace)
        {
            return true;
        }

        SCENE_GPU_SCOPE(CL, "GTAO Upsample");
        // The final denoise pass wrote the ping-pong target its parity selects.
        const uint32 NumPasses = (uint32)Math::Max(Params.DenoisePasses, 1);
        const ENamedImage Denoised = ((NumPasses - 1u) & 1u) == 0u ? ENamedImage::GTAODenoise : ENamedImage::GTAO;
        return GTAOUpsample(CL, Params, UpsampleCS, GetNamedImage(Denoised), GetNamedImage(ENamedImage::GTAOBlur), 1u);
    }

    bool FDefaultSceneRenderer::MakeGTAOParams(FGTAOParams& P) const
    {
        const FSceneImage& Depth        = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& WorkingDepth = GetNamedImage(ENamedImage::GTAOWorkingDepth);
        const FSceneImage& TermA        = GetNamedImage(ENamedImage::GTAO);
        const FSceneImage& Output       = GetNamedImage(ENamedImage::GTAOBlur);

        P.DepthSlot = Depth.GetResourceID();
        if (P.DepthSlot < 0 || !WorkingDepth.IsValid() || WorkingDepth.GetNumMips() < GTAODepthMipLevels)
        {
            LOG_ERROR_ONCE("GTAO working depth pyramid is missing or too shallow; skipping the pass.");
            return false;
        }

        // The trace runs at the stage targets' size, half the view below ultra quality, and upsamples into Output.
        P.Width      = TermA.GetSizeX();
        P.Height     = TermA.GetSizeY();
        P.bHalfTrace = P.Width != Output.GetSizeX() || P.Height != Output.GetSizeY();

        float Radius           = 0.5f;
        float RadiusMultiplier = 1.457f;
        if (const CRendererSettings* Settings = GetDefault<CRendererSettings>())
        {
            Radius                     = Settings->GTAORadius;
            RadiusMultiplier           = Settings->GTAORadiusMultiplier;
            P.Intensity                = Settings->GTAOIntensity;
            P.FinalValuePower          = Settings->GTAOPower;
            P.FalloffRange             = Settings->GTAOFalloffRange;
            P.SampleDistributionPower  = Settings->GTAOSampleDistributionPower;
            P.ThinOccluderCompensation = Settings->GTAOThinOccluderCompensation;
            P.DepthMipSamplingOffset   = Settings->GTAODepthMipSamplingOffset;
            P.QualityLevel             = Math::Clamp(Settings->GTAOQualityLevel, 0, 3);
            P.DenoisePasses            = Math::Clamp(Settings->GTAODenoisePasses, 0, 3);
        }

        // Every heuristic in the reference is tuned against this pre-multiplied radius, never the raw one.
        P.EffectRadius = Radius * RadiusMultiplier;
        return true;
    }

    bool FDefaultSceneRenderer::GTAOPrefilterDepth(RHI::FCmdListH CL, const FGTAOParams& P, FShaderH PrefilterCS)
    {
        SCENE_GPU_SCOPE(CL, "GTAO Prefilter Depth");

        const FSceneImage& WorkingDepth = GetNamedImage(ENamedImage::GTAOWorkingDepth);

        struct FPrefilterConstants
        {
            uint32 ViewportSize[2];
            uint32 SrcDepthIndex;
            uint32 MipUAV[GTAODepthMipLevels];
            float  EffectRadius;
            float  EffectFalloffRange;
            uint32 SrcScale;
        } PC = {};

        PC.SrcScale           = P.bHalfTrace ? 2u : 1u;
        PC.ViewportSize[0]    = P.Width;
        PC.ViewportSize[1]    = P.Height;
        PC.SrcDepthIndex      = (uint32)P.DepthSlot;
        PC.EffectRadius       = P.EffectRadius;
        PC.EffectFalloffRange = P.FalloffRange;

        for (uint32 Mip = 0; Mip < GTAODepthMipLevels; ++Mip)
        {
            const int32 Slot = WorkingDepth.GetMipUAVIndex(Mip);
            if (Slot < 0)
            {
                LOG_ERROR_ONCE("GTAO working depth mip {} has no storage heap slot; skipping the pass.", Mip);
                return false;
            }
            PC.MipUAV[Mip] = (uint32)Slot;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(PrefilterCS));

        // One group covers a 16x16 tile, which reduces to exactly one texel of the last mip.
        constexpr uint32 PrefilterTile = 16u;
        RHI::CmdDispatch(CL, MakeArgs(PC),
            RenderUtils::GetGroupCount(P.Width, PrefilterTile),
            RenderUtils::GetGroupCount(P.Height, PrefilterTile), 1);

        RHI::Barriers::ComputeToCompute(CL);
        return true;
    }

    bool FDefaultSceneRenderer::GTAOMain(RHI::FCmdListH CL, const FGTAOParams& P, FShaderH MainCS)
    {
        SCENE_GPU_SCOPE(CL, "GTAO Main");

        const FSceneImage& WorkingDepth = GetNamedImage(ENamedImage::GTAOWorkingDepth);
        const FSceneImage& Edges        = GetNamedImage(ENamedImage::GTAOEdges);
        const FSceneImage& TermA        = GetNamedImage(ENamedImage::GTAO);

        struct FMainConstants
        {
            uint32 ViewportSize[2];
            uint32 WorkingDepthIndex;
            uint32 AOTermUAV;
            uint32 EdgesUAV;
            uint32 SliceCount;
            uint32 StepsPerSlice;
            uint32 NoiseIndex;
            float  EffectRadius;
            float  EffectFalloffRange;
            float  SampleDistributionPower;
            float  ThinOccluderCompensation;
            float  FinalValuePower;
            float  DepthMIPSamplingOffset;
        } PC = {};

        const int32 WorkingDepthSlot = WorkingDepth.GetResourceID();
        const int32 TermSlot         = TermA.GetMipUAVIndex(0);
        const int32 EdgesSlot        = Edges.GetMipUAVIndex(0);
        if (WorkingDepthSlot < 0 || TermSlot < 0 || EdgesSlot < 0)
        {
            LOG_ERROR_ONCE("GTAO main pass is missing a heap slot; skipping the pass.");
            return false;
        }

        PC.ViewportSize[0]          = P.Width;
        PC.ViewportSize[1]          = P.Height;
        PC.WorkingDepthIndex        = (uint32)WorkingDepthSlot;
        PC.AOTermUAV                = (uint32)TermSlot;
        PC.EdgesUAV                 = (uint32)EdgesSlot;
        PC.SliceCount               = GGTAOSliceCounts[P.QualityLevel];
        PC.StepsPerSlice            = GGTAOStepsPerSlice[P.QualityLevel];
        PC.NoiseIndex               = GetScreenSpaceNoiseIndex(*CurrentView);
        PC.EffectRadius             = P.EffectRadius;
        PC.EffectFalloffRange       = P.FalloffRange;
        PC.SampleDistributionPower  = P.SampleDistributionPower;
        PC.ThinOccluderCompensation = P.ThinOccluderCompensation;
        PC.FinalValuePower          = P.FinalValuePower;
        PC.DepthMIPSamplingOffset   = P.DepthMipSamplingOffset;

        DispatchCompute(CL, MainCS, PC,
            RenderUtils::GetGroupCount(P.Width, 8u),
            RenderUtils::GetGroupCount(P.Height, 8u), 1);

        RHI::Barriers::ComputeToCompute(CL);
        return true;
    }

    bool FDefaultSceneRenderer::GTAODenoise(RHI::FCmdListH CL, const FGTAOParams& P, FShaderH DenoiseCS)
    {
        SCENE_GPU_SCOPE(CL, "GTAO Denoise");

        const FSceneImage& Edges        = GetNamedImage(ENamedImage::GTAOEdges);
        const FSceneImage& TermA        = GetNamedImage(ENamedImage::GTAO);
        const FSceneImage& TermB        = GetNamedImage(ENamedImage::GTAODenoise);
        const FSceneImage& Output       = GetNamedImage(ENamedImage::GTAOBlur);

        struct FDenoiseConstants
        {
            uint32 ViewportSize[2];
            uint32 AOTermIndex;
            uint32 EdgesIndex;
            uint32 OutputUAV;
            uint32 FinalApply;
            float  DenoiseBlurBeta;
            float  Intensity;
        } PC = {};

        const int32 EdgesSlot = Edges.GetResourceID();
        if (EdgesSlot < 0)
        {
            LOG_ERROR_ONCE("GTAO edge texture has no sampled heap slot; skipping the denoise.");
            return false;
        }

        PC.ViewportSize[0] = P.Width;
        PC.ViewportSize[1] = P.Height;
        PC.EdgesIndex      = (uint32)EdgesSlot;
        PC.DenoiseBlurBeta = (P.DenoisePasses == 0) ? GGTAODenoiseDisabledBeta : GGTAODenoiseBeta;
        PC.Intensity       = P.Intensity;

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(DenoiseCS));

        // A disabled denoise still runs one pass, because the final apply is what undoes the packing scale.
        const uint32 NumPasses = (uint32)Math::Max(P.DenoisePasses, 1);
        for (uint32 PassIndex = 0; PassIndex < NumPasses; ++PassIndex)
        {
            const bool bFinal = (PassIndex + 1u) == NumPasses;
            const bool bEven  = (PassIndex & 1u) == 0u;

            const FSceneImage& Src = bEven ? TermA : TermB;
            const FSceneImage& Dst = (bFinal && !P.bHalfTrace) ? Output : (bEven ? TermB : TermA);

            const int32 SrcSlot = Src.GetResourceID();
            const int32 DstSlot = Dst.GetMipUAVIndex(0);
            if (SrcSlot < 0 || DstSlot < 0)
            {
                LOG_ERROR_ONCE("GTAO denoise pass {} is missing a heap slot; stopping the chain.", PassIndex);
                return false;
            }

            PC.AOTermIndex = (uint32)SrcSlot;
            PC.OutputUAV   = (uint32)DstSlot;
            PC.FinalApply  = bFinal ? 1u : 0u;

            // Each thread denoises two horizontal pixels, so a group spans 16 across and 8 down.
            RHI::CmdDispatch(CL, MakeArgs(PC),
                RenderUtils::GetGroupCount(P.Width, 16u),
                RenderUtils::GetGroupCount(P.Height, 8u), 1);

            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }
        return true;
    }

    bool FDefaultSceneRenderer::GTAOUpsample(RHI::FCmdListH CL, const FGTAOParams& P, FShaderH UpsampleCS,
        const FSceneImage& Source, const FSceneImage& Output, uint32 DepthScale)
    {
        const FSceneImage& WorkingDepth = GetNamedImage(ENamedImage::GTAOWorkingDepth);

        struct FUpsampleConstants
        {
            uint32 FullSize[2];
            uint32 HalfSize[2];
            uint32 AOIndex;
            uint32 HalfDepthIndex;
            uint32 FullDepthIndex;
            uint32 OutputUAV;
            uint32 DepthScale;
        } PC = {};

        const int32 AOSlot        = Source.GetResourceID();
        const int32 HalfDepthSlot = WorkingDepth.GetResourceID();
        const int32 OutputSlot    = Output.GetMipUAVIndex(0);
        if (AOSlot < 0 || HalfDepthSlot < 0 || OutputSlot < 0)
        {
            LOG_ERROR_ONCE("Bilateral upsample is missing a heap slot; skipping it.");
            return false;
        }

        PC.FullSize[0]    = Output.GetSizeX();
        PC.FullSize[1]    = Output.GetSizeY();
        PC.HalfSize[0]    = Source.GetSizeX();
        PC.HalfSize[1]    = Source.GetSizeY();
        PC.AOIndex        = (uint32)AOSlot;
        PC.HalfDepthIndex = (uint32)HalfDepthSlot;
        PC.FullDepthIndex = (uint32)P.DepthSlot;
        PC.OutputUAV      = (uint32)OutputSlot;
        PC.DepthScale     = DepthScale;

        DispatchCompute(CL, UpsampleCS, PC,
            RenderUtils::GetGroupCount(PC.FullSize[0], 8u),
            RenderUtils::GetGroupCount(PC.FullSize[1], 8u), 1);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        return true;
    }

    namespace
    {
        // Denser than GTAO's tables, since only a temporal upscaler accumulates the bounce across frames to hide the noise.
        constexpr uint32 kScreenSpaceGISlices        = 4u;
        constexpr uint32 kScreenSpaceGIStepsPerSlice = 8u;
    }

    bool FDefaultSceneRenderer::WantsScreenSpaceGI(const FSceneView& View) const
    {
        return IsGTAOEnabled() && GetDefault<CRendererSettings>()->bScreenSpaceGI && View.bIsPrimary;
    }

    bool FDefaultSceneRenderer::IsScreenSpaceGITraced() const
    {
        return WantsScreenSpaceGI(*CurrentView) && CurrentView->bScreenSpaceGIHistoryValid && CameraCutFramesLeft == 0u
            && !RenderFrame->Geometry.DrawCommands.IsEmpty();
    }

    void FDefaultSceneRenderer::ScreenSpaceGIPass(RHI::FCmdListH CL)
    {
        static const FString  BlurDefine = "SSGI_BLUR";
        static const FShaderH TraceCS    = FShaderLibrary::Get("ScreenSpaceGI.slang");
        static const FShaderH BlurCS     = FShaderLibrary::Get("ScreenSpaceGI.slang", TSpan<const FString>(&BlurDefine, 1));
        static const FShaderH UpsampleCS = FShaderLibrary::Get("GTAOUpsample.slang");
        FGTAOParams P;
        if (!TraceCS || !BlurCS || !UpsampleCS || !MakeGTAOParams(P))
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Screen Space GI", tracy::Color::Orange);

        const FSceneImage& WorkingDepth = GetNamedImage(ENamedImage::GTAOWorkingDepth);
        const FSceneImage& Trace        = GetNamedImage(ENamedImage::ScreenSpaceGITrace);
        const FSceneImage& Blur         = GetNamedImage(ENamedImage::ScreenSpaceGIBlur);
        const FSceneImage& History      = GetNamedImage(ENamedImage::ScreenSpaceGIHistory);

        const int32 WorkingDepthSlot = WorkingDepth.GetResourceID();
        const int32 HistorySlot      = History.GetResourceID();
        const int32 TraceSlot        = Trace.GetResourceID();
        const int32 TraceUAV         = Trace.GetMipUAVIndex(0);
        const int32 BlurSlot         = Blur.GetResourceID();
        const int32 BlurUAV          = Blur.GetMipUAVIndex(0);
        if (WorkingDepthSlot < 0 || HistorySlot < 0 || TraceSlot < 0 || TraceUAV < 0 || BlurSlot < 0 || BlurUAV < 0)
        {
            LOG_ERROR_ONCE("Screen space GI is missing a heap slot; skipping the pass.");
            return;
        }

        struct FTraceConstants
        {
            FMatrix4 HistoryViewProjection;
            uint32   OutputSize[2];
            uint32   WorkingDepthSize[2];
            uint32   WorkingDepthIndex;
            uint32   SceneColorHistoryIndex;
            uint32   OutputUAV;
            uint32   DepthScale;
            uint32   SliceCount;
            uint32   StepsPerSlice;
            float    EffectRadius;
            float    EffectFalloffRange;
            float    SampleDistributionPower;
            float    ThinOccluderCompensation;
            float    DepthMIPSamplingOffset;
            uint32   NoiseIndex;
        } PC = {};

        // The working depth is already half size below ultra quality, and full size steps two texels per output.
        const uint32 DepthScale = P.bHalfTrace ? 1u : 2u;

        PC.HistoryViewProjection    = CurrentView->ScreenSpaceGIHistoryViewProjection;
        PC.OutputSize[0]            = Trace.GetSizeX();
        PC.OutputSize[1]            = Trace.GetSizeY();
        PC.WorkingDepthSize[0]      = P.Width;
        PC.WorkingDepthSize[1]      = P.Height;
        PC.WorkingDepthIndex        = (uint32)WorkingDepthSlot;
        PC.SceneColorHistoryIndex   = (uint32)HistorySlot;
        PC.OutputUAV                = (uint32)TraceUAV;
        PC.DepthScale               = DepthScale;
        PC.SliceCount               = kScreenSpaceGISlices;
        PC.StepsPerSlice            = kScreenSpaceGIStepsPerSlice;
        PC.EffectRadius             = Math::Max(GetDefault<CRendererSettings>()->ScreenSpaceGIRadius, 0.01f);
        PC.EffectFalloffRange       = P.FalloffRange;
        PC.SampleDistributionPower  = P.SampleDistributionPower;
        PC.ThinOccluderCompensation = P.ThinOccluderCompensation;
        PC.DepthMIPSamplingOffset   = P.DepthMipSamplingOffset;
        PC.NoiseIndex               = GetScreenSpaceNoiseIndex(*CurrentView);

        DispatchCompute(CL, TraceCS, PC,
            RenderUtils::GetGroupCount(PC.OutputSize[0], 8u),
            RenderUtils::GetGroupCount(PC.OutputSize[1], 8u), 1);
        RHI::Barriers::ComputeToCompute(CL);

        struct FBlurConstants
        {
            uint32 Size[2];
            uint32 WorkingDepthSize[2];
            int32  Direction[2];
            uint32 WorkingDepthIndex;
            uint32 SourceIndex;
            uint32 OutputUAV;
            uint32 DepthScale;
        } BlurPC = {};

        BlurPC.Size[0]             = PC.OutputSize[0];
        BlurPC.Size[1]             = PC.OutputSize[1];
        BlurPC.WorkingDepthSize[0] = P.Width;
        BlurPC.WorkingDepthSize[1] = P.Height;
        BlurPC.WorkingDepthIndex   = (uint32)WorkingDepthSlot;
        BlurPC.DepthScale          = DepthScale;

        // Across from the trace into the blur target, then down back into the trace.
        for (const bool bVertical : { false, true })
        {
            BlurPC.Direction[0] = bVertical ? 0 : 1;
            BlurPC.Direction[1] = bVertical ? 1 : 0;
            BlurPC.SourceIndex  = (uint32)(bVertical ? BlurSlot : TraceSlot);
            BlurPC.OutputUAV    = (uint32)(bVertical ? TraceUAV : BlurUAV);

            DispatchCompute(CL, BlurCS, BlurPC,
                RenderUtils::GetGroupCount(BlurPC.Size[0], 8u),
                RenderUtils::GetGroupCount(BlurPC.Size[1], 8u), 1);
            RHI::Barriers::ComputeToCompute(CL);
        }

        GTAOUpsample(CL, P, UpsampleCS, Trace, GetNamedImage(ENamedImage::ScreenSpaceGI), DepthScale);
    }

    void FDefaultSceneRenderer::CaptureScreenSpaceGIHistory(RHI::FCmdListH CL)
    {
        // A debug view has replaced the lit color in HDR, so the last lit frame stays the history.
        const FSceneImage& History = GetNamedImage(ENamedImage::ScreenSpaceGIHistory);
        if (!History.IsValid() || RenderFrame->SceneGlobalData.CullData.DebugMode != (uint32)ERenderSceneDebugFlags::None)
        {
            return;
        }

        RHI::Barriers::SceneToTransfer(CL);
        RHI::CmdBlitTexture(CL, GetNamedImage(ENamedImage::HDR).Texture, RHI::FTextureSlice{}, History.Texture, RHI::FTextureSlice{});
        // The next HDR writers wait out the read, and the next frame's trace the write.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute | RHI::EStageFlags::RasterColorOut);

        CurrentView->ScreenSpaceGIHistoryViewProjection = CurrentView->PendingViewProjection;
        CurrentView->bScreenSpaceGIHistoryValid         = true;
    }

    namespace ScreenSpaceInternal
    {
        // Shared by SSRHiZ.slang and SSRFilter.slang, which both build one level of a chain from the level below.
        struct FChainLevelPushConstants
        {
            uint32 ChainIndex;
            uint32 SourceMip;
            uint32 DestUAV;
            uint32 DestMip;

            uint32 SourceW;
            uint32 SourceH;
            uint32 DestW;
            uint32 DestH;
        };
        static_assert(sizeof(FChainLevelPushConstants) == 32, "FChainLevelPushConstants must match FSSRChainLevelArgs.");
    }

    void FDefaultSceneRenderer::ScreenSpaceReflectionsPass(RHI::FCmdListH CL)
    {
        const CRendererSettings* RS = GetDefault<CRendererSettings>();
        if (RS == nullptr || !RS->bScreenSpaceReflections || RS->SSRIntensity <= 0.0f)
        {
            return;
        }

        if (RenderFrame->Geometry.DrawCommands.IsEmpty())
        {
            return;
        }

        static const FShaderH DownsampleCS = FShaderLibrary::Get("SSRDownsample.slang");
        static const FShaderH PyramidCS    = FShaderLibrary::Get("SSRHiZ.slang");
        static const FShaderH TraceCS      = FShaderLibrary::Get("ScreenSpaceReflections.slang");
        static const FShaderH FilterCS     = FShaderLibrary::Get("SSRFilter.slang");
        static const FShaderH ResolveCS    = FShaderLibrary::Get("SSRResolve.slang");
        if (!DownsampleCS || !PyramidCS || !TraceCS || !FilterCS || !ResolveCS)
        {
            return;
        }

        const FSceneImage& Chain    = GetNamedImage(ENamedImage::SSRTrace);
        const FSceneImage& Pyramid  = GetNamedImage(ENamedImage::SSRPyramid);
        const FSceneImage& Surface  = GetNamedImage(ENamedImage::SSRSurface);
        const FSceneImage& MipLevel = GetNamedImage(ENamedImage::SSRMipLevel);
        if (!Chain.IsValid() || !Pyramid.IsValid() || !Surface.IsValid() || !MipLevel.IsValid())
        {
            return;
        }

        // Only GBuffer pixels reflect, and the slot table that identifies them exists once classify has run.
        if (MaterialClassifyLayout.NumSlots == 0u)
        {
            return;
        }

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);
        const int32 HDRUAV = HDR.GetMipUAVIndex(0);
        if (HDRUAV < 0)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Screen Space Reflections");

        FSSRParams P;
        P.ScreenW        = HDR.GetSizeX();
        P.ScreenH        = HDR.GetSizeY();
        P.TraceW         = Chain.GetSizeX();
        P.TraceH         = Chain.GetSizeY();
        P.Scale          = GetSSRTraceScale();
        P.MipCount       = Math::Min(Chain.GetNumMips(), Pyramid.GetNumMips());
        P.HDRUAV         = (uint32)HDRUAV;
        P.DepthTolerance = Math::Max(RS->SSRDepthTolerance, 0.01f);
        P.FadeIn         = Math::Max(RS->SSRFadeIn, 0.0f);
        P.FadeOut        = Math::Max(RS->SSRFadeOut, 0.0f);
        P.MaxRoughness   = Math::Clamp(RS->SSRMaxRoughness, 0.0f, 1.0f);
        P.Intensity      = Math::Clamp(RS->SSRIntensity, 0.0f, 1.0f);

        // Lighting and the forward passes before this wrote HDR, which the trace samples as scene color.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute | RHI::EStageFlags::RasterColorOut, RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        SSRDownsample(CL, P, DownsampleCS);
        SSRBuildChain(CL, P, PyramidCS, Pyramid);
        SSRTrace(CL, P, TraceCS);
        SSRBuildChain(CL, P, FilterCS, Chain);
        SSRResolve(CL, P, ResolveCS);
    }

    void FDefaultSceneRenderer::SSRDownsample(RHI::FCmdListH CL, const FSSRParams& P, FShaderH DownsampleCS)
    {
        struct FDownsamplePushConstants
        {
            uint32 GBufferAIndex;
            uint32 GBufferBIndex;
            uint32 GBufferCIndex;
            uint32 GBufferDIndex;

            uint32 DepthIndex;
            uint32 SlotImageIndex;
            uint32 OutDepthUAV;
            uint32 OutSurfaceUAV;

            uint32 ScreenW;
            uint32 ScreenH;
            uint32 TraceW;
            uint32 TraceH;

            uint32 Scale;
            uint32 _Pad0;
            uint32 _Pad1;
            uint32 _Pad2;
        } PC = {};
        static_assert(sizeof(FDownsamplePushConstants) == 64, "FDownsamplePushConstants must match SSRDownsample.slang FSSRDownsampleArgs.");

        PC.GBufferAIndex  = (uint32)GetNamedImage(ENamedImage::GBufferA).GetResourceID();
        PC.GBufferBIndex  = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        PC.GBufferCIndex  = (uint32)GetNamedImage(ENamedImage::GBufferC).GetResourceID();
        PC.GBufferDIndex  = (uint32)GetNamedImage(ENamedImage::GBufferD).GetResourceID();
        PC.DepthIndex     = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.SlotImageIndex = (uint32)GetNamedImage(ENamedImage::MaterialSlot).GetResourceID();
        PC.OutDepthUAV    = (uint32)GetNamedImage(ENamedImage::SSRPyramid).GetMipUAVIndex(0);
        PC.OutSurfaceUAV  = (uint32)GetNamedImage(ENamedImage::SSRSurface).GetMipUAVIndex(0);
        PC.ScreenW        = P.ScreenW;
        PC.ScreenH        = P.ScreenH;
        PC.TraceW         = P.TraceW;
        PC.TraceH         = P.TraceH;
        PC.Scale          = P.Scale;

        DispatchCompute(CL, DownsampleCS, PC, RenderUtils::GetGroupCount(P.TraceW, 8u), RenderUtils::GetGroupCount(P.TraceH, 8u), 1u);
        RHI::Barriers::ComputeToCompute(CL);
    }

    void FDefaultSceneRenderer::SSRBuildChain(RHI::FCmdListH CL, const FSSRParams& P, FShaderH LevelCS, const FSceneImage& ChainImage)
    {
        for (uint32 Mip = 1; Mip < P.MipCount; ++Mip)
        {
            ScreenSpaceInternal::FChainLevelPushConstants PC = {};
            PC.ChainIndex = (uint32)ChainImage.GetResourceID();
            PC.SourceMip  = Mip - 1u;
            PC.DestUAV    = (uint32)ChainImage.GetMipUAVIndex(Mip);
            PC.DestMip    = Mip;
            PC.SourceW    = Math::Max(P.TraceW >> (Mip - 1u), 1u);
            PC.SourceH    = Math::Max(P.TraceH >> (Mip - 1u), 1u);
            PC.DestW      = Math::Max(P.TraceW >> Mip, 1u);
            PC.DestH      = Math::Max(P.TraceH >> Mip, 1u);

            DispatchCompute(CL, LevelCS, PC, RenderUtils::GetGroupCount(PC.DestW, 8u), RenderUtils::GetGroupCount(PC.DestH, 8u), 1u);
            RHI::Barriers::ComputeToCompute(CL);
        }
    }

    void FDefaultSceneRenderer::SSRTrace(RHI::FCmdListH CL, const FSSRParams& P, FShaderH TraceCS)
    {
        struct FTracePushConstants
        {
            uint32 PyramidIndex;
            uint32 SurfaceIndex;
            uint32 SceneColorIndex;
            uint32 OutColorUAV;

            uint32 OutMipUAV;
            uint32 TraceW;
            uint32 TraceH;
            uint32 MipCount;

            uint32 MaxIterations;
            float  DepthTolerance;
            float  FadeIn;
            float  FadeOut;

            float  MaxRoughness;
            uint32 DepthIndex;
            uint32 ScreenW;
            uint32 ScreenH;

            uint32 Scale;
            uint32 _Pad0;
            uint32 _Pad1;
            uint32 _Pad2;
        } PC = {};
        static_assert(sizeof(FTracePushConstants) == 80, "FTracePushConstants must match ScreenSpaceReflections.slang FSSRTraceArgs.");

        PC.PyramidIndex    = (uint32)GetNamedImage(ENamedImage::SSRPyramid).GetResourceID();
        PC.SurfaceIndex    = (uint32)GetNamedImage(ENamedImage::SSRSurface).GetResourceID();
        PC.SceneColorIndex = (uint32)GetNamedImage(ENamedImage::HDR).GetResourceID();
        PC.OutColorUAV     = (uint32)GetNamedImage(ENamedImage::SSRTrace).GetMipUAVIndex(0);
        PC.OutMipUAV       = (uint32)GetNamedImage(ENamedImage::SSRMipLevel).GetMipUAVIndex(0);
        PC.TraceW          = P.TraceW;
        PC.TraceH          = P.TraceH;
        PC.MipCount        = P.MipCount;
        PC.MaxIterations   = GetSSRMaxIterations();
        PC.DepthTolerance  = P.DepthTolerance;
        PC.FadeIn          = P.FadeIn;
        PC.FadeOut         = P.FadeOut;
        PC.MaxRoughness    = P.MaxRoughness;
        PC.DepthIndex      = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.ScreenW         = P.ScreenW;
        PC.ScreenH         = P.ScreenH;
        PC.Scale           = P.Scale;

        DispatchCompute(CL, TraceCS, PC, RenderUtils::GetGroupCount(P.TraceW, 8u), RenderUtils::GetGroupCount(P.TraceH, 8u), 1u);
        RHI::Barriers::ComputeToCompute(CL);
    }

    void FDefaultSceneRenderer::SSRResolve(RHI::FCmdListH CL, const FSSRParams& P, FShaderH ResolveCS)
    {
        struct FResolvePushConstants
        {
            uint32 GBufferAIndex;
            uint32 GBufferBIndex;
            uint32 GBufferCIndex;
            uint32 GBufferDIndex;

            uint32 DepthIndex;
            uint32 SlotImageIndex;
            uint32 PyramidIndex;
            uint32 SurfaceIndex;

            uint32 ChainIndex;
            uint32 MipLevelIndex;
            uint32 HDRUAV;
            uint32 Scale;

            uint32 ScreenW;
            uint32 ScreenH;
            uint32 TraceW;
            uint32 TraceH;

            float  Intensity;
            float  MaxRoughness;
            uint32 _Pad1;
            uint32 _Pad2;
        } PC = {};
        static_assert(sizeof(FResolvePushConstants) == 80, "FResolvePushConstants must match SSRResolve.slang FSSRResolveArgs.");

        PC.GBufferAIndex  = (uint32)GetNamedImage(ENamedImage::GBufferA).GetResourceID();
        PC.GBufferBIndex  = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        PC.GBufferCIndex  = (uint32)GetNamedImage(ENamedImage::GBufferC).GetResourceID();
        PC.GBufferDIndex  = (uint32)GetNamedImage(ENamedImage::GBufferD).GetResourceID();
        PC.DepthIndex     = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.SlotImageIndex = (uint32)GetNamedImage(ENamedImage::MaterialSlot).GetResourceID();
        PC.PyramidIndex   = (uint32)GetNamedImage(ENamedImage::SSRPyramid).GetResourceID();
        PC.SurfaceIndex   = (uint32)GetNamedImage(ENamedImage::SSRSurface).GetResourceID();
        PC.ChainIndex     = (uint32)GetNamedImage(ENamedImage::SSRTrace).GetResourceID();
        PC.MipLevelIndex  = (uint32)GetNamedImage(ENamedImage::SSRMipLevel).GetResourceID();
        PC.HDRUAV         = P.HDRUAV;
        PC.Scale          = P.Scale;
        PC.ScreenW        = P.ScreenW;
        PC.ScreenH        = P.ScreenH;
        PC.TraceW         = P.TraceW;
        PC.TraceH         = P.TraceH;
        PC.Intensity      = P.Intensity;
        PC.MaxRoughness   = P.MaxRoughness;

        DispatchCompute(CL, ResolveCS, PC, RenderUtils::GetGroupCount(P.ScreenW, 8u), RenderUtils::GetGroupCount(P.ScreenH, 8u), 1u);

        // The next HDR writer may be the composite or a translucent raster pass that blends without a barrier of its own.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute | RHI::EStageFlags::RasterColorOut,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorRead | RHI::EAccessFlags::ColorWrite);
    }
}
