#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "World/Scene/RenderScene/SunShadowMath.h"

#include <bit>

namespace Lumina
{
    namespace SceneRendererExtractLightsInternal
    {
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
        auto DirectionalView = Registry.View<ECS::Read<SDirectionalLightComponent>, ECS::Without<SDisabledTag>>();
        auto PointView       = Registry.View<ECS::Read<SPointLightComponent>, ECS::Without<SDisabledTag>>();
        auto SpotView        = Registry.View<ECS::Read<SSpotLightComponent>, ECS::Without<SDisabledTag>>();
        auto AreaView        = Registry.View<ECS::Read<SAreaLightComponent>, ECS::Without<SDisabledTag>>();

        #if USING(WITH_EDITOR)
        bRecordLightIcons = !World->IsGameWorld();
        #else
        bRecordLightIcons = false;
        #endif

        const SDirectionalLightComponent* Sun = SelectActiveSun(DirectionalView, Transforms);

        BuildSceneCullContext(Sun);

        // Needs the captures and the baking probe, both already extracted.
        BuildLightRelevanceVolumes();

        NumPointLightChunks = ChunksFor(PointView.NumDenseSlots(), FLightBatch::Capacity);
        NumSpotLightChunks  = ChunksFor(SpotView.NumDenseSlots(), FLightBatch::Capacity);
        NumAreaLightChunks  = ChunksFor(AreaView.NumDenseSlots(), FLightBatch::Capacity);
        const uint32 NumChunks = NumPointLightChunks + NumSpotLightChunks + NumAreaLightChunks;
        if ((uint32)LightBatches.Num() < NumChunks)
        {
            LightBatches.SetNum(NumChunks);
        }

