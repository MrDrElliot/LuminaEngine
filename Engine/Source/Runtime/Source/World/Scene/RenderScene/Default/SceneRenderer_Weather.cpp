#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/WeatherComponent.h"
#include "World/Entity/Systems/WeatherInteractionState.h"

namespace Lumina
{
    namespace
    {
        TConsoleVar<bool> CVarWeather(
            "r.Weather",
            true,
            "Renders the weather a Weather component plays. Off skips every weather pass and surface effect.");

        TConsoleVar<int32> CVarWeatherOcclusionResolution(
            "r.Weather.OcclusionResolution",
            1024,
            "Texels across the map that keeps rain and wetness out from under roofs.");

        TConsoleVar<float> CVarWeatherOcclusionExtent(
            "r.Weather.OcclusionExtent",
            48.0f,
            "Half width in meters of the rain occlusion map around the camera.");

        TConsoleVar<int32> CVarWeatherMaxDrops(
            "r.Weather.MaxDrops",
            262144,
            "Upper bound on rain drops simulated around the camera.");

        TConsoleVar<int32> CVarWeatherSplashesPerFrame(
            "r.Weather.SplashesPerFrame",
            128,
            "Splashes spawned per frame at full rain intensity and splash density.");

        TConsoleVar<float> CVarWeatherStreakExposure(
            "r.Weather.StreakExposure",
            0.022f,
            "Seconds of fall each rain streak smears across, before the preset's streak length scale.");

        TConsoleVar<int32> CVarWeatherDebug(
            "r.Weather.Debug",
            0,
            "Paints a weather surface term over the scene. 1 rain exposure, 2 flatness, 3 puddle mask, 4 wetness, 5 ripple field, 6 foam and prints, 7 snow cover.");

        TConsoleVar<bool> CVarWeatherInteraction(
            "r.Weather.Interaction",
            true,
            "Lets Weather Interactors ripple water and puddles, splash, churn foam, leave prints and get wet.");

        TConsoleVar<int32> CVarWeatherFieldResolution(
            "r.Weather.Interaction.Resolution",
            512,
            "Texels across the ripple field that follows the camera.");

        TConsoleVar<float> CVarWeatherFieldExtent(
            "r.Weather.Interaction.Extent",
            16.0f,
            "Half width in meters of the ripple field around the camera.");

        TConsoleVar<float> CVarWeatherFieldRippleGain(
            "r.Weather.Interaction.RippleGain",
            1.0f,
            "How strongly the field's ripples bend the normals of water and puddles.");

        TConsoleVar<float> CVarWeatherFieldFoamGain(
            "r.Weather.Interaction.FoamGain",
            1.0f,
            "How much foam wakes and splashes leave on open water.");

        TConsoleVar<float> CVarWeatherFieldTrailGain(
            "r.Weather.Interaction.TrailGain",
            1.0f,
            "How deep prints read where interactors pressed into wet ground.");

        // Fixed so a ripple crosses the field at the same speed whatever the frame rate.
        constexpr float  kFieldStepRate            = 60.0f;
        constexpr uint32 kMaxFieldStepsPerFrame    = 4u;
        // A wave crosses a texel in about three steps, slow enough to read as water rather than a shock front.
        constexpr float  kFieldCourant             = 0.12f;
        constexpr float  kFieldWaterDamping        = 0.996f;
        constexpr float  kFieldShallowDamping      = 0.975f;
        constexpr float  kFieldFoamDecay           = 0.985f;
        constexpr float  kFieldTrailDecay          = 0.9995f;
        // Prints in snow outlast a wet footprint by minutes.
        constexpr float  kSnowTrailDecay           = 0.99997f;
        // Long enough for the last wake to die out before the field stops and its images are cleared.
        constexpr uint32 kFieldSettleFrames        = 600u;
        constexpr uint32 kSnowSettleFrames         = 36000u;
        constexpr float  kSnowCoverThreshold       = 0.01f;
        constexpr uint32 kMaxWeatherWaterBodies    = 16u;
        constexpr uint32 kInteractionSplashCapacity = 512u;
        constexpr uint32 kInteractionSplashVertices = (1u + 10u + 24u) * 6u;
        // Frames a splash may still be in flight after the last one spawned.
        constexpr uint32 kSplashDrawFrames         = 180u;

        // Seconds of fall a flake smears across, short since a slow tumbling flake barely blurs.
        constexpr float  kSnowExposure             = 0.012f;

        constexpr float  kOcclusionHalfDepth       = 150.0f;
        constexpr uint32 kSplashCapacity           = 8192u;
        constexpr uint32 kWeatherIdleReleaseFrames = 600u;
        constexpr double kWeatherTimeWrap          = 3600.0;
        // The drop field repeats every tile, so wrapping its offsets on a whole number of tiles never moves a drop.
        constexpr double kFieldWrapTiles           = 1024.0;

        uint32 ClampedCount(float Value, uint32 Max)
        {
            if (!(Value > 0.0f))
            {
                return 0u;
            }
            return Value >= (float)Max ? Max : (uint32)Value;
        }

        struct FRainSplashGPU
        {
            FVector3 Position;
            float    SpawnTime;
            uint32   PackedNormal;
            uint32   PackedLightRG;
            uint32   PackedLightB;
            uint32   Seed;
        };
        static_assert(sizeof(FRainSplashGPU) == 32, "FRainSplashGPU must match FRainSplash in RainCommon.slang.");

        struct FRainDropsPC
        {
            RHI::TGPUSpan<FUIntVector4> Visible;
            RHI::TGPUSpan<uint32>       Args;
            uint32                      NumDrops;
            float                       Exposure;
            uint32                      bSnow;
            uint32                      _Pad0;
        };
        static_assert(sizeof(FRainDropsPC) == 48, "FRainDropsPC must match RainDrops.slang.");

