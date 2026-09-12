#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace
    {
        struct FColorGradingConstants
        {
            float    Exposure;
            float    Contrast;
            float    Saturation;
            float    Gamma;

            float    WhiteTemp;
            float    WhiteTint;
            float    VignetteIntensity;
            float    VignetteSmoothness;

            float    VignetteRoundness;
            uint32   TonemapMode;
            float    Time;
            float    BloomIntensity;     // 0 disables the bloom composite path in the shader.

            FVector4 ColorFilter;

            FVector4 Shadows;
            FVector4 Midtones;
            FVector4 Highlights;
            FVector4 VignetteColor;

            // .rgb = bloom tint, .a = chromatic aberration intensity.
            FVector4 BloomTint;

            float    AutoExposureKey;    // middle-gray key; <= 0 disables auto-exposure.
            float    AutoExposureMinMul; // 2^MinEV clamp on the adapted multiplier.
            float    AutoExposureMaxMul; // 2^MaxEV clamp on the adapted multiplier.
            float    _PadAE;
        };
        static_assert(sizeof(FColorGradingConstants) == 160, "FColorGradingConstants layout must match ColorGrading.slang::FColorGradingConstants.");
        
        FColorGradingConstants MakeDefaultColorGrading(float Time)
        {
            FColorGradingConstants PC{};
            PC.Exposure           = 1.0f;
            PC.Contrast           = 1.0f;
            PC.Saturation         = 1.0f;
            PC.Gamma              = 1.0f;
            PC.WhiteTemp          = 0.0f;
            PC.WhiteTint          = 0.0f;
            PC.VignetteIntensity  = 0.0f;
            PC.VignetteSmoothness = 0.5f;
            PC.VignetteRoundness  = 1.0f;
            // Matches SPostProcessSettings::ToneMapper's default, so a settings-less view grades the same.
            PC.TonemapMode        = (uint32)EToneMapper::AGX;
            PC.Time               = Time;
            PC.BloomIntensity     = 0.0f;
            PC.ColorFilter        = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
            PC.Shadows            = FVector4(1.0f, 1.0f, 1.0f, 0.0f);
            PC.Midtones           = FVector4(1.0f, 1.0f, 1.0f, 0.0f);
            PC.Highlights         = FVector4(1.0f, 1.0f, 1.0f, 0.0f);
            PC.VignetteColor      = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
            PC.BloomTint          = FVector4(1.0f, 1.0f, 1.0f, 0.0f);
            PC.AutoExposureKey    = 0.0f;
            PC.AutoExposureMinMul = 0.0f;
            PC.AutoExposureMaxMul = 1.0f;
            return PC;
        }

        FColorGradingConstants BuildColorGradingConstants(const SPostProcessSettings* Settings, float Time)
        {
            if (Settings == nullptr || !Settings->bEnabled)
            {
                return MakeDefaultColorGrading(Time);
            }

            FColorGradingConstants PC{};
            PC.Exposure           = std::exp2(Settings->ExposureCompensation);
            PC.Contrast           = Settings->Contrast;
            PC.Saturation         = Settings->Saturation;
            PC.Gamma              = Settings->Gamma;
            PC.WhiteTemp          = Settings->Temperature;
            PC.WhiteTint          = Settings->Tint;
            PC.VignetteIntensity  = Settings->VignetteIntensity;
            PC.VignetteSmoothness = Settings->VignetteSmoothness;
            PC.VignetteRoundness  = Settings->VignetteRoundness;
            PC.TonemapMode        = (uint32)Settings->ToneMapper;
            PC.Time               = Time;
            PC.BloomIntensity     = Settings->BloomIntensity;
            PC.ColorFilter        = FVector4(Settings->ColorFilter, Settings->ColorFilterIntensity);
            PC.Shadows            = FVector4(Settings->Shadows,    Settings->FilmGrainIntensity);
            PC.Midtones           = FVector4(Settings->Midtones,   Math::Max(Settings->FilmGrainSize, 0.0001f));
            PC.Highlights         = FVector4(Settings->Highlights, Settings->FilmGrainResponse);
            PC.VignetteColor      = FVector4(Settings->VignetteColor, 0.0f);
            PC.BloomTint          = FVector4(Settings->BloomTint, Settings->ChromaticAberration);
            PC.AutoExposureKey    = Settings->bAutoExposure ? 0.18f : 0.0f;
            PC.AutoExposureMinMul = std::exp2(Settings->AutoExposureMinEV);
            PC.AutoExposureMaxMul = std::exp2(Math::Max(Settings->AutoExposureMaxEV, Settings->AutoExposureMinEV));
            return PC;
        }
    }

    namespace
    {
        // Push constants for BloomDownsample.slang; one dispatch per mip.
        struct FBloomDownPushConstants
        {
            FVector2     SrcTexelSize;
            uint32       SrcIndex;
            float        SrcMip;

            FUIntVector2 DstSize;
            uint32       DstUAV;
            uint32       bFirstPass;

            float        Threshold;
            float        SoftKnee;
            uint32       AdaptedLumIndex;
            float        Exposure;

            float        AutoExposureKey;
            float        AutoExposureMinMul;
            float        AutoExposureMaxMul;
            float        _Pad;
        };
        static_assert(sizeof(FBloomDownPushConstants) == 64, "FBloomDownPushConstants must match BloomDownsample.slang::FPushConstants.");

        struct FBloomUpCSPushConstants
        {
            FVector2  SrcTexelSize;
            float      Radius;
            uint32     SrcIndex;

            FUIntVector2 DstSize;
            uint32     DstUAV;
            float      SrcMip;

            float      Scatter;
            uint32     _Pad0;
            uint32     _Pad1;
            uint32     _Pad2;
        };
        static_assert(sizeof(FBloomUpCSPushConstants) == 48,
            "FBloomUpCSPushConstants must match BloomUpsampleCS.slang::FPushConstants.");

        constexpr uint32 BloomTileSize = 8;
    }

    void FDefaultSceneRenderer::BloomPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const SPostProcessSettings* ActivePostProcess = Frame.PostProcess.bHasActivePostProcess ? &Frame.PostProcess.ActivePostProcessStorage : nullptr;

        if (ActivePostProcess == nullptr || !ActivePostProcess->bEnabled || ActivePostProcess->BloomIntensity <= 0.0f)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Bloom Pass", tracy::Color::Yellow3);

        static const FShaderH DownCS = FShaderLibrary::Get("BloomDownsample.slang");
        static const FShaderH UpCS = FShaderLibrary::Get("BloomUpsampleCS.slang");
        if (!DownCS || !UpCS)
        {
            return;
        }

        const FSceneImage& HDR   = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Bloom = CurrentView->BloomChainImage;
        const uint32 HDRWidth = HDR.GetSizeX();
        const uint32 HDRHght  = HDR.GetSizeY();
        const uint32 Mip0W    = std::max<uint32>(HDRWidth >> 1u, 1u);
        const uint32 Mip0H    = std::max<uint32>(HDRHght  >> 1u, 1u);

        const uint32 MinDim  = Math::Min(Mip0W, Mip0H);
        const uint32 Octaves = MinDim >= 8u ? (uint32)Math::Log2((float)MinDim) - 2u : 1u;
        const uint32 NumMips = Math::Clamp(Octaves, 1u, Math::Max(Bloom.GetNumMips(), 1u));

        const FSceneImage& AdaptedLum = GetNamedImage(ENamedImage::AdaptedLuminance);

        // Threshold is now post-exposure; the shader divides it by the same scale ColorGrading applies.
        const float Threshold = ActivePostProcess->BloomThreshold;

        // Down chain does a 13-tap filtered reduction per mip, prefiltering on the first.
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(DownCS));
        for (uint32 Mip = 0; Mip < NumMips; ++Mip)
        {
            const uint32 DstW = std::max<uint32>(Mip0W >> Mip, 1u);
            const uint32 DstH = std::max<uint32>(Mip0H >> Mip, 1u);
            const uint32 SrcW = (Mip == 0) ? HDRWidth : std::max<uint32>(Mip0W >> (Mip - 1u), 1u);
            const uint32 SrcH = (Mip == 0) ? HDRHght  : std::max<uint32>(Mip0H >> (Mip - 1u), 1u);

            if (Mip > 0)
            {
                // Order against the previous mip's writes.
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                    RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            }

            FBloomDownPushConstants PC = {};
            PC.SrcTexelSize = FVector2(1.0f / (float)SrcW, 1.0f / (float)SrcH);
            PC.SrcIndex     = (Mip == 0) ? (uint32)HDR.GetResourceID() : (uint32)Bloom.GetResourceID();
            PC.SrcMip       = (Mip == 0) ? 0.0f : (float)(Mip - 1u);
            PC.DstSize      = FUIntVector2(DstW, DstH);
            PC.DstUAV       = (uint32)Bloom.GetMipUAVIndex(Mip);
            PC.bFirstPass   = (Mip == 0) ? 1u : 0u;
            PC.Threshold          = Threshold;
            PC.SoftKnee           = ActivePostProcess->BloomSoftKnee;
            PC.AdaptedLumIndex    = (uint32)AdaptedLum.GetResourceID();
            PC.Exposure           = std::exp2(ActivePostProcess->ExposureCompensation);
            PC.AutoExposureKey    = ActivePostProcess->bAutoExposure ? 0.18f : 0.0f;
            PC.AutoExposureMinMul = std::exp2(ActivePostProcess->AutoExposureMinEV);
            PC.AutoExposureMaxMul = std::exp2(Math::Max(ActivePostProcess->AutoExposureMaxEV, ActivePostProcess->AutoExposureMinEV));

            RHI::CmdDispatch(CL, MakeArgs(PC),
                             RenderUtils::GetGroupCount(DstW, BloomTileSize),
                             RenderUtils::GetGroupCount(DstH, BloomTileSize), 1);
        }

        // Up chain does tent-filtered progressive accumulation, scatter-weighted.
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(UpCS));
        for (uint32 i = NumMips - 1; i > 0; --i)
        {
            const uint32 SrcMip = i;
            const uint32 DstMip = i - 1;
            const uint32 SrcW   = std::max<uint32>(Mip0W >> SrcMip, 1u);
            const uint32 SrcH   = std::max<uint32>(Mip0H >> SrcMip, 1u);
            const uint32 DstW   = std::max<uint32>(Mip0W >> DstMip, 1u);
            const uint32 DstH   = std::max<uint32>(Mip0H >> DstMip, 1u);

            // Order against the previous mip's writes (down chain, then each up step).
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

            FBloomUpCSPushConstants PC = {};
            PC.SrcTexelSize = FVector2(1.0f / (float)SrcW, 1.0f / (float)SrcH);
            PC.Radius       = 1.0f;
            PC.SrcIndex     = (uint32)Bloom.GetResourceID();
            PC.DstSize      = FUIntVector2(DstW, DstH);
            PC.DstUAV       = (uint32)Bloom.GetMipUAVIndex(DstMip);
            PC.SrcMip       = (float)SrcMip;
            PC.Scatter      = Math::Clamp(ActivePostProcess->BloomScatter, 0.0f, 1.0f);

            RHI::CmdDispatch(CL, MakeArgs(PC),
                             RenderUtils::GetGroupCount(DstW, BloomTileSize),
                             RenderUtils::GetGroupCount(DstH, BloomTileSize), 1);
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    namespace
    {
        struct FHistogramBuildPushConstants
        {
            RHI::TGPUSpan<uint32> Histogram;
            uint32       HDRIndex;
            uint32       _Pad0;

            FUIntVector2 HDRSize;
            float        MinLogLum;
            float        InvLogLumRange;
        };
        static_assert(sizeof(FHistogramBuildPushConstants) == 40,
            "FHistogramBuildPushConstants must match LuminanceHistogram.slang::FPushConstants.");

        struct FHistogramAvgPushConstants
        {
            RHI::TGPUSpan<uint32> Histogram;
            uint32 AdaptUAV;
            float  MinLogLum;

            float  LogLumRange;
            float  LowPercent;
            float  HighPercent;
            float  DeltaTime;

            float  AdaptationSpeed;
            float  _Pad;
        };
        static_assert(sizeof(FHistogramAvgPushConstants) == 48,
            "FHistogramAvgPushConstants must match LuminanceHistogramAverage.slang::FPushConstants.");

        // TILE_DIM in LuminanceHistogram.slang; its square must equal kLuminanceHistogramBins.
        constexpr uint32 HistogramTileSize = 16;
    }

    void FDefaultSceneRenderer::AutoExposurePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const SPostProcessSettings* ActivePostProcess = Frame.PostProcess.bHasActivePostProcess ? &Frame.PostProcess.ActivePostProcessStorage : nullptr;

        if (ActivePostProcess == nullptr || !ActivePostProcess->bEnabled || !ActivePostProcess->bAutoExposure)
        {
            return;
        }

        static const FShaderH BuildCS = FShaderLibrary::Get("LuminanceHistogram.slang");
        static const FShaderH AvgCS   = FShaderLibrary::Get("LuminanceHistogramAverage.slang");
        if (!BuildCS || !AvgCS)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Auto Exposure Pass", tracy::Color::Orange3);

        const FSceneImage& HDR     = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Adapted = GetNamedImage(ENamedImage::AdaptedLuminance);
        const RHI::FGPURange Histogram = GetLuminanceHistogram();

        // Scene-luminance span the bins cover, in stops. Wide enough for starlight to a clipped sun.
        constexpr float MinLogLum = -10.0f;
        constexpr float MaxLogLum =  12.0f;
        constexpr float LogLumRange = MaxLogLum - MinLogLum;

        const uint32 HDRWidth = HDR.GetSizeX();
        const uint32 HDRHght  = HDR.GetSizeY();

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BuildCS));

        FHistogramBuildPushConstants BuildPC = {};
        BuildPC.Histogram      = { Histogram };
        BuildPC.HDRIndex       = (uint32)HDR.GetResourceID();
        BuildPC.HDRSize        = FUIntVector2(HDRWidth, HDRHght);
        BuildPC.MinLogLum      = MinLogLum;
        BuildPC.InvLogLumRange = 1.0f / LogLumRange;

        RHI::CmdDispatch(CL, MakeArgs(BuildPC),
                         RenderUtils::GetGroupCount(HDRWidth, HistogramTileSize),
                         RenderUtils::GetGroupCount(HDRHght,  HistogramTileSize), 1);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(AvgCS));

        // Ordered so a mis-set pair narrows the window instead of inverting it and metering nothing.
        const float LowPercent  = Math::Clamp(ActivePostProcess->AutoExposureLowPercent, 0.0f, 1.0f);
        const float HighPercent = Math::Clamp(ActivePostProcess->AutoExposureHighPercent, LowPercent, 1.0f);

        FHistogramAvgPushConstants AvgPC = {};
        AvgPC.Histogram       = { Histogram };
        AvgPC.AdaptUAV        = (uint32)Adapted.GetMipUAVIndex(0);
        AvgPC.MinLogLum       = MinLogLum;
        AvgPC.LogLumRange     = LogLumRange;
        AvgPC.LowPercent      = LowPercent;
        AvgPC.HighPercent     = HighPercent;
        AvgPC.DeltaTime       = Frame.SceneGlobalData.DeltaTime;
        AvgPC.AdaptationSpeed = ActivePostProcess->AutoExposureSpeed;

        RHI::CmdDispatch(CL, MakeArgs(AvgPC), 1, 1, 1);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::ToneMappingPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Color Grading + Tone Map Pass", tracy::Color::Red2);

        const FFrameData& Frame = *RenderFrame;
        const auto& ActivePostProcessMaterials = Frame.PostProcess.ActivePostProcessMaterials;
        const SPostProcessSettings* ActivePostProcess = Frame.PostProcess.bHasActivePostProcess ? &Frame.PostProcess.ActivePostProcessStorage : nullptr;
        const auto& SceneGlobalData            = Frame.SceneGlobalData;

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("ColorGrading.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const bool bSMAAEnabled = GetSMAAMode() != ESMAAMode::Off;
        const bool bPPMaterials = !ActivePostProcessMaterials.empty();
        const FSceneImage& Output = (bSMAAEnabled || bPPMaterials) ? GetNamedImage(ENamedImage::LDR) : CurrentView->Output;

        const FSceneImage& HDRTex     = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& BloomTex   = CurrentView->BloomChainImage;
        const FSceneImage& AdaptedTex = GetNamedImage(ENamedImage::AdaptedLuminance);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Undefined;
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
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        FColorGradingConstants Constants = BuildColorGradingConstants(ActivePostProcess, SceneGlobalData.Time);

        struct FComposePushConstants
        {
            uint64 ConstantsAddr;
            uint32 HDRIndex;
            uint32 BloomIndex;
            uint32 AdaptedLumIndex;
            uint32 _Pad;
        };
        static_assert(sizeof(FComposePushConstants) == 24, "FComposePushConstants must match the slang pass block.");

        FComposePushConstants PC = {};
        PC.ConstantsAddr   = RHI::CopyTransient(Constants);
        PC.HDRIndex        = (uint32)HDRTex.GetResourceID();
        PC.BloomIndex      = (uint32)BloomTex.GetResourceID();
        PC.AdaptedLumIndex = (uint32)AdaptedTex.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    namespace
    {
        struct FPostProcessMaterialPushConstants
        {
            uint32 MaterialIndex;
            uint32 SceneColorIndex;   // bindless SRV of the ping-pong source
            uint32 SceneDepthIndex;   // bindless SRV of opaque depth
            uint32 HDRIndex;          // bindless SRV of pre-tone-map HDR
        };
        static_assert(sizeof(FPostProcessMaterialPushConstants) == 16,
            "FPostProcessMaterialPushConstants must match the slang push block.");
    }

    void FDefaultSceneRenderer::PostProcessMaterialPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& ActivePostProcessMaterials = Frame.PostProcess.ActivePostProcessMaterials;

        if (ActivePostProcessMaterials.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Post Process Material Pass", tracy::Color::Magenta);

        const FSceneImage& DepthTex = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& HDRTex   = GetNamedImage(ENamedImage::HDR);

        const bool bSMAAEnabled = GetSMAAMode() != ESMAAMode::Off;

        const FSceneImage* Source = &GetNamedImage(ENamedImage::LDR);
        const FSceneImage* Dest   = &GetNamedImage(ENamedImage::PostProcessScratch);

        for (const FFrameData::FPostProcessMaterial& PPMaterial : ActivePostProcessMaterials)
        {
            // Resolved + ref-held at extract; the render phase never touches the CMaterial.
            FShaderH VS = PPMaterial.Shaders.VertexShader;
            FShaderH PS = PPMaterial.Shaders.PixelShader;
            if (VS == nullptr || PS == nullptr)
            {
                continue;
            }

            RHI::FRenderAttachment Color;
            Color.Texture = Dest->Texture;
            Color.LoadOp  = RHI::ELoadOp::Undefined;
            Color.StoreOp = RHI::EStoreOp::Store;

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            Pass.RenderArea       = Dest->GetExtent();

            RHI::CmdBeginRenderPass(CL, Pass);
            SetViewportScissor(CL, Dest->GetExtent());
            RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

            FGraphicsPipelineKey Key;
            Key.VS = VS;
            Key.PS = PS;
            Key.ColorTargets.push_back({ Dest->Desc.Format, {} });
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FPostProcessMaterialPushConstants PC = {};
            PC.MaterialIndex    = PPMaterial.MaterialIndex;
            PC.SceneColorIndex  = (uint32)Source->GetResourceID();
            PC.SceneDepthIndex  = (uint32)DepthTex.GetResourceID();
            PC.HDRIndex         = (uint32)HDRTex.GetResourceID();

            RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);

            std::swap(Source, Dest);
        }

        const FSceneImage& LDR = GetNamedImage(ENamedImage::LDR);
        if (bSMAAEnabled)
        {
            if (Source->Texture.Handle != LDR.Texture.Handle)
            {
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite,
                    RHI::EStageFlags::Transfer,
                    RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
                RHI::CmdCopyTexture(CL, Source->Texture, RHI::FTextureSlice{}, LDR.Texture, RHI::FTextureSlice{});
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                    RHI::EStageFlags::PixelShader,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            }
        }
        else
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite,
                RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            RHI::CmdCopyTexture(CL, Source->Texture, RHI::FTextureSlice{}, CurrentView->Output.Texture, RHI::FTextureSlice{});
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::PixelShader,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }
    }

    struct FSMAAPushConstants
    {
        FVector4 RTMetrics;  // x = 1/w, y = 1/h, z = w, w = h
        float     EdgeThreshold;
        float     DebugMode;
        uint32    TexIndex0;  // bindless SRV index of the pass's primary input
        uint32    TexIndex1;  // pass-specific extra input (0 if unused)
        uint32    TexIndex2;  // pass-specific extra input (0 if unused)
        float     MaxSearchSteps;      // blend-weight pass only; the other two ignore these
        float     MaxSearchStepsDiag;
        uint32    _Pad2;
        // AreaTex slice per axis; 0 is the SMAA 1x pattern and 1 and 2 are the T2x subsample pair.
        FVector4  SubsampleIndices;
    };
    static_assert(sizeof(FSMAAPushConstants) == 64, "FSMAAPushConstants must match the slang push block.");

    static float GetSMAAEdgeThreshold(ESMAAQuality Quality)
    {
        switch (Quality)
        {
        case ESMAAQuality::Low:    return 0.15f;
        case ESMAAQuality::Medium: return 0.12f;
        case ESMAAQuality::High:   return 0.10f;
        case ESMAAQuality::Ultra:  return 0.05f;
        default:                   return 0.10f;
        }
    }

    // Reference SMAA preset step counts. High matches the old hardcoded 16/8, so it is unchanged.
    static FVector2 GetSMAASearchSteps(ESMAAQuality Quality)
    {
        switch (Quality)
        {
        case ESMAAQuality::Low:    return FVector2(4.0f,  2.0f);
        case ESMAAQuality::Medium: return FVector2(8.0f,  4.0f);
        case ESMAAQuality::High:   return FVector2(16.0f, 8.0f);
        case ESMAAQuality::Ultra:  return FVector2(32.0f, 16.0f);
        default:                   return FVector2(16.0f, 8.0f);
        }
    }

    static FSMAAPushConstants BuildSMAAPushConstants(const FSceneImage& Image, ESMAAQuality Quality)
    {
        FSMAAPushConstants PC;
        const float W = (float)Image.GetSizeX();
        const float H = (float)Image.GetSizeY();
        PC.RTMetrics      = FVector4(1.0f / W, 1.0f / H, W, H);
        PC.EdgeThreshold  = GetSMAAEdgeThreshold(Quality);
        PC.DebugMode      = 0.0f;
        PC.TexIndex0 = 0; PC.TexIndex1 = 0; PC.TexIndex2 = 0;

        const FVector2 Steps = GetSMAASearchSteps(Quality);
        PC.MaxSearchSteps     = Steps.x;
        PC.MaxSearchStepsDiag = Steps.y;
        PC._Pad2 = 0;
        PC.SubsampleIndices = FVector4(0.0f);
        return PC;
    }

    void FDefaultSceneRenderer::SMAAEdgeDetectionPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("SMAA Edge Detection", tracy::Color::Red2);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SMAAEdgeDetection.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output     = GetNamedImage(ENamedImage::SMAAEdges);
        const FSceneImage& InputColor = GetNamedImage(ENamedImage::LDR);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Clear;
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
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        FSMAAPushConstants PC = BuildSMAAPushConstants(Output, GetSMAAQuality());
        PC.TexIndex0 = (uint32)InputColor.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::SMAABlendWeightPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("SMAA Blend Weight", tracy::Color::Red2);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SMAABlendWeight.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output    = GetNamedImage(ENamedImage::SMAABlend);
        const FSceneImage& EdgesTex  = GetNamedImage(ENamedImage::SMAAEdges);
        const FSceneImage& AreaTex   = GetNamedImage(ENamedImage::SMAAArea);
        const FSceneImage& SearchTex = GetNamedImage(ENamedImage::SMAASearch);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Clear;
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
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        FSMAAPushConstants PC = BuildSMAAPushConstants(Output, GetSMAAQuality());
        PC.TexIndex0 = (uint32)EdgesTex.GetResourceID();
        PC.TexIndex1 = (uint32)AreaTex.GetResourceID();
        PC.TexIndex2 = (uint32)SearchTex.GetResourceID();
        PC.SubsampleIndices = GetSMAASubsampleIndices();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::SMAANeighborhoodBlendPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("SMAA Neighborhood Blend", tracy::Color::Red2);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SMAANeighborhoodBlend.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        // Under T2x this lands in the history slot instead, and the resolve is what writes the view output.
        const bool bTemporal          = IsTemporalResolveReady();
        const FSceneImage& Output     = bTemporal ? GetNamedImage(GetTemporalCurrentImage()) : CurrentView->Output;
        const FSceneImage& InputColor = GetNamedImage(ENamedImage::LDR);
        const FSceneImage& BlendTex   = GetNamedImage(ENamedImage::SMAABlend);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Undefined;
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
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        FSMAAPushConstants PC = BuildSMAAPushConstants(Output, GetSMAAQuality());
        PC.TexIndex0 = (uint32)InputColor.GetResourceID();
        PC.TexIndex1 = (uint32)BlendTex.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

