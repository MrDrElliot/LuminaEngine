#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "World/Scene/RenderScene/SkyAtmosphere.h"
#include "World/Scene/RenderScene/SunShadowMath.h"

#include <bit>

namespace Lumina
{
    namespace SceneRendererExtractLightsInternal
    {
        // A cube face spans 54.7 degrees from its axis to a corner, so it sees only directions behind the panel once its axis is past 144.7.
        uint32 EmittingCubeFaces(const FVector3& Forward)
        {
            constexpr float kBehindPanelCos = -0.8165f;
            const FVector3 Axes[6] = { FVector3(1, 0, 0), FVector3(-1, 0, 0), FVector3(0, 1, 0), FVector3(0, -1, 0), FVector3(0, 0, 1), FVector3(0, 0, -1) };
            uint32 Mask = 0u;
            for (uint32 Face = 0; Face < 6u; ++Face)
            {
                Mask |= Math::Dot(Axes[Face], Forward) > kBehindPanelCos ? (1u << Face) : 0u;
            }
            return Mask;
        }

        // The haze a storm adds tints the sunlight too, so the light agrees with the sky ExtractEnvironment draws.
        float WeatheredAerosol(ECS::FRegistry& Registry, const SEnvironmentComponent& Env)
        {
            const SWeatherState* Weather = Registry.Ctx().Find<SWeatherState>();
            if (Weather == nullptr || !Weather->bActive || Weather->Params.Environment.Weight <= 0.0f)
            {
                return Env.AerosolDensity;
            }
            SEnvironmentComponent Weathered = Env;
            Weather->Params.Environment.ApplyTo(Weathered);
            return Weathered.AerosolDensity;
        }

        FVector3 ColorTemperatureToRGB(float Kelvin)
        {
            const float Temp = Math::Clamp(Kelvin, 1000.0f, 40000.0f) / 100.0f;

            float R;
            float G;
            float B;

            if (Temp <= 66.0f)
            {
                R = 255.0f;
                G = 99.4708025861f * Math::Log(Temp) - 161.1195681661f;
            }
            else
            {
                R = 329.698727446f * Math::Pow(Temp - 60.0f, -0.1332047592f);
                G = 288.1221695283f * Math::Pow(Temp - 60.0f, -0.0755148492f);
            }

            if (Temp >= 66.0f)
            {
                B = 255.0f;
            }
            else if (Temp <= 19.0f)
            {
                B = 0.0f;
            }
            else
            {
                B = 138.5177312231f * Math::Log(Temp - 10.0f) - 305.0447927307f;
            }

            FVector3 RGB = Math::Clamp(FVector3(R, G, B) / 255.0f, FVector3(0.0f), FVector3(1.0f));
            const float MaxC = Math::Max(RGB.x, Math::Max(RGB.y, RGB.z));
            return MaxC > 1e-4f ? RGB / MaxC : FVector3(1.0f);
        }

        uint32 ChunksFor(int32 DenseSlots, uint32 ChunkSize)
        {
            return RenderUtils::GetGroupCount((uint32)DenseSlots, ChunkSize);
        }
    }

    void FDefaultSceneRenderer::ScheduleLightExtract(ECS::FRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();
        using namespace SceneRendererExtractLightsInternal;

        auto Transforms      = Registry.GetStorage<STransformComponent>();
        auto Points          = Registry.GetStorage<SPointLightComponent>();
        auto Spots           = Registry.GetStorage<SSpotLightComponent>();
        auto Areas           = Registry.GetStorage<SAreaLightComponent>();
        auto DirectionalView = Registry.View<ECS::Read<SDirectionalLightComponent>, ECS::Without<SDisabledTag>>();

        #if USING(WITH_EDITOR)
        bRecordLightIcons = !World->IsGameWorld();
        #else
        bRecordLightIcons = false;
        #endif

        const SDirectionalLightComponent* Sun = SelectActiveSun(DirectionalView, Transforms);
        const FSkyLightInputs SkyInputs = GatherSkyLightInputs(Registry, Sun);

        BuildSceneCullContext(Sun, SkyInputs);

        // Needs the captures and the baking probe, both already extracted.
        BuildLightRelevanceVolumes();

        // Only what changed since the last frame reaches the retained set, and only its shadow casters come back through the CPU below.
        LightSet.Sync();
        GatherLightIcons(Registry);

        NumLightChunks = ChunksFor((int32)LightSet.GetCPULights().Num(), FLightBatch::Capacity);
        if ((uint32)LightBatches.Num() < NumLightChunks)
        {
            LightBatches.SetNum(NumLightChunks);
        }

        // Ten passes read Lights[0] as the sun, so a local light must never win it, and the sun's job writes it alongside the chunks.
        const uint32 FirstLocalLight = Sun != nullptr ? 1u : 0u;
        FFrameData::FLighting& Lighting = ExtractFrame->Lighting;
        Lighting.Lights.SetNum(FirstLocalLight);
        Lighting.LightLayers.SetNum(FirstLocalLight);
        Lighting.LightSlots.SetNum(FirstLocalLight);
        if (FirstLocalLight != 0u)
        {
            Lighting.LightSlots[0] = Constants::kIndexNoneU32;
        }

        Jobs::Spawn(Jobs::EJobPriority::High, ExtractLatch,
            [this, Transforms, Points, Spots, Areas, Sun, SkyInputs, FirstLocalLight]() -> Jobs::TTask<void>
        {
            co_await Jobs::Fork(Jobs::ForEach(NumLightChunks, [this, Transforms, Points, Spots, Areas](const Jobs::FRange& Range)
            {
                LUMINA_PROFILE_SECTION("Process CPU Lights");
                for (uint32 Chunk = Range.Start; Chunk < Range.End; ++Chunk)
                {
                    ProcessLightChunk(Chunk, Transforms, Points, Spots, Areas);
                }
            }, 1));

            if (Sun != nullptr)
            {
                co_await Jobs::Call([this, Sun, SkyInputs]
                {
                    LUMINA_PROFILE_SECTION("Process Directional Light");
                    ProcessDirectionalLight(*Sun, SkyInputs);
                });
            }

            co_await Jobs::Join();
            CommitLightBatches(FirstLocalLight);
            LightsReady.Signal();
        });
    }