        struct FRainSplashSpawnPC
        {
            RHI::TGPUSpan<FRainSplashGPU> Splashes;
            uint32                        Cursor;
            uint32                        Count;
            uint32                        FrameSeed;
            float                         SpawnWindow;
            uint32                        GBufferB;
            uint32                        VisBuffer;
            uint32                        SlotImage;
            uint32                        _Pad0;
        };
        static_assert(sizeof(FRainSplashSpawnPC) == 48, "FRainSplashSpawnPC must match RainSplashSpawn.slang.");

        struct FRainDrawPC
        {
            RHI::TGPUSpan<FUIntVector4>   Visible;
            RHI::TGPUSpan<FRainSplashGPU> Splashes;
            float                         Exposure;
            float                         SoftFadeDistance;
            uint32                        _Pad0;
            uint32                        _Pad1;
        };
        static_assert(sizeof(FRainDrawPC) == 48, "FRainDrawPC must match FRainDrawArgs in RainCommon.slang.");

        struct FRainHazePC
        {
            uint32 HDRUAV;
            uint32 Width;
            uint32 Height;
            uint32 _Pad0;
        };
        static_assert(sizeof(FRainHazePC) == 16, "FRainHazePC must match RainHaze.slang.");

        struct FWeatherFieldSimPC
        {
            RHI::TGPUSpan<FWeatherContactGPU>   Contacts;
            RHI::TGPUSpan<FWeatherWaterBodyGPU> Waters;
            uint32      Previous;
            uint32      Current;
            FIntVector2 Shift;
            FVector2    Origin;
            float       TexelWorld;
            uint32      Resolution;
            float       Courant;
            float       WaterDamping;
            float       ShallowDamping;
            float       FoamDecay;
            float       TrailDecay;
            uint32      bStep;
        };
        static_assert(sizeof(FWeatherFieldSimPC) == 88, "FWeatherFieldSimPC must match WeatherFieldSim.slang.");

        struct FWeatherImpactSpawnPC
        {
            RHI::TGPUSpan<FWeatherImpactGPU> Impacts;
            RHI::TGPUSpan<FRainSplashGPU>    Splashes;
            uint32                           Cursor;
            float                            Time;
            uint32                           _Pad0;
            uint32                           _Pad1;
        };
        static_assert(sizeof(FWeatherImpactSpawnPC) == 48, "FWeatherImpactSpawnPC must match WeatherImpactSpawn.slang.");

        // Matches InteractorSlot in Includes/WeatherInteraction.slang, which probes the table the same way.
        uint32 InteractorHash(uint32 X)
        {
            X ^= X >> 16u;
            X *= 0x7FEB352Du;
            X ^= X >> 15u;
            X *= 0x846CA68Bu;
            X ^= X >> 16u;
            return X;
        }

        FVector4 MatrixRow(const FMatrix4& Matrix, int32 Row)
        {
            return FVector4(Matrix[0][Row], Matrix[1][Row], Matrix[2][Row], Matrix[3][Row]);
        }

        constexpr uint32 kRainStreakVertices = 6u;
        constexpr uint32 kSplashVertices     = 30u;

        RHI::FBlendDesc PremultipliedBlend()
        {
            RHI::FBlendDesc Blend;
            Blend.bBlendEnable   = true;
            Blend.ColorOp        = RHI::EBlend::Add;
            Blend.AlphaOp        = RHI::EBlend::Add;
            Blend.SrcColorFactor = RHI::EFactor::One;
            Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;
            return Blend;
        }
    }

