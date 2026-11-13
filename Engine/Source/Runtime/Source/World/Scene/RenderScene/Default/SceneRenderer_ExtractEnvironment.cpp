#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace SceneRendererExtractEnvironmentInternal
    {
        FIBLBakeResolution ResolveIBLQuality(EIBLQuality Quality)
        {
            switch (Quality)
            {
                case EIBLQuality::Low:    return FIBLBakeResolution{ 256u,  128u, 5u, 32u };
                case EIBLQuality::Medium: return FIBLBakeResolution{ 512u,  256u, 6u, 32u };
                case EIBLQuality::Ultra:  return FIBLBakeResolution{ 2048u, 512u, 7u, 64u };
                case EIBLQuality::High:
                default:                  return FIBLBakeResolution{ 1024u, 256u, 6u, 32u };
            }
        }

        // Koschmieder extinction ln(50)/V, where contrast falls to 2%.
        constexpr float FogContrastThreshold = 3.912f;
    }

    const SEnvironmentComponent* FDefaultSceneRenderer::ExtractEnvironment(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        using namespace SceneRendererExtractEnvironmentInternal;

        LUMINA_PROFILE_SECTION("Environment Processing");

        auto& EnvironmentParams = Frame.Volumetrics.EnvironmentParams;
        auto EnvironmentView = Registry.View<ECS::Read<SEnvironmentComponent>, ECS::Without<SDisabledTag>>();
        const SEnvironmentComponent* ActiveEnv = nullptr;

        bool bHasEnvironment           = false;
        FrameFlags.bGTAO               = false;
        EnvironmentParams              = FEnvironmentParams{};
        Frame.Volumetrics.EnvironmentMapID    = -1;
        Frame.Volumetrics.EnvironmentMapWidth = 0;
        // Set true below if any IBL input differs from the last bake snapshot.
        Frame.Volumetrics.bIBLDirty                = false;
        Frame.Volumetrics.bIBLConvolutionDirty     = false;

        EnvironmentView.ForEach([&bHasEnvironment, &Frame, &EnvironmentParams, &ActiveEnv] (const SEnvironmentComponent& Env)
        {
            ActiveEnv = &Env;

            // bRenderSky gates the sky pass; ambient/skylight still flow when off (indoor scenes).
            bHasEnvironment = Env.bRenderSky;

            if (Env.SkyMode == ESkyMode::HDRI)
            {
                if (CTexture* EnvMap = Env.EnvironmentMap.Get())
                {
                    const int32 EnvMapID = EnvMap->GetResourceID();
                    if (EnvMapID >= 0)
                    {
                        Frame.Volumetrics.EnvironmentMapID    = EnvMapID;
                        Frame.Volumetrics.EnvironmentMapWidth = EnvMap->GetTextureResource().ImageDescription.Extent.x;
                    }
                }
            }

            // Misc.x carries sky mode as float-cast uint; shader pulls it back via asuint().
            const uint32 SkyModeBits = (Env.SkyMode == ESkyMode::SolidColor) ? GSkyMode_SolidColor
                                    : (Env.SkyMode == ESkyMode::Gradient)   ? GSkyMode_Gradient
                                    : (Env.SkyMode == ESkyMode::HDRI)       ? GSkyMode_HDRI
                                                                            : GSkyMode_Dynamic;
            const float SkyModeAsFloat = std::bit_cast<float>(SkyModeBits);

            EnvironmentParams.SolidSkyColor = FVector4(Env.SolidSkyColor, 0.0f);
            EnvironmentParams.ZenithColor   = FVector4(Env.ZenithColor, Env.HorizonExponent);
            EnvironmentParams.HorizonColor  = FVector4(Env.HorizonColor, 0.0f);
            EnvironmentParams.GroundColor   = FVector4(Env.GroundColor, 0.0f);
            EnvironmentParams.SunTint       = FVector4(Env.SunColorTint, Env.SunIntensity);
            EnvironmentParams.Misc          = FVector4(SkyModeAsFloat,
                                                        Env.SunDiscScale,
                                                        Env.SkyExposure,
                                                        Env.MieAnisotropy);

            EnvironmentParams.NightSkyColor = FVector4(Env.NightSkyColor, Env.NightBrightness);
            EnvironmentParams.StarParams    = FVector4(Env.StarDensity,
                                                        Env.StarBrightness,
                                                        Env.StarTwinkleSpeed,
                                                        Env.StarSize);
            EnvironmentParams.MoonParams    = FVector4(Env.MoonSize,
                                                        Env.MoonGlowSize,
                                                        Env.MoonBrightness,
                                                        Env.bMoonOpposeSun ? 1.0f : 0.0f);
            EnvironmentParams.MoonDirection = FVector4(Env.MoonDirection, 0.0f);
            EnvironmentParams.GalaxyParams  = FVector4(Env.GalaxyIntensity, Env.GalaxyTilt, 0.0f, 0.0f);

            const float HDRIYaw = Math::Radians(Env.HDRIRotation);
            EnvironmentParams.HDRIParams    = FVector4(Math::Max(Env.HDRIIntensity, 0.0f),
                                                        std::cos(HDRIYaw),
                                                        std::sin(HDRIYaw),
                                                        0.0f);

            // Only the dynamic sky shares this atmosphere; an HDRI's haze is already in its pixels.
            Frame.Volumetrics.bAerialPerspective = Env.bAerialPerspective
                                                && Env.SkyMode == ESkyMode::Dynamic;
            Frame.Volumetrics.AerialRange     = Math::Max(Env.AerialPerspectiveRange, 100.0f);
            Frame.Volumetrics.AerialIntensity = Math::Clamp(Env.AerialPerspectiveIntensity, 0.0f, 1.0f);
        });

        FrameFlags.bHasEnvironment = bHasEnvironment;

        Frame.Volumetrics.IBLResolution = ActiveEnv
            ? ResolveIBLQuality(ActiveEnv->IBLQuality)
            : LastExtractedIBLResolution;

        return ActiveEnv;
    }

    void FDefaultSceneRenderer::ExtractSkyLight(ECS::FRegistry& Registry, FFrameData& Frame, const SEnvironmentComponent* ActiveEnv)
    {
        LUMINA_PROFILE_SECTION("Skylight Processing");

        auto& LightData = Frame.Lighting.LightData;
        auto SkyLightView = Registry.View<ECS::Read<SSkyLightComponent>, ECS::Without<SDisabledTag>>();

        LightData.AmbientLight = FVector4(0.0f);

        SkyLightView.ForEach([&LightData, ActiveEnv] (const SSkyLightComponent& Sky)
        {
            if (!Sky.bAffectsWorld)
            {
                return;
            }

            FVector3 AmbientRGB = Sky.AmbientColor;
            if (Sky.bAmbientFromSky && ActiveEnv)
            {
                if (ActiveEnv->SkyMode == ESkyMode::SolidColor)
                {
                    AmbientRGB = ActiveEnv->SolidSkyColor;
                }
                else if (ActiveEnv->SkyMode == ESkyMode::Gradient)
                {
                    // 70/30 zenith/horizon matches what an upward-facing surface would integrate.
                    AmbientRGB = ActiveEnv->ZenithColor * 0.7f + ActiveEnv->HorizonColor * 0.3f;
                }
                // Dynamic and HDRI always have the baked irradiance cube, which every consumer prefers.
            }
            // The irradiance cube darkens with the sun, but this flat term, which clouds read, would otherwise hold noon brightness all night.
            if (ActiveEnv && ActiveEnv->SkyMode == ESkyMode::Dynamic && LightData.bHasSun)
            {
                constexpr float NightAmbient = 0.04f;
                const float SunElevation = Math::Normalize(LightData.SkySunDirection).y;
                AmbientRGB *= Math::Lerp(NightAmbient, 1.0f, Math::SmoothStep(-0.15f, 0.1f, SunElevation));
            }
            LightData.AmbientLight = FVector4(AmbientRGB, Sky.Intensity);
        });

        LightData.bHasIBL = FrameFlags.bHasEnvironment ? 1u : 0u;
    }

    // Reads neither the sun nor the weather, so it runs beside the other emitters; the weather's cloud override applies after the wait.
    void FDefaultSceneRenderer::ScheduleFogExtract(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SCOPE();
        auto Transforms    = Registry.GetStorage<STransformComponent>();
        auto CloudView     = Registry.View<ECS::Read<SCloudComponent>, ECS::Without<SDisabledTag>>();
        auto FogView       = Registry.View<ECS::Read<SExponentialHeightFogComponent>, ECS::Without<SDisabledTag>>();
        auto FogVolumeView = Registry.View<ECS::Read<SLocalFogVolumeComponent>, ECS::Without<SDisabledTag>>();

        Jobs::Spawn(Jobs::EJobPriority::Normal, ExtractLatch, [this, &Frame, Transforms, CloudView, FogView, FogVolumeView]
        {
            ExtractFog(Frame, CloudView, FogView, FogVolumeView, Transforms);
        });
    }

    void FDefaultSceneRenderer::ExtractFog(FFrameData& Frame, const auto& CloudView, const auto& FogView, const auto& FogVolumeView, const auto& Transforms)
    {
        using namespace SceneRendererExtractEnvironmentInternal;

        LUMINA_PROFILE_SECTION("Fog Processing");

        Frame.Volumetrics.bHasFog = false;
        Frame.Volumetrics.bClouds = false;
        CloudView.ForEach([&Frame] (const SCloudComponent& Cloud)
        {
            if (!Cloud.bEnabled || Cloud.Coverage <= 0.0f || Cloud.Density <= 0.0f)
            {
                return;
            }
            Frame.Volumetrics.bClouds = true;
            Frame.Volumetrics.Clouds  = Cloud;
        });

        Frame.Volumetrics.bVolumetricFog = false;
        Frame.Volumetrics.FogParams      = FExponentialHeightFogParams{};

        FogView.ForEach([&Frame, &Transforms] (ECS::FEntity Entity, const SExponentialHeightFogComponent& Fog)
        {
            if (!Fog.bEnabled || Fog.FogVisibilityDistance <= 0.0f)
            {
                return;
            }

            const float FogDensity = FogContrastThreshold / Math::Max(Fog.FogVisibilityDistance, 1.0f);

            float BaseHeight = Fog.FogBaseHeight;
            if (const STransformComponent* Transform = Transforms.TryGet(Entity))
            {
                BaseHeight += Transform->GetWorldLocationCached().y;
            }

            FExponentialHeightFogParams& P = Frame.Volumetrics.FogParams;
            P.InscatteringColor = FVector4(Fog.FogInscatteringColor, FogDensity);
            P.HeightParams      = FVector4(Fog.FogHeightFalloff, BaseHeight,
                                            Fog.FogStartDistance, Fog.FogMaxOpacity);
            P.DirectionalColor  = FVector4(Fog.DirectionalInscatteringColor,
                                            Fog.DirectionalInscatteringExponent);
            P.VolumetricParams  = FVector4(Fog.VolumetricScatteringIntensity,
                                            Fog.VolumetricAnisotropy,
                                            Fog.VolumetricMaxDistance,
                                            Fog.DirectionalInscatteringStartDistance);
            // Phase attenuation reuses MultiScatterFalloff, so separate sliders could only conflict.
            P.MultiScatterParams = FVector4((float)Math::Clamp(Fog.MultiScatterOctaves, 1, 4),
                                            Fog.MultiScatterFalloff,
                                            Fog.MultiScatterShadowLeak,
                                            Fog.MultiScatterFalloff);

            const float NoiseStrength = Fog.bDensityNoise ? Math::Clamp(Fog.NoiseStrength, 0.0f, 1.0f) : 0.0f;
            P.NoiseParams = FVector4(NoiseStrength,
                                     1.0f / Math::Max(Fog.NoiseScale, 1.0f),
                                     (float)Math::Clamp(Fog.NoiseOctaves, 1, 4),
                                     Math::Clamp(Fog.NoiseDetailGain, 0.0f, 1.0f));

            FVector3 NoiseWind = Fog.NoiseWindDirection;
            const float WindLen = Math::Length(NoiseWind);
            NoiseWind = WindLen > 1e-4f ? (NoiseWind / WindLen) * Math::Max(Fog.NoiseWindSpeed, 0.0f)
                                        : FVector3(0.0f);
            // Capped at the froxel range, since noise still fading at the hand-off reads as a seam.
            const float NoiseFade = Math::Min(Math::Max(Fog.NoiseFadeDistance, 1.0f),
                                              Math::Max(Fog.VolumetricMaxDistance, 1.0f));
            P.NoiseWind = FVector4(NoiseWind, NoiseFade);

            Frame.Volumetrics.FarShaftSteps    = (uint32)Math::Clamp(Fog.FarShaftSteps, 0, 64);
            Frame.Volumetrics.FarShaftDistance = Math::Max(Fog.FarShaftDistance, 1.0f);

            Frame.Volumetrics.bHasFog        = true;
            Frame.Volumetrics.bVolumetricFog = Fog.bVolumetricFog;
        });

        Frame.Volumetrics.FogVolumes.Reset();
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog)
        {
            return;
        }

        FogVolumeView.ForEach([&](ECS::FEntity Entity, const SLocalFogVolumeComponent& Volume)
        {
            if (!Volume.bEnabled)
            {
                return;
            }

            if (static_cast<uint32>(Frame.Volumetrics.FogVolumes.Num()) >= GFogMaxVolumes)
            {
                LOG_WARN_ONCE("[Rendering] More than {} local fog volumes reach the view (entity {} is dropped), so raise GFogMaxVolumes or disable the extra volumes.", GFogMaxVolumes, Entity.GetPacked());
                return;
            }

            FVector3 Extent = FVector3(Math::Max(Volume.Extent.x, 0.01f),
                                       Math::Max(Volume.Extent.y, 0.01f),
                                       Math::Max(Volume.Extent.z, 0.01f));
            if (Volume.bSphere)
            {
                // A sphere is a uniform scale, so the shader's radial test stays a plain length.
                const float R = Math::Max(Extent.x, Math::Max(Extent.y, Extent.z));
                Extent = FVector3(R, R, R);
            }

            const float Density = FogContrastThreshold / Math::Max(Volume.VisibilityDistance, 1.0f);

            FGPUFogVolume& Item = Frame.Volumetrics.FogVolumes.EmplaceGetRef();
            Item.WorldToVolume = Math::Inverse(Math::Scale(Transforms.Get(Entity).GetWorldMatrixCached(), Extent));
            Item.Albedo        = FVector4(Volume.Albedo, Density);
            Item.Emissive      = FVector4(Volume.EmissiveColor * Math::Max(Volume.EmissiveIntensity, 0.0f), 0.0f);
            Item.Params        = FVector4(Volume.bSphere ? 1.0f : 0.0f,
                                          Math::Clamp(Volume.EdgeSoftness, 0.001f, 1.0f),
                                          Math::Max(Volume.ScatteringIntensity, 0.0f),
                                          0.0f);
        });
    }
}