    // The first directional light with a usable direction is the sun for culling and shading alike.
    const SDirectionalLightComponent* FDefaultSceneRenderer::SelectActiveSun(const auto& DirectionalView, const auto& Transforms)
    {
        DirectionalLightIcons.Reset();

        const SDirectionalLightComponent* Sun = nullptr;
        DirectionalView.ForEach([&](ECS::FEntity Entity, const SDirectionalLightComponent& Light)
        {
            if (bRecordLightIcons)
            {
                DirectionalLightIcons.Emplace(Entity, Transforms.Get(Entity).GetWorldLocation(), Light.Color);
            }
            if (Sun == nullptr && Math::Dot(Light.Direction, Light.Direction) > 0.0001f)
            {
                Sun = &Light;
            }
        });
        return Sun;
    }

    // Editor icons cover every light, which the retained set no longer walks each frame.
    void FDefaultSceneRenderer::GatherLightIcons(ECS::FRegistry& Registry)
    {
        for (TVector<FLightIcon>& Icons : LocalLightIcons)
        {
            Icons.Reset();
        }
        if (!bRecordLightIcons)
        {
            return;
        }

        auto Transforms = Registry.GetStorage<STransformComponent>();
        auto Gather = [&]<typename T>(ESceneLightKind Kind, T*)
        {
            Registry.View<ECS::Read<T>, ECS::Without<SDisabledTag>>().ForEach([&](ECS::FEntity Entity, const T& Light)
            {
                if (const STransformComponent* Transform = Transforms.TryGetUntracked(Entity))
                {
                    LocalLightIcons[(uint32)Kind].Emplace(Entity, Transform->GetWorldLocation(), Light.LightColor);
                }
            });
        };
        Gather(ESceneLightKind::Point, (SPointLightComponent*)nullptr);
        Gather(ESceneLightKind::Spot,  (SSpotLightComponent*)nullptr);
        Gather(ESceneLightKind::Area,  (SAreaLightComponent*)nullptr);
    }

    void FDefaultSceneRenderer::ProcessLightChunk(uint32 Chunk, const auto& Transforms, const auto& Points, const auto& Spots, const auto& Areas)
    {
        FLightBatch& Batch = LightBatches[Chunk];
        Batch.NumLights         = 0;
        Batch.NumShadows        = 0;
        Batch.NumLightFunctions = 0;
        Batch.NumShadowLights   = 0;

        const TSpan<const FSceneLightSet::FCPULight> CPULights = LightSet.GetCPULights();
        const uint32 Begin = Chunk * FLightBatch::Capacity;
        const uint32 End   = Math::Min(Begin + FLightBatch::Capacity, (uint32)CPULights.Num());
        for (uint32 Index = Begin; Index < End; ++Index)
        {
            const FSceneLightSet::FCPULight& Light = CPULights[Index];
            const STransformComponent* Transform = Transforms.TryGetUntracked(Light.Entity);
            if (Transform == nullptr)
            {
                continue;
            }

            switch (Light.Kind)
            {
            case ESceneLightKind::Point:
                if (const SPointLightComponent* Point = Points.TryGetUntracked(Light.Entity))
                {
                    ProcessPointLight(Light.Slot, *Point, *Transform, Batch);
                }
                break;
            case ESceneLightKind::Spot:
                if (const SSpotLightComponent* Spot = Spots.TryGetUntracked(Light.Entity))
                {
                    ProcessSpotLight(Light.Slot, *Spot, *Transform, Batch);
                }
                break;
            default:
                if (const SAreaLightComponent* Area = Areas.TryGetUntracked(Light.Entity))
                {
                    ProcessAreaLight(Light.Slot, *Area, *Transform, Batch);
                }
                break;
            }
        }
    }

    void FDefaultSceneRenderer::CommitLightBatches(uint32 FirstLocalLight)
    {
        LUMINA_PROFILE_SECTION("Commit Light Batches");

        FFrameData& Frame     = *ExtractFrame;
        auto&       Lighting  = Frame.Lighting;
        FSceneCullContext& SceneCull = Frame.Geometry.SceneCullContext;

        uint32 Count = FirstLocalLight;
        for (uint32 Chunk = 0; Chunk < NumLightChunks; ++Chunk)
        {
            Count += LightBatches[Chunk].NumLights;
        }
        Lighting.Lights.SetNum(Count);
        Lighting.LightLayers.SetNum(Count);
        Lighting.LightSlots.SetNum(Count);

        uint32 Base = FirstLocalLight;
        for (uint32 Chunk = 0; Chunk < NumLightChunks; ++Chunk)
        {
            const FLightBatch& Batch = LightBatches[Chunk];

            if (SceneCull.bEnabled && Batch.NumShadowLights != 0)
            {
                SceneCull.ShadowLights.Append(Batch.ShadowLights, (int32)Batch.NumShadowLights);
            }

            for (uint32 Slot = 0; Slot < Batch.NumLights; ++Slot)
            {
                Lighting.Lights[Base + Slot]      = Batch.Lights[Slot];
                Lighting.LightLayers[Base + Slot] = Batch.LightLayers[Slot];
                Lighting.LightSlots[Base + Slot]  = Batch.Slots[Slot];
            }
            for (uint32 Index = 0; Index < Batch.NumShadows; ++Index)
            {
                Lighting.ShadowRequests.AddGetRef(Batch.Shadows[Index]).LightIndex += Base;
            }
            for (uint32 Index = 0; Index < Batch.NumLightFunctions; ++Index)
            {
                Lighting.LightFunctionRequests.AddGetRef(Batch.LightFunctions[Index]).LightIndex += Base;
            }
            Base += Batch.NumLights;
        }

        NumLiveLights = Count;
    }