    void FDefaultSceneRenderer::ExtractWeather(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        Frame.Weather = {};
        if (!CVarWeather.GetValue())
        {
            return;
        }

        const ECS::TComponentStorage<SWeatherComponent> Storage = Registry.FindStorage<SWeatherComponent>();
        if (!Storage || Storage.IsEmpty())
        {
            return;
        }

        const FWeatherState* State = Registry.Ctx().Find<FWeatherState>();
        if (State == nullptr || !State->bActive)
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Weather Extract");

        FFrameData::FWeather& Weather = Frame.Weather;
        Weather.bActive = true;
        Weather.State   = *State;

        const FRainParams& Rain = State->Params.Rain;
        const FSnowParams& Snow = State->Params.Snow;
        const float Tile = Rain.Radius * 2.0f;
        const uint32 MaxDrops = (uint32)Math::Clamp(CVarWeatherMaxDrops.GetValue(), 0, 1 << 22);
        if (Rain.Intensity > 0.001f)
        {
            Weather.NumDrops    = ClampedCount(Rain.DropDensity * Rain.Intensity * Tile * Tile * Tile, MaxDrops);
            Weather.NumSplashes = ClampedCount((float)CVarWeatherSplashesPerFrame.GetValue() * Rain.SplashDensity * Rain.Intensity,
                                               kSplashCapacity / 4u);
        }
        if (Snow.Intensity > 0.001f)
        {
            const float SnowTile = Snow.Radius * 2.0f;
            Weather.NumFlakes = ClampedCount(Snow.FlakeDensity * Snow.Intensity * SnowTile * SnowTile * SnowTile, MaxDrops);
        }
        Weather.bDrops = Weather.NumDrops > 0u;

        const bool bWetSurfaces = State->Wetness > 0.001f || State->PuddleLevel > 0.001f || State->SnowCover > 0.001f;
        if (!Weather.bDrops && Weather.NumFlakes == 0u && !bWetSurfaces)
        {
            return;
        }

        // The preset's steady wind rather than the gusting one, or every gust would swing the rain shadows and the wetness under them.
        const FWindParams& Wind = State->Params.Wind;
        const FVector2 SteadyWind = Wind.Direction * Wind.Speed;
        // Snow drifts under the eaves on a slant, so without rain the map follows the flakes.
        const bool  bSnowFall     = Rain.Intensity <= 0.001f && Snow.Intensity > 0.001f;
        const float FallSpeed     = bSnowFall ? Math::Max(Snow.FallSpeed, 0.2f) : Math::Max(Rain.FallSpeed, 0.5f);
        const float WindInfluence = bSnowFall ? Snow.WindInfluence : Rain.WindInfluence;
        const FVector3 FallVelocity(SteadyWind.x * WindInfluence, -FallSpeed, SteadyWind.y * WindInfluence);
        const FVector3 Fall = Math::Normalize(FallVelocity);
        Weather.FallDirection = Fall;

        const uint32 Resolution = (uint32)Math::Clamp(CVarWeatherOcclusionResolution.GetValue(), 128, 4096);
        const float  Extent     = Math::Max(CVarWeatherOcclusionExtent.GetValue(), 4.0f);
        const float  TexelWorld = (Extent * 2.0f) / (float)Resolution;
        const FVector3 Up       = Math::Abs(Fall.z) < 0.99f ? FVector3(0.0f, 0.0f, 1.0f) : FVector3(1.0f, 0.0f, 0.0f);
        const FVector3 Camera   = ExtractFrame->ViewVolume.GetViewPosition();

        // Snapped to whole texels like the cascades, so the map does not crawl as the camera moves.
        const FMatrix4 Rotation = Math::LookAt(-Fall * kOcclusionHalfDepth, FVector3(0.0f), Up);
        FVector4 CenterLS = Rotation * FVector4(Camera, 1.0f);
        CenterLS.x = std::round(CenterLS.x / TexelWorld) * TexelWorld;
        CenterLS.y = std::round(CenterLS.y / TexelWorld) * TexelWorld;
        const FVector3 Center = FVector3(Math::Inverse(Rotation) * CenterLS);

        const FMatrix4 View = Math::LookAt(Center - Fall * kOcclusionHalfDepth, Center, Up);
        FMatrix4 Projection = Math::Ortho(-Extent, Extent, -Extent, Extent, 0.0f, kOcclusionHalfDepth * 2.0f);
        Projection[1][1] *= -1.0f;

        const uint32 ShadowSlot = Frame.Lighting.ShadowDataCount.fetch_add(1, std::memory_order_acquire);
        if (ShadowSlot >= (uint32)MAX_SHADOWS)
        {
            return;
        }

        const FMatrix4 ViewProjection = Projection * View;
        FLightShadowData& ShadowData = Frame.Lighting.Shadows[ShadowSlot];
        ShadowData.ViewProjection[0] = ViewProjection;
        ShadowData.Shadow[0] = {};
        ShadowData.Shadow[0].AtlasUVScale    = FVector2(1.0f);
        ShadowData.Shadow[0].ShadowMapIndex  = Constants::kIndexNone;
        ShadowData.Shadow[0].LightIndex      = Constants::kIndexNone;
        ShadowData.Shadow[0].ShadowDataIndex = (int32)ShadowSlot;

        Weather.OcclusionShadowDataIndex = (int32)ShadowSlot;
        Weather.OcclusionResolution      = Resolution;
        Weather.OcclusionViewProjection  = ViewProjection;
        Weather.OcclusionDepthRange      = kOcclusionHalfDepth * 2.0f;
        Weather.OcclusionTexelWorld      = TexelWorld;
    }