        // Fixed chunk of dense slots per batch keeps light order stable; ten passes read Lights[0] as the sun, so a local light must never win it.
        const uint32 FirstLocalLight = Sun != nullptr ? 1u : 0u;
        Jobs::Spawn(Jobs::EJobPriority::High, ExtractLatch,
            [this, PointView, SpotView, AreaView, Transforms, Sun, NumChunks, FirstLocalLight]() -> Jobs::TTask<void>
        {
            co_await Jobs::Fork(Jobs::ForEach(NumChunks, [this, PointView, SpotView, AreaView, Transforms](const Jobs::FParallelRange& Range)
            {
                LUMINA_PROFILE_SECTION("Process Local Lights");
                for (uint32 Chunk = Range.Start; Chunk < Range.End; ++Chunk)
                {
                    ProcessLightChunk(Chunk, PointView, SpotView, AreaView, Transforms);
                }
            }, 1));

            if (Sun != nullptr)
            {
                co_await Jobs::Call([this, Sun]
                {
                    LUMINA_PROFILE_SECTION("Process Directional Light");
                    ProcessDirectionalLight(*Sun);
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
                DirectionalLightIcons.Emplace(Entity, Transforms.Get(Entity).GetWorldLocationCached(), Light.Color);
            }
            if (Sun == nullptr && Math::Dot(Light.Direction, Light.Direction) > 0.0001f)
            {
                Sun = &Light;
            }
        });
        return Sun;
    }

    void FDefaultSceneRenderer::ProcessLightChunk(uint32 Chunk, const auto& PointView, const auto& SpotView, const auto& AreaView, const auto& Transforms)
    {
        FLightBatch& Batch = LightBatches[Chunk];
        Batch.NumLights         = 0;
        Batch.NumShadows        = 0;
        Batch.NumLightFunctions = 0;
        Batch.NumShadowLights   = 0;
        Batch.NumIcons          = 0;

        constexpr int32 ChunkSize = (int32)FLightBatch::Capacity;
        if (Chunk < NumPointLightChunks)
        {
            const int32 Begin = (int32)Chunk * ChunkSize;
            PointView.ForEachInRange(Begin, Begin + ChunkSize, [&](ECS::FEntity Entity, const SPointLightComponent& Light)
            {
                ProcessPointLight(Entity, Light, Transforms.Get(Entity), Batch);
            });
            return;
        }

        Chunk -= NumPointLightChunks;
        if (Chunk < NumSpotLightChunks)
        {
            const int32 Begin = (int32)Chunk * ChunkSize;
            SpotView.ForEachInRange(Begin, Begin + ChunkSize, [&](ECS::FEntity Entity, const SSpotLightComponent& Light)
            {
                ProcessSpotLight(Entity, Light, Transforms.Get(Entity), Batch);
            });
            return;
        }

        const int32 Begin = (int32)(Chunk - NumSpotLightChunks) * ChunkSize;
        AreaView.ForEachInRange(Begin, Begin + ChunkSize, [&](ECS::FEntity Entity, const SAreaLightComponent& Light)
        {
            ProcessAreaLight(Entity, Light, Transforms.Get(Entity), Batch);
        });
    }

    void FDefaultSceneRenderer::CommitLightBatches(uint32 FirstLocalLight)
    {
        LUMINA_PROFILE_SECTION("Commit Light Batches");

        FFrameData& Frame     = *ExtractFrame;
        auto&       Lighting  = Frame.Lighting;
        FSceneCullContext& SceneCull = Frame.Geometry.SceneCullContext;

        const uint32 NumChunks = NumPointLightChunks + NumSpotLightChunks + NumAreaLightChunks;
        uint32 Count     = FirstLocalLight;
        bool   bOverflow = false;
        for (uint32 Chunk = 0; Chunk < NumChunks; ++Chunk)
        {
            const FLightBatch& Batch = LightBatches[Chunk];

            if (SceneCull.bEnabled && Batch.NumShadowLights != 0)
            {
                SceneCull.ShadowLights.Append(Batch.ShadowLights, (int32)Batch.NumShadowLights);
            }

            const uint32 Base = Count;
            const uint32 Fits = Base >= (uint32)MAX_LIGHTS ? 0u : Math::Min(Batch.NumLights, (uint32)MAX_LIGHTS - Base);
            bOverflow |= Fits < Batch.NumLights;
            Count     += Batch.NumLights;

            for (uint32 Slot = 0; Slot < Fits; ++Slot)
            {
                Lighting.Lights[Base + Slot] = Batch.Lights[Slot];
            }

            for (uint32 Index = 0; Index < Batch.NumShadows; ++Index)
            {
                if (Batch.Shadows[Index].LightIndex < Fits)
                {
                    Lighting.ShadowRequests.AddGetRef(Batch.Shadows[Index]).LightIndex += Base;
                }
            }

            for (uint32 Index = 0; Index < Batch.NumLightFunctions; ++Index)
            {
                if (Batch.LightFunctions[Index].LightIndex < Fits)
                {
                    Lighting.LightFunctionRequests.AddGetRef(Batch.LightFunctions[Index]).LightIndex += Base;
                }
            }
        }

        NumLiveLights = Math::Min(Count, (uint32)MAX_LIGHTS);
        if (bOverflow)
        {
            NotifyMaxLightsHit();
        }
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

    void FDefaultSceneRenderer::BuildSceneCullContext(const SDirectionalLightComponent* Sun)
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
            SceneCullContext.SunDirection     = Sun->GetLightingDirection();
            SceneCullContext.bHasSun          = true;
            SceneCullContext.SunShadowFrustum = SceneCullContext.Frustum.Extruded(SceneCullContext.SunDirection, ShadowSweepDistance);
        }
    }

