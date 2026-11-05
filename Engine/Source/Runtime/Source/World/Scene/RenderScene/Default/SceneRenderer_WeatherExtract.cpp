#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/WeatherComponent.h"
#include "World/Entity/Systems/WeatherInteractionState.h"

namespace Lumina
{
    namespace WeatherExtractInternal
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

        // Fixed so a ripple crosses the field at the same speed whatever the frame rate.
        constexpr float  kFieldStepRate            = 60.0f;
        constexpr uint32 kMaxFieldStepsPerFrame    = 4u;
        // Long enough for the last wake to die out before the field stops and its images are cleared.
        constexpr uint32 kFieldSettleFrames        = 600u;
        constexpr uint32 kSnowSettleFrames         = 36000u;
        constexpr float  kSnowCoverThreshold       = 0.01f;
        constexpr uint32 kMaxWeatherWaterBodies    = 16u;
        constexpr float  kOcclusionHalfDepth       = 150.0f;

        uint32 ClampedCount(float Value, uint32 Max)
        {
            if (!(Value > 0.0f))
            {
                return 0u;
            }
            return Value >= (float)Max ? Max : (uint32)Value;
        }

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
    }

    void FDefaultSceneRenderer::ExtractWeather(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        using namespace WeatherExtractInternal;

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
                                               kWeatherRainSplashCapacity / 4u);
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
        using namespace WeatherExtractInternal;

        FFrameData::FWeatherInteraction& Out = Frame.WeatherInteraction;
        Out.bField          = false;
        Out.bReset          = false;
        Out.bFirstPassSteps = false;
        Out.bSnow           = false;
        Out.Passes          = 0u;
        Out.Shift           = FIntVector2(0);
        Out.Contacts.Reset();
        Out.Impacts.Reset();
        Out.InteractorTable.Reset();
        Out.InteractorRecords.Reset();
        Out.WaterBodies.Reset();

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
                FWeatherContactGPU& GPU = Out.Contacts.EmplaceGetRef();
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
                if (static_cast<uint32>(Out.Impacts.Num()) >= kWeatherImpactSplashCapacity / 4u)
                {
                    break;
                }
                if (!InsideWindow(Impact.Position, 0.0f))
                {
                    continue;
                }
                FWeatherImpactGPU& GPU = Out.Impacts.EmplaceGetRef();
                GPU.Position = Impact.Position;
                GPU.Scale    = Impact.Scale;
                GPU.Normal   = Impact.Normal;
                GPU.Surface  = (uint32)Impact.Surface;
                GPU.Seed     = InteractorHash(Impact.Entity ^ (WeatherFieldSerial * 0x9E3779B9u) ^ (uint32)Out.Impacts.Num());
                GPU._Pad[0] = GPU._Pad[1] = GPU._Pad[2] = 0u;
            }

            if (!State->Interactors.IsEmpty())
            {
                const uint32 Capacity = static_cast<uint32>(Math::NextPowerOfTwo(Math::Max(State->Interactors.Num() * 2, 16)));
                Out.InteractorTable.SetNum(Capacity, FUIntVector2(Constants::kIndexNoneU32, 0u));
                for (const FWeatherInteractorState& Interactor : State->Interactors)
                {
                    uint32 Slot = InteractorHash(Interactor.EntityID) & (Capacity - 1u);
                    while (Out.InteractorTable[Slot].x != Constants::kIndexNoneU32 && Out.InteractorTable[Slot].x != Interactor.EntityID)
                    {
                        Slot = (Slot + 1u) & (Capacity - 1u);
                    }
                    Out.InteractorTable[Slot] = FUIntVector2(Interactor.EntityID, (uint32)Out.InteractorRecords.Num());
                    Out.InteractorRecords.Emplace(Interactor.RootY, Interactor.Soak, Interactor.WaterlineHeight, Interactor.WaterlineWetness);
                }
            }
        }

        Out.bSnow = bSnowLying;
        const bool bDisturbed = !Out.Contacts.IsEmpty() || !Out.Impacts.IsEmpty();
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
        Registry.View<ECS::Read<SWaterComponent>, ECS::Without<SDisabledTag>>().ForEach([&](ECS::FEntity Entity, const SWaterComponent& Water)
        {
            if (static_cast<uint32>(Out.WaterBodies.Num()) >= kMaxWeatherWaterBodies)
            {
                return;
            }
            const STransformComponent& Transform = Transforms.Get(Entity);
            const FMatrix4 WorldToWater = Math::Inverse(WaterToWorld(Water, Transform.GetWorldMatrix()));

            FWeatherWaterBodyGPU& Body = Out.WaterBodies.EmplaceGetRef();
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
}