    void FDefaultSceneRenderer::ExtractWeatherInteraction(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        FFrameData::FWeatherInteraction& Out = Frame.WeatherInteraction;
        Out.bField          = false;
        Out.bReset          = false;
        Out.bFirstPassSteps = false;
        Out.bSnow           = false;
        Out.Passes          = 0u;
        Out.Shift           = FIntVector2(0);
        Out.Contacts.clear();
        Out.Impacts.clear();
        Out.InteractorTable.clear();
        Out.InteractorRecords.clear();
        Out.WaterBodies.clear();

        const FWeatherInteractionState* State = CVarWeatherInteraction.GetValue() ? Registry.Ctx().Find<FWeatherInteractionState>() : nullptr;
        const bool bSnowLying = Frame.Weather.bActive && Frame.Weather.State.SnowCover > kSnowCoverThreshold;
        // Snow keeps its prints for minutes, so the field keeps running long after the last step.
        const uint32 SettleFrames = bSnowLying ? kSnowSettleFrames : kFieldSettleFrames;
        const bool bWasIdle = WeatherFieldQuietFrames >= SettleFrames;
        if (State == nullptr && bWasIdle)
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Weather Interaction Extract");

        // A multiple of the sim's 8 by 8 groups, so no group straddles the edge.
        const uint32   Resolution = (uint32)Math::Clamp(CVarWeatherFieldResolution.GetValue(), 128, 2048) & ~7u;
        const float    HalfExtent = Math::Max(CVarWeatherFieldExtent.GetValue(), 4.0f);
        const float    TexelWorld = HalfExtent * 2.0f / (float)Resolution;
        const FVector3 Camera     = ExtractFrame->ViewVolume.GetViewPosition();
        const FIntVector2 OriginTexel((int32)Math::Floor((Camera.x - HalfExtent) / TexelWorld),
                                      (int32)Math::Floor((Camera.z - HalfExtent) / TexelWorld));
        const FVector2 Min((float)OriginTexel.x * TexelWorld, (float)OriginTexel.y * TexelWorld);
        const FVector2 Max = Min + FVector2(HalfExtent * 2.0f);

        const auto InsideWindow = [&](const FVector3& Point, float Reach)
        {
            return Point.x + Reach > Min.x && Point.x - Reach < Max.x && Point.z + Reach > Min.y && Point.z - Reach < Max.y;
        };

        if (State != nullptr)
        {
            for (const FWeatherContact& Contact : State->Contacts)
            {
                if (!InsideWindow(Contact.Position, Contact.Radius) && !InsideWindow(Contact.PrevPosition, Contact.Radius))
                {
                    continue;
                }
                FWeatherContactGPU& GPU = Out.Contacts.emplace_back();
                GPU.Position       = Contact.Position;
                GPU.Radius         = Contact.Radius;
                GPU.PrevPosition   = Contact.PrevPosition;
                GPU.SurfaceY       = Contact.SurfaceY;
                GPU.PrevSurfaceY   = Contact.PrevSurfaceY;
                GPU.RippleStrength = Contact.RippleStrength;
                GPU.FoamStrength   = Contact.FoamStrength;
                GPU.TrailStrength  = Contact.TrailStrength;
                GPU.Surface        = (uint32)Contact.Surface;
                GPU._Pad[0] = GPU._Pad[1] = GPU._Pad[2] = 0u;
            }

            for (const FWeatherImpact& Impact : State->Impacts)
            {
                if (!InsideWindow(Impact.Position, 0.0f) || Out.Impacts.size() >= kInteractionSplashCapacity / 4u)
                {
                    continue;
                }
                FWeatherImpactGPU& GPU = Out.Impacts.emplace_back();
                GPU.Position = Impact.Position;
                GPU.Scale    = Impact.Scale;
                GPU.Normal   = Impact.Normal;
                GPU.Surface  = (uint32)Impact.Surface;
                GPU.Seed     = InteractorHash(Impact.Entity ^ (WeatherFieldSerial * 0x9E3779B9u) ^ (uint32)Out.Impacts.size());
                GPU._Pad[0] = GPU._Pad[1] = GPU._Pad[2] = 0u;
            }

            if (!State->Interactors.empty())
            {
                const uint32 Capacity = (uint32)Math::NextPowerOfTwo((int)Math::Max<size_t>(State->Interactors.size() * 2u, 16u));
                Out.InteractorTable.resize(Capacity, FUIntVector2(Constants::kIndexNoneU32, 0u));
                for (const FWeatherInteractorState& Interactor : State->Interactors)
                {
                    uint32 Slot = InteractorHash(Interactor.EntityID) & (Capacity - 1u);
                    while (Out.InteractorTable[Slot].x != Constants::kIndexNoneU32 && Out.InteractorTable[Slot].x != Interactor.EntityID)
                    {
                        Slot = (Slot + 1u) & (Capacity - 1u);
                    }
                    Out.InteractorTable[Slot] = FUIntVector2(Interactor.EntityID, (uint32)Out.InteractorRecords.size());
                    Out.InteractorRecords.push_back(FVector4(Interactor.RootY, Interactor.Soak, Interactor.WaterlineHeight, Interactor.WaterlineWetness));
                }
            }
        }

        Out.bSnow = bSnowLying;
        const bool bDisturbed = !Out.Contacts.empty() || !Out.Impacts.empty();
        WeatherFieldQuietFrames = bDisturbed ? 0u : (bWasIdle ? WeatherFieldQuietFrames : WeatherFieldQuietFrames + 1u);
        Out.bField = WeatherFieldQuietFrames < SettleFrames;

        if (Out.bField)
        {
            Out.bReset = bWasIdle || Resolution != WeatherFieldResolution || TexelWorld != WeatherFieldTexelWorld;
            if (Out.bReset)
            {
                WeatherFieldAccumulator = 0.0f;
                WeatherFieldParity      = 0u;
            }
            else
            {
                Out.Shift = OriginTexel - WeatherFieldOriginTexel;
            }

            WeatherFieldAccumulator += Math::Clamp(Frame.SceneGlobalData.DeltaTime, 0.0f, 0.1f);
            const uint32 Steps = Math::Min((uint32)(WeatherFieldAccumulator * kFieldStepRate), kMaxFieldStepsPerFrame);
            WeatherFieldAccumulator = Math::Max(WeatherFieldAccumulator - (float)Steps / kFieldStepRate, 0.0f);

            // Every frame takes its contacts even when no step is due, since a skipped frame would lose the water they moved.
            Out.Passes          = Math::Max(Steps, 1u);
            Out.bFirstPassSteps = Steps > 0u;
            Out.InputImage      = WeatherFieldParity;
            Out.OutputImage     = WeatherFieldParity ^ (Out.Passes & 1u);
            Out.Resolution      = Resolution;
            Out.TexelWorld      = TexelWorld;
            Out.OriginTexel     = OriginTexel;
            Out.Serial          = ++WeatherFieldSerial;

            WeatherFieldParity      = Out.OutputImage;
            WeatherFieldOriginTexel = OriginTexel;
            WeatherFieldResolution  = Resolution;
            WeatherFieldTexelWorld  = TexelWorld;
        }

        // Only the field reads the water bodies, to tell open water from puddles.
        if (!Out.bField)
        {
            return;
        }

        const ECS::TComponentStorage<STransformComponent> Transforms = Registry.GetStorage<STransformComponent>();
        Registry.View<SWaterComponent>(ECS::TExclude<SDisabledTag>{}).ForEach([&](ECS::FEntity Entity, const SWaterComponent& Water)
        {
            if (Out.WaterBodies.size() >= kMaxWeatherWaterBodies)
            {
                return;
            }
            const STransformComponent& Transform = Transforms.Get(Entity);
            const FMatrix4 WorldToWater = Math::Inverse(WaterToWorld(Water, Transform.GetWorldMatrix()));

            FWeatherWaterBodyGPU& Body = Out.WaterBodies.emplace_back();
            Body.WorldToWaterX = MatrixRow(WorldToWater, 0);
            Body.WorldToWaterZ = MatrixRow(WorldToWater, 2);
            Body.Params        = FVector4(Transform.GetWorldLocation().y,
                                          Water.bInteractive ? Water.InteractionRippleScale : 0.0f,
                                          Water.bInteractive ? Water.InteractionFoamScale : 0.0f,
                                          Water.RainRippleScale);
        });
    }