    void FDefaultSceneRenderer::ProcessPointLight(ECS::FEntity Entity, const SPointLightComponent& PointLight, const STransformComponent& TransformComponent, FLightBatch& Batch)
    {
        const FVector3 Position = TransformComponent.GetWorldLocationCached();
        if (bRecordLightIcons)
        {
            Batch.Icons[Batch.NumIcons++] = FLightIcon{ Entity, Position, PointLight.LightColor };
        }

        // A switched-off light adds nothing to the image, so it should not take a slot or a cluster test either.
        if (PointLight.Intensity <= 0.0f || !ExtractFrame->ViewLayers.Draws(PointLight.RenderLayers))
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

        FLight Light                = {};
        Light.Flags                 = ELightFlags::Point;
        Light.Falloff               = PointLight.Falloff;
        Light.Color                 = PackColor(FVector4(PointLight.LightColor, 1.0));
        Light.Intensity             = PointLight.Intensity;
        Light.Radius                = Radius;
        Light.Position              = Position;
        Light.ShadowDataIndex       = Constants::kIndexNone;
        if (PointLight.bVolumetric)
        {
            Light.Flags             |= ELightFlags::Volumetric;
            Light.VolumetricIntensity = PointLight.VolumetricIntensity;
            Light.VolumetricScatteringRadius = PointLight.VolumetricScatteringRadius;
        }
        Light.Flags = PackLightContactShadow(Light.Flags, PointLight.bContactShadows, PointLight.ContactShadowLength);
        Light.Flags = PackLightMinRoughness(Light.Flags, PointLight.MinRoughness);

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

        Batch.Lights[Batch.NumLights++] = Light;
    }

    void FDefaultSceneRenderer::ProcessSpotLight(ECS::FEntity Entity, const SSpotLightComponent& SpotLight, const STransformComponent& TransformComponent, FLightBatch& Batch)
    {
        const FVector3 Position = TransformComponent.GetWorldLocationCached();
        if (bRecordLightIcons)
        {
            Batch.Icons[Batch.NumIcons++] = FLightIcon{ Entity, Position, SpotLight.LightColor };
        }

        if (SpotLight.Intensity <= 0.0f || !ExtractFrame->ViewLayers.Draws(SpotLight.RenderLayers))
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

        const FQuat WorldRotation = TransformComponent.GetWorldRotation();
        FVector3 UpdatedForward    = WorldRotation * FViewVolume::ForwardAxis;
        FVector3 UpdatedUp         = WorldRotation * FViewVolume::UpAxis;

        float InnerDegrees = SpotLight.InnerConeAngle;
        float OuterDegrees = SpotLight.OuterConeAngle;

        float InnerCos = Math::Cos(Math::Radians(InnerDegrees));
        float OuterCos = Math::Cos(Math::Radians(OuterDegrees));

        FLight Light                = {};
        Light.Flags                 = ELightFlags::Spot;
        Light.Position              = Position;
        Light.Direction             = Math::Normalize(-UpdatedForward);
        Light.Falloff               = SpotLight.Falloff;
        Light.Color                 = PackColor(FVector4(SpotLight.LightColor, 1.0));
        Light.Intensity             = SpotLight.Intensity;
        Light.Radius                = Radius;
        Light.Angles                = FVector2(InnerCos, OuterCos);
        Light.ShadowDataIndex       = Constants::kIndexNone;
        if (SpotLight.bVolumetric)
        {
            Light.Flags             |= ELightFlags::Volumetric;
            Light.VolumetricIntensity = SpotLight.VolumetricIntensity;
            Light.VolumetricScatteringRadius = SpotLight.VolumetricScatteringRadius;
        }
        Light.Flags = PackLightContactShadow(Light.Flags, SpotLight.bContactShadows, SpotLight.ContactShadowLength);
        Light.Flags = PackLightMinRoughness(Light.Flags, SpotLight.MinRoughness);

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

        Batch.Lights[Batch.NumLights++] = Light;
    }