    uint32 FDefaultSceneRenderer::VisibleCubeFaces(const FVector3& Position, float Radius) const
    {
        const TVector<FFrustum>& Volumes = ExtractFrame->Lighting.RelevanceFrusta;
        if (Volumes.IsEmpty())
        {
            return kAllCubeFaces;
        }

        // Same order as the shadow views and DirectionToCubemapCoord, which is +X, -X, +Y, -Y, +Z, -Z.
        static const FVector3 Axes[6]  = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        static const FVector3 SideU[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 } };
        static const FVector3 SideV[6] = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };

        uint32 Mask = 0u;
        for (uint32 Face = 0; Face < 6; ++Face)
        {
            // A 90 degree face reaches Radius along its axis, so the apex and these four corners hull its part of the sphere.
            const FVector3 Far = Position + Axes[Face] * Radius;
            const FVector3 U   = SideU[Face] * Radius;
            const FVector3 V   = SideV[Face] * Radius;
            const FVector3 Hull[5] = { Position, Far + U + V, Far + U - V, Far - U + V, Far - U - V };

            for (const FFrustum& Volume : Volumes)
            {
                bool bSeparated = false;
                for (const FVector4& Plane : Volume.Planes)
                {
                    bool bAllOutside = true;
                    for (const FVector3& Point : Hull)
                    {
                        if (Math::Dot(FVector3(Plane.x, Plane.y, Plane.z), Point) + Plane.w >= 0.0f)
                        {
                            bAllOutside = false;
                            break;
                        }
                    }
                    if (bAllOutside)
                    {
                        bSeparated = true;
                        break;
                    }
                }

                if (!bSeparated)
                {
                    Mask |= 1u << Face;
                    break;
                }
            }
        }
        return Mask;
    }

    void FDefaultSceneRenderer::BuildLightRelevanceVolumes()
    {
        FFrameData& Frame = *ExtractFrame;
        TVector<FFrustum>& Volumes = Frame.Lighting.RelevanceFrusta;

        Volumes.Reset();

        if (!FrameSettings.bCullLights)
        {
            return;
        }

        // First, which is what lets IsLightRelevant skip it for a light it already knows is in the camera.
        Volumes.Add(Frame.CameraFrustum);

        // Captures shade through the same light buffer via MakeSecondaryViewGlobals, so they vote too.
        for (const FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            Volumes.Add(Capture.ViewVolume.GetFrustum());
        }

        if (Frame.ReflectionProbes.BakingProbe >= 0)
        {
            for (int32 Face = 0; Face < 6; ++Face)
            {
                Volumes.Add(Frame.ReflectionProbes.FaceVolumes[Face].GetFrustum());
            }
        }
    }

    bool FDefaultSceneRenderer::IsLightRelevant(const FVector3& LightPosition, float LightRadius, bool bInCamera) const
    {
        if (!FrameSettings.bCullLights)
        {
            return true;
        }

        if (LightRadius <= 0.0f)
        {
            return false;
        }

        if (bInCamera)
        {
            return true;
        }

        const TVector<FFrustum>& Volumes = ExtractFrame->Lighting.RelevanceFrusta;
        for (int32 Index = 1; Index < Volumes.Num(); ++Index)
        {
            if (Volumes[Index].IntersectsSphere(LightPosition, LightRadius))
            {
                return true;
            }
        }

        return false;
    }

    FDefaultSceneRenderer::FSkyLightInputs FDefaultSceneRenderer::GatherSkyLightInputs(ECS::FRegistry& Registry, const SDirectionalLightComponent* Sun)
    {
        FSkyLightInputs Inputs;
        FFrameData& Frame = *ExtractFrame;
        Frame.Volumetrics.MoonlitSkyRatio = 0.0f;
        if (Sun == nullptr)
        {
            return Inputs;
        }

        // The last one, as ExtractEnvironment takes it.
        const SEnvironmentComponent* Env = nullptr;
        Registry.View<ECS::Read<SEnvironmentComponent>, ECS::Without<SDisabledTag>>().ForEach([&Env](const SEnvironmentComponent& Candidate)
        {
            Env = &Candidate;
        });

        const FVector3 SunDirection = Math::Normalize(Sun->Direction);
        Inputs.MoonDirection = -SunDirection;
        Inputs.Altitude      = Frame.ViewVolume.GetViewPosition().y;
        if (Env != nullptr)
        {
            Inputs.MoonDirection       = SkyAtmosphere::MoonDirection(*Env, SunDirection);
            Inputs.MoonPhaseBrightness = SkyAtmosphere::MoonPhaseBrightness(*Env);
            Inputs.Aerosol             = Math::Max(SceneRendererExtractLightsInternal::WeatheredAerosol(Registry, *Env), 0.0f);
            Inputs.bThroughAtmosphere  = Env->bSunlightThroughAtmosphere && Env->SkyMode == ESkyMode::Dynamic;
        }

        // Without moonlight on the ground the sky stays unlit too, so the two never disagree.
        if (Sun->bMoonlight && Sun->Intensity > 0.0f)
        {
            Frame.Volumetrics.MoonlitSkyRatio = Sun->MoonIntensity / Sun->Intensity * Inputs.MoonPhaseBrightness;
        }
        return Inputs;
    }

    void FDefaultSceneRenderer::BuildSceneCullContext(const SDirectionalLightComponent* Sun, const FSkyLightInputs& Sky)
    {
        LUMINA_PROFILE_SCOPE();

        FFrameData& Frame = *ExtractFrame;
        auto& SceneCullContext = Frame.Geometry.SceneCullContext;

        SceneCullContext.Reset();
        SceneCullContext.bEnabled = FrameSettings.bCPUInstanceCull;
        SceneCullContext.Frustum  = Frame.CameraFrustum;

        if (!SceneCullContext.bEnabled)
        {
            return;
        }

        for (const FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            SceneCullContext.CaptureFrusta.Add(Capture.ViewVolume.GetFrustum());
        }

        if (Frame.ReflectionProbes.BakingProbe >= 0)
        {
            for (int32 Face = 0; Face < 6; ++Face)
            {
                SceneCullContext.CaptureFrusta.Add(Frame.ReflectionProbes.FaceVolumes[Face].GetFrustum());
            }
        }

        if (Sun != nullptr)
        {
            constexpr float ShadowSweepDistance = 2000.0f;
            SceneCullContext.SunDirection     = Sun->GetLightingDirection(Sky.MoonDirection);
            SceneCullContext.bHasSun          = true;
            SceneCullContext.SunShadowFrustum = SceneCullContext.Frustum.Extruded(SceneCullContext.SunDirection, ShadowSweepDistance);
        }
    }

    void FDefaultSceneRenderer::ProcessPointLight(uint32 Slot, const SPointLightComponent& PointLight, const STransformComponent& TransformComponent, FLightBatch& Batch)
    {
        const FVector3 Position = TransformComponent.GetWorldLocation();

        // A switched-off light adds nothing to the image, so it should not take a slot or a cluster test either.
        if (PointLight.Intensity <= 0.0f || !IsLightOnDrawnLayer(PointLight.RenderLayers))
        {
            return;
        }

        const float Radius    = PointLight.Attenuation;
        const bool  bInCamera = ExtractFrame->CameraFrustum.IntersectsSphere(Position, Radius);

        // The skinned cull keeps casters this sphere reaches, whether or not the light wins a slot.
        if (PointLight.bCastShadows && bInCamera)
        {
            Batch.ShadowLights[Batch.NumShadowLights++] = FSceneCullContext::FLightSphere{ Position, Radius };
        }

        // Ahead of the slot handout, so an unreachable light costs neither a light index nor a cluster test.
        if (!IsLightRelevant(Position, Radius, bInCamera))
        {
            return;
        }

        const uint32 Lights = Batch.NumLights;
        const FLight Light  = FSceneLightSet::MakePointLight(PointLight, TransformComponent);

        if (PointLight.LightFunctionMaterial.Get() != nullptr)
        {
            FLightFunctionRequest& Request = Batch.LightFunctions[Batch.NumLightFunctions++];
            Request            = {};
            Request.Material   = PointLight.LightFunctionMaterial.Get();
            Request.LightIndex = Lights;
            Request.Type       = ELightFlags::Point;
            Request.Position   = Position;
        }

        if (PointLight.bCastShadows && bInCamera)
        {
            const uint32 FaceMask = VisibleCubeFaces(Position, Radius);
            if (FaceMask != 0u)
            {
                const float Dist = Math::Distance(ExtractFrame->ViewVolume.GetViewPosition(), Position);
                constexpr float ResolutionScale = 2048.0f;

                FShadowRequest& Req  = Batch.Shadows[Batch.NumShadows++];
                Req.FaceMask         = FaceMask;
                Req.LightIndex       = Lights;
                Req.Type             = ELightType::Point;
                Req.DesiredPixels    = (uint32)((Radius / Math::Max(Dist, 0.01f)) * ResolutionScale);
                Req.DistanceToCamera = Dist;
                Req.Position         = Position;
                Req.Direction        = FVector3(0.0f);
                Req.Up               = FVector3(0.0f);
                Req.Attenuation      = Radius;
                Req.OuterFOVDegrees  = 0.0f;
            }
        }

        Batch.Slots[Batch.NumLights]       = Slot;
        Batch.LightLayers[Batch.NumLights] = PointLight.RenderLayers.Bits;
        Batch.Lights[Batch.NumLights++] = Light;
    }

    void FDefaultSceneRenderer::ProcessSpotLight(uint32 Slot, const SSpotLightComponent& SpotLight, const STransformComponent& TransformComponent, FLightBatch& Batch)
    {
        const FVector3 Position = TransformComponent.GetWorldLocation();

        if (SpotLight.Intensity <= 0.0f || !IsLightOnDrawnLayer(SpotLight.RenderLayers))
        {
            return;
        }

        const float Radius    = SpotLight.Attenuation;
        const bool  bInCamera = ExtractFrame->CameraFrustum.IntersectsSphere(Position, Radius);

        if (SpotLight.bCastShadows && bInCamera)
        {
            Batch.ShadowLights[Batch.NumShadowLights++] = FSceneCullContext::FLightSphere{ Position, Radius };
        }

        // Tested as the full attenuation sphere; the cone would reject more but needs the rotation first.
        if (!IsLightRelevant(Position, Radius, bInCamera))
        {
            return;
        }

        const uint32 Lights = Batch.NumLights;

        const FQuat WorldRotation = TransformComponent.ComputeWorldTransform().GetRotation();
        FVector3 UpdatedForward    = WorldRotation * FViewVolume::ForwardAxis;
        FVector3 UpdatedUp         = WorldRotation * FViewVolume::UpAxis;

        const float  OuterDegrees = SpotLight.OuterConeAngle;
        const FLight Light        = FSceneLightSet::MakeSpotLight(SpotLight, TransformComponent);

        if (SpotLight.LightFunctionMaterial.Get() != nullptr)
        {
            const FVector3 Forward = Math::Normalize(UpdatedForward);
            const FVector3 Right   = Math::Normalize(Math::Cross(Forward, UpdatedUp));

            FLightFunctionRequest& Request = Batch.LightFunctions[Batch.NumLightFunctions++];
            Request                 = {};
            Request.Material        = SpotLight.LightFunctionMaterial.Get();
            Request.LightIndex      = Lights;
            Request.Type            = ELightFlags::Spot;
            Request.Position        = Position;
            Request.Forward         = Forward;
            Request.Right           = Right;
            Request.Up              = Math::Cross(Right, Forward);
            Request.ProjectionScale = 1.0f / Math::Max(std::tan(Math::Radians(Math::Clamp(OuterDegrees, 0.1f, 89.0f))), 1e-3f);
        }

        if (SpotLight.bCastShadows && bInCamera)
        {
            const float Dist = Math::Distance(ExtractFrame->ViewVolume.GetViewPosition(), Position);
            constexpr float ResolutionScale = 2048.0f;

            FShadowRequest& Req  = Batch.Shadows[Batch.NumShadows++];
            Req                  = FShadowRequest{};
            Req.LightIndex       = Lights;
            Req.Type             = ELightType::Spot;
            Req.DesiredPixels    = (uint32)((Radius / Math::Max(Dist, 0.01f)) * ResolutionScale);
            Req.DistanceToCamera = Dist;
            Req.Position         = Position;
            // The shadow camera looks along the aim, which is where the spot lights.
            Req.Direction        = UpdatedForward;
            Req.Up               = UpdatedUp;
            Req.Attenuation      = Radius;
            Req.OuterFOVDegrees  = OuterDegrees;
        }

        Batch.Slots[Batch.NumLights]       = Slot;
        Batch.LightLayers[Batch.NumLights] = SpotLight.RenderLayers.Bits;
        Batch.Lights[Batch.NumLights++] = Light;
    }

    void FDefaultSceneRenderer::ProcessAreaLight(uint32 Slot, const SAreaLightComponent& AreaLight, const STransformComponent& TransformComponent, FLightBatch& Batch)
    {
        const FVector3 Position = TransformComponent.GetWorldLocation();

        if (AreaLight.Intensity <= 0.0f || !IsLightOnDrawnLayer(AreaLight.RenderLayers))
        {
            return;
        }

        const FVector2 HalfSize = FVector2(Math::Max(AreaLight.Width, 0.01f), Math::Max(AreaLight.Height, 0.01f)) * 0.5f;
        // Padded by the half diagonal, so the cluster sphere still covers the reach past the rectangle's corners.
        const float    Radius    = Math::Max(AreaLight.Attenuation, 0.01f) + Math::Length(HalfSize);
        const bool     bInCamera = ExtractFrame->CameraFrustum.IntersectsSphere(Position, Radius);

        // The skinned cull keeps casters this sphere reaches, whether or not the light wins a slot.
        if (AreaLight.bCastShadows && bInCamera)
        {
            Batch.ShadowLights[Batch.NumShadowLights++] = FSceneCullContext::FLightSphere{ Position, Radius };
        }

        if (!IsLightRelevant(Position, Radius, bInCamera))
        {
            return;
        }

        const uint32   Lights        = Batch.NumLights;
        const FQuat    WorldRotation = TransformComponent.ComputeWorldTransform().GetRotation();
        const FVector3 Forward       = Math::Normalize(WorldRotation * FViewVolume::ForwardAxis);
        const FLight   Light         = FSceneLightSet::MakeAreaLight(AreaLight, TransformComponent);

        if (AreaLight.bCastShadows && bInCamera)
        {
            const uint32 FaceMask = VisibleCubeFaces(Position, Radius) & SceneRendererExtractLightsInternal::EmittingCubeFaces(Forward);
            if (FaceMask != 0u)
            {
                const float Dist = Math::Distance(ExtractFrame->ViewVolume.GetViewPosition(), Position);
                constexpr float ResolutionScale = 2048.0f;

                FShadowRequest& Req  = Batch.Shadows[Batch.NumShadows++];
                Req                  = FShadowRequest{};
                Req.FaceMask         = FaceMask;
                Req.LightIndex       = Lights;
                Req.Type             = ELightType::Point;
                Req.DesiredPixels    = (uint32)((Radius / Math::Max(Dist, 0.01f)) * ResolutionScale);
                Req.DistanceToCamera = Dist;
                Req.Position         = Position;
                Req.Attenuation      = Radius;
            }
        }

        Batch.Slots[Batch.NumLights]       = Slot;
        Batch.LightLayers[Batch.NumLights] = AreaLight.RenderLayers.Bits;
        Batch.Lights[Batch.NumLights++] = Light;
    }

    void FDefaultSceneRenderer::ProcessVirtualSunShadow(const SDirectionalLightComponent& DirectionalLight, const FVector3& LightDir,
                                                        const FVector3& ShadowUp, FLightShadowData* ShadowData, int32 ShadowDataBase, float ShadowFar)
    {
        FFrameData& Frame     = *ExtractFrame;
        auto& LightData       = Frame.Lighting.LightData;
        auto& Virtual         = Frame.Lighting.VirtualShadow;
        auto& Placement       = VirtualShadowPlacement;
        const FViewVolume& ViewVolume = Frame.ViewVolume;

        // World size of one pixel per meter of distance, from the render height the page requests come from.
        const float RenderHeight       = (float)Math::Max(SceneViews[0].Size.y, 1u);
        const float PixelWorldPerMeter = 2.0f * std::tan(Math::Radians(ViewVolume.GetFOV()) * 0.5f) / RenderHeight;
        const float FootprintScale     = SunShadow::VirtualShadowFootprintScale(PixelWorldPerMeter, DirectionalLight.ShadowResolutionBias);
        constexpr int32 MaxLevel = (int32)VSM_MAX_LEVELS - 1;
        const int32 FirstLevel = Math::Clamp(SunShadow::VirtualShadowLevelForDistance(Math::Max(ViewVolume.GetNear(), 0.05f), FootprintScale), 0, MaxLevel);
        const int32 LastLevel  = Math::Clamp(SunShadow::VirtualShadowLevelForDistance(ShadowFar, FootprintScale), FirstLevel, MaxLevel);

        // Pages come in whole pool rows.
        const uint32 PoolPages = (uint32)Math::Clamp(DirectionalLight.ShadowPoolPages, 256, 4096);
        const uint32 RoundedPool = (PoolPages + VSM_POOL_PAGES_X - 1u) / VSM_POOL_PAGES_X * VSM_POOL_PAGES_X;

        // Zero means never requested, so the count skips it on wrap.
        Placement.Frame = Placement.Frame + 1u == 0u ? 1u : Placement.Frame + 1u;

        // Any turn of the sun moves every texel, and a new pool holds nothing.
        const bool bLightMoved = !Placement.bPlaced || Math::Length(LightDir - Placement.LightDir) > 1e-6f;
        const bool bPoolMoved  = Placement.PoolPages != RoundedPool;
        const float MinHalfDepth = Math::Max(DirectionalLight.ShadowCasterReach, 1.0f);
        const FVector3 Camera = ViewVolume.GetViewPosition();
        for (int32 Level = 0; Level < (int32)VSM_MAX_LEVELS; ++Level)
        {
            const SunShadow::FVirtualShadowLevelBox Box = SunShadow::MakeVirtualShadowLevel(Camera, Level, MinHalfDepth, LightDir, ShadowUp);
            FVirtualShadowLevelGPU& GPU = Virtual.Levels[Level];
            GPU.ViewProjection = Box.ViewProjection;
            GPU.PageOrigin     = Box.PageOrigin;
            GPU.TexelWorld     = Box.TexelWorld;
            GPU.DepthRange     = Box.DepthRange;
            GPU.bActive        = Level >= FirstLevel && Level <= LastLevel ? 1u : 0u;
            GPU.bInvalidate    = (bLightMoved || bPoolMoved || Placement.DepthKeys[Level] != Box.DepthKey) ? 1u : 0u;
            Placement.DepthKeys[Level] = Box.DepthKey;

            if (ShadowData != nullptr)
            {
                ShadowData[Level / 6].ViewProjection[Level % 6] = Box.ViewProjection;
            }
        }
        Placement.LightDir  = LightDir;
        Placement.PoolPages = RoundedPool;
        Placement.bPlaced   = true;

        LightData.SunShadow.FirstLevel     = (uint32)FirstLevel;
        LightData.SunShadow.LastLevel      = (uint32)LastLevel;
        LightData.SunShadow.FootprintScale = FootprintScale;
        LightData.SunShadow.Frame          = Placement.Frame;

        Virtual.bActive        = ShadowData != nullptr;
        Virtual.FirstLevel     = (uint32)FirstLevel;
        Virtual.LastLevel      = (uint32)LastLevel;
        Virtual.PoolPages      = RoundedPool;
        Virtual.Frame          = Placement.Frame;
        Virtual.ShadowDataBase = ShadowData != nullptr ? ShadowDataBase : Constants::kIndexNone;
        Virtual.ViewBase       = Constants::kIndexNoneU32;
        Virtual.NumLevelViews  = 0u;
        Virtual.LODTexels      = Math::Max(DirectionalLight.ShadowLODTexels, 0.25f);

        SunShadowCache.SetEnabled(DirectionalLight.bCacheStaticShadows && ShadowData != nullptr);
        const CRendererSettings* Renderer = GetDefault<CRendererSettings>();
        SunShadowCache.SetSwayDistance(Renderer != nullptr ? Math::Max(Renderer->ShadowSwayDistance, 0.0f) : 0.0f);
    }

    void FDefaultSceneRenderer::ProcessDirectionalLight(const SDirectionalLightComponent& DirectionalLight, const FSkyLightInputs& Sky)
    {
        using namespace SceneRendererExtractLightsInternal;

        FFrameData& Frame          = *ExtractFrame;
        auto& LightData            = Frame.Lighting.LightData;
        auto& ShadowDataCount      = Frame.Lighting.ShadowDataCount;
        auto& SceneGlobalData      = Frame.SceneGlobalData;

        LightData.bHasSun = true;
        const FViewVolume& ViewVolume = Frame.ViewVolume;

        const float NearClip = ViewVolume.GetNear();
        const float FarClip  = ViewVolume.GetFar();

        // Optional black-body tint, a physical sun color from correlated color temperature.
        FVector3 LightColor = DirectionalLight.Color;
        if (DirectionalLight.bUseTemperature)
        {
            LightColor *= ColorTemperatureToRGB(DirectionalLight.Temperature);
        }

        // A 4 km cloud still sees the sun about 2 degrees after ground sunset, so the moon takes the clouds only once none can.
        constexpr float CloudSunCutoffY = -0.08f;
        const FVector3 SunDirection = Math::Normalize(DirectionalLight.Direction);
        Frame.Lighting.SkyMoonDirection = Math::Normalize(Sky.MoonDirection);
        if (!DirectionalLight.bMoonlight || SunDirection.y > CloudSunCutoffY)
        {
            Frame.Lighting.CloudLightDirection = SunDirection;
            Frame.Lighting.CloudLightColor     = LightColor * DirectionalLight.Intensity;
        }
        else
        {
            Frame.Lighting.CloudLightDirection = Math::Normalize(Sky.MoonDirection);
            Frame.Lighting.CloudLightColor     = DirectionalLight.MoonColor * (DirectionalLight.MoonIntensity * Sky.MoonPhaseBrightness);
        }

        if (DirectionalLight.IsMoonLit())
        {
            LightColor = DirectionalLight.MoonColor;
        }

        const FVector3 LightingDirection = DirectionalLight.GetLightingDirection(Sky.MoonDirection);
        float LightIntensity = DirectionalLight.GetLightingIntensity(Sky.MoonDirection, Sky.MoonPhaseBrightness);
        if (Sky.bThroughAtmosphere)
        {
            // Full hue but the square root of the dimming, since under a fixed exposure the true falloff blacks out a golden hour scene.
            const FVector3 Tint      = SkyAtmosphere::SunlightTint(LightingDirection.y, Sky.Altitude, Sky.Aerosol);
            const float    MaxTint   = Math::Max(Tint.x, Math::Max(Tint.y, Tint.z));
            const float    Luminance = Tint.x * 0.2126f + Tint.y * 0.7152f + Tint.z * 0.0722f;
            if (MaxTint > 1e-6f && Luminance > 1e-8f)
            {
                // The packed color is 8 bits a channel, so the brightness rides on the intensity and only the hue on the color.
                LightColor     *= Tint / MaxTint;
                LightIntensity *= MaxTint / std::sqrt(Luminance);
            }
            else
            {
                LightIntensity = 0.0f;
            }
        }

        FLight Light            = {};
        Light.Flags             = ELightFlags::Directional;
        Light.Color             = PackColor(FVector4(LightColor, 1.0));
        Light.Intensity         = LightIntensity;
        Light.Direction         = LightingDirection;
        Light.ShadowDataIndex   = Constants::kIndexNone;
        LightData.SunDirection  = Light.Direction;
        LightData.SkySunDirection = Math::Normalize(DirectionalLight.Direction);
        if (DirectionalLight.bVolumetric)
        {
            Light.Flags             |= ELightFlags::Volumetric;
            Light.VolumetricIntensity = DirectionalLight.VolumetricIntensity;
        }

        // One projection per shadow level, six to a shadow data slot.
        constexpr uint32 SlotsWanted = (VSM_MAX_LEVELS + 5u) / 6u;

        uint32 ShadowSlot             = 0u;
        FLightShadowData* ShadowData  = nullptr;
        if (DirectionalLight.bCastShadows)
        {
            ShadowSlot = ShadowDataCount.fetch_add(SlotsWanted, std::memory_order_acquire);
            if (ShadowSlot + SlotsWanted <= (uint32)MAX_SHADOWS)
            {
                Light.ShadowDataIndex = (int32)ShadowSlot;
                ShadowData            = &Frame.Lighting.Shadows[ShadowSlot];
            }
        }

        SceneGlobalData.CullData.ShadowMaxDistance = DirectionalLight.ShadowMaxDistance;
        const float ShadowFar = Math::Min(FarClip, DirectionalLight.ShadowMaxDistance);

        auto& SunFarShadow           = Frame.Lighting.SunFarShadow;
        SunFarShadow.bTerrain        = DirectionalLight.bCastShadows && DirectionalLight.bTerrainShadows;
        SunFarShadow.ToSun           = Math::Normalize(Light.Direction);
        SunFarShadow.TanHalfAngle    = std::tan(Math::Radians(Math::Clamp(DirectionalLight.SourceAngle, 0.0f, 10.0f)) * 0.5f);
        SunFarShadow.TerrainDistance = Math::Max(DirectionalLight.TerrainShadowDistance, 1.0f);

        // Starts where the shadow pages begin to fade, so the hand-off has no gap.
        SunFarShadow.DFNear         = ShadowFar * (1.0f - Math::Clamp(DirectionalLight.ShadowDistanceFade, 0.0f, 1.0f));
        SunFarShadow.DFFar          = Math::Min(FarClip, DirectionalLight.DistanceFieldShadowDistance);
        SunFarShadow.bDistanceField = DirectionalLight.bCastShadows && DirectionalLight.bDistanceFieldShadows
                                   && SunFarShadow.DFFar > SunFarShadow.DFNear;

        ShadowMinTexels       = Math::Max(DirectionalLight.ShadowMinTexels, 0.0f);

        // A hard edge needs only the center tap, so a blur of zero drops the rest.
        const FSunShadowFilter Filter = GetSunShadowFilter();
        SunFarShadow.ContactSteps  = DirectionalLight.bCastShadows && DirectionalLight.bContactShadows ? Filter.ContactSteps : 0u;
        SunFarShadow.ContactLength = Math::Clamp(DirectionalLight.ContactShadowLength, 0.01f, 2.0f);

        LightData.SunShadow = FSunShadowParams
        {
            .NormalBias         = DirectionalLight.ShadowNormalBias,
            .DepthBias          = DirectionalLight.ShadowDepthBias,
            .FilterRadiusTexels = Filter.RadiusTexels * Math::Max(DirectionalLight.ShadowBlur, 0.0f),
            .DistanceFade       = DirectionalLight.ShadowDistanceFade,
            .TanSunRadius       = SunFarShadow.TanHalfAngle,
            .ShadowFar          = ShadowFar,
        };

        // LookAt degenerates when the light runs along the up axis, as a noon sun overhead does.
        const FVector3 LightDir = Light.Direction;
        const FVector3 ShadowUp = Math::Abs(Math::Dot(Math::Normalize(LightDir), FViewVolume::UpAxis)) > 0.9f
            ? FVector3(1.0f, 0.0f, 0.0f)
            : FViewVolume::UpAxis;
        ProcessVirtualSunShadow(DirectionalLight, LightDir, ShadowUp, ShadowData, (int32)ShadowSlot, ShadowFar);

        // The commit appends the local lights' requests after this one, and runs only once this node is done.
        if (DirectionalLight.LightFunctionMaterial.Get() != nullptr)
        {
            const FVector3 Forward   = -Math::Normalize(Light.Direction);
            const FVector3 Reference = Math::Abs(Forward.y) < 0.99f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
            const FVector3 Right     = Math::Normalize(Math::Cross(Forward, Reference));

            FLightFunctionRequest& Request = Frame.Lighting.LightFunctionRequests.EmplaceGetRef();
            Request.Material        = DirectionalLight.LightFunctionMaterial.Get();
            Request.LightIndex      = 0;
            Request.Type            = ELightFlags::Directional;
            Request.Forward         = Forward;
            Request.Right           = Right;
            Request.Up              = Math::Cross(Right, Forward);
            Request.ProjectionScale = 1.0f / Math::Max(DirectionalLight.LightFunctionScale, 0.01f);
        }

        // Slot 0 is reserved for the sun by ScheduleLightExtract, which is why no index is taken here.
        Frame.Lighting.Lights[0]      = Light;
        Frame.Lighting.LightLayers[0] = DirectionalLight.RenderLayers.Bits;
    }

    void FDefaultSceneRenderer::ResolveLightFunctions()
    {
        auto& Lighting = ExtractFrame->Lighting;
        Lighting.LightFunctionDraws.Reset();

        // Sorted, so the sun stays first and a capped frame keeps the same lights.
        Algo::Sort(Lighting.LightFunctionRequests,
            [](const FLightFunctionRequest& A, const FLightFunctionRequest& B) { return A.LightIndex < B.LightIndex; });

        for (const FLightFunctionRequest& Request : Lighting.LightFunctionRequests)
        {
            if (Lighting.LightFunctionDraws.Num() == MAX_LIGHT_FUNCTIONS)
            {
                LOG_WARN_ONCE("Renderer: more than {} lights carry a light function, so the rest light unmasked.", MAX_LIGHT_FUNCTIONS);
                break;
            }
            if (Request.LightIndex >= NumLiveLights)
            {
                continue;
            }

            FShaderH VS;
            FShaderH PS;
            CMaterialInterface* Material = Request.Material;
            if (!Material->ResolveDomainShaders(EMaterialType::LightFunction, VS, PS) || !Material->RequestTexturesResolved())
            {
                continue;
            }
            const int32 MaterialIndex = Material->GetMaterialIndex();
            if (MaterialIndex < 0)
            {
                continue;
            }

            const uint32 Slot = (uint32)Lighting.LightFunctionDraws.Num();
            FFrameData::FLighting::FLightFunctionDraw& Draw = Lighting.LightFunctionDraws.EmplaceGetRef();
            Draw.Shaders.VertexShader = VS;
            Draw.Shaders.PixelShader  = PS;
            Draw.Request              = Request;
            Draw.MaterialIndex        = (uint32)MaterialIndex;
            Draw.Slot                 = Slot;

            Lighting.LightFunctions[Slot].Right = FVector4(Request.Right, Request.ProjectionScale);
            Lighting.LightFunctions[Slot].Up    = FVector4(Request.Up, 0.0f);

            FLight& Light = Lighting.Lights[Request.LightIndex];
            Light.Flags = (ELightFlags)((uint32)Light.Flags | (uint32)ELightFlags::LightFunction | (Slot << LIGHT_FUNCTION_SLOT_SHIFT));
        }

        Lighting.LightFunctionRequests.Reset();
    }

    void FDefaultSceneRenderer::AllocateShadowTiles()
    {
        FFrameData& Frame = *ExtractFrame;
        auto& LightData       = Frame.Lighting.LightData;
        auto& ShadowRequests  = Frame.Lighting.ShadowRequests;
        auto& ShadowDataCount = Frame.Lighting.ShadowDataCount;
        auto& PackedShadows   = Frame.Lighting.PackedShadows;

        LUMINA_PROFILE_VALUE("Shadows/Requested", ShadowRequests.Num());
        if (ShadowRequests.IsEmpty())
        {
            return;
        }

        {
            const auto&  Virtual         = Frame.Lighting.VirtualShadow;
            const uint32 SunViews        = LightData.bHasSun && Virtual.bActive ? Virtual.LastLevel - Virtual.FirstLevel + 1u : 0u;
            // The camera early and late, each shadow level's static and dynamic view, then every other view.
            const uint32 ReservedViews   = 1u
                                         + SunViews
                                         + SunViews
                                         + 1u
                                         + (uint32)Frame.Views.CaptureViews.Num()
                                         + (Frame.ReflectionProbes.BakingProbe >= 0 ? 6u : 0u)
                                         + (HasCustomDepthPrimitives() ? 1u : 0u);
            const uint32 AvailableViews  = (ReservedViews >= (uint32)GMaxCullViews)
                                         ? 0u
                                         : ((uint32)GMaxCullViews - ReservedViews);

            auto ViewCost = [](const FShadowRequest& Req)
            {
                return Req.Type == ELightType::Point ? (uint32)std::popcount(Req.FaceMask) : 1u;
            };

            uint32 UsedViews = 0u;
            for (const FShadowRequest& Req : ShadowRequests)
            {
                UsedViews += ViewCost(Req);
            }

            if (UsedViews > AvailableViews)
            {
                TVector<uint32>& Order = ShadowDropOrderScratch;
                Order.SetNumUninitialized(ShadowRequests.Num());
                for (int32 i = 0; i < ShadowRequests.Num(); ++i)
                {
                    Order[i] = i;
                }
                Algo::StableSort(Order,
                    [&](uint32 A, uint32 B)
                    {
                        return ShadowRequests[A].DistanceToCamera > ShadowRequests[B].DistanceToCamera;
                    });

                TVector<bool>& Drop = ShadowDropScratch;
                Drop.Init(false, ShadowRequests.Num());
                for (int32 i = 0; i < Order.Num() && UsedViews > AvailableViews; ++i)
                {
                    const uint32 Idx = Order[i];
                    Drop[Idx] = true;
                    UsedViews -= ViewCost(ShadowRequests[Idx]);
                }

                // Compacted in place; moving a fresh vector in would drop the request buffer's capacity.
                int32 Write = 0;
                for (int32 i = 0; i < ShadowRequests.Num(); ++i)
                {
                    if (!Drop[i])
                    {
                        ShadowRequests[Write++] = ShadowRequests[i];
                    }
                }
                ShadowRequests.SetNum(Write);
            }
        }

        if (ShadowRequests.IsEmpty())
        {
            return;
        }

        const FShadowAtlasConfig& AtlasConfig = ShadowAtlas.GetConfig();
        const uint32 MinTile   = AtlasConfig.MinTileResolution;
        const uint32 MaxTile   = AtlasConfig.MaxTileResolution;
        const uint32 AtlasSize = AtlasConfig.AtlasResolution;

        // A point light pays its texel density six times over, so its faces get their own, lower cap.
        uint32 PointFaceCap = 512u;
        if (const CRendererSettings* Settings = GetDefault<CRendererSettings>())
        {
            PointFaceCap = std::bit_floor((uint32)Math::Clamp(Settings->PointShadowResolution, 128, 1024));
        }

        const uint64 Budget = (uint64)AtlasSize * (uint64)AtlasSize;

        const uint32 NumRequests = (uint32)ShadowRequests.Num();

        TVector<uint32>& Sizes = ShadowSizeScratch;
        Sizes.SetNumUninitialized(NumRequests);
        for (uint32 i = 0; i < NumRequests; ++i)
        {
            uint32 V = ShadowRequests[i].DesiredPixels;
            if (V <= 1)
            {
                V = 1;
            }
            else
            {
                --V;
                V |= V >> 1;  V |= V >> 2;  V |= V >> 4;
                V |= V >> 8;  V |= V >> 16;
                ++V;
            }
            Sizes[i] = Math::Clamp(V, MinTile, MaxTile);
            if (ShadowRequests[i].Type == ELightType::Point)
            {
                Sizes[i] = Math::Max(Math::Min(Sizes[i], PointFaceCap), MinTile);
            }
        }

        auto AreaCost = [&](uint32 i) -> uint64
        {
            const uint64 PerTile = (uint64)Sizes[i] * (uint64)Sizes[i];
            return ShadowRequests[i].Type == ELightType::Point ? PerTile * (uint64)std::popcount(ShadowRequests[i].FaceMask) : PerTile;
        };

        auto AreaSum = [&]() -> uint64
        {
            uint64 S = 0;
            for (uint32 i = 0; i < NumRequests; ++i)
            {
                S += AreaCost(i);
            }
            return S;
        };

        while (AreaSum() > Budget)
        {
            uint32 LargestIdx = 0;
            uint32 LargestVal = Sizes[0];
            for (uint32 i = 1; i < NumRequests; ++i)
            {
                if (Sizes[i] > LargestVal)
                {
                    LargestVal = Sizes[i];
                    LargestIdx = i;
                }
            }
            if (LargestVal <= MinTile)
            {
                break;
            }
            Sizes[LargestIdx] = LargestVal >> 1;
        }

        TVector<uint32>& SortedIndices = ShadowSortedScratch;
        SortedIndices.SetNumUninitialized(NumRequests);
        for (uint32 i = 0; i < NumRequests; ++i)
        {
            SortedIndices[i] = i;
        }
        Algo::Sort(SortedIndices,
            [&](uint32 A, uint32 B) { return Sizes[A] > Sizes[B]; });

        for (uint32 SortedI = 0; SortedI < NumRequests; ++SortedI)
        {
            const uint32 ReqIdx = SortedIndices[SortedI];
            AllocateShadowSlot(ShadowAtlas, ShadowRequests[ReqIdx], Sizes[ReqIdx], ShadowDataCount,
                Frame.Lighting.Lights, Frame.Lighting.Shadows, PackedShadows);
        }
    }

}