    void FDefaultSceneRenderer::ApplyWeatherToClouds(FFrameData& Frame)
    {
        Frame.Volumetrics.CloudWindOffset = FVector2(0.0f);
        if (!Frame.Weather.bActive)
        {
            return;
        }

        const FWeatherState& State = Frame.Weather.State;
        Frame.Volumetrics.CloudWindOffset = State.CloudWindOffset;

        const FCloudWeatherParams& Override = State.Params.Clouds;
        if (Override.Weight <= 0.001f)
        {
            return;
        }

        SCloudComponent Clouds = Frame.Volumetrics.bClouds ? Frame.Volumetrics.Clouds : SCloudComponent{};
        const float W = Math::Saturate(Override.Weight);
        Clouds.Coverage         = Math::Lerp(Clouds.Coverage, Override.Coverage, W);
        Clouds.Density          = Math::Lerp(Clouds.Density, Override.Density, W);
        Clouds.Billow           = Math::Lerp(Clouds.Billow, Override.Billow, W);
        Clouds.DetailStrength   = Math::Lerp(Clouds.DetailStrength, Override.DetailStrength, W);
        Clouds.SunIntensity    *= Math::Lerp(1.0f, Override.SunScale, W);
        Clouds.AmbientIntensity *= Math::Lerp(1.0f, Override.AmbientScale, W);

        Frame.Volumetrics.Clouds  = Clouds;
        Frame.Volumetrics.bClouds = Clouds.Coverage > 0.0f && Clouds.Density > 0.0f;
    }

    void FDefaultSceneRenderer::PublishWeatherGlobals(FSceneGlobalData& Globals)
    {
        const FFrameData::FWeather& Weather = RenderFrame->Weather;
        const FFrameData::FWeatherInteraction& Interaction = RenderFrame->WeatherInteraction;
        Globals.Weather = {};

        const bool bInteraction = Interaction.bField || !Interaction.InteractorRecords.empty();
        if (!Weather.bActive && !bInteraction)
        {
            return;
        }

        WeatherIdleFrames = 0u;

        FWeatherGPU GPU = {};
        if (Weather.bActive)
        {
            const bool bOcclusion = Weather.OcclusionCullView != Constants::kIndexNoneU32;
            if (bOcclusion && (!RainOcclusionImage.IsValid() || RainOcclusionImage.GetSizeX() != Weather.OcclusionResolution))
            {
                RetireSceneImage(RainOcclusionImage);

                RHI::FTextureDesc Desc;
                Desc.Type      = RHI::ETextureType::Tex2D;
                Desc.Dimension = FUIntVector3(Weather.OcclusionResolution, Weather.OcclusionResolution, 1);
                Desc.Format    = EFormat::D32;
                Desc.Usage     = RHI::EImageUsageFlags::DepthAttachment | RHI::EImageUsageFlags::Sampled;
                RainOcclusionImage = CreateSceneImage(Desc);
                RHI::SetDebugName(RainOcclusionImage.Texture, "Weather.RainOcclusion");
            }

            const FWeatherState& State = Weather.State;
            const FRainParams&   Rain  = State.Params.Rain;
            const double Tile = Math::Max((double)Rain.Radius * 2.0, 1.0);
            const double Wrap = Tile * kFieldWrapTiles;

            GPU.RainViewProjection        = Weather.OcclusionViewProjection;
            GPU.RainInverseViewProjection = Math::Inverse(Weather.OcclusionViewProjection);
            GPU.RainFall            = FVector4(Weather.FallDirection, Rain.FallSpeed);
            GPU.RainWind            = FVector4((float)std::fmod(State.RainWindX, Wrap), (float)std::fmod(State.RainWindZ, Wrap),
                                               Rain.WindInfluence, Rain.Turbulence);
            GPU.RainDrops           = FVector4(Rain.Intensity, Rain.DropDensity, Rain.DropSize, Rain.SizeVariation);
            GPU.RainLook            = FVector4(Rain.StreakLength, Rain.Brightness, Rain.Opacity, Rain.HazeDensity);
            GPU.RainTint            = FVector4(Rain.Tint, Rain.Radius);
            GPU.Surface             = FVector4(State.Wetness, State.PuddleLevel, Rain.PuddleCoverage, Rain.PuddleScale);
            GPU.Surface2            = FVector4(Rain.RippleIntensity, Rain.DripIntensity, (float)std::fmod(State.Time, kWeatherTimeWrap), Rain.SplashSize);
            GPU.Motion              = FVector4(State.Wind.x, State.Wind.y, (float)std::fmod(State.FallDistance, Wrap), (float)Tile);
            GPU.RainOcclusionIndex  = bOcclusion && RainOcclusionImage.IsValid() ? (uint32)RainOcclusionImage.GetResourceID() : Constants::kIndexNoneU32;
            GPU.OcclusionDepthRange = Weather.OcclusionDepthRange;
            GPU.OcclusionTexelWorld = Weather.OcclusionTexelWorld;
            GPU.NumDrops            = Weather.NumDrops;

            const FSnowParams& Snow = State.Params.Snow;
            const double SnowTile = Math::Max((double)Snow.Radius * 2.0, 1.0);
            GPU.Snow        = FVector4(Snow.Intensity, Snow.FlakeDensity, Snow.FlakeSize, Snow.SizeVariation);
            GPU.SnowMotion  = FVector4(Snow.FallSpeed, Snow.Flutter, Snow.WindInfluence, (float)std::fmod(State.SnowFallDistance, SnowTile * kFieldWrapTiles));
            GPU.SnowLook    = FVector4(Snow.Brightness, Snow.Opacity, Snow.Radius, (float)SnowTile);
            GPU.SnowSurface = FVector4(State.SnowCover, Snow.CoverScale, Snow.PrintDepth, 0.0f);
        }
        GPU.DebugMode = (uint32)Math::Clamp(CVarWeatherDebug.GetValue(), 0, 7);

        if (Interaction.bField)
        {
            for (FSceneImage& Image : WeatherFieldImages)
            {
                if (Image.IsValid() && Image.GetSizeX() == Interaction.Resolution)
                {
                    continue;
                }
                RetireSceneImage(Image);

                RHI::FTextureDesc Desc;
                Desc.Type      = RHI::ETextureType::Tex2D;
                Desc.Dimension = FUIntVector3(Interaction.Resolution, Interaction.Resolution, 1);
                Desc.Format    = EFormat::RGBA16_FLOAT;
                Desc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::TransferDst;
                Image = CreateSceneImage(Desc, true, true);
                RHI::SetDebugName(Image.Texture, "Weather.InteractionField");
            }

            const FSceneImage& Output = WeatherFieldImages[Interaction.OutputImage];
            const float Width = Interaction.TexelWorld * (float)Interaction.Resolution;
            GPU.InteractionField = Output.IsValid() ? (uint32)Output.GetResourceID() : Constants::kIndexNoneU32;
            GPU.FieldParams      = FVector4((float)Interaction.OriginTexel.x * Interaction.TexelWorld, (float)Interaction.OriginTexel.y * Interaction.TexelWorld,
                                            1.0f / Width, Interaction.TexelWorld);
            GPU.FieldShading     = FVector4(Math::Max(CVarWeatherFieldRippleGain.GetValue(), 0.0f), Math::Max(CVarWeatherFieldFoamGain.GetValue(), 0.0f),
                                            Math::Max(CVarWeatherFieldTrailGain.GetValue(), 0.0f), 0.0f);
        }

        if (!Interaction.InteractorRecords.empty())
        {
            GPU.InteractorTable     = RHI::CopyTransientArray(Interaction.InteractorTable.data(), Interaction.InteractorTable.size());
            GPU.Interactors         = RHI::CopyTransientArray(Interaction.InteractorRecords.data(), Interaction.InteractorRecords.size());
            GPU.InteractorTableMask = (uint32)Interaction.InteractorTable.size() - 1u;
        }
        Globals.Weather = RHI::CopyTransientArray(&GPU, 1);
    }