    void FDefaultSceneRenderer::ProcessAreaLight(ECS::FEntity Entity, const SAreaLightComponent& AreaLight, const STransformComponent& TransformComponent, FLightBatch& Batch)
    {
        const FVector3 Position = TransformComponent.GetWorldLocationCached();
        if (bRecordLightIcons)
        {
            Batch.Icons[Batch.NumIcons++] = FLightIcon{ Entity, Position, AreaLight.LightColor };
        }

        if (AreaLight.Intensity <= 0.0f || !ExtractFrame->ViewLayers.Draws(AreaLight.RenderLayers))
        {
            return;
        }

        const FVector2 HalfSize = FVector2(Math::Max(AreaLight.Width, 0.01f), Math::Max(AreaLight.Height, 0.01f)) * 0.5f;
        // Padded by the half diagonal, so the cluster sphere still covers the reach past the rectangle's corners.
        const float    Radius   = Math::Max(AreaLight.Attenuation, 0.01f) + Math::Length(HalfSize);
        if (!IsLightRelevant(Position, Radius, ExtractFrame->CameraFrustum.IntersectsSphere(Position, Radius)))
        {
            return;
        }

        const FQuat    WorldRotation = TransformComponent.GetWorldRotation();
        const FVector3 Forward       = Math::Normalize(WorldRotation * FViewVolume::ForwardAxis);
        const FVector3 Right         = Math::Normalize(WorldRotation * FViewVolume::RightAxis);

        FLight Light                = {};
        Light.Flags                 = ELightFlags::Area;
        Light.Position              = Position;
        Light.Direction             = -Forward;
        Light.Falloff               = AreaLight.Falloff;
        Light.Color                 = PackColor(FVector4(AreaLight.LightColor, 1.0));
        Light.Intensity             = AreaLight.Intensity;
        Light.Radius                = Radius;
        Light.Angles                = HalfSize;
        Light.ShadowDataIndex       = Constants::kIndexNone;
        Light.VolumetricScatteringRadius = std::bit_cast<float>(PackNormal(Right));
        if (AreaLight.bVolumetric)
        {
            Light.Flags              |= ELightFlags::Volumetric;
            Light.VolumetricIntensity = AreaLight.VolumetricIntensity;
        }
        Light.Flags = PackLightMinRoughness(Light.Flags, AreaLight.MinRoughness);

        Batch.Lights[Batch.NumLights++] = Light;
    }

    void FDefaultSceneRenderer::ProcessDirectionalLight(const SDirectionalLightComponent& DirectionalLight)
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
        if (DirectionalLight.IsMoonLit())
        {
            LightColor = DirectionalLight.MoonColor;
        }

        FLight Light            = {};
        Light.Flags             = ELightFlags::Directional;
        Light.Color             = PackColor(FVector4(LightColor, 1.0));
        Light.Intensity         = DirectionalLight.GetLightingIntensity();
        Light.Direction         = DirectionalLight.GetLightingDirection();
        Light.ShadowDataIndex   = Constants::kIndexNone;
        LightData.SunDirection  = Light.Direction;
        LightData.SkySunDirection = Math::Normalize(DirectionalLight.Direction);
        if (DirectionalLight.bVolumetric)
        {
            Light.Flags             |= ELightFlags::Volumetric;
            Light.VolumetricIntensity = DirectionalLight.VolumetricIntensity;
        }

        uint32 ShadowSlot                  = 0u;
        FLightShadowData* CascadeShadowData = nullptr;
        if (DirectionalLight.bCastShadows)
        {
            ShadowSlot = ShadowDataCount.fetch_add(1, std::memory_order_acquire);
            if (ShadowSlot < (uint32)MAX_SHADOWS)
            {
                Light.ShadowDataIndex = (int32)ShadowSlot;
                CascadeShadowData     = &Frame.Lighting.Shadows[ShadowSlot];
            }
        }

        SceneGlobalData.CullData.ShadowMaxDistance = DirectionalLight.ShadowMaxDistance;

        auto& SunFarShadow           = Frame.Lighting.SunFarShadow;
        SunFarShadow.bTerrain        = DirectionalLight.bCastShadows && DirectionalLight.bTerrainShadows;
        SunFarShadow.ToSun           = Math::Normalize(Light.Direction);
        SunFarShadow.TanHalfAngle    = std::tan(Math::Radians(Math::Clamp(DirectionalLight.SourceAngle, 0.0f, 10.0f)) * 0.5f);
        SunFarShadow.TerrainDistance = Math::Max(DirectionalLight.TerrainShadowDistance, 1.0f);

