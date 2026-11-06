#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    static constexpr uint32 GPrefilterSampleCount = 256;

    struct FPrefilterPC
    {
        uint32 SrcCubeSRV     = 0;
        uint32 OutMipUAV      = 0;
        float  Roughness      = 0.0f;
        uint32 NumSamples     = 0;
        uint32 DstLayerOffset = 0;
        uint32 _Pad0          = 0;
        uint32 _Pad1          = 0;
        uint32 _Pad2          = 0;
    };
    static_assert(sizeof(FPrefilterPC) == 32, "FPrefilterPC must match PrefilterEnvMap.slang::FPushConstants.");

    void FDefaultSceneRenderer::ReflectionProbeBakePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& Bake = Frame.ReflectionProbes;

        if (Bake.BakingProbe < 0 || Bake.BakeViewIndex <= 0 || Bake.BakeViewIndex >= SceneViews.Num())
        {
            if (Bake.BakingProbe >= 0)
            {
                LOG_WARN_ONCE("[Probe] Bake SKIPPED: probe={} viewIndex={} numSceneViews={}",
                         Bake.BakingProbe, Bake.BakeViewIndex, SceneViews.Num());
            }
            return;
        }

        const FSceneImage& CaptureCube = NamedImages[(int)ENamedImage::ProbeCaptureCube];
        const FSceneImage& ProbeArray  = NamedImages[(int)ENamedImage::ProbePrefiltered];
        if (!CaptureCube.IsValid() || !ProbeArray.IsValid())
        {
            LOG_WARN_ONCE("[Probe] Bake SKIPPED: captureCube={} probeArray={} (targets not allocated)",
                     CaptureCube.IsValid() ? 1 : 0, ProbeArray.IsValid() ? 1 : 0);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Reflection Probe Bake", tracy::Color::SkyBlue3);
        SCENE_GPU_SCOPE(CL, "Reflection Probe Bake");

        FSceneView& View = SceneViews[Bake.BakeViewIndex];

        bCapturingProbe = true;

        const uint32 FaceSize = Bake.BakeFaceSize;

        const bool     bClearToColor = Bake.Captures[Bake.BakingProbe].bClearToColor;
        const FVector3 ClearRGB      = Bake.Captures[Bake.BakingProbe].ClearColor;

        for (int32 Face = 0; Face < 6; ++Face)
        {
            if (Bake.FaceCullViews[Face] == Constants::kIndexNoneU32)
            {
                continue;
            }

            PointAtView(View);
            CurrentCameraEarlyView = Bake.FaceCullViews[Face];

            SetSceneRoot(CL, View,
                RHI::CopyTransient(MakeSecondaryViewGlobals(Bake.FaceGlobals[Face])));

            TerrainCullPass(CL);
            const bool bTerrainCleared = TerrainDepthPrePass(CL, /*bClear*/ true);
            VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ !bTerrainCleared);
            ClusterBuildPass(CL);
            LightCullPass(CL);

            if (bClearToColor)
            {
                const FSceneImage& ClearRT = GetNamedImage(ENamedImage::HDR);

                RHI::FRenderAttachment ClearColorAttachment;
                ClearColorAttachment.Texture        = ClearRT.Texture;
                ClearColorAttachment.LoadOp         = RHI::ELoadOp::Clear;
                ClearColorAttachment.StoreOp        = RHI::EStoreOp::Store;
                ClearColorAttachment.Color[0]       = ClearRGB.x;
                ClearColorAttachment.Color[1]       = ClearRGB.y;
                ClearColorAttachment.Color[2]       = ClearRGB.z;
                ClearColorAttachment.Color[3]       = 1.0f;

                RHI::FRenderPassDesc ClearPass;
                ClearPass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&ClearColorAttachment, 1);
                ClearPass.RenderArea       = GetNamedImage(ENamedImage::HDR).GetExtent();

                RHI::CmdBeginRenderPass(CL, ClearPass);
                RHI::CmdEndRenderPass(CL);
                Barriers::RasterToRead(CL);
            }
            else
            {
                EnvironmentPass(CL);
            }

            DecalPass(CL);
            VisBufferClassifyPass(CL);
            MaterialGBufferPass(CL);
            DeferredLightingPass(CL);
            TerrainRenderPass(CL);

            const FSceneImage& FaceColor = GetNamedImage(ENamedImage::HDR);

            RHI::FTextureSlice SrcSlice;
            SrcSlice.Mip        = 0;
            SrcSlice.Layer      = 0;
            SrcSlice.LayerCount = 1;
            SrcSlice.Extent     = FUIntVector3(FaceSize, FaceSize, 1);

            RHI::FTextureSlice DstSlice = SrcSlice;
            DstSlice.Layer = (uint32)Face;

            RHI::CmdBarrier(CL,
                RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite,
                RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            RHI::CmdCopyTexture(CL, FaceColor.Texture, SrcSlice, CaptureCube.Texture, DstSlice);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }

        {
            static const FShaderH ComputeShader = FShaderLibrary::Get("PrefilterEnvMap.slang");
            if (ComputeShader != nullptr)
            {
                SCENE_GPU_SCOPE(CL, "Probe Prefilter");
                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));
                DispatchPrefilterMips(CL, CaptureCube, ProbeArray, (uint32)Bake.BakingProbe * 6u);

                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                    RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            }
        }

        bCapturingProbe = false;

        PointAtView(SceneViews[0]);
        CurrentCameraEarlyView = 0u;

        CompletedProbeBakes.fetch_or(1u << (uint32)Bake.BakingProbe, std::memory_order_acq_rel);
    }

    void FDefaultSceneRenderer::SkyCubeCapturePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Sky Cube Capture", tracy::Color::SkyBlue);

        const FFrameData& Frame = *RenderFrame;
        const auto& LightData         = Frame.Lighting.LightData;
        const auto& SceneGlobalData   = Frame.SceneGlobalData;
        const int32 EnvironmentMapID  = Frame.Volumetrics.EnvironmentMapID;
        const bool bIBLDirty          = Frame.Volumetrics.bIBLDirty;

        if (!FrameFlags.bHasEnvironment)
        {
            if (bSkyTargetsBlack)
            {
                return;
            }
            bSkyTargetsBlack = true;

            const float Black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyCube).Texture, Black);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyIrradiance).Texture, Black);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyPrefilter).Texture, Black);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            return;
        }
        bSkyTargetsBlack = false;

        if (!bIBLDirty)
        {
            return;
        }

        const FSceneImage& SkyCube = GetNamedImage(ENamedImage::SkyCube);
        if (!SkyCube.IsValid())
        {
            return;
        }

        // HDRI path where equirect to cube replaces the procedural fill.
        if (EnvironmentMapID >= 0)
        {
            static const FShaderH ComputeShader = FShaderLibrary::Get("EquirectToCubemap.slang");
            if (!ComputeShader)
            {
                return;
            }

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

            struct FEquirectPC
            {
                uint32 EquirectSRV;
                uint32 SkyCubeUAV;
                float  Intensity;
                float  CosYaw;
                float  SinYaw;
                uint32 bRemoveSun;
            };
            static_assert(sizeof(FEquirectPC) == 24, "FEquirectPC must match EquirectToCubemap.slang::FPushConstants.");

            const FVector4& HDRIParams = Frame.Volumetrics.EnvironmentParams.HDRIParams;

            FEquirectPC PC = {};
            PC.EquirectSRV = (uint32)EnvironmentMapID;
            PC.SkyCubeUAV  = (uint32)SkyCube.GetMipUAVIndex(0);
            PC.Intensity   = HDRIParams.x;
            PC.CosYaw      = HDRIParams.y;
            PC.SinYaw      = HDRIParams.z;
            PC.bRemoveSun  = Frame.Lighting.LightData.bHasSun ? 1u : 0u;

            constexpr uint32 EquirectTile = 8u;
            const uint32 FaceSize = SkyCube.GetSizeX();
            const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, EquirectTile);
            RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("SkyCubeCapture.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        struct FSkyCapturePC
        {
            uint64   EnvAddr;
            uint32   SkyCubeUAV;
            float    Time;
            FVector3 SunDirection;
            float    _Pad;
        } PC = {};
        PC.EnvAddr    = RHI::CopyTransient(Frame.Volumetrics.EnvironmentParams);
        PC.SkyCubeUAV = (uint32)SkyCube.GetMipUAVIndex(0);

        if (LightData.bHasSun)
        {
            PC.SunDirection = Math::Normalize(LightData.SkySunDirection);
        }
        else
        {
            PC.SunDirection = Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));
        }
        PC.Time = SceneGlobalData.Time;

        constexpr uint32 SkyCaptureTile = 8u;
        const uint32 FaceSize = SkyCube.GetSizeX();
        const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, SkyCaptureTile);
        // Z is 6 layers, one per cube face, and each thread owns one (face, x, y).
        RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);

        // Convolution + environment pass read the cube next.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::IrradianceConvolutionPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Sky Irradiance Convolution", tracy::Color::SkyBlue1);

        const FFrameData& Frame = *RenderFrame;
        const bool bIBLConvolutionDirty = Frame.Volumetrics.bIBLConvolutionDirty;

        if (!FrameFlags.bHasEnvironment)
        {
            return;
        }

        if (!bIBLConvolutionDirty)
        {
            return;
        }

        const FSceneImage& SkyCube        = GetNamedImage(ENamedImage::SkyCube);
        const FSceneImage& IrradianceCube = GetNamedImage(ENamedImage::SkyIrradiance);
        if (!SkyCube.IsValid() || !IrradianceCube.IsValid())
        {
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("IrradianceConvolution.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        struct FIrradiancePC { uint32 SrcCubeSRV; uint32 OutCubeUAV; uint32 _Pad0; uint32 _Pad1; };
        FIrradiancePC PC = {};
        PC.SrcCubeSRV = (uint32)SkyCube.GetResourceID();
        PC.OutCubeUAV = (uint32)IrradianceCube.GetMipUAVIndex(0);

        constexpr uint32 IrradianceTile = 8u;
        const uint32 FaceSize = IrradianceCube.GetSizeX();
        const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, IrradianceTile);
        RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::DispatchPrefilterMips(RHI::FCmdListH CL, const FSceneImage& Source, const FSceneImage& Target, uint32 DstLayerOffset)
    {
        constexpr uint32 PrefilterTile = 8u;

        const uint32 NumMips      = Target.GetNumMips();
        const uint32 BaseFaceSize = Target.GetSizeX();

        for (uint32 Mip = 0; Mip < NumMips; ++Mip)
        {
            FPrefilterPC PC = {};
            PC.SrcCubeSRV     = (uint32)Source.GetResourceID();
            PC.OutMipUAV      = (uint32)Target.GetMipUAVIndex(Mip);
            PC.Roughness      = (NumMips <= 1u) ? 0.0f : (float)Mip / (float)(NumMips - 1u);
            // At zero roughness every GGX sample is the mirror direction, so one sample is the whole integral.
            PC.NumSamples     = PC.Roughness > 0.0f ? GPrefilterSampleCount : 1u;
            PC.DstLayerOffset = DstLayerOffset;

            const uint32 MipFaceSize = Math::Max(BaseFaceSize >> Mip, 1u);
            const uint32 GroupsXY    = RenderUtils::GetGroupCount(MipFaceSize, PrefilterTile);
            RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
        }
    }

    void FDefaultSceneRenderer::PrefilterEnvMapPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const bool bIBLConvolutionDirty = Frame.Volumetrics.bIBLConvolutionDirty;

        if (!FrameFlags.bHasEnvironment)
        {
            return;
        }

        if (!bIBLConvolutionDirty)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Sky Prefilter Convolution", tracy::Color::SkyBlue2);

        const FSceneImage& SkyCube       = GetNamedImage(ENamedImage::SkyCube);
        const FSceneImage& PrefilterCube = GetNamedImage(ENamedImage::SkyPrefilter);
        if (!SkyCube.IsValid() || !PrefilterCube.IsValid())
        {
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("PrefilterEnvMap.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));
        DispatchPrefilterMips(CL, SkyCube, PrefilterCube, 0u);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::EnvironmentPass(RHI::FCmdListH CL)
    {
        if (!FrameFlags.bHasEnvironment)
        {
            const FSceneImage& ColorRT = GetNamedImage(ENamedImage::HDR);

            RHI::FRenderAttachment Color;
            Color.Texture        = ColorRT.Texture;
            Color.LoadOp         = RHI::ELoadOp::Clear;
            Color.StoreOp        = RHI::EStoreOp::Store;

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            Pass.RenderArea       = GetNamedImage(ENamedImage::HDR).GetExtent();

            RHI::CmdBeginRenderPass(CL, Pass);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Environment Pass", tracy::Color::Green3);

        static const FString   FarPlaneDefine = "FULLSCREEN_AT_FAR_PLANE";
        static const FShaderH VertexShader   = FShaderLibrary::Get("FullscreenQuad.slang", TSpan<const FString>(&FarPlaneDefine, 1));
        static const FShaderH PixelShader    = FShaderLibrary::Get("Environment.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& ColorRT = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& DepthRT = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& SkyCube = GetNamedImage(ENamedImage::SkyCube);
        const FUIntVector2 Extent  = GetNamedImage(ENamedImage::HDR).GetExtent();

        const int32  EnvMapID    = RenderFrame->Volumetrics.EnvironmentMapID;
        const uint32 EquirectIdx = EnvMapID >= 0 ? (uint32)EnvMapID : (uint32)GetNamedImage(ENamedImage::BRDFLut).GetResourceID();
        const uint32 EquirectW   = EnvMapID >= 0 ? RenderFrame->Volumetrics.EnvironmentMapWidth : 256u;

        RHI::FRenderAttachment Color;
        Color.Texture        = ColorRT.Texture;
        Color.LoadOp         = RHI::ELoadOp::Clear;
        Color.StoreOp        = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = DepthRT.Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        // Only pixels still at the cleared far plane show sky; every other one is shaded over by the passes after this.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::Equal;
        RHI::CmdSetDepthStencil(CL, DepthDesc);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // Mirrors the bit-cast the extract does into Misc.x; specializing on it strips the other modes.
        const uint32 SkyModeBits = std::bit_cast<uint32>(RenderFrame->Volumetrics.EnvironmentParams.Misc.x);

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.SkyMode     = (SkyModeBits <= GSkyMode_HDRI) ? (uint8)SkyModeBits : (uint8)GSkyMode_Runtime;
        Key.DepthFormat = DepthRT.Desc.Format;
        Key.ColorTargets.Add({ ColorRT.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FEnvPushConstants
        {
            uint64 EnvAddr;
            uint32 SkyCubeIndex;
            uint32 EquirectIndex;
            uint32 EquirectWidth;   // HDRI mode LOD that anti-aliases the sky
            uint32 _Pad1;
        };
        static_assert(sizeof(FEnvPushConstants) == 24, "FEnvPushConstants must match the slang pass block.");

        FEnvPushConstants PC = {};
        PC.EnvAddr       = RHI::CopyTransient(RenderFrame->Volumetrics.EnvironmentParams);
        PC.SkyCubeIndex  = (uint32)SkyCube.GetResourceID();
        PC.EquirectIndex = EquirectIdx;
        PC.EquirectWidth = EquirectW;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
}