    void FDefaultSceneRenderer::WeatherOcclusionPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const FFrameData::FWeather& Weather = Frame.Weather;
        if (!Weather.bActive || Weather.OcclusionCullView == Constants::kIndexNoneU32 || !RainOcclusionImage.IsValid())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Rain Occlusion Pass", tracy::Color::SteelBlue);
        SCENE_GPU_SCOPE(CL, "Rain Occlusion");

        const FUIntVector2 Extent = RainOcclusionImage.GetExtent();

        RHI::FRenderPassDesc Pass;
        Pass.DepthAttachment.Texture  = RainOcclusionImage.Texture;
        Pass.DepthAttachment.LoadOp   = RHI::ELoadOp::Clear;
        Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        Pass.DepthAttachment.Color[0] = 1.0f;
        Pass.RenderArea               = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode            = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest            = RHI::EOp::Less;
        DepthDesc.DepthBias            = 1.0f;
        DepthDesc.DepthBiasSlopeFactor = 1.5f;
        RHI::CmdSetDepthStencil(CL, DepthDesc);

        for (uint32 OpaqueIdx : Frame.Geometry.OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = Frame.Geometry.DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, FShaderH{});
            DrawShadowBatch(CL, Batch, bUseMesh, Weather.OcclusionCullView, Weather.OcclusionShadowDataIndex, 0, Extent);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::WeatherInteractionPass(RHI::FCmdListH CL)
    {
        const FFrameData::FWeatherInteraction& Interaction = RenderFrame->WeatherInteraction;
        const bool bSimulate = Interaction.bField && Interaction.Serial != WeatherFieldSimulatedSerial
                            && WeatherFieldImages[0].IsValid() && WeatherFieldImages[1].IsValid();
        const bool bSpawn    = !Interaction.Impacts.empty();
        if (!bSimulate && !bSpawn)
        {
            return;
        }

        static const FShaderH SimShader   = FShaderLibrary::Get("WeatherFieldSim.slang");
        static const FShaderH SpawnShader = FShaderLibrary::Get("WeatherImpactSpawn.slang");

        LUMINA_PROFILE_SECTION_COLORED("Weather Interaction Pass", tracy::Color::SteelBlue);
        SCENE_GPU_SCOPE(CL, "Weather Interaction");

        const RHI::TGPUSpan<FWeatherWaterBodyGPU> Waters = Interaction.WaterBodies.empty()
            ? RHI::TGPUSpan<FWeatherWaterBodyGPU>()
            : RHI::TGPUSpan<FWeatherWaterBodyGPU>(RHI::CopyTransientArray(Interaction.WaterBodies.data(), Interaction.WaterBodies.size()));

        if (bSimulate && SimShader)
        {
            WeatherFieldSimulatedSerial = Interaction.Serial;

            if (Interaction.bReset)
            {
                constexpr float Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                RHI::CmdClearTexture(CL, WeatherFieldImages[0].Texture, Zero);
                RHI::CmdClearTexture(CL, WeatherFieldImages[1].Texture, Zero);
                Barriers::TransferToCompute(CL);
            }

            const RHI::TGPUSpan<FWeatherContactGPU> Contacts = Interaction.Contacts.empty()
                ? RHI::TGPUSpan<FWeatherContactGPU>()
                : RHI::TGPUSpan<FWeatherContactGPU>(RHI::CopyTransientArray(Interaction.Contacts.data(), Interaction.Contacts.size()));
            const uint32 Groups = Interaction.Resolution / 8u;

            for (uint32 Step = 0; Step < Interaction.Passes; ++Step)
            {
                const uint32 Input = Interaction.InputImage ^ (Step & 1u);

                FWeatherFieldSimPC PC = {};
                PC.Contacts       = Step == 0u ? Contacts : RHI::TGPUSpan<FWeatherContactGPU>();
                PC.Waters         = Waters;
                PC.Previous       = (uint32)WeatherFieldImages[Input].GetResourceID();
                PC.Current        = (uint32)WeatherFieldImages[Input ^ 1u].GetMipUAVIndex(0);
                PC.Shift          = Step == 0u ? Interaction.Shift : FIntVector2(0);
                PC.Origin         = FVector2((float)Interaction.OriginTexel.x, (float)Interaction.OriginTexel.y) * Interaction.TexelWorld;
                PC.TexelWorld     = Interaction.TexelWorld;
                PC.Resolution     = Interaction.Resolution;
                PC.Courant        = kFieldCourant;
                PC.WaterDamping   = kFieldWaterDamping;
                PC.ShallowDamping = kFieldShallowDamping;
                PC.FoamDecay      = kFieldFoamDecay;
                PC.TrailDecay     = Interaction.bSnow ? kSnowTrailDecay : kFieldTrailDecay;
                PC.bStep          = Step > 0u || Interaction.bFirstPassSteps ? 1u : 0u;
                DispatchCompute(CL, SimShader, PC, Groups, Groups);

                RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            }
        }

        if (bSpawn && SpawnShader)
        {
            ReserveBuffer(CL, WeatherSplashBuffer, (uint64)kInteractionSplashCapacity * sizeof(FRainSplashGPU));
            if (WeatherSplashBuffer)
            {
                const uint32 Count = (uint32)Interaction.Impacts.size();

                FWeatherImpactSpawnPC PC = {};
                PC.Impacts  = RHI::CopyTransientArray(Interaction.Impacts.data(), Count);
                PC.Splashes = RHI::TGPUSpan<FRainSplashGPU>::FromAddress(WeatherSplashBuffer.Gpu, kInteractionSplashCapacity);
                PC.Cursor   = WeatherSplashCursor;
                PC.Time     = RenderFrame->SceneGlobalData.Time;
                DispatchCompute(CL, SpawnShader, PC, RenderUtils::GetGroupCount(Count, 64u));

                WeatherSplashCursor      = (WeatherSplashCursor + Count) % kInteractionSplashCapacity;
                WeatherSplashQuietFrames = 0u;
            }
        }

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                        RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::MeshShader,
                        RHI::EAccessFlags::ShaderRead);
    }

    void FDefaultSceneRenderer::WeatherDrawPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const FFrameData::FWeather& Weather = Frame.Weather;
        uint32 NumDrops  = Weather.bActive && Weather.bDrops ? Weather.NumDrops : 0u;
        uint32 NumFlakes = Weather.bActive ? Weather.NumFlakes : 0u;
        const bool bImpactSplashes = WeatherSplashBuffer && WeatherSplashQuietFrames <= kSplashDrawFrames;
        if (NumDrops == 0u && NumFlakes == 0u && !bImpactSplashes)
        {
            return;
        }

        static const FShaderH CullShader         = FShaderLibrary::Get("RainDrops.slang");
        static const FShaderH SpawnShader        = FShaderLibrary::Get("RainSplashSpawn.slang");
        static const FShaderH StreakShader       = FShaderLibrary::Get("RainStreakVertex.slang");
        static const FShaderH RainSplashShader   = FShaderLibrary::Get("RainSplashVertex.slang");
        static const FShaderH FlakeShader        = FShaderLibrary::Get("SnowFlakeVertex.slang");
        static const FShaderH ImpactSplashShader = FShaderLibrary::Get("WeatherSplashVertex.slang");
        static const FShaderH PixelShader        = FShaderLibrary::Get("RainPixel.slang");
        static const FShaderH HazeShader         = FShaderLibrary::Get("RainHaze.slang");
        if (!CullShader || !SpawnShader || !StreakShader || !RainSplashShader || !FlakeShader || !ImpactSplashShader || !PixelShader || !HazeShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Weather Draw Pass", tracy::Color::SteelBlue);
        SCENE_GPU_SCOPE(CL, "Weather");

        if (NumDrops > 0u)
        {
            ReserveBuffer(CL, RainVisibleBuffer, (uint64)NumDrops * sizeof(FUIntVector4));
            ReserveBuffer(CL, RainArgsBuffer, sizeof(RHI::FDrawIndirectArguments));
            ReserveBuffer(CL, RainSplashBuffer, (uint64)kSplashCapacity * sizeof(FRainSplashGPU));
            NumDrops = RainVisibleBuffer && RainArgsBuffer && RainSplashBuffer ? NumDrops : 0u;
        }
        if (NumFlakes > 0u)
        {
            ReserveBuffer(CL, SnowVisibleBuffer, (uint64)NumFlakes * sizeof(FUIntVector4));
            ReserveBuffer(CL, SnowArgsBuffer, sizeof(RHI::FDrawIndirectArguments));
            NumFlakes = SnowVisibleBuffer && SnowArgsBuffer ? NumFlakes : 0u;
        }
        if (bImpactSplashes)
        {
            ++WeatherSplashQuietFrames;
        }

        const FSceneImage& HDR   = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Depth = GetNamedImage(ENamedImage::DepthAttachment);

        const FRainParams& Rain = Weather.State.Params.Rain;
        if (NumDrops > 0u && Rain.HazeDensity * Rain.Intensity > 0.0f && HDR.GetMipUAVIndex(0) >= 0)
        {
            FRainHazePC PC = {};
            PC.HDRUAV = (uint32)HDR.GetMipUAVIndex(0);
            PC.Width  = HDR.GetSizeX();
            PC.Height = HDR.GetSizeY();
            DispatchCompute(CL, HazeShader, PC, RenderUtils::GetGroupCount(PC.Width, 8u), RenderUtils::GetGroupCount(PC.Height, 8u));
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                            RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader,
                            RHI::EAccessFlags::ColorRead | RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::ShaderRead);
        }

        const RHI::FGPURange RainArgs{ RainArgsBuffer.Gpu, sizeof(RHI::FDrawIndirectArguments) };
        const RHI::FGPURange SnowArgs{ SnowArgsBuffer.Gpu, sizeof(RHI::FDrawIndirectArguments) };
        const float RainExposure = CVarWeatherStreakExposure.GetValue() * Math::Max(Rain.StreakLength, 0.0f);

        FRainDrawPC RainPC = {};
        FRainDrawPC SnowPC = {};
        if (NumDrops > 0u || NumFlakes > 0u)
        {
            if (NumDrops > 0u)
            {
                RHI::CmdMemset(CL, RainArgs, 0u);
            }
            if (NumFlakes > 0u)
            {
                RHI::CmdMemset(CL, SnowArgs, 0u);
            }
            Barriers::TransferToCompute(CL);

            // Rain and snow share one stateless field cull, told apart by how each drop moves.
            const auto CullField = [&](const FSceneBuffer& VisibleBuffer, const FSceneBuffer& ArgsBuffer, uint32 Count, float Exposure, uint32 bSnow)
            {
                FRainDropsPC PC = {};
                PC.Visible  = RHI::TGPUSpan<FUIntVector4>::FromAddress(VisibleBuffer.Gpu, Count);
                PC.Args     = RHI::TGPUSpan<uint32>::FromAddress(ArgsBuffer.Gpu, 4u);
                PC.NumDrops = Count;
                PC.Exposure = Exposure;
                PC.bSnow    = bSnow;
                DispatchCompute(CL, CullShader, PC, RenderUtils::GetGroupCount(Count, 64u));
                return PC.Visible;
            };

            if (NumDrops > 0u)
            {
                RainPC.Visible          = CullField(RainVisibleBuffer, RainArgsBuffer, NumDrops, RainExposure, 0u);
                RainPC.Splashes         = RHI::TGPUSpan<FRainSplashGPU>::FromAddress(RainSplashBuffer.Gpu, kSplashCapacity);
                RainPC.Exposure         = RainExposure;
                RainPC.SoftFadeDistance = 0.1f;

                if (Weather.NumSplashes > 0u)
                {
                    FRainSplashSpawnPC PC = {};
                    PC.Splashes    = RainPC.Splashes;
                    PC.Cursor      = RainSplashCursor;
                    PC.Count       = Weather.NumSplashes;
                    PC.FrameSeed   = (uint32)(Frame.SceneGlobalData.Time * 1000.0f) * 2654435761u;
                    PC.SpawnWindow = Math::Clamp(Frame.SceneGlobalData.DeltaTime, 0.0f, 0.1f);
                    PC.GBufferB    = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
                    PC.VisBuffer   = (uint32)GetNamedImage(ENamedImage::VisBuffer).GetResourceID();
                    PC.SlotImage   = (uint32)GetNamedImage(ENamedImage::MaterialSlot).GetResourceID();
                    DispatchCompute(CL, SpawnShader, PC, RenderUtils::GetGroupCount(Weather.NumSplashes, 64u));
                    RainSplashCursor = (RainSplashCursor + Weather.NumSplashes) % kSplashCapacity;
                }
            }
            if (NumFlakes > 0u)
            {
                SnowPC.Visible          = CullField(SnowVisibleBuffer, SnowArgsBuffer, NumFlakes, kSnowExposure, 1u);
                SnowPC.Exposure         = kSnowExposure;
                SnowPC.SoftFadeDistance = 0.1f;
            }

            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                            RHI::EStageFlags::VertexShader | RHI::EStageFlags::IndirectArguments,
                            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::IndirectRead);
        }

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = Depth.Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencil(CL, DepthDesc);

        const auto DrawWith = [&](FShaderH Vertex) -> bool
        {
            FGraphicsPipelineKey Key;
            Key.VS          = Vertex;
            Key.PS          = PixelShader;
            Key.DepthFormat = EFormat::D32;
            Key.ColorTargets.push_back({ HDR.Desc.Format, PremultipliedBlend() });
            const RHI::FPipelineH Pipeline = GetOrCreatePipeline(Key);
            if (!Pipeline)
            {
                return false;
            }
            RHI::CmdSetPipeline(CL, Pipeline);
            return true;
        };

        if (NumDrops > 0u && DrawWith(StreakShader))
        {
            RHI::CmdDrawIndirect(CL, MakeArgs(RainPC), RainArgs, 1u, sizeof(RHI::FDrawIndirectArguments));
        }
        if (NumDrops > 0u && DrawWith(RainSplashShader))
        {
            RHI::CmdDraw(CL, MakeArgs(RainPC), kSplashVertices, kSplashCapacity, 0u, 0u);
        }
        if (NumFlakes > 0u && DrawWith(FlakeShader))
        {
            RHI::CmdDrawIndirect(CL, MakeArgs(SnowPC), SnowArgs, 1u, sizeof(RHI::FDrawIndirectArguments));
        }
        if (bImpactSplashes && DrawWith(ImpactSplashShader))
        {
            FRainDrawPC SplashPC = {};
            SplashPC.Splashes         = RHI::TGPUSpan<FRainSplashGPU>::FromAddress(WeatherSplashBuffer.Gpu, kInteractionSplashCapacity);
            SplashPC.SoftFadeDistance = 0.15f;
            RHI::CmdDraw(CL, MakeArgs(SplashPC), kInteractionSplashVertices, kInteractionSplashCapacity, 0u, 0u);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::ReleaseIdleWeatherResources()
    {
        const bool bInteraction = RenderFrame != nullptr && (RenderFrame->WeatherInteraction.bField || !RenderFrame->WeatherInteraction.InteractorRecords.empty());
        if ((RenderFrame != nullptr && RenderFrame->Weather.bActive) || bInteraction || WeatherSplashQuietFrames <= kSplashDrawFrames)
        {
            return;
        }
        if (!RainOcclusionImage.IsValid() && !RainVisibleBuffer && !RainSplashBuffer && !WeatherSplashBuffer && !SnowVisibleBuffer
            && !WeatherFieldImages[0].IsValid() && !WeatherFieldImages[1].IsValid())
        {
            return;
        }
        if (++WeatherIdleFrames < kWeatherIdleReleaseFrames)
        {
            return;
        }

        RetireSceneImage(RainOcclusionImage);
        RetireSceneImage(WeatherFieldImages[0]);
        RetireSceneImage(WeatherFieldImages[1]);
        RainVisibleBuffer.Release();
        RainArgsBuffer.Release();
        RainSplashBuffer.Release();
        WeatherSplashBuffer.Release();
        SnowVisibleBuffer.Release();
        SnowArgsBuffer.Release();
        RainSplashCursor    = 0u;
        WeatherSplashCursor = 0u;
        WeatherIdleFrames   = 0u;
    }
}