        // Starts where the cascades begin to fade, so the hand-off has no gap.
        const float CascadeEnd = Math::Min(FarClip, DirectionalLight.ShadowMaxDistance);
        SunFarShadow.DFNear         = CascadeEnd * (1.0f - Math::Clamp(DirectionalLight.ShadowDistanceFade, 0.0f, 1.0f));
        SunFarShadow.DFFar          = Math::Min(FarClip, DirectionalLight.DistanceFieldShadowDistance);
        SunFarShadow.bDistanceField = DirectionalLight.bCastShadows && DirectionalLight.bDistanceFieldShadows
                                   && SunFarShadow.DFFar > SunFarShadow.DFNear;
        SunFarShadow.DFMinRadius    = Math::Max(DirectionalLight.CasterMinScreenRadius, 0.0f) * SunFarShadow.DFNear;
        if (SunFarShadow.bDistanceField)
        {
            // A pixel's distance exceeds its view depth toward the screen corners, so the slice starts a little nearer.
            const FMatrix4 SliceProj = Math::Perspective(Math::Radians(ViewVolume.GetFOV()), ViewVolume.GetAspectRatio(),
                                                         Math::Max(SunFarShadow.DFNear * 0.8f, NearClip), SunFarShadow.DFFar);
            FVector3 Corners[8];
            FFrustum::ComputeFrustumCorners(SliceProj * ViewVolume.GetViewMatrix(), Corners);

            const SunShadow::FSunGrid Grid = SunShadow::FitSunGrid(SunFarShadow.ToSun, Corners, kSunDFGridDimension);
            SunFarShadow.SunRight     = Grid.Right;
            SunFarShadow.SunUp        = Grid.Up;
            SunFarShadow.GridOrigin   = Grid.Origin;
            SunFarShadow.GridCellSize = Grid.CellSize;
        }

        if (!DirectionalLight.bCascadeOcclusionCull)
        {
            SceneGlobalData.CullData.bShadowOcclusionCull = 0u;
        }
        CascadeMinTexels      = Math::Max(DirectionalLight.CascadeMinTexels, 0.0f);
        CasterMinScreenRadius = Math::Max(DirectionalLight.CasterMinScreenRadius, 0.0f);

        constexpr float ShadowMinDistance = 1.0f;

        const float ShadowFar  = Math::Min(FarClip, DirectionalLight.ShadowMaxDistance);
        const float ShadowNear = Math::Max(NearClip, ShadowMinDistance);

        // The far cascade takes the last atlas tile, so it costs the near split one cascade when all four are asked for.
        const float FarCascadeDistance = Math::Min(FarClip, DirectionalLight.FarShadowDistance);
        const bool  bFarCascade        = DirectionalLight.bFarShadowCascade && FarCascadeDistance > ShadowFar;
        NearCascadeCount   = Math::Clamp(DirectionalLight.CascadeCount, 1, bFarCascade ? NumCascades - 1 : NumCascades);
        ActiveCascadeCount = NearCascadeCount + (bFarCascade ? 1 : 0);

        // A hard edge needs only the center tap, so a blur of zero drops the rest.
        const FSunShadowFilter Filter       = GetSunShadowFilter();
        const float            FilterRadius = Filter.RadiusTexels * Math::Max(DirectionalLight.ShadowBlur, 0.0f);
        const uint32           FilterTaps   = FilterRadius > 0.0f ? Filter.Taps : 1u;

        SunFarShadow.ContactSteps  = DirectionalLight.bCastShadows && DirectionalLight.bContactShadows ? Filter.ContactSteps : 0u;
        SunFarShadow.ContactLength = Math::Clamp(DirectionalLight.ContactShadowLength, 0.01f, 2.0f);

        // Shadow tuning forwarded to the lit pixel shaders via the light buffer.
        LightData.SunShadow = FSunShadowParams
        {
            .NormalBias         = DirectionalLight.ShadowNormalBias,
            .DepthBias          = DirectionalLight.ShadowDepthBias,
            .FilterRadiusTexels = FilterRadius,
            .CascadeBlend       = DirectionalLight.CascadeBlend,
            .DistanceFade       = DirectionalLight.ShadowDistanceFade,
            .FilterTaps         = FilterTaps,
            .ActiveCascadeCount = uint32(ActiveCascadeCount),
            .bFarCascade        = bFarCascade ? 1u : 0u,
            .TanSunRadius       = SunFarShadow.TanHalfAngle,
            .BlockerTaps        = FilterTaps > 1u ? Filter.BlockerTaps : 0u,
            .MinFilterRadiusTexels = Math::Min(GSunShadowMinFilterRadiusTexels, FilterRadius),
        };