#if !defined(LE_SHIPPING)
    // Its own pass, because SceneDebugView runs before terrain and only owns the deferred lane's pixels.
    void FDefaultSceneRenderer::VelocityDebugPass(RHI::FCmdListH CL)
    {
        if (RenderFrame == nullptr ||
            RenderFrame->SceneGlobalData.CullData.DebugMode != (uint32)ERenderSceneDebugFlags::Velocity)
        {
            return;
        }

        const FSceneImage& Velocity = GetNamedImage(ENamedImage::Velocity);
        if (!Velocity.IsValid())
        {
            return;
        }

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader  = FShaderLibrary::Get("VelocityDebugPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Velocity Debug", tracy::Color::Magenta);
        SCENE_GPU_SCOPE(CL, "Velocity Debug");

        const FSceneImage& Output = GetNamedImage(ENamedImage::HDR);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Undefined;
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
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FData
        {
            uint32 VelocityIndex;
        } PC;

        PC.VelocityIndex = (uint32)Velocity.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
#endif

    void FDefaultSceneRenderer::SnapshotMotionState(RHI::FCmdListH CL)
    {
        if (!IsTemporalAAEnabledFor(SceneViews[0]))
        {
            if (PrevBoneArenaBuffer)         { DeferFree(PrevBoneArenaBuffer); PrevBoneArenaBuffer = {}; }
            if (PrevRetainedTransformBuffer) { DeferFree(PrevRetainedTransformBuffer); PrevRetainedTransformBuffer = {}; }
            bPrevMotionStateValid = false;
            return;
        }

        auto Mirror = [this, CL](const RHI::FGPUAllocation& Source, RHI::FGPUAllocation& Dest, const char* DebugName)
        {
            if (!Source || Source.Size == 0)
            {
                return false;
            }

            const uint64 Bytes = Source.Size;
            if (!Dest || Dest.Size != Bytes)
            {
                if (Dest)
                {
                    DeferFree(Dest);
                }
                Dest = CreateSceneBuffer(Bytes, DebugName);
            }

            if (!Dest)
            {
                return false;
            }

            RHI::CmdMemcpy(CL, { Dest.Gpu, Bytes }, { Source.Gpu, Bytes });
            return true;
        };

        const bool bTransforms = Mirror(RetainedTransformBuffer, PrevRetainedTransformBuffer,
                                        "Motion.PrevRetainedTransforms");
        Mirror(BoneArenaBuffer, PrevBoneArenaBuffer, "Motion.PrevBoneArena");

        // Without a previous transform there is no object motion to read, so the pass stays camera-only.
        bPrevMotionStateValid = bTransforms;
    }

    bool FDefaultSceneRenderer::IsTemporalAARequested()
    {
        return GetSMAAMode() == ESMAAMode::SMAAT2x;
    }

    bool FDefaultSceneRenderer::IsTemporalAAEnabledFor(const FSceneView& View)
    {
        // Captures and probe bakes carry no jitter sequence, so they would resolve against another view's frame.
        return IsTemporalAARequested() && View.bIsPrimary;
    }

    bool FDefaultSceneRenderer::IsTemporalAAEnabled() const
    {
        return CurrentView != nullptr && IsTemporalAAEnabledFor(*CurrentView);
    }

    // One predicate for both the blend redirect and the resolve, or a half-allocated set strands the output.
    bool FDefaultSceneRenderer::IsTemporalResolveReady() const
    {
        return IsTemporalAAEnabled()
            && GetNamedImage(ENamedImage::Velocity).IsValid()
            && GetNamedImage(ENamedImage::TemporalHistoryA).IsValid()
            && GetNamedImage(ENamedImage::TemporalHistoryB).IsValid();
    }

    bool FDefaultSceneRenderer::IsVelocityDebugActive() const
    {
        #if !defined(LE_SHIPPING)
        return RenderFrame != nullptr
            && RenderFrame->SceneGlobalData.CullData.DebugMode == (uint32)ERenderSceneDebugFlags::Velocity;
        #else
        return false;
        #endif
    }

    // The debug view is the only way to see whether motion vectors are sane, so it must not need T2x on.
    bool FDefaultSceneRenderer::IsVelocityWanted() const
    {
        return IsTemporalAAEnabled() || (CurrentView != nullptr && CurrentView->bIsPrimary && IsVelocityDebugActive());
    }

    FVector4 FDefaultSceneRenderer::GetSMAASubsampleIndices() const
    {
        // A lone frame with no resolve behind it wants the unbiased 1x pattern, which is slice zero.
        if (!IsTemporalResolveReady())
        {
            return FVector4(0.0f);
        }

        // Parity 1 is the +0.25 px subsample, which is the slice the reference calls pass zero.
        const float Slice = (CurrentView->TemporalFrameIndex & 1u) == 1u ? 1.0f : 2.0f;
        return FVector4(Slice, Slice, Slice, 0.0f);
    }

    void FDefaultSceneRenderer::VelocityPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Velocity", tracy::Color::Orange2);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader  = FShaderLibrary::Get("Velocity.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = GetNamedImage(ENamedImage::Velocity);
        const FSceneImage& Depth  = GetNamedImage(ENamedImage::DepthAttachment);
        if (!Output.IsValid() || !Depth.IsValid())
        {
            return;
        }

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Undefined;
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
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FData
        {
            uint32 DepthIndex;
        } PC;

        PC.DepthIndex = (uint32)Depth.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::TemporalResolvePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Temporal Resolve", tracy::Color::Orange3);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader  = FShaderLibrary::Get("TemporalResolve.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        if (!IsTemporalResolveReady())
        {
            return;
        }

        const FSceneImage& Output   = CurrentView->Output;
        const FSceneImage& Current  = GetNamedImage(GetTemporalCurrentImage());
        const FSceneImage& History  = GetNamedImage(GetTemporalHistoryImage());
        const FSceneImage& Velocity = GetNamedImage(ENamedImage::Velocity);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Undefined;
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
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FData
        {
            uint32 CurrentIndex;
            uint32 HistoryIndex;
            uint32 VelocityIndex;
            float  HistoryWeight;
            float  ClampGamma;
            float  MaxReprojection;
            uint32 bNeighborhoodClamp;
            uint32 _Pad0;
        } PC;
        static_assert(sizeof(FData) == 32, "FData must match TemporalResolve.slang FData.");

        const CRendererSettings* Settings = GetDefault<CRendererSettings>();

        PC.CurrentIndex  = (uint32)Current.GetResourceID();
        PC.HistoryIndex  = (uint32)History.GetResourceID();
        PC.VelocityIndex = (uint32)Velocity.GetResourceID();

        // The first frame after a resize or a toggle has nothing behind it, so it resolves to itself.
        const float Weight = Settings != nullptr ? Math::Clamp(Settings->TemporalHistoryWeight, 0.0f, 0.5f) : 0.5f;
        PC.HistoryWeight  = CurrentView->bTemporalHistoryValid ? Weight : 0.0f;

        PC.ClampGamma         = Settings != nullptr ? Math::Max(Settings->TemporalClampGamma, 0.25f) : 1.0f;
        PC.MaxReprojection    = Settings != nullptr ? Math::Max(Settings->TemporalMaxReprojection, 0.01f) : 0.25f;
        PC.bNeighborhoodClamp = (Settings == nullptr || Settings->bTemporalNeighborhoodClamp) ? 1u : 0u;
        PC._Pad0              = 0u;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);

        CurrentView->bTemporalHistoryValid = true;
    }
}
