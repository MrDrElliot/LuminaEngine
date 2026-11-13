#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace
    {
        // Mirrors the FPushConstants in the three VolumetricFog*.slang shaders.
        struct FFroxelInjectPushConstants
        {
            uint32   GridSize[3];
            float    NearPlane;

            float    FogRange;            // froxel far plane (max fog distance, view units)
            uint32   bSunVolumetric;      // 1 if light 0 (sun) opted into volumetrics
            float    Time;
            uint32   ScatterUAV;          // bindless 3D UAV index of the scatter volume

            uint32   LocalLightSamples;   // 0 skips local lights, 1 is the froxel center, 4 supersamples
            uint32   _PadNumFogVolumes;
            uint32   CloudShadowIndex;    // bindless 2D SRV, ~0u when no cloud shadow was built
            float    CloudShadowExtent;

            float    CloudShadowCenter[2];
            RHI::TGPUSpan<FGPUFogVolume> FogVolumes;   // offset 56, 8-aligned
        };
        static_assert(sizeof(FFroxelInjectPushConstants) <= 128, "Froxel inject PC must fit 128B");
        static_assert(offsetof(FFroxelInjectPushConstants, FogVolumes) % 8 == 0, "PC pointer must be 8-aligned");

        struct FCloudShadowPushConstants
        {
            // Mirrors FCloudMedium in Includes/CloudCommon.slang.
            uint32 NoiseIndex;
            float  ShapeScale;
            float  DetailScale;
            float  DetailStrength;

            float  Billow;
            float  Coverage;
            float  Density;
            float  LayerBottom;

            float  LayerTop;
            uint32 WeatherIndex;
            float  WindOffset[2];

            float  DetailWindOffset[2];
            float  _MediumPad1[2];

            uint32 ShadowUAV;
            uint32 Resolution;
            uint32 MarchSteps;
            float  HalfExtent;

            float  Center[2];
            float  Absorption;
            float  _Pad0;

            float  SunDirection[3];
            float  _Pad1;
        };
        static_assert(sizeof(FCloudShadowPushConstants) <= 128, "Cloud shadow PC must fit 128B");

        struct FFroxelIntegratePushConstants
        {
            uint32 GridSize[3];
            float  NearPlane;
            float  FogRange;
            uint32 ScatterSRV;     // bindless 3D SRV index of the scatter volume
            uint32 IntegratedUAV;  // bindless 3D UAV index of the integrated volume
            uint32 _Pad0;
        };

        struct FAtmosphereCompositePushConstants
        {
            uint32 HDRUAV;
            uint32 DepthIndex;       // bindless 2D SRV of scene depth
            uint32 ScreenW;
            uint32 ScreenH;

            uint32 AerialInScatterIndex;      // ~0u disables aerial perspective
            uint32 AerialTransmittanceIndex;
            float  AerialRange;
            float  AerialIntensity;

            uint32 CloudScatterIndex;         // ~0u disables clouds
            uint32 bFog;
            uint32 FroxelIntegratedIndex;     // bindless 3D SRV of the integrated froxel volume
            uint32 GridZ;

            float  NearPlane;
            float  FogRange;
            uint32 bVolumetric;      // froxel volume valid this frame; 0 = analytic height fog only
            float  FarShaftDistance;

            uint32 FogShaftIndex;      // bindless 2D SRV of the half-res shaft segment, ~0u keeps the far field unshadowed
            uint32 FogShaftDepthIndex; // bindless 2D SRV of the depth each shaft texel was marched against
            uint32 CloudDepthIndex;    // bindless 2D SRV of the depth each cloud texel was marched against
            float  CloudFloor;         // altitude of the cloud layer's base, so the composite can skip clouds below reach
        };
        static_assert(sizeof(FAtmosphereCompositePushConstants) == 80,
            "FAtmosphereCompositePushConstants must match AtmosphereComposite.slang::FPushConstants.");

        constexpr uint32 AtmosphereTileSize = 8;

        struct FFogShaftPushConstants
        {
            uint32 DepthIndex;
            uint32 ShaftUAV;
            uint32 ShaftDepthUAV;
            uint32 ScreenW;

            uint32 ScreenH;
            uint32 TraceW;
            uint32 TraceH;
            uint32 Steps;

            float  FogRange;
            float  ShaftDistance;
            uint32 bVolumetric;
            uint32 CloudShadowIndex;

            float  CloudShadowExtent;
            float  CloudShadowCenterX;
            float  CloudShadowCenterY;
            uint32 _Pad0;
        };
        static_assert(sizeof(FFogShaftPushConstants) == 64,
            "FFogShaftPushConstants must match FogShafts.slang::FPushConstants.");

        constexpr uint32 FogShaftTileSize = 8;
    }

    namespace AtmosphereInternal
    {
        struct FFogViewTerms
        {
            float    NearPlane         = 0.05f;
            float    FogRange          = 1.0f;
            uint32   CloudShadowIndex  = Constants::kIndexNoneU32;
            float    CloudShadowExtent = 0.0f;
            FVector2 CloudShadowCenter = FVector2(0.0f, 0.0f);
        };

        // Probe faces never build a cloud shadow map, so they publish none rather than sample an unwritten one.
        FFogViewTerms MakeFogViewTerms(const FDefaultSceneRenderer::FFrameData& Frame, const FDefaultSceneRenderer::FSceneView& View,
                                       const FSceneGlobalData& ViewGlobals, bool bProbeFace)
        {
            FFogViewTerms Terms;
            Terms.NearPlane = Math::Max(ViewGlobals.NearPlane, 0.05f);
            Terms.FogRange  = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, ViewGlobals.FarPlane);

            bool  bCloudShadows = true;
            float Extent        = 4000.0f;
            if (const CRendererSettings* RS = GetDefault<CRendererSettings>())
            {
                bCloudShadows = RS->bCloudShadows;
                Extent        = Math::Max(RS->CloudShadowExtent, 100.0f);
            }
            if (!bCloudShadows || !Frame.Volumetrics.bClouds || bProbeFace)
            {
                return Terms;
            }

            const FSceneImage& Shadow = View.Images[(int)FDefaultSceneRenderer::ENamedImage::CloudShadow];
            if (!Shadow.IsValid())
            {
                return Terms;
            }

            // Snapped to its own texel grid, or every camera step reshuffles the integral and the fog crawls.
            const float Texel = (2.0f * Extent) / (float)Math::Max(Shadow.GetSizeX(), 1u);
            const FVector3 Cam = FVector3(ViewGlobals.CameraData.Location);

            Terms.CloudShadowIndex  = (uint32)Shadow.GetResourceID();
            Terms.CloudShadowExtent = Extent;
            Terms.CloudShadowCenter = FVector2(Math::Floor(Cam.x / Texel) * Texel, Math::Floor(Cam.z / Texel) * Texel);
            return Terms;
        }
    }

    const FSceneGlobalData& FDefaultSceneRenderer::GetCurrentViewGlobals() const
    {
        const FFrameData& Frame = *RenderFrame;
        if (CurrentView == nullptr || CurrentView->bIsPrimary)
        {
            return Frame.SceneGlobalData;
        }

        if (bCapturingProbe)
        {
            const auto& Bake = Frame.ReflectionProbes;
            for (int32 Face = 0; Face < 6; ++Face)
            {
                if (Bake.FaceCullViews[Face] == CurrentCameraEarlyView)
                {
                    return Bake.FaceGlobals[Face];
                }
            }
        }

        for (const FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            if (Capture.SceneViewIndex > 0 && Capture.SceneViewIndex < SceneViews.Num() && &SceneViews[Capture.SceneViewIndex] == CurrentView)
            {
                return Capture.SceneGlobalData;
            }
        }
        return Frame.SceneGlobalData;
    }

    void FDefaultSceneRenderer::PublishFogGlobals(FSceneGlobalData& Globals) const
    {
        const FFrameData& Frame = *RenderFrame;
        const bool bHasFog      = Frame.Volumetrics.bHasFog;
        const bool bVolumetric  = bHasFog && Frame.Volumetrics.bVolumetricFog;
        const AtmosphereInternal::FFogViewTerms Fog = AtmosphereInternal::MakeFogViewTerms(Frame, *CurrentView, Globals, bCapturingProbe);

        Globals.FogParams           = Frame.Volumetrics.FogParams;
        Globals.bFogEnabled         = bHasFog ? 1u : 0u;
        Globals.FogGridZ            = FroxelGridSize.z;
        Globals.FogNearPlane        = Fog.NearPlane;
        Globals.FogRange            = Fog.FogRange;
        Globals.FogFarShaftSteps    = Frame.Volumetrics.FarShaftSteps;
        Globals.FogFarShaftDistance = Frame.Volumetrics.FarShaftDistance;

        Globals.FogIntegratedIndex = bVolumetric
            ? (uint32)CurrentView->Images[(int)ENamedImage::FroxelIntegrated].GetResourceID()
            : Constants::kIndexNoneU32;

        Globals.FogCloudShadowIndex  = Fog.CloudShadowIndex;
        Globals.FogCloudShadowExtent = Fog.CloudShadowExtent;
        Globals.FogCloudShadowCenter = Fog.CloudShadowCenter;
    }

    bool FDefaultSceneRenderer::CloudShadowMapPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const AtmosphereInternal::FFogViewTerms Fog = AtmosphereInternal::MakeFogViewTerms(Frame, *CurrentView, GetCurrentViewGlobals(), bCapturingProbe);
        if (Fog.CloudShadowIndex == Constants::kIndexNoneU32)
        {
            return false;
        }

        static const FShaderH CS = FShaderLibrary::Get("CloudShadowMap.slang");
        if (!CS)
        {
            return false;
        }

        SCENE_GPU_SCOPE(CL, "Cloud Shadow Map");

        const FSceneImage& Noise  = GetNamedImage(ENamedImage::CloudNoise);
        const FSceneImage& Shadow = GetNamedImage(ENamedImage::CloudShadow);
        if (!Noise.IsValid() || !Shadow.IsValid() || !BakeCloudNoiseIfNeeded(CL))
        {
            return false;
        }

        const int32 ShadowUAV = Shadow.GetMipUAVIndex(0);
        if (ShadowUAV < 0)
        {
            return false;
        }

        const SCloudComponent& C = Frame.Volumetrics.Clouds;
        const auto& LightData    = Frame.Lighting.LightData;

        const FVector3 SunDir = LightData.bHasSun
            ? Math::Normalize(LightData.SunDirection)
            : Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));

        FVector2 Wind       = C.WindDirection;
        const float WindLen = Math::Sqrt(Wind.x * Wind.x + Wind.y * Wind.y);
        Wind = (WindLen > 1e-4f) ? FVector2(Wind.x / WindLen, Wind.y / WindLen) : FVector2(1.0f, 0.0f);
        const float Drift = C.WindSpeed * Frame.SceneGlobalData.Time;

        int32 Steps = 8;
        if (const CRendererSettings* RS = GetDefault<CRendererSettings>())
        {
            Steps = Math::Clamp(RS->CloudShadowSteps, 1, 32);
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        FCloudShadowPushConstants PC = {};
        PC.NoiseIndex          = (uint32)Noise.GetResourceID();
        PC.WeatherIndex        = (uint32)GetNamedImage(ENamedImage::CloudWeather).GetResourceID();
        PC.ShapeScale          = Math::Max(C.ShapeScale, 100.0f);
        PC.DetailScale         = Math::Max(C.DetailScale, 10.0f);
        PC.DetailStrength      = Math::Clamp(C.DetailStrength, 0.0f, 1.0f);
        PC.Billow              = Math::Clamp(C.Billow, 0.0f, 1.0f);
        PC.Coverage            = Math::Clamp(C.Coverage, 0.0f, 1.0f);
        PC.Density             = Math::Max(C.Density, 0.0f);
        PC.LayerBottom         = Math::Max(C.LayerBottom, 100.0f);
        PC.LayerTop            = Math::Max(C.LayerTop, C.LayerBottom + 1.0f);
        const FVector2 WeatherDrift = Frame.Volumetrics.CloudWindOffset;
        PC.WindOffset[0]       = Wind.x * Drift + WeatherDrift.x;
        PC.WindOffset[1]       = Wind.y * Drift + WeatherDrift.y;
        PC.DetailWindOffset[0] = Wind.x * Drift * 2.0f + WeatherDrift.x * 2.0f;
        PC.DetailWindOffset[1] = Wind.y * Drift * 2.0f + WeatherDrift.y * 2.0f;

        PC.ShadowUAV       = (uint32)ShadowUAV;
        PC.Resolution      = Shadow.GetSizeX();
        PC.MarchSteps      = (uint32)Steps;
        PC.HalfExtent      = Fog.CloudShadowExtent;
        PC.Center[0]       = Fog.CloudShadowCenter.x;
        PC.Center[1]       = Fog.CloudShadowCenter.y;
        PC.Absorption      = 1.0f;
        PC.SunDirection[0] = SunDir.x;
        PC.SunDirection[1] = SunDir.y;
        PC.SunDirection[2] = SunDir.z;

        const uint32 Groups = RenderUtils::GetGroupCount(Shadow.GetSizeX(), 8);
        RHI::CmdDispatch(CL, MakeArgs(PC), Groups, Groups, 1u);
        // Particles light in the vertex stage, surfaces and water in the pixel stage, fog in compute.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        return true;
    }

    void FDefaultSceneRenderer::FroxelInjectPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog)
        {
            return;
        }

        const auto& LightData = Frame.Lighting.LightData;
        const AtmosphereInternal::FFogViewTerms Fog = AtmosphereInternal::MakeFogViewTerms(Frame, *CurrentView, GetCurrentViewGlobals(), bCapturingProbe);

        const bool bSunVolumetric = LightData.Lights.Count > 0
            && EnumHasAnyFlags(Frame.Lighting.Lights[0].Flags, ELightFlags::Directional)
            && EnumHasAnyFlags(Frame.Lighting.Lights[0].Flags, ELightFlags::Volumetric);

        SCENE_GPU_SCOPE(CL, "Froxel Fog Inject");

        static const FShaderH CS = FShaderLibrary::Get("VolumetricFogInject.slang");
        if (!CS)
        {
            return;
        }

        const FSceneImage& Scatter = GetNamedImage(ENamedImage::FroxelScatter);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        const uint32 NumVolumes = Math::Min((uint32)Frame.Volumetrics.FogVolumes.Num(), GFogMaxVolumes);

        FFroxelInjectPushConstants PC = {};
        PC.ScatterUAV           = (uint32)Scatter.GetMipUAVIndex(0);
        PC.GridSize[0]          = FroxelGridSize.x;
        PC.GridSize[1]          = FroxelGridSize.y;
        PC.GridSize[2]          = FroxelGridSize.z;
        PC.NearPlane            = Fog.NearPlane;
        PC.FogRange             = Fog.FogRange;
        PC.bSunVolumetric       = bSunVolumetric ? 1u : 0u;
        PC.Time                 = Frame.SceneGlobalData.Time;

        PC.CloudShadowIndex     = Fog.CloudShadowIndex;
        PC.CloudShadowExtent    = Fog.CloudShadowExtent;
        PC.CloudShadowCenter[0] = Fog.CloudShadowCenter.x;
        PC.CloudShadowCenter[1] = Fog.CloudShadowCenter.y;
        const CRendererSettings* RS = GetDefault<CRendererSettings>();
        const bool bSupersample = RS == nullptr || RS->bSupersampleVolumetricLights;
        PC.LocalLightSamples    = !Frame.Volumetrics.bLocalVolumetricLights ? 0u : (bSupersample ? 4u : 1u);
        if (NumVolumes > 0)
        {
            PC.FogVolumes = RHI::CopyTransientArray(Frame.Volumetrics.FogVolumes.GetData(), NumVolumes);
        }

        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(FroxelGridSize.x, 4),
                         RenderUtils::GetGroupCount(FroxelGridSize.y, 4),
                         RenderUtils::GetGroupCount(FroxelGridSize.z, 4));

        // Integrate reads the scatter volume next.
        RHI::Barriers::ComputeToCompute(CL);
    }

    void FDefaultSceneRenderer::FroxelIntegratePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Froxel Fog Integrate");

        static const FShaderH CS = FShaderLibrary::Get("VolumetricFogIntegrate.slang");
        if (!CS)
        {
            return;
        }

        const FSceneImage& Scatter    = GetNamedImage(ENamedImage::FroxelScatter);
        const FSceneImage& Integrated = GetNamedImage(ENamedImage::FroxelIntegrated);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        const AtmosphereInternal::FFogViewTerms Fog = AtmosphereInternal::MakeFogViewTerms(Frame, *CurrentView, GetCurrentViewGlobals(), bCapturingProbe);

        FFroxelIntegratePushConstants PC = {};
        PC.GridSize[0]    = FroxelGridSize.x;
        PC.GridSize[1]    = FroxelGridSize.y;
        PC.GridSize[2]    = FroxelGridSize.z;
        PC.NearPlane      = Fog.NearPlane;
        PC.FogRange       = Fog.FogRange;
        PC.ScatterSRV     = (uint32)Scatter.GetResourceID();
        PC.IntegratedUAV  = (uint32)Integrated.GetMipUAVIndex(0);

        // One thread per (x,y) column; each marches the full Z range.
        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(FroxelGridSize.x, 8),
                         RenderUtils::GetGroupCount(FroxelGridSize.y, 8),
                         1u);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::FogShaftPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        AtmosphereTerms.FogShaftIndex      = Constants::kIndexNoneU32;
        AtmosphereTerms.FogShaftDepthIndex = Constants::kIndexNoneU32;

        if (!Frame.Volumetrics.bHasFog || Frame.Volumetrics.FarShaftSteps == 0u)
        {
            return;
        }

        static const FShaderH CS = FShaderLibrary::Get("FogShafts.slang");
        const FSceneImage& Shaft      = GetNamedImage(ENamedImage::FogShaft);
        const FSceneImage& ShaftDepth = GetNamedImage(ENamedImage::FogShaftDepth);
        if (!CS || !Shaft.IsValid() || !ShaftDepth.IsValid())
        {
            return;
        }

        const int32 ShaftUAV      = Shaft.GetMipUAVIndex(0);
        const int32 ShaftDepthUAV = ShaftDepth.GetMipUAVIndex(0);
        if (ShaftUAV < 0 || ShaftDepthUAV < 0)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Fog Shafts");

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);
        const AtmosphereInternal::FFogViewTerms Fog = AtmosphereInternal::MakeFogViewTerms(Frame, *CurrentView, GetCurrentViewGlobals(), bCapturingProbe);

        FFogShaftPushConstants PC = {};
        PC.DepthIndex         = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.ShaftUAV           = (uint32)ShaftUAV;
        PC.ShaftDepthUAV      = (uint32)ShaftDepthUAV;
        PC.ScreenW            = HDR.GetSizeX();
        PC.ScreenH            = HDR.GetSizeY();
        PC.TraceW             = Shaft.GetSizeX();
        PC.TraceH             = Shaft.GetSizeY();
        PC.Steps              = Frame.Volumetrics.FarShaftSteps;
        PC.FogRange           = Fog.FogRange;
        PC.ShaftDistance      = Frame.Volumetrics.FarShaftDistance;
        PC.bVolumetric        = Frame.Volumetrics.bVolumetricFog ? 1u : 0u;
        PC.CloudShadowIndex   = Fog.CloudShadowIndex;
        PC.CloudShadowExtent  = Fog.CloudShadowExtent;
        PC.CloudShadowCenterX = Fog.CloudShadowCenter.x;
        PC.CloudShadowCenterY = Fog.CloudShadowCenter.y;

        DispatchCompute(CL, CS, PC,
                        RenderUtils::GetGroupCount(PC.TraceW, FogShaftTileSize),
                        RenderUtils::GetGroupCount(PC.TraceH, FogShaftTileSize), 1);

        RHI::Barriers::ComputeToCompute(CL);

        AtmosphereTerms.FogShaftIndex      = (uint32)Shaft.GetResourceID();
        AtmosphereTerms.FogShaftDepthIndex = (uint32)ShaftDepth.GetResourceID();
    }

    void FDefaultSceneRenderer::AtmosphereCompositePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        const bool bFog    = Frame.Volumetrics.bHasFog;
        const bool bAerial = AtmosphereTerms.AerialInScatterIndex != Constants::kIndexNoneU32;
        const bool bClouds = AtmosphereTerms.CloudScatterIndex != Constants::kIndexNoneU32;

        if (!bFog && !bAerial && !bClouds)
        {
            return;
        }

        static const FShaderH CS = FShaderLibrary::Get("AtmosphereComposite.slang");
        if (!CS)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Atmosphere Composite Pass", tracy::Color::Orange3);

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& Integrated = GetNamedImage(ENamedImage::FroxelIntegrated);

        const int32 HDRUAV = HDR.GetMipUAVIndex(0);
        if (HDRUAV < 0)
        {
            return;
        }

        const uint32 Width  = HDR.GetSizeX();
        const uint32 Height = HDR.GetSizeY();

        FAtmosphereCompositePushConstants PC = {};
        PC.HDRUAV     = (uint32)HDRUAV;
        PC.DepthIndex = (uint32)SceneDepth.GetResourceID();
        PC.ScreenW    = Width;
        PC.ScreenH    = Height;

        PC.AerialInScatterIndex     = AtmosphereTerms.AerialInScatterIndex;
        PC.AerialTransmittanceIndex = AtmosphereTerms.AerialTransmittanceIndex;
        PC.AerialRange              = AtmosphereTerms.AerialRange;
        PC.AerialIntensity          = AtmosphereTerms.AerialIntensity;

        PC.CloudScatterIndex     = AtmosphereTerms.CloudScatterIndex;
        PC.CloudDepthIndex       = AtmosphereTerms.CloudDepthIndex;
        PC.bFog                  = bFog ? 1u : 0u;
        PC.FroxelIntegratedIndex = (uint32)Integrated.GetResourceID();
        PC.GridZ                 = FroxelGridSize.z;

        const AtmosphereInternal::FFogViewTerms Fog = AtmosphereInternal::MakeFogViewTerms(Frame, *CurrentView, GetCurrentViewGlobals(), bCapturingProbe);
        PC.NearPlane          = Fog.NearPlane;
        PC.FogRange           = Fog.FogRange;
        PC.bVolumetric        = Frame.Volumetrics.bVolumetricFog ? 1u : 0u;
        PC.FarShaftDistance   = Frame.Volumetrics.FarShaftDistance;
        PC.FogShaftIndex      = AtmosphereTerms.FogShaftIndex;
        PC.FogShaftDepthIndex = AtmosphereTerms.FogShaftDepthIndex;

        PC.CloudFloor         = Math::Max(Frame.Volumetrics.Clouds.LayerBottom, 100.0f);

        auto Spec = [](uint32 Id, bool bOn)
        {
            return RHI::FSpecializationConstant{ .ConstantID = Id, .AsInt = bOn ? 1u : 0u, .Type = RHI::ESpecializationConstantType::UInt32 };
        };
        const RHI::FSpecializationConstant CompositeConsts[] =
        {
            Spec(13u, PC.AerialInScatterIndex != Constants::kIndexNoneU32),
            Spec(14u, PC.CloudScatterIndex != Constants::kIndexNoneU32),
            Spec(15u, PC.bFog != 0u),
            Spec(16u, PC.bVolumetric != 0u),
            Spec(17u, PC.FogShaftIndex != Constants::kIndexNoneU32),
        };
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS, TSpan<const RHI::FSpecializationConstant>(CompositeConsts, std::size(CompositeConsts))));

        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(Width,  AtmosphereTileSize),
                         RenderUtils::GetGroupCount(Height, AtmosphereTileSize), 1);

        // Translucent, particle and overlay passes load and blend HDR next, with no raster barrier in between when nothing is translucent.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute | RHI::EStageFlags::RasterColorOut,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorRead | RHI::EAccessFlags::ColorWrite);
    }

    namespace
    {
        struct FAerialLUTPushConstants
        {
            uint64   EnvAddr;
            uint32   InScatterUAV;
            uint32   TransmittanceUAV;

            FVector3 SunDirection;
            float    Range;
        };
        static_assert(sizeof(FAerialLUTPushConstants) == 32,
            "FAerialLUTPushConstants must match AerialPerspectiveLUT.slang::FPushConstants.");

        constexpr uint32 AerialTileSize = 8;
    }

    namespace
    {
        struct FCloudNoisePushConstants
        {
            uint32 NoiseUAV;
            uint32 _Pad0;
            uint32 _Pad1;
            uint32 _Pad2;
        };
        static_assert(sizeof(FCloudNoisePushConstants) == 16,
            "FCloudNoisePushConstants must match CloudNoiseBake.slang::FPushConstants.");

        struct FCloudPushConstants
        {
            uint32   ScatterUAV;
            uint32   DepthIndex;
            uint32   NoiseIndex;
            uint32   ScreenW;

            uint32   ScreenH;
            uint32   MarchSteps;
            uint32   LightSteps;
            float    MaxDistance;

            FVector3 SunDirection;
            float    LayerBottom;

            FVector3 SunColor;
            float    LayerTop;

            FVector3 AmbientColor;
            float    Coverage;

            float    Density;
            float    ShapeScale;
            float    DetailScale;
            float    DetailStrength;

            float    Billow;
            float    ForwardScattering;
            float    BackScattering;
            float    PowderStrength;

            FVector2 WindOffset;
            FVector2 DetailWindOffset;

            uint32   CloudDepthUAV;
            uint32   Stride;
            uint32   OffsetX;
            uint32   OffsetY;

            uint32   WeatherIndex;
            uint32   _Pad1;
            uint32   _Pad2;
            uint32   _Pad3;
        };
        static_assert(sizeof(FCloudPushConstants) == 160,
            "FCloudPushConstants must match VolumetricClouds.slang::FPushConstants.");

        struct FCloudResolvePushConstants
        {
            uint32 ScatterUAV;
            uint32 CloudDepthUAV;
            uint32 HistoryIndex;
            uint32 HistoryDepthIndex;

            uint32 DepthIndex;
            uint32 ScreenW;
            uint32 ScreenH;
            uint32 OffsetX;

            uint32 OffsetY;
            float  LayerBottom;
            float  LayerTop;
            float  MaxDistance;

            uint32   FreshIndex;
            uint32   _Pad0;
            FVector2 WindDelta;
        };
        static_assert(sizeof(FCloudResolvePushConstants) == 64,
            "FCloudResolvePushConstants must match VolumetricCloudsResolve.slang::FPushConstants.");

        // Visits every texel of a 2x2 block once in four frames, alternating diagonals so a still frame refreshes evenly.
        constexpr uint32 kCloudMarchOffsets[4][2] = { { 0u, 0u }, { 1u, 1u }, { 1u, 0u }, { 0u, 1u } };

        constexpr uint32 CloudTileSize      = 8;
        constexpr uint32 CloudNoiseTileSize = 4;
    }

    bool FDefaultSceneRenderer::BakeCloudNoiseIfNeeded(RHI::FCmdListH CL)
    {
        const RHI::EQueueType Queue = RHI::GetCommandListQueue(CL);
        if (CurrentView->bCloudNoiseBaked && CurrentView->CloudNoiseQueue == Queue)
        {
            return true;
        }

        static const FShaderH BakeCS = FShaderLibrary::Get("CloudNoiseBake.slang");
        const FSceneImage& Noise     = GetNamedImage(ENamedImage::CloudNoise);
        if (!BakeCS || !Noise.IsValid())
        {
            return false;
        }

        // The volume is view-independent and static, so it is generated once and kept.
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BakeCS));

        FCloudNoisePushConstants BakePC = {};
        BakePC.NoiseUAV = (uint32)Noise.GetMipUAVIndex(0);

        const uint32 BakeGroups = RenderUtils::GetGroupCount(kCloudNoiseSize, CloudNoiseTileSize);
        RHI::CmdDispatch(CL, MakeArgs(BakePC), BakeGroups, BakeGroups, BakeGroups);

        RHI::Barriers::ComputeToCompute(CL);

        static const FShaderH WeatherCS = FShaderLibrary::Get("CloudWeatherBake.slang");
        const FSceneImage& Weather = GetNamedImage(ENamedImage::CloudWeather);
        if (!WeatherCS || !Weather.IsValid())
        {
            return false;
        }

        struct FCloudWeatherPushConstants
        {
            uint32 NoiseIndex;
            uint32 WeatherUAV;
            uint32 Size;
            uint32 _Pad0;
        };
        FCloudWeatherPushConstants WeatherPC = {};
        WeatherPC.NoiseIndex = (uint32)Noise.GetResourceID();
        WeatherPC.WeatherUAV = (uint32)Weather.GetMipUAVIndex(0);
        WeatherPC.Size       = Weather.GetSizeX();

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(WeatherCS));
        const uint32 WeatherGroups = RenderUtils::GetGroupCount(WeatherPC.Size, 8u);
        RHI::CmdDispatch(CL, MakeArgs(WeatherPC), WeatherGroups, WeatherGroups, 1);

        RHI::Barriers::ComputeToCompute(CL);
        CurrentView->bCloudNoiseBaked = true;
        CurrentView->CloudNoiseQueue  = Queue;
        return true;
    }

    bool FDefaultSceneRenderer::VolumetricCloudPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        AtmosphereTerms.CloudScatterIndex = Constants::kIndexNoneU32;
        AtmosphereTerms.CloudDepthIndex   = Constants::kIndexNoneU32;

        if (!Frame.Volumetrics.bClouds)
        {
            // Off for a while means whatever the history holds is stale by the time clouds return.
            if (CurrentView != nullptr)
            {
                CurrentView->bCloudHistoryValid = false;
            }
            return false;
        }

        static const FShaderH CloudCS = FShaderLibrary::Get("VolumetricClouds.slang");
        if (!CloudCS)
        {
            return false;
        }

        static const FShaderH ResolveCS = FShaderLibrary::Get("VolumetricCloudsResolve.slang");
        if (!ResolveCS || CurrentView == nullptr)
        {
            return false;
        }

        // Last frame's targets become the history, so the march writes into the pair the composite read two frames ago.
        ElementOps::SwapElements(CurrentView->Images[(int)ENamedImage::CloudScatter], CurrentView->Images[(int)ENamedImage::CloudScatterHistory]);
        ElementOps::SwapElements(CurrentView->Images[(int)ENamedImage::CloudDepth], CurrentView->Images[(int)ENamedImage::CloudDepthHistory]);

        // Cleared until this frame's clouds are written, so a bail below leaves no half-swapped history to reproject.
        const bool bHistoryValid = CurrentView->bCloudHistoryValid;
        CurrentView->bCloudHistoryValid = false;

        const FSceneImage& Noise        = GetNamedImage(ENamedImage::CloudNoise);
        const FSceneImage& Scatter      = GetNamedImage(ENamedImage::CloudScatter);
        const FSceneImage& CloudDepth   = GetNamedImage(ENamedImage::CloudDepth);
        const FSceneImage& History      = GetNamedImage(ENamedImage::CloudScatterHistory);
        const FSceneImage& HistoryDepth = GetNamedImage(ENamedImage::CloudDepthHistory);
        const FSceneImage& March        = GetNamedImage(ENamedImage::CloudMarch);
        const FSceneImage& Depth        = GetNamedImage(ENamedImage::DepthAttachment);
        if (!Noise.IsValid() || !Scatter.IsValid() || !CloudDepth.IsValid() || !History.IsValid() || !HistoryDepth.IsValid() || !March.IsValid())
        {
            return false;
        }

        SCENE_GPU_SCOPE(CL, "Volumetric Clouds");

        if (!BakeCloudNoiseIfNeeded(CL))
        {
            return false;
        }

        const SCloudComponent& C = Frame.Volumetrics.Clouds;
        const auto& LightData    = Frame.Lighting.LightData;

        FVector3 SunDir = LightData.bHasSun
            ? Math::Normalize(LightData.SunDirection)
            : Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));

        FVector3 SunColor = FVector3(1.0f);
        if (LightData.bHasSun && LightData.Lights.Count > 0)
        {
            const FLight&  Sun    = Frame.Lighting.Lights[0];
            const FVector4 Unpack = UnpackColor(Sun.Color);
            SunColor = FVector3(Unpack.x, Unpack.y, Unpack.z) * Sun.Intensity;
        }

        const float Time     = Frame.SceneGlobalData.Time;
        FVector2 Wind        = C.WindDirection;
        const float WindLen  = Math::Sqrt(Wind.x * Wind.x + Wind.y * Wind.y);
        Wind = (WindLen > 1e-4f) ? FVector2(Wind.x / WindLen, Wind.y / WindLen) : FVector2(1.0f, 0.0f);

        const float Drift = C.WindSpeed * Time;

        const int32 ScatterUAV    = Scatter.GetMipUAVIndex(0);
        const int32 CloudDepthUAV = CloudDepth.GetMipUAVIndex(0);
        const int32 MarchUAV      = March.GetMipUAVIndex(0);
        if (ScatterUAV < 0 || CloudDepthUAV < 0 || MarchUAV < 0)
        {
            return false;
        }

        FCloudPushConstants PC = {};
        PC.ScatterUAV        = (uint32)ScatterUAV;
        PC.DepthIndex        = (uint32)Depth.GetResourceID();
        PC.NoiseIndex        = (uint32)Noise.GetResourceID();
        PC.WeatherIndex      = (uint32)GetNamedImage(ENamedImage::CloudWeather).GetResourceID();
        PC.ScreenW           = Scatter.GetSizeX();
        PC.ScreenH           = Scatter.GetSizeY();
        PC.MarchSteps        = (uint32)Math::Clamp(C.MarchSteps, 16, 256);
        PC.LightSteps        = (uint32)Math::Clamp(C.LightSteps, 1, 16);
        PC.MaxDistance       = Math::Max(C.MaxDistance, 1000.0f);
        PC.SunDirection      = SunDir;
        PC.LayerBottom       = Math::Max(C.LayerBottom, 100.0f);
        PC.SunColor          = SunColor * Math::Max(C.SunIntensity, 0.0f);
        PC.LayerTop          = Math::Max(C.LayerTop, C.LayerBottom + 1.0f);
        PC.AmbientColor      = FVector3(LightData.AmbientLight.x, LightData.AmbientLight.y, LightData.AmbientLight.z)
                             * LightData.AmbientLight.w * Math::Max(C.AmbientIntensity, 0.0f);
        PC.Coverage          = Math::Clamp(C.Coverage, 0.0f, 1.0f);
        PC.Density           = Math::Max(C.Density, 0.0f);
        PC.ShapeScale        = Math::Max(C.ShapeScale, 100.0f);
        PC.DetailScale       = Math::Max(C.DetailScale, 10.0f);
        PC.DetailStrength    = Math::Clamp(C.DetailStrength, 0.0f, 1.0f);
        PC.Billow            = Math::Clamp(C.Billow, 0.0f, 1.0f);
        PC.ForwardScattering = Math::Clamp(C.ForwardScattering, 0.0f, 0.95f);
        PC.BackScattering    = Math::Clamp(C.BackScattering, -0.95f, 0.0f);
        PC.PowderStrength    = Math::Clamp(C.PowderStrength, 0.0f, 1.0f);
        PC.WindOffset        = FVector2(Wind.x * Drift, Wind.y * Drift) + Frame.Volumetrics.CloudWindOffset;
        PC.DetailWindOffset  = FVector2(PC.WindOffset.x * C.DetailWindFactor,
                                        PC.WindOffset.y * C.DetailWindFactor);
        PC.CloudDepthUAV     = (uint32)CloudDepthUAV;

        // Only the primary view carries a previous view projection, and a cut leaves nothing worth reprojecting.
        const bool bTemporal = CurrentView->bIsPrimary && bHistoryValid && CameraCutFramesLeft == 0u;
        const uint32* Offset = kCloudMarchOffsets[CurrentView->CloudFrame++ & 3u];
        PC.Stride  = bTemporal ? 2u : 1u;
        PC.OffsetX = bTemporal ? Offset[0] : 0u;
        PC.OffsetY = bTemporal ? Offset[1] : 0u;
        PC.ScatterUAV = bTemporal ? (uint32)MarchUAV : (uint32)ScatterUAV;

        DispatchCompute(CL, CloudCS, PC,
                         RenderUtils::GetGroupCount((PC.ScreenW + PC.Stride - 1u) / PC.Stride, CloudTileSize),
                         RenderUtils::GetGroupCount((PC.ScreenH + PC.Stride - 1u) / PC.Stride, CloudTileSize), 1);

        if (bTemporal)
        {
            RHI::Barriers::ComputeToCompute(CL);

            FCloudResolvePushConstants Resolve = {};
            Resolve.ScatterUAV        = (uint32)ScatterUAV;
            Resolve.CloudDepthUAV     = (uint32)CloudDepthUAV;
            Resolve.HistoryIndex      = (uint32)History.GetResourceID();
            Resolve.HistoryDepthIndex = (uint32)HistoryDepth.GetResourceID();
            Resolve.DepthIndex        = (uint32)Depth.GetResourceID();
            Resolve.ScreenW           = PC.ScreenW;
            Resolve.ScreenH           = PC.ScreenH;
            Resolve.OffsetX           = PC.OffsetX;
            Resolve.OffsetY           = PC.OffsetY;
            Resolve.LayerBottom       = PC.LayerBottom;
            Resolve.LayerTop          = PC.LayerTop;
            Resolve.MaxDistance       = PC.MaxDistance;
            Resolve.FreshIndex        = (uint32)March.GetResourceID();
            Resolve.WindDelta         = PC.WindOffset - CurrentView->CloudHistoryWindOffset;
            DispatchCompute(CL, ResolveCS, Resolve,
                             RenderUtils::GetGroupCount(PC.ScreenW, CloudTileSize),
                             RenderUtils::GetGroupCount(PC.ScreenH, CloudTileSize), 1);
        }
        CurrentView->bCloudHistoryValid     = true;
        CurrentView->CloudHistoryWindOffset = PC.WindOffset;

        // The scatter volume feeds AtmosphereCompositePass and the cloud shadow map, both dispatches.
        RHI::Barriers::ComputeToCompute(CL);

        AtmosphereTerms.CloudScatterIndex = (uint32)Scatter.GetResourceID();
        AtmosphereTerms.CloudDepthIndex   = (uint32)CloudDepth.GetResourceID();
        return true;
    }

    bool FDefaultSceneRenderer::AerialPerspectivePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        AtmosphereTerms.AerialInScatterIndex     = Constants::kIndexNoneU32;
        AtmosphereTerms.AerialTransmittanceIndex = Constants::kIndexNoneU32;

        if (!FrameFlags.bHasEnvironment
            || !Frame.Volumetrics.bAerialPerspective
            || Frame.Volumetrics.AerialIntensity <= 0.0f)
        {
            return false;
        }

        static const FShaderH LutCS = FShaderLibrary::Get("AerialPerspectiveLUT.slang");
        if (!LutCS)
        {
            return false;
        }

        LUMINA_PROFILE_SECTION_COLORED("Aerial Perspective Pass", tracy::Color::LightSkyBlue);

        const FSceneImage& InScatter     = GetNamedImage(ENamedImage::AerialInScatter);
        const FSceneImage& Transmittance = GetNamedImage(ENamedImage::AerialTransmittance);

        if (!InScatter.IsValid() || !Transmittance.IsValid())
        {
            return false;
        }

        const float Range = Math::Max(Frame.Volumetrics.AerialRange, 100.0f);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(LutCS));

        FAerialLUTPushConstants LutPC = {};
        LutPC.EnvAddr          = RHI::CopyTransient(Frame.Volumetrics.EnvironmentParams);
        LutPC.InScatterUAV     = (uint32)InScatter.GetMipUAVIndex(0);
        LutPC.TransmittanceUAV = (uint32)Transmittance.GetMipUAVIndex(0);
        LutPC.Range            = Range;

        if (Frame.Lighting.LightData.bHasSun)
        {
            LutPC.SunDirection = Math::Normalize(Frame.Lighting.LightData.SkySunDirection);
        }
        else
        {
            LutPC.SunDirection = Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));
        }

        const uint32 LutGroups = RenderUtils::GetGroupCount(kAerialLUTSize, AerialTileSize);
        RHI::CmdDispatch(CL, MakeArgs(LutPC), LutGroups, LutGroups, 1);

        // Only AtmosphereCompositePass reads the LUT, and it is a dispatch.
        RHI::Barriers::ComputeToCompute(CL);

        AtmosphereTerms.AerialInScatterIndex     = (uint32)InScatter.GetResourceID();
        AtmosphereTerms.AerialTransmittanceIndex = (uint32)Transmittance.GetResourceID();
        AtmosphereTerms.AerialRange              = Range;
        AtmosphereTerms.AerialIntensity          = Frame.Volumetrics.AerialIntensity;
        return true;
    }
}