        const float DistributionExponent = Math::Clamp(DirectionalLight.CascadeDistributionExponent, 1.0f, 10.0f);

        float CascadeFarDistances[NumCascades];
        SunShadow::ComputeCascadeSplits(ShadowNear, ShadowFar, NearCascadeCount, DistributionExponent, bFarCascade,
                                        FarCascadeDistance, CascadeFarDistances);
        for (int i = 0; i < NumCascades; ++i)
        {
            // World distance, measured as view-space Z.
            LightData.CascadeSplits[i] = CascadeFarDistances[i];
        }

        const FMatrix4& CamView   = ViewVolume.GetViewMatrix();
        const float      CamFOV    = ViewVolume.GetFOV();
        const float      CamAspect = ViewVolume.GetAspectRatio();
        const FVector3  LightDir  = Light.Direction;

        // LookAt degenerates when the light runs along the up axis, as a noon sun overhead does.
        const FVector3 ShadowUp = Math::Abs(Math::Dot(Math::Normalize(LightDir), FViewVolume::UpAxis)) > 0.9f
            ? FVector3(1.0f, 0.0f, 0.0f)
            : FViewVolume::UpAxis;

        if (bCascadeHZBTransformsValid)
        {
            for (int i = 0; i < NumCascades; ++i)
            {
                SceneGlobalData.CullData.CascadeHZBViewProjection[i] = CascadeHZBViewProjection[i];
                SceneGlobalData.CullData.CascadeHZBNdcScale[i]       = CascadeHZBNdcScale[i];
            }
            SceneGlobalData.CullData.bCascadeHZBValid = 1u;
        }

        const float CascadeBlendFraction = Math::Clamp(DirectionalLight.CascadeBlend, 0.0f, 1.0f);

        float LastSplitDistance = ShadowNear;
        for (int i = 0; i < ActiveCascadeCount; ++i)
        {
            const float SplitNear = LastSplitDistance;
            const float SplitFar  = CascadeFarDistances[i];

            const int   CascadeRes        = GCSMCascadeSize;
            const float CascadeResFloat   = (float)CascadeRes;
            LightData.CascadeResolutions[i] = CascadeResFloat;

            const FMatrix4 SliceProj = Math::Perspective(Math::Radians(CamFOV), CamAspect, SplitNear, SplitFar);
            const FMatrix4 SliceVP   = SliceProj * CamView;

            FVector3 Corners[8];
            FFrustum::ComputeFrustumCorners(SliceVP, Corners);

            FVector3 SphereCenter(0.0f);
            for (int j = 0; j < 8; ++j)
            {
                SphereCenter += Corners[j];
            }
            SphereCenter /= 8.0f;

            float Radius = 0.0f;
            for (int j = 0; j < 8; ++j)
            {
                Radius = Math::Max(Radius, Math::Length(Corners[j] - SphereCenter));
            }

            const float Octave    = std::exp2(std::floor(std::log2(Math::Max(Radius, 1e-4f))));
            const float QuantStep = Octave / 8.0f;
            Radius = std::ceil(Radius / QuantStep) * QuantStep;
            const float TexelSize = (Radius * 2.0f) / CascadeResFloat;

            const float BackDistance = Math::Max(DirectionalLight.CascadeBackDistance, 1.0f);
            const float OrthoRange   = Radius * 2.0f + BackDistance;

            const FMatrix4 LightRotation = Math::LookAt(
                LightDir * (Radius + BackDistance),
                FVector3(0.0f),
                ShadowUp);

            FVector4 CenterLS = LightRotation * FVector4(SphereCenter, 1.0f);
            CenterLS.x = std::round(CenterLS.x / TexelSize) * TexelSize;
            CenterLS.y = std::round(CenterLS.y / TexelSize) * TexelSize;
            const FVector3 SnappedCenter = FVector3(Math::Inverse(LightRotation) * CenterLS);

            const FMatrix4 LightView = Math::LookAt(
                SnappedCenter + LightDir * (Radius + BackDistance),
                SnappedCenter,
                ShadowUp);

            FMatrix4 LightProjection = Math::Ortho(
                -Radius, +Radius,
                -Radius, +Radius,
                0.0f, OrthoRange);
            LightProjection[1][1] *= -1.0f;

            const FMatrix4 CascadeVP = LightProjection * LightView;
            if (CascadeShadowData)
            {
                CascadeShadowData->ViewProjection[i] = CascadeVP;

                FLightShadow& CascadeTile = CascadeShadowData->Shadow[i];
                // The same count PrepareRender sizes the atlas from, so these UVs match the image this frame draws into.
                const float AtlasW = (float)GCSMAtlasWidth(ActiveCascadeCount);
                CascadeTile.AtlasUVOffset = FVector2(
                    (float)GCSMCascadeOriginX(i) / AtlasW,
                    (float)GCSMCascadeOriginY(i) / (float)GCSMAtlasHeight);
                CascadeTile.AtlasUVScale = FVector2(
                    (float)GCSMCascadeSize / AtlasW,
                    (float)GCSMCascadeSize / (float)GCSMAtlasHeight);
                CascadeTile.ShadowMapIndex  = Constants::kIndexNone;
                CascadeTile.LightIndex      = 0;
                CascadeTile.ShadowDataIndex = (int32)ShadowSlot;
                CascadeTile._Padding        = 0;
            }

            LightData.CascadeRadii[i]       = Radius;
            LightData.CascadeDepthRanges[i] = OrthoRange;

            {
                const float PrevBandNear = (i >= 2) ? CascadeFarDistances[i - 2] : 0.0f;
                // The shader fades near shadows into the far cascade over the larger of the two fractions.
                const float BandFraction = (bFarCascade && i == NearCascadeCount)
                    ? Math::Max(CascadeBlendFraction, Math::Clamp(DirectionalLight.ShadowDistanceFade, 0.0f, 1.0f))
                    : CascadeBlendFraction;
                const float BlendBack    = (i == 0) ? 0.0f : BandFraction * (SplitNear - PrevBandNear);

                const float MinCullNear = Math::Max(NearClip, 0.01f);
                const float CullNear    = (i == 0) ? MinCullNear : Math::Max(SplitNear - BlendBack, MinCullNear);
                const FMatrix4 CullProj = Math::Perspective(Math::Radians(CamFOV), CamAspect, CullNear, SplitFar);

                const FFrustum SliceFrustum = FFrustum::FromViewProjection(CullProj * CamView);
                SceneGlobalData.CullData.CascadeFrustum[i] = AsGPU(SliceFrustum.Extruded(LightDir, OrthoRange));
            }

            CascadeHZBViewProjection[i] = CascadeVP;
            CascadeHZBNdcScale[i]       = FVector4(1.0f / Radius, 1.0f / Radius, 1.0f / OrthoRange, 0.0f);

            SceneGlobalData.CullData.CascadeHZBViewProjectionMid[i] = CascadeVP;
            SceneGlobalData.CullData.CascadeHZBNdcScaleMid[i]       = CascadeHZBNdcScale[i];

            LastSplitDistance = SplitFar;
        }

        // Only true once the loop above has run at least once; the republish at the top reads it.
        bCascadeHZBTransformsValid = true;

        SceneGlobalData.CullData.bCascadeHZBMidValid = 1u;

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
        Frame.Lighting.Lights[0] = Light;
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
            const uint32 SunViews        = LightData.bHasSun ? (uint32)ActiveCascadeCount : 0u;
            // The camera early and late, the cascades in both phases, then every other view.
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

    void FDefaultSceneRenderer::NotifyMaxLightsHit()
    {
        LOG_WARN_ONCE("[Rendering] More than {} lights reach the view, so the rest are left out.", MAX_LIGHTS);
    }
}
