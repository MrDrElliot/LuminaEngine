#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "World/Scene/RenderScene/SunShadowMath.h"

#include <bit>

namespace Lumina
{
    // Row 1 of a view-projection holds the pixel scale at unit distance whatever the rotation, perspective or orthographic.
    static float MakeLODErrorScale(const FMatrix4& ViewProjection, float HeightPixels, float ToleratedPixels)
    {
        if (HeightPixels <= 0.0f || ToleratedPixels <= 0.0f)
        {
            return 0.0f;
        }
        const FVector3 Row1 = FVector3(ViewProjection[0][1], ViewProjection[1][1], ViewProjection[2][1]);
        return 0.5f * HeightPixels * Math::Length(Row1) / ToleratedPixels;
    }

    static float CameraLODPixels()
    {
        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        return 1.0f / Math::Clamp(Settings != nullptr ? Settings->LODDistanceScale : 1.0f, 0.25f, 16.0f);
    }

    static float ShadowLODTexels()
    {
        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        return 1.0f / Math::Clamp(Settings != nullptr ? Settings->ShadowLODScale : 1.0f, 0.25f, 16.0f);
    }

    static FCullView MakeCameraLODView(const FViewVolume& ViewVolume, float DisplayHeight)
    {
        const FMatrix4 CameraVP = ViewVolume.GetProjectionMatrix() * ViewVolume.GetViewMatrix();
        const uint32   Flags    = ViewVolume.IsOrthographic() ? (uint32)ECullViewFlags::OrthographicLOD : 0u;
        float FlagsAsFloat;
        std::memcpy(&FlagsAsFloat, &Flags, sizeof(float));

        FCullView View = {};
        View.ViewOriginAndFlags = FVector4(ViewVolume.GetViewPosition(), FlagsAsFloat);
        View.LODErrorScale      = MakeLODErrorScale(CameraVP, DisplayHeight, CameraLODPixels());
        return View;
    }

    #if !defined(LE_SHIPPING)
    // Scripts and agents have no viewport menu, so this forces a debug view mode by its ERenderSceneDebugFlags value.
    static TConsoleVar<int32> CVarDebugViewOverride("r.DebugViewOverride", -1,
        "Forces the scene debug view to this ERenderSceneDebugFlags value in every view; -1 leaves each viewport's own.");
    #endif

    namespace
    {
        static TAtomic<uint32> GReflectionProbeRebakeRequests{0};

        static FAutoConsoleCommand GCmdRebakeReflectionProbes(
            "r.ReflectionProbes.Rebake",
            "Recapture every reflection probe. Needed after moving world geometry, which does not itself "
            "invalidate a bake (only changing a probe does).",
            []{ RequestReflectionProbeRebake(); });
    }

    void RequestReflectionProbeRebake()
    {
        GReflectionProbeRebakeRequests.fetch_add(1, std::memory_order_relaxed);
    }

    namespace
    {
        // Defined in the terrain helper block below; Extract prep.
        void PrepareTerrainExtract(STerrainComponent& Terrain, const FMatrix4& WorldMatrix, FDefaultSceneRenderer::FFrameData::FTerrainExtract& Out);

        // Flattens the terrain material's declared species into the extract. Reads the material rather than
        // the component on purpose: the GrassOutput node is the authoring surface, the component only says
        // "this terrain grows grass" and carries the budget.
        void PrepareGrassExtract(ECS::FRegistry& Registry, ECS::FEntity Entity, const STerrainComponent& Terrain,
                                 FDefaultSceneRenderer::FFrameData::FTerrainExtract& Out);

        // Reuses the live element so its heap buffers outlive the frame instead of being freed and remade.
        template <typename T>
        T& ReuseAt(TVector<T>& Container, SIZE_T Index)
        {
            return (Index < Container.size()) ? Container[Index] : Container.emplace_back();
        }
    }

    void FDefaultSceneRenderer::Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Render Scene");

        ExtractFrame = &FrameData;
        FFrameData& Frame = *ExtractFrame;

        RefreshFrameSettings();

        // Before anything reads the primary size, and only on a frame that will also render.
        ApplyPendingPrimarySize();
        RefreshPrimaryRenderSize();

        Frame.bExtractedThisFrame  = false;
        Frame.CachedWorldDeltaTime = (float)World->GetWorldDeltaTime();
        Frame.ViewVolume           = ViewVolume;

        // PostProcess is a stack temporary in CWorld::Extract, so value-copy it.
        if (PostProcess != nullptr)
        {
            Frame.PostProcess.ActivePostProcessStorage = *PostProcess;
            Frame.PostProcess.bHasActivePostProcess    = true;
        }
        else
        {
            Frame.PostProcess.bHasActivePostProcess    = false;
        }

        Frame.PostProcess.ActivePostProcessMaterials.clear();
        for (CMaterialInterface* PPInterface : PendingPostProcessMaterials)
        {
            FShaderH VS;
            FShaderH PS;
            if (PPInterface == nullptr || !PPInterface->ResolveDomainShaders(EMaterialType::PostProcess, VS, PS))
            {
                continue;
            }
            // Held back until its textures land, since the placeholder would tint the whole frame.
            if (!PPInterface->RequestTexturesResolved())
            {
                continue;
            }
            // VS/PS from the concrete material; index from the interface (instances own their param slot).
            FFrameData::FPostProcessMaterial& Out = Frame.PostProcess.ActivePostProcessMaterials.emplace_back();
            Out.Shaders.VertexShader = VS;
            Out.Shaders.PixelShader  = PS;
            Out.MaterialIndex        = (uint32)PPInterface->GetMaterialIndex();
        }

        const FUIntVector2 PrimarySize = SceneViews[0].Size;

        FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;
        SceneGlobalData.CameraData.Location             = FVector4(ViewVolume.GetViewPosition(), 1.0f);
        SceneGlobalData.CameraData.Up                   = FVector4(ViewVolume.GetUpVector(), 1.0f);
        SceneGlobalData.CameraData.Right                = FVector4(ViewVolume.GetRightVector(), 1.0f);
        SceneGlobalData.CameraData.Forward              = FVector4(ViewVolume.GetForwardVector(), 1.0f);
        SceneGlobalData.CameraData.View                 = ViewVolume.GetViewMatrix();
        SceneGlobalData.CameraData.InverseView          = ViewVolume.GetInverseViewMatrix();
        SceneGlobalData.CameraData.Projection           = ViewVolume.GetProjectionMatrix();
        SceneGlobalData.CameraData.InverseProjection    = ViewVolume.GetInverseProjectionMatrix();

        {
            FSceneView& PrimaryView = SceneViews[0];

            // Motion is measured between unjittered positions, so the history matrix must not carry a jitter.
            PrimaryView.PendingViewProjection = SceneGlobalData.CameraData.Projection * SceneGlobalData.CameraData.View;
            SceneGlobalData.TemporalJitterU = 0.0f;
            SceneGlobalData.TemporalJitterV = 0.0f;

            if (IsTemporalAAEnabledFor(PrimaryView) || IsTemporalUpscalerFor(PrimaryView))
            {
                // A bailed extract must not flip parity, or the resolve blends a slot against itself.
                PrimaryView.PendingTemporalFrameIndex = PrimaryView.TemporalFrameIndex + 1;

                // Column-major, so the mathematical row-0/row-1 of column 2 are these two elements.
                const FVector2 Jitter = GetTemporalJitterNDC(PrimaryView, PrimaryView.PendingTemporalFrameIndex);
                SceneGlobalData.CameraData.Projection[2][0] += Jitter.x;
                SceneGlobalData.CameraData.Projection[2][1] += Jitter.y;
                SceneGlobalData.TemporalJitterU = Jitter.x * 0.5f;
                SceneGlobalData.TemporalJitterV = Jitter.y * 0.5f;

                // Every depth-reconstructing pass reads this, so it has to match the jitter that was rendered.
                SceneGlobalData.CameraData.InverseProjection = Math::Inverse(SceneGlobalData.CameraData.Projection);

                // Same period as the jitter, or the samples the resolve averages never repeat and never settle.
                SceneGlobalData.TemporalPhase = PrimaryView.PendingTemporalFrameIndex % GetTemporalPhaseCount(PrimaryView);
            }
            else
            {
                PrimaryView.bTemporalHistoryValid = false;
                SceneGlobalData.TemporalPhase    = 0u;
            }

            SceneGlobalData.CameraData.PrevViewProjection = PrimaryView.PrevViewProjection;
        }
        const FUIntVector2 DisplaySize = SceneViews[0].DisplaySize;
        SceneGlobalData.ScreenSize                      = FUIntVector4(PrimarySize.x, PrimarySize.y, DisplaySize.x, DisplaySize.y);
        // Below zero when upscaling, so textures pick the mips the display resolution would have.
        SceneGlobalData.TextureMipBias                  = PrimarySize.x < DisplaySize.x
                                                        ? Math::Log2((float)PrimarySize.x / (float)DisplaySize.x) : 0.0f;
        SceneGlobalData.GridSize                        = ComputeClusterGrid(PrimarySize);
        SceneGlobalData.Time                            = (float)World->GetTimeSinceWorldCreation();
        SceneGlobalData.DeltaTime                       = Frame.CachedWorldDeltaTime;
        // Derived rather than remembered, so it cannot drift out of step with the clock the graph reads.
        SceneGlobalData.PrevTime                        = SceneGlobalData.Time - SceneGlobalData.DeltaTime;
        SceneGlobalData.FarPlane                        = ViewVolume.GetFar();
        SceneGlobalData.NearPlane                       = ViewVolume.GetNear();
        SceneGlobalData.GTAOSettings                    = FGTAOSettings{};
        Frame.Lighting.SunFarShadow                     = {};
        SceneGlobalData.ParallaxSettings.SampleScale       = 1.0f;
        SceneGlobalData.ParallaxSettings.LODBias           = 0.0f;
        SceneGlobalData.ParallaxSettings.ShadowSampleScale = 1.0f;
        Frame.CameraFrustum                             = ViewVolume.GetFrustum();
        SceneGlobalData.CullData.Frustum                = AsGPU(Frame.CameraFrustum);
        SceneGlobalData.CullData.ShadowFrustum          = SceneGlobalData.CullData.Frustum; // Rebuilt after directional light is processed.
        SceneGlobalData.CullData.bHasDirectional        = 0u;
        SceneGlobalData.CullData.bFrustumCull           = FrameSettings.bFrustumCull;
        SceneGlobalData.CullData.bOcclusionCull         = FrameSettings.bOcclusionCull;

        // Copied, not aliased, since freezing the cull replaces these and leaves the render camera.
        SceneGlobalData.CullData.CullCameraPosition     = SceneGlobalData.CameraData.Location;
        SceneGlobalData.CullData.CullCameraView         = SceneGlobalData.CameraData.View;
        SceneGlobalData.CullData.CullCameraProjection   = SceneGlobalData.CameraData.Projection;
        SceneGlobalData.CullData.CullNearPlane          = SceneGlobalData.NearPlane;
        SceneGlobalData.CullData.CullFarPlane           = SceneGlobalData.FarPlane;
        SceneGlobalData.CullData.ShadowMaxDistance      = 5000.0f;
        SceneGlobalData.CullData.bShadowOcclusionCull   = FrameSettings.bShadowOcclusionCull;
        CascadeMinTexels                                = 1.0f;
        // Built here because the skinned gather picks against it on workers before the cull views exist.
        Frame.Views.CameraLODView                       = MakeCameraLODView(ViewVolume, (float)DisplaySize.y);
        SceneGlobalData.CullData.DebugMode              = (uint32)FrameSettings.Flags;
        #if !defined(LE_SHIPPING)
        if (const int32 Override = CVarDebugViewOverride.GetValue(); Override >= 0)
        {
            SceneGlobalData.CullData.DebugMode = (uint32)Override;
        }
        #endif
        SceneGlobalData.CullData.bCascadeHZBValid       = 0u;
        SceneGlobalData.CullData.bCascadeHZBMidValid    = 0u;

        CMaterial* FallbackMaterial = CMaterial::GetDefaultMaterial();
        if (!IsValid(FallbackMaterial) || !FallbackMaterial->IsReadyForRender())
        {
            // A frame or two at startup is normal; a default material that never compiled never recovers.
            if (!bWarnedFallbackMaterial)
            {
                bWarnedFallbackMaterial = true;
                LOG_WARN("Extract skipped: the default material is {}. Every frame is skipped until it is "
                         "ready, and the viewport shows its clear color.",
                         IsValid(FallbackMaterial) ? "still compiling" : "missing");
            }

            ExtractFrame = nullptr;

            // Nothing rendered, so next frame's HZB is not this frame's depth.
            bDepthPyramidValid.store(false, std::memory_order_release);
            return;
        }

        if (bWarnedFallbackMaterial)
        {
            bWarnedFallbackMaterial = false;
            LOG_DISPLAY("Default material is ready; extract resumed.");
        }

        ResetPass_Extract();

        ExtractReflectionProbes(ECS::GetWorldRegistry(*World), Frame);
        ExtractSplines(ECS::GetWorldRegistry(*World), Frame);

        for (int32 i = 1; i < (int32)SceneViews.size(); ++i)
        {
            if (!SceneViews[i].bEnabled)
            {
                continue;
            }
            FFrameData::FCaptureViewData Capture;
            Capture.ViewVolume     = SceneViews[i].PendingViewVolume;
            Capture.SceneViewIndex = i;
            Frame.Views.CaptureViews.push_back(Capture);
        }

        // CPU half is the parallel ECS gather and cull setup.
        CompileDrawCommands_Extract();

        for (FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            const FViewVolume& VV = Capture.ViewVolume;
            FSceneGlobalData& Data = Capture.SceneGlobalData;
            Data = Frame.SceneGlobalData;
            Data.CameraData.Location          = FVector4(VV.GetViewPosition(), 1.0f);
            Data.CameraData.Up                = FVector4(VV.GetUpVector(), 1.0f);
            Data.CameraData.Right             = FVector4(VV.GetRightVector(), 1.0f);
            Data.CameraData.Forward           = FVector4(VV.GetForwardVector(), 1.0f);
            Data.CameraData.View              = VV.GetViewMatrix();
            Data.CameraData.InverseView       = VV.GetInverseViewMatrix();
            Data.CameraData.Projection        = VV.GetProjectionMatrix();
            Data.CameraData.InverseProjection = VV.GetInverseProjectionMatrix();
            const FUIntVector2 CaptureSize      = SceneViews[Capture.SceneViewIndex].Size;
            Data.ScreenSize                   = FUIntVector4(CaptureSize.x, CaptureSize.y, CaptureSize.x, CaptureSize.y);
            Data.TextureMipBias               = 0.0f;
            Data.GridSize                     = ComputeClusterGrid(CaptureSize);
            Data.FarPlane                     = VV.GetFar();
            Data.NearPlane                    = VV.GetNear();
            Data.CullData.Frustum             = AsGPU(VV.GetFrustum());
            // No resolve runs here, so a varying phase would just boil the capture's screen-space noise.
            Data.TemporalPhase                = 0u;
        }

        if (Frame.ReflectionProbes.BakingProbe >= 0)
        {
            const uint32 FaceSize = Frame.ReflectionProbes.BakeFaceSize;
            for (int32 Face = 0; Face < 6; ++Face)
            {
                const FViewVolume& VV  = Frame.ReflectionProbes.FaceVolumes[Face];
                FSceneGlobalData& Data = Frame.ReflectionProbes.FaceGlobals[Face];
                Data = Frame.SceneGlobalData;
                Data.CameraData.Location          = FVector4(VV.GetViewPosition(), 1.0f);
                Data.CameraData.Up                = FVector4(VV.GetUpVector(), 1.0f);
                Data.CameraData.Right             = FVector4(VV.GetRightVector(), 1.0f);
                Data.CameraData.Forward           = FVector4(VV.GetForwardVector(), 1.0f);
                Data.CameraData.View              = VV.GetViewMatrix();
                Data.CameraData.InverseView       = VV.GetInverseViewMatrix();
                Data.CameraData.Projection        = VV.GetProjectionMatrix();
                Data.CameraData.InverseProjection = VV.GetInverseProjectionMatrix();
                Data.ScreenSize                   = FUIntVector4(FaceSize, FaceSize, FaceSize, FaceSize);
                Data.TextureMipBias               = 0.0f;
                Data.GridSize                     = ComputeClusterGrid(FUIntVector2(FaceSize, FaceSize));
                Data.FarPlane                     = VV.GetFar();
                Data.NearPlane                    = VV.GetNear();
                Data.CullData.Frustum             = AsGPU(VV.GetFrustum());
                Data.TemporalPhase                = 0u;
            }
        }

        Frame.Lighting.AtlasTiles = ShadowAtlas.GetAllocatedTiles();

        for (uint32 Channel = 0; Channel < FImmediateLineRenderer::NumChannels; ++Channel)
        {
            Frame.Primitives.ImmediateLines[Channel] = ImmediateLines.Snapshot((FImmediateLineRenderer::EChannel)Channel);
        }

        Frame.bExtractedThisFrame = true;
        SceneViews[0].TemporalFrameIndex = SceneViews[0].PendingTemporalFrameIndex;
        SceneViews[0].PrevViewProjection = SceneViews[0].PendingViewProjection;

        ExtractFrame = nullptr;
    }

    void FDefaultSceneRenderer::ExtractSplines(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Extract Splines");

        auto& Splines = Frame.Splines.Splines;
        auto& Points  = Frame.Splines.Points;
        auto& Samples = Frame.Splines.Samples;
        Splines.clear();
        Points.clear();
        Samples.clear();

        auto SplineView = Registry.View<SSplineComponent, STransformComponent>(ECS::TExclude<SDisabledTag>{});

        static thread_local TVector<FSplineSample> ScratchSamples;
        ScratchSamples.clear();

        SplineView.ForEach([&](ECS::FEntity Entity, const SSplineComponent& Spline, const STransformComponent& Transform)
        {
            // The opt-in is the whole point, since pure authoring data must cost no upload.
            if (!Spline.bSendToGPU || Spline.Points.empty())
            {
                return;
            }

            const FMatrix4 LocalToWorld = Transform.GetWorldMatrix();

            ScratchSamples.clear();
            const float TotalLength = BuildSplineSamples(Spline, LocalToWorld, ScratchSamples);

            FGPUSpline Header;
            Header.LocalToWorld = LocalToWorld;
            Header.WorldToLocal = Math::Inverse(LocalToWorld);
            Header.PointOffset  = (uint32)Points.size();
            Header.PointCount   = (uint32)Spline.Points.size();
            Header.SampleOffset = (uint32)Samples.size();
            Header.SampleCount  = (uint32)ScratchSamples.size();
            Header.TotalLength  = TotalLength;
            Header.Flags        = Spline.bClosedLoop ? SPLINE_FLAG_CLOSED_LOOP : 0u;
            Header.EntityID     = (uint32)(Entity).Value;
            Header._Pad         = 0u;

            // Control points go up in WORLD space to match the samples, so no shader needs to know which.
            for (const SSplinePoint& Point : Spline.Points)
            {
                FGPUSplinePoint Gpu;
                Gpu.Location      = FVector3(LocalToWorld * FVector4(Point.Location, 1.0f));
                Gpu.Roll          = Point.Roll;
                // Tangents are directions, so w = 0 keeps the translation out of them.
                Gpu.ArriveTangent = FVector3(LocalToWorld * FVector4(Point.ArriveTangent, 0.0f));
                Gpu._Pad0         = 0.0f;
                Gpu.LeaveTangent  = FVector3(LocalToWorld * FVector4(Point.LeaveTangent, 0.0f));
                Gpu._Pad1         = 0.0f;
                Gpu.Scale         = Point.Scale;
                Gpu._Pad2         = 0.0f;
                Points.push_back(Gpu);
            }

            for (const FSplineSample& Sample : ScratchSamples)
            {
                FGPUSplineSample Gpu;
                Gpu.Position      = Sample.Position;
                Gpu.DistanceAlong = Sample.DistanceAlong;
                Gpu.Tangent       = Sample.Tangent;
                Gpu.Key           = Sample.Key;
                Gpu.Up            = Sample.Up;
                Gpu.Roll          = Sample.Roll;
                Gpu.Scale         = Sample.Scale;
                Gpu._Pad          = 0.0f;
                Samples.push_back(Gpu);
            }

            Splines.push_back(Header);
        });
    }

    void FDefaultSceneRenderer::ExtractReflectionProbes(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Extract Reflection Probes");

        auto& Probes   = Frame.ReflectionProbes.Probes;
        auto& Captures = Frame.ReflectionProbes.Captures;
        Probes.clear();
        Captures.clear();
        Frame.ReflectionProbes.bNeedsRebake = false;

        auto ProbeView = Registry.View<SReflectionProbeComponent, STransformComponent>(ECS::TExclude<SDisabledTag>{});

        struct FProbeSortEntry
        {
            int32                   Priority;
            FGPUReflectionProbe     Gpu;
            FReflectionProbeCapture Capture;
        };
        static thread_local TVector<FProbeSortEntry> Sorted;
        Sorted.clear();

        ProbeView.ForEach([&](ECS::FEntity Entity, const SReflectionProbeComponent& Probe, const STransformComponent& Transform)
        {
            if (!Probe.bEnabled || Sorted.size() >= MaxReflectionProbes)
            {
                return;
            }

            const FVector3 Extent = (Probe.Shape == EReflectionProbeShape::Sphere)
                                        ? FVector3(Math::Max(Probe.Extent.x, 0.001f))
                                        : Math::Max(Probe.Extent, FVector3(0.001f));

            const FMatrix4 WorldMatrix = Transform.GetWorldMatrix();

            FProbeSortEntry Entry;
            Entry.Gpu.ProbeToWorld    = Math::Scale(WorldMatrix, Extent);
            Entry.Gpu.WorldToProbe    = Math::Inverse(Entry.Gpu.ProbeToWorld);

            const FVector3 CaptureWorld = FVector3(WorldMatrix * FVector4(Probe.CaptureOffset, 1.0f));
            Entry.Gpu.CapturePosition = FVector4(CaptureWorld, 0.0f);
            Entry.Gpu.Params          = FVector4(Math::Max(Probe.Brightness, 0.0f),
                                                 Probe.Shape == EReflectionProbeShape::Sphere ? 1.0f : 0.0f,
                                                 0.0f,  // slice, assigned after sorting
                                                 Math::Max(Probe.BlendDistance, 0.0f));

            Entry.Capture.Position  = CaptureWorld;
            Entry.Capture.NearPlane = Math::Max(Probe.CaptureNearPlane, 0.001f);
            Entry.Capture.FarPlane  = Math::Max(Probe.CaptureFarPlane, Entry.Capture.NearPlane + 1.0f);
            Entry.Capture.FaceSize  = GetReflectionProbeFaceSize(Probe.Resolution);
            Entry.Capture.bAlwaysUpdate = (Probe.UpdateMode == EReflectionProbeUpdateMode::Always);
            Entry.Capture.bClearToColor = (Probe.ClearMode == EReflectionProbeClearMode::SolidColor);
            Entry.Capture.ClearColor    = Probe.BackgroundColor;

            Entry.Priority = Probe.Priority;
            Sorted.push_back(Entry);
        });

        Algo::StableSort(Sorted, [](const FProbeSortEntry& A, const FProbeSortEntry& B)
        {
            return A.Priority > B.Priority;
        });

        Probes.reserve(Sorted.size());
        Captures.reserve(Sorted.size());
        for (uint32 i = 0; i < (uint32)Sorted.size(); ++i)
        {
            FGPUReflectionProbe Gpu = Sorted[i].Gpu;
            Gpu.Params.z = (float)i;   // slice = final position in the array
            Probes.push_back(Gpu);
            Captures.push_back(Sorted[i].Capture);
        }

        constexpr float ProbeEpsilon = 1e-4f;

        auto MatrixNearlyEqual = [](const FMatrix4& A, const FMatrix4& B)
        {
            for (int32 c = 0; c < 4; ++c)
            {
                for (int32 r = 0; r < 4; ++r)
                {
                    if (Math::Abs(A[c][r] - B[c][r]) > ProbeEpsilon)
                    {
                        return false;
                    }
                }
            }
            return true;
        };

        const bool bLayoutChanged = (Probes.size() != LastExtractedProbes.size());

        bool bContentChanged = bLayoutChanged;
        if (!bContentChanged)
        {
            for (uint32 i = 0; i < (uint32)Probes.size() && !bContentChanged; ++i)
            {
                bContentChanged =
                       !MatrixNearlyEqual(Probes[i].WorldToProbe, LastExtractedProbes[i].WorldToProbe)
                    || Math::Abs(Probes[i].CapturePosition.x - LastExtractedProbes[i].CapturePosition.x) > ProbeEpsilon
                    || Math::Abs(Probes[i].CapturePosition.y - LastExtractedProbes[i].CapturePosition.y) > ProbeEpsilon
                    || Math::Abs(Probes[i].CapturePosition.z - LastExtractedProbes[i].CapturePosition.z) > ProbeEpsilon
                    || (Captures[i].FaceSize  != LastExtractedCaptures[i].FaceSize)
                    || (Captures[i].NearPlane != LastExtractedCaptures[i].NearPlane)
                    || (Captures[i].FarPlane  != LastExtractedCaptures[i].FarPlane);
            }
        }

        if (bContentChanged)
        {
            Frame.ReflectionProbes.bNeedsRebake = true;
            LastExtractedProbes   = Probes;
            LastExtractedCaptures = Captures;
        }
        Frame.ReflectionProbes.bLayoutChanged = bLayoutChanged;

        const uint32 RebakeRequests = GReflectionProbeRebakeRequests.load(std::memory_order_relaxed);
        if (RebakeRequests != LastSeenRebakeRequest)
        {
            LastSeenRebakeRequest = RebakeRequests;
            Frame.ReflectionProbes.bNeedsRebake = true;
        }

        BakedProbeMask |= CompletedProbeBakes.exchange(0, std::memory_order_acq_rel);

        if (Frame.ReflectionProbes.bNeedsRebake)
        {
            if (Frame.ReflectionProbes.bLayoutChanged)
            {
                BakedProbeMask = 0;
            }

            PendingProbeBakes.clear();
            for (uint32 i = 0; i < (uint32)Probes.size(); ++i)
            {
                PendingProbeBakes.push_back(i);
            }
        }

        for (uint32 i = 0; i < (uint32)Probes.size(); ++i)
        {
            Probes[i].CapturePosition.w = ((BakedProbeMask >> i) & 1u) ? 1.0f : 0.0f;
        }

        ScheduleReflectionProbeBake(Frame);
    }

    void FDefaultSceneRenderer::ScheduleReflectionProbeBake(FFrameData& Frame)
    {
        auto& Bake = Frame.ReflectionProbes;
        Bake.BakingProbe   = -1;
        Bake.BakeViewIndex = -1;

        const uint32 NumProbes = (uint32)Bake.Captures.size();

        uint32 ProbeIndex = Constants::kIndexNoneU32;
        bool   bFromQueue = false;

        while (!PendingProbeBakes.empty())
        {
            const uint32 Queued = PendingProbeBakes.front();
            if (Queued < NumProbes)
            {
                ProbeIndex = Queued;
                bFromQueue = true;
                break;
            }
            // The set shrank out from under a queued index; drop it.
            PendingProbeBakes.erase(PendingProbeBakes.begin());
        }

        if (ProbeIndex == Constants::kIndexNoneU32)
        {
            for (uint32 Step = 0; Step < NumProbes; ++Step)
            {
                const uint32 Candidate = (AlwaysProbeCursor + Step) % NumProbes;
                if (Bake.Captures[Candidate].bAlwaysUpdate)
                {
                    ProbeIndex        = Candidate;
                    AlwaysProbeCursor = (Candidate + 1) % NumProbes;
                    break;
                }
            }
        }

        if (ProbeIndex == Constants::kIndexNoneU32)
        {
            return;
        }

        const FReflectionProbeCapture& Capture = Bake.Captures[ProbeIndex];

        if (ProbeBakeViewIndex >= 0 && ProbeBakeViewSize != Capture.FaceSize)
        {
            // Tier changed. Release the reservation so the wrong-sized view returns to the pool.
            if (ProbeBakeViewIndex < (int32)SceneViews.size())
            {
                SceneViews[ProbeBakeViewIndex].bReservedForProbeBake = false;
            }
            ProbeBakeViewIndex = -1;
        }

        if (ProbeBakeViewIndex < 0)
        {
            const int32 ViewIndex = RegisterCaptureView(FUIntVector2(Capture.FaceSize, Capture.FaceSize));
            if (ViewIndex < 0)
            {
                // Out of view slots; leave the probe queued rather than dropping it.
                return;
            }
            SceneViews[ViewIndex].bReservedForProbeBake = true;
            ProbeBakeViewIndex = ViewIndex;
            ProbeBakeViewSize  = Capture.FaceSize;
        }

        const int32 ViewIndex = ProbeBakeViewIndex;

        static const FVector3 FaceForward[6] = {
            FVector3( 1.0f,  0.0f,  0.0f), FVector3(-1.0f,  0.0f,  0.0f),
            FVector3( 0.0f,  1.0f,  0.0f), FVector3( 0.0f, -1.0f,  0.0f),
            FVector3( 0.0f,  0.0f,  1.0f), FVector3( 0.0f,  0.0f, -1.0f),
        };
        static const FVector3 FaceUp[6] = {
            FVector3( 0.0f,  1.0f,  0.0f), FVector3( 0.0f,  1.0f,  0.0f),
            FVector3( 0.0f,  0.0f, -1.0f), FVector3( 0.0f,  0.0f,  1.0f),
            FVector3( 0.0f,  1.0f,  0.0f), FVector3( 0.0f,  1.0f,  0.0f),
        };

        for (int32 Face = 0; Face < 6; ++Face)
        {
            // 90 degrees at aspect 1 is what makes six frusta tile the sphere exactly with no seam.
            FViewVolume& Volume = Bake.FaceVolumes[Face];
            Volume = FViewVolume(90.0f, 1.0f, Capture.NearPlane, Capture.FarPlane);
            Volume.SetView(Capture.Position, FaceForward[Face], FaceUp[Face]);
            Bake.FaceCullViews[Face] = Constants::kIndexNoneU32;   // filled by BuildCullViews
        }

        Bake.BakingProbe   = (int32)ProbeIndex;
        Bake.BakeViewIndex = ViewIndex;
        Bake.BakeFaceSize  = Capture.FaceSize;
        if (bFromQueue)
        {
            PendingProbeBakes.erase(PendingProbeBakes.begin());
        }
    }

    static FIBLBakeResolution ResolveIBLQuality(EIBLQuality Quality)
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

    FDefaultSceneRenderer::FThreadLocalDrawData& FDefaultSceneRenderer::AcquireThreadLocalDrawData(uint32 Slot)
    {
        // PrepareGatherScratch already reset every entry, so this only marks it for the merge.
        FThreadLocalDrawData& Local = ThreadLocalStorage[Slot];
        Local.bTouched = true;
        return Local;
    }

    // Routes this frame's transform + component changes into the persistent primitive table.
    void FDefaultSceneRenderer::SyncScenePrimitives()
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        // The bounds build reads the retained slots this sync rewrites.
        JoinInstanceBlockBounds();

        MovedTransformScratch.clear();
        ECS::Utils::DrainMovedTransforms(Registry, MovedTransformScratch);
        ScenePrimitives.Sync(*World, TSpan<const ECS::FEntity>(MovedTransformScratch.data(), MovedTransformScratch.size()));

        PublishRetainedUpload();
        LaunchInstanceBlockBounds();
    }

    // Collects what changed in the retained scene, before the render phase uploads it.
    void FDefaultSceneRenderer::PublishRetainedUpload()
    {
        FFrameData::FGeometry::FRetainedUpload& Out = ExtractFrame->Geometry.RetainedUpload;

        // Ahead of everything below, since a base that moves marks its slot static-dirty.
        ScenePrimitives.AssignMeshletVisibility();
        Out.MeshletVisibilityWords = ScenePrimitives.GetMeshletVisibilityExtent();

        const uint32 SlotCount = ScenePrimitives.GetRetainedSlotCount();
        Out.SlotCount = SlotCount;

        Out.DirtySlots.clear();
        Out.DirtyStaticSlots.clear();

        const uint32 DeviceCapacity = RetainedDeviceCapacity.load(std::memory_order_acquire);
        // Growth keeps the device contents, so only a device holding nothing valid needs everything again.
        const bool bDeviceEmpty = DeviceCapacity == 0;
        Out.bFull       = ScenePrimitives.NeedsFullInstanceUpload() || bDeviceEmpty
                       || FScenePrimitiveSet::ShouldResendAllInstances(ScenePrimitives.GetDirtyInstanceSlots().size(), SlotCount);
        Out.bFullStatic = ScenePrimitives.NeedsFullStaticUpload() || bDeviceEmpty
                       || ScenePrimitives.GetDirtyStaticSlots().size() * 4 >= (SIZE_T)SlotCount;

        if (!Out.bFull || !Out.bFullStatic)
        {
            // A bitmap over the slot range yields the sorted, unique list in linear time, where a sort did not.
            DirtySlotBits.resize(((SIZE_T)SlotCount + 63u) / 64u);
            auto Collect = [this, SlotCount](const TVector<uint32>& In, TVector<uint32>& OutList)
            {
                // A few slots sort faster than a walk over every bitmap word.
                constexpr SIZE_T SortedWordRatio = 16;
                if (In.size() * SortedWordRatio < DirtySlotBits.size())
                {
                    for (uint32 DirtySlot : In)
                    {
                        if (DirtySlot < SlotCount)
                        {
                            OutList.push_back(DirtySlot);
                        }
                    }
                    Algo::Sort(OutList);
                    OutList.erase(std::unique(OutList.begin(), OutList.end()), OutList.end());
                    return;
                }

                for (uint32 DirtySlot : In)
                {
                    if (DirtySlot < SlotCount)   // a slot freed after being marked is simply dropped
                    {
                        DirtySlotBits[DirtySlot >> 6] |= 1ull << (DirtySlot & 63u);
                    }
                }
                for (SIZE_T Word = 0; Word < DirtySlotBits.size(); ++Word)
                {
                    uint64 Bits = DirtySlotBits[Word];
                    if (Bits == 0)
                    {
                        continue;
                    }
                    DirtySlotBits[Word] = 0;
                    while (Bits != 0)
                    {
                        OutList.push_back((uint32)(Word * 64u) + (uint32)std::countr_zero(Bits));
                        Bits &= Bits - 1u;
                    }
                }
            };

            if (!Out.bFull)
            {
                Collect(ScenePrimitives.GetDirtyInstanceSlots(), Out.DirtySlots);
                if (FScenePrimitiveSet::ShouldResendAllInstances(Out.DirtySlots.size(), SlotCount))
                {
                    Out.bFull = true;
                    Out.DirtySlots.clear();
                }
            }
            if (!Out.bFullStatic)
            {
                Collect(ScenePrimitives.GetDirtyStaticSlots(), Out.DirtyStaticSlots);
                if (Out.DirtyStaticSlots.size() * 4 >= (SIZE_T)SlotCount)
                {
                    Out.bFullStatic = true;
                    Out.DirtyStaticSlots.clear();
                }
            }
        }

        // Interned; a full instance re-send also re-sends these, since a replaced device buffer loses them.
        Out.SurfaceDescCount        = ScenePrimitives.GetSurfaceDescCount();
        Out.bSurfaceDescsChanged    = ScenePrimitives.AreSurfaceDescsDirty() || Out.bFull || Out.bFullStatic;
        Out.MaxSurfaceDescMeshlets  = ScenePrimitives.GetMaxSurfaceDescMeshlets();

        // Channel consumed, since the decisions above are now owned by this frame.
        ScenePrimitives.ClearDirtyInstanceSlots();
        ScenePrimitives.ClearFullInstanceUpload();
        ScenePrimitives.ClearSurfaceDescsDirty();
    }

    namespace
    {
        template <typename TStorage>
        FORCEINLINE auto& PackedPayloadAt(TStorage& Storage, uint32 Index)
        {
            return Storage.GetAtDense(Index);
        }
        
    }

    namespace
    {
        template <typename TComponent>
        FORCEINLINE bool IsResolveCurrent(const TComponent& Component, const CMesh* Mesh,
                                          const FMeshResolveCache& Cache)
        {
            if (Component.CachedMeshKey != (const void*)Mesh
                || Component.CachedEntryState == MESH_RESOLVE_STATE_STALE)
            {
                return false;
            }

            if (Component.ResolveHandle == INVALID_MESH_RESOLVE_HANDLE)
            {
                return Component.CachedEntryState == MESH_RESOLVE_STATE_NO_MESH;
            }

            return Component.CachedEntryState == Cache.GetEntryState(Component.ResolveHandle);
        }

        template <typename TComponent>
        FORCEINLINE uint32 HashOverrides(const TComponent& Component)
        {
            uint32 Hash = 2166136261u;
            for (const TStrongObjectPtr<CMaterialInterface>& Override : Component.MaterialOverrides)
            {
                const uint64 Bits = (uint64)(uintptr_t)Override.Get();
                for (uint32 b = 0; b < 8u; ++b)
                {
                    Hash ^= (uint32)((Bits >> (b * 8u)) & 0xFFull);
                    Hash *= 16777619u;
                }
            }
            return Hash | 1u;
        }

        // Copies the mesh-level values into the component so the cull path never reaches the asset, and reports whether they changed.
        template <typename TComponent>
        bool ResolveMeshComponent(TComponent& Component, CMesh* Mesh,
                                  EInstanceFlags SeedFlags, TVector<CMaterialInterface*>& OverrideScratch)
        {
            OverrideScratch.clear();
            OverrideScratch.reserve(Component.MaterialOverrides.size());
            for (const TStrongObjectPtr<CMaterialInterface>& Override : Component.MaterialOverrides)
            {
                OverrideScratch.push_back(Override.Get());
            }
            Component.CachedMaterialHash = HashOverrides(Component);

            FMeshResolveCache& Cache = FMeshResolveCache::Get();

            const uint32 Handle = Cache.Resolve(Mesh, OverrideScratch);

            // A static mesh swapped for one still uploading keeps drawing the old one, which a skeletal mesh cannot since its bones follow the new skeleton.
            if constexpr (std::is_same_v<TComponent, SStaticMeshComponent>)
            {
                const bool bNewReady = Handle != INVALID_MESH_RESOLVE_HANDLE && Cache.GetEntry(Handle).bResolved;
                const uint32 Shown = Component.ResolveHandle;
                const bool bShownReady = Shown != INVALID_MESH_RESOLVE_HANDLE && Cache.IsValidHandle(Shown) && Cache.GetEntry(Shown).bResolved;
                if (Mesh != nullptr && !bNewReady && bShownReady && Component.CachedMeshKey != (const void*)Mesh)
                {
                    // Stale makes the upload that completes the new mesh wake this component again.
                    Component.bShowingPreviousMesh = true;
                    Component.CachedEntryState = MESH_RESOLVE_STATE_STALE;
                    return false;
                }
            }
            Component.bShowingPreviousMesh = false;

            Component.ResolveHandle = Handle;
            Component.CachedMeshKey = (const void*)Mesh;

            if (Handle == INVALID_MESH_RESOLVE_HANDLE)
            {
                Component.CachedLocalCenter          = FVector3(0.0f);
                Component.CachedLocalRadius          = 0.0f;
                Component.CachedMeshletHeaderSlot = 0;
                Component.CachedBaseFlags            = EInstanceFlags::None;

                if (Mesh == nullptr)
                {
                    Component.CachedEntryState = MESH_RESOLVE_STATE_NO_MESH;
                }
                else
                {
                    Component.CachedEntryState = MESH_RESOLVE_STATE_STALE;
                    FMeshResolveCache::MarkPendingWork();
                }
                return true;
            }

            const FResolvedMesh& Entry = Cache.GetEntry(Handle);
            Component.CachedLocalCenter          = Entry.LocalCenter;
            Component.CachedLocalRadius          = Entry.LocalRadius;
            Component.CachedMeshletHeaderSlot = Entry.MeshletHeaderSlot;

            EInstanceFlags BaseFlags = SeedFlags;
            if (Component.bReceiveShadow)          { BaseFlags |= EInstanceFlags::ReceiveShadow; }
            if (!Component.bReceiveDecals)         { BaseFlags |= EInstanceFlags::NoDecals; }
            if (Component.bIgnoreOcclusionCulling) { BaseFlags |= EInstanceFlags::IgnoreOcclusionCulling; }
            Component.CachedBaseFlags = BaseFlags;

            // An unready entry carries its own token too; the asset that completes it wakes the pass.
            Component.CachedEntryState = Cache.GetEntryState(Handle);
            return true;
        }

        template <typename TStorage, typename TGetMesh>
        uint32 ResolveMeshPool(TStorage Storage, EInstanceFlags SeedFlags,
                               TVector<CMaterialInterface*>& OverrideScratch, TGetMesh&& GetMesh,
                               FRenderDirtyTracker& Tracker, EPrimitiveSource Source)
        {
            using TComponent = typename TStorage::ValueType;

            const FMeshResolveCache& Cache = FMeshResolveCache::Get();

            uint32 Refreshed = 0;

            const uint32 Count = (uint32)Storage.GetDenseSize();
            for (uint32 i = 0; i < Count; ++i)
            {
                const ECS::FEntity Entity = Storage.GetDenseData()[i];
                if (Entity.IsTombstone())
                {
                    continue;
                }

                TComponent& Component = PackedPayloadAt(Storage, i);

                CMesh* Mesh = GetMesh(Component);
                if (IsResolveCurrent(Component, Mesh, Cache)
                    && Component.CachedMaterialHash == HashOverrides(Component))
                {
                    continue;
                }

                if (!ResolveMeshComponent(Component, Mesh, SeedFlags, OverrideScratch))
                {
                    continue;
                }

                Tracker.Mark(Entity, Source, EPrimitiveDirty::Data);
                ++Refreshed;
            }

            return Refreshed;
        }
    }

    void FDefaultSceneRenderer::ResolveDynamicMeshMaterials(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker)
    {
        LUMINA_PROFILE_SCOPE();

        // Recompiles, destroys and epoch bumps all move the generation, so the stale sweep waits for it.
        const uint32 Generation = FMeshResolveCache::GetPendingGeneration();
        const bool   bSweep     = Generation != LastDynamicResolveGeneration;
        LastDynamicResolveGeneration = Generation;

        const uint32 Epoch = FMeshResolveCache::GetEpoch();

        auto Storage = Registry.GetStorage<SDynamicMeshComponent>();
        const uint32 Count = (uint32)Storage.GetDenseSize();

        for (uint32 i = 0; i < Count; ++i)
        {
            const ECS::FEntity Entity = Storage.GetDenseData()[i];
            if (Entity.IsTombstone())
            {
                continue;
            }

            SDynamicMeshComponent& Component = PackedPayloadAt(Storage, i);

            // Dynamic meshes own no cache entry, so an override write is detected from the component alone.
            const uint32 OverrideHash = HashOverrides(Component);
            const uint32 DataVersion  = Component.LoadRenderDataVersion();

            if (!bSweep
                && OverrideHash == Component.CachedMaterialHash
                && DataVersion == Component.ResolvedRenderDataVersion)
            {
                continue;
            }

            Component.ResolvedRenderDataVersion = DataVersion;

            const TSharedPtr<FDynamicMeshRenderData> Data = Component.LoadRenderData();
            if (!Data)
            {
                continue;
            }

            bool bShaderStale = false;
            for (const FResolvedSurface& Surface : Data->Surfaces)
            {
                if (MeshResolve::IsSurfaceStale(Surface))
                {
                    bShaderStale = true;
                    break;
                }
            }

            if (OverrideHash == Component.CachedMaterialHash
                && Epoch == Component.CachedResolveEpoch
                && !bShaderStale
                && Data->bAllMaterialsReady)
            {
                continue;
            }

            Component.RefreshResolvedMaterials();
            Component.CachedMaterialHash  = OverrideHash;
            Component.CachedResolveEpoch  = Epoch;

            Tracker.Mark(Entity, EPrimitiveSource::DynamicMesh, EPrimitiveDirty::Data);
        }

        ValidateNoStaleResolves(Registry);
    }

    // Tripwire that turns a silently missed re-resolve gate into a log line; editor-only.
    void FDefaultSceneRenderer::ValidateNoStaleResolves(ECS::FRegistry& Registry)
    {
#if USING(WITH_EDITOR)
        // Anything still stale after the re-resolve pass is a gate that did not fire.
        const uint32 Generation = FMeshResolveCache::GetPendingGeneration();
        if (Generation == LastStaleValidationGeneration)
        {
            return;
        }
        LastStaleValidationGeneration = Generation;

        uint32 StaleSurfaces = 0;
        uint32 StaleEntities = 0;

        auto Storage = Registry.GetStorage<SDynamicMeshComponent>();
        for (uint32 i = 0, Count = (uint32)Storage.GetDenseSize(); i < Count; ++i)
        {
            if (Storage.GetDenseData()[i].IsTombstone())
            {
                continue;
            }

            const TSharedPtr<FDynamicMeshRenderData> Data = PackedPayloadAt(Storage, i).LoadRenderData();
            if (!Data)
            {
                continue;
            }

            uint32 Stale = 0;
            for (const FResolvedSurface& Surface : Data->Surfaces)
            {
                Stale += MeshResolve::IsSurfaceStale(Surface) ? 1u : 0u;
            }

            StaleSurfaces += Stale;
            StaleEntities += (Stale > 0) ? 1u : 0u;
        }

        if (StaleSurfaces > 0)
        {
            LOG_WARN("MeshResolve: {} surface(s) across {} dynamic mesh entities are still resolved against "
                     "superseded shaders after a full resolve pass. Something recompiled that the resolve "
                     "gate cannot see -- those entities are drawing last build's shader.",
                     StaleSurfaces, StaleEntities);
        }
#else
        (void)Registry;
#endif
    }

    void FDefaultSceneRenderer::SettleResolveWork(int32 MaxIterations)
    {
        for (int32 Iteration = 0; Iteration < MaxIterations; ++Iteration)
        {
            const uint32 Before = FMeshResolveCache::GetPendingGeneration();
            ResolveDirtyMeshComponents();
            if (FMeshResolveCache::GetPendingGeneration() == Before)
            {
                return;
            }
        }
    }

    void FDefaultSceneRenderer::ResolveDirtyMeshComponents()
    {
        const uint32 PendingGeneration = FMeshResolveCache::GetPendingGeneration();
        if (PendingGeneration == LastResolvedPendingGeneration)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        LastResolvedPendingGeneration = PendingGeneration;

        FMeshResolveCache& Cache = FMeshResolveCache::Get();

        Cache.ApplyPendingInvalidations();

        const uint32 TableGenerationBefore = Cache.GetTableGeneration();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        TVector<CMaterialInterface*>& Scratch = ResolveOverrideScratch;
        FRenderDirtyTracker& Tracker = FRenderDirtyTracker::Ensure(Registry);

        uint32 Refreshed = ResolveMeshPool(Registry.GetStorage<SStaticMeshComponent>(), EInstanceFlags::None, Scratch,
            [](const SStaticMeshComponent& C) -> CMesh* { return C.StaticMesh.Get(); },
            Tracker, EPrimitiveSource::StaticMesh);

        // Skeletal assets always carry FMeshletSkinnedVertex, so Skinned is unconditional here.
        Refreshed += ResolveMeshPool(Registry.GetStorage<SSkeletalMeshComponent>(), EInstanceFlags::Skinned, Scratch,
            [](const SSkeletalMeshComponent& C) -> CMesh* { return C.SkeletalMesh.Get(); },
            Tracker, EPrimitiveSource::SkeletalMesh);

        // Foliage types carry no material overrides.
        for (auto&& [Entity, Foliage] : Registry.View<SFoliageComponent>().Each())
        {
            for (SFoliageType& Type : Foliage.Types)
            {
                // Same gate as ResolveMeshPool; SFoliageType carries the same three cached fields.
                if (IsResolveCurrent(Type, Type.Mesh.Get(), Cache))
                {
                    continue;
                }

                // Every instance of this type re-binds; see ResolveMeshPool for why this belongs here.
                Tracker.Mark(Entity, EPrimitiveSource::Foliage, EPrimitiveDirty::Data);
                ++Refreshed;

                Scratch.clear();
                const uint32 Handle = Cache.Resolve(Type.Mesh.Get(), Scratch);
                Type.ResolveHandle = Handle;
                Type.CachedMeshKey = (const void*)Type.Mesh.Get();

                if (Handle == INVALID_MESH_RESOLVE_HANDLE)
                {
                    Type.CachedMeshletHeaderSlot = 0;
                    Type.CachedBaseFlags            = EInstanceFlags::None;

                    // Same distinction as ResolveMeshComponent, where only a null mesh is settled.
                    if (Type.Mesh.Get() == nullptr)
                    {
                        Type.CachedEntryState = MESH_RESOLVE_STATE_NO_MESH;
                    }
                    else
                    {
                        Type.CachedEntryState = MESH_RESOLVE_STATE_STALE;
                        FMeshResolveCache::MarkPendingWork();
                    }
                    continue;
                }

                const FResolvedMesh& Entry = Cache.GetEntry(Handle);
                Type.CachedMeshletHeaderSlot = Entry.MeshletHeaderSlot;
                Type.CachedBaseFlags = Type.bReceiveShadow ? EInstanceFlags::ReceiveShadow : EInstanceFlags::None;
                Type.CachedEntryState = Cache.GetEntryState(Handle);
            }
        }

        const uint32 EntriesRebuilt = Cache.GetTableGeneration() - TableGenerationBefore;
        if (EntriesRebuilt != 0)
        {
            ScenePrimitives.NotifyResolveTableChanged();
        }

        LUMINA_PROFILE_VALUE("Resolve/ComponentsRefreshed", (int64)Refreshed);
        LUMINA_PROFILE_VALUE("Resolve/EntriesRebuilt",      (int64)EntriesRebuilt);
    }

    void FDefaultSceneRenderer::CompileDrawCommands_Extract()
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Render Scene");

        FFrameData&     Frame    = *ExtractFrame;
        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        auto            TransformStorage = Registry.GetStorage<STransformComponent>();
        TAtomic<uint32> LightCount{0};

        auto DirectionalView = Registry.View<SDirectionalLightComponent>(ECS::TExclude<SDisabledTag>{});
        auto SpotLightView   = Registry.View<SSpotLightComponent>(ECS::TExclude<SDisabledTag>{});
        auto PointLightView  = Registry.View<SPointLightComponent>(ECS::TExclude<SDisabledTag>{});
        auto AreaLightView   = Registry.View<SAreaLightComponent>(ECS::TExclude<SDisabledTag>{});

        ECS::Utils::ResolveAllDirtyTransforms(Registry);
        ResolveDynamicMeshMaterials(Registry, FRenderDirtyTracker::Ensure(Registry));
        ResolveDirtyMeshComponents();
        SyncScenePrimitives();

        // Per-frame CPU reject volumes built before parallel gather so workers query lock-free.
        BuildSceneCullContext();

        // Needs the captures and the baking probe, both already extracted, and the light tasks below.
        BuildLightRelevanceVolumes();

        // Ten passes read Lights[0] as the sun, so a local light must never win that slot.
        if (DirectionalView.begin() != DirectionalView.end())
        {
            LightCount.store(1, std::memory_order_relaxed);
        }

        ResetGeometry_Extract();
        PrepareGatherScratch(Frame);

        DrawTaskGraph.Reset();
        ScheduleSkinnedGather(DrawTaskGraph);
        DrawTaskGraph.Dispatch();

        EmitTaskGraph.Reset();
        FTaskGraph& EmitGraph = EmitTaskGraph;

        ScheduleLineBatching(EmitGraph, Registry);

        // Emitters, so none may outrank the mesh critical path.
        EmitGraph.Add([this, &Registry]         { ExtractBatchedTriangles(Registry); },   ETaskPriority::Medium);
        EmitGraph.Add([this, &Registry, &Frame] { ExtractWidgets(Registry, Frame); },     ETaskPriority::Medium);
        EmitGraph.Add([this, &Registry, &Frame] { ExtractText(Registry, Frame); },        ETaskPriority::Medium);
        EmitGraph.Add([this, &Registry, &Frame] { ExtractSprites(Registry, Frame); },     ETaskPriority::Medium);
        EmitGraph.Add([this, &Registry, &Frame] { ExtractBillboards(Registry, Frame); },  ETaskPriority::Medium);
        #if USING(WITH_EDITOR)
        EmitGraph.Add([this, &Registry, &Frame] { ExtractSelection(Registry, Frame); },   ETaskPriority::Medium);
        #endif

        auto DLightTask = EmitGraph.AddParallelFor((uint32)DirectionalView.NumDenseSlots(), 32, [&](Task::FParallelRange Range)
        {
            LUMINA_PROFILE_SECTION("Process Directional Light");
            DirectionalView.ForEachInRange(Range.Start, Range.End,
                [&](ECS::FEntity, SDirectionalLightComponent& DirectionalLight)
            {
                ProcessDirectionalLight(DirectionalLight);
            });
        });
        
        auto PointLightTask = EmitGraph.AddParallelFor((uint32)PointLightView.NumDenseSlots(), 32, [&](Task::FParallelRange Range)
        {
            LUMINA_PROFILE_SECTION("Process Point Light Range");

            FLightBatch Batch;
            PointLightView.ForEachInRange(Range.Start, Range.End,
                [&](ECS::FEntity Entity, SPointLightComponent& PointLight)
            {
                ProcessPointLight(PointLight, TransformStorage.Get(Entity), Batch, LightCount);
            });
            FlushLightBatch(Batch, LightCount);
        });
        
        auto SpotLightTask = EmitGraph.AddParallelFor((uint32)SpotLightView.NumDenseSlots(), 32, [&](Task::FParallelRange Range)
        {
            LUMINA_PROFILE_SECTION("Process Spot Light Range");

            FLightBatch Batch;
            SpotLightView.ForEachInRange(Range.Start, Range.End,
                [&](ECS::FEntity Entity, SSpotLightComponent& SpotLight)
            {
                ProcessSpotLight(SpotLight, TransformStorage.Get(Entity), Batch, LightCount);
            });
            FlushLightBatch(Batch, LightCount);
        });

        auto AreaLightTask = EmitGraph.AddParallelFor((uint32)AreaLightView.NumDenseSlots(), 32, [&](Task::FParallelRange Range)
        {
            LUMINA_PROFILE_SECTION("Process Area Light Range");

            FLightBatch Batch;
            AreaLightView.ForEachInRange(Range.Start, Range.End,
                [&](ECS::FEntity Entity, SAreaLightComponent& AreaLight)
            {
                ProcessAreaLight(AreaLight, TransformStorage.Get(Entity), Batch, LightCount);
            });
            FlushLightBatch(Batch, LightCount);
        });

        EmitGraph.Add([this, &Registry, &Frame] { ExtractTerrain(Registry, Frame); },     ETaskPriority::Medium);
        EmitGraph.Add([this, &Registry, &Frame] { ExtractParticles(Registry, Frame); },   ETaskPriority::Medium);
        EmitGraph.Add([this, &Registry, &Frame] { ExtractDecals(Registry, Frame); },      ETaskPriority::Medium);
        EmitGraph.Add([this, &Registry, &Frame] { ExtractWater(Registry, Frame); },       ETaskPriority::Medium);
        EmitGraph.Add([this, &Frame]
        {
            LUMINA_PROFILE_SECTION("Extract Paint Ops");
            Frame.Extracts.PaintOps.clear();
            World->DrainRenderTargetPaints(Frame.Extracts.PaintOps);
        }, ETaskPriority::Medium);

        EmitGraph.AddDependency(PointLightTask, DLightTask);
        EmitGraph.AddDependency(SpotLightTask, DLightTask);
        EmitGraph.AddDependency(AreaLightTask, DLightTask);

        EmitGraph.Dispatch();

        ExtractDebugText(Frame);

        {
            LUMINA_PROFILE_SECTION_COLORED("Wait Draw Graphs", tracy::Color::Crimson);
            DrawTaskGraph.Wait();
            EmitGraph.Wait();
        }

        // After the waits, so the streamer walks CMaterial state with no gather in flight.
        PublishStreamingFeedback();

        // LightCount can overshoot MAX_LIGHTS; clamp to match what Process*Light wrote.
        NumLiveLights = Math::Min(LightCount.load(std::memory_order_acquire), (uint32)MAX_LIGHTS);

        Frame.Volumetrics.bLocalVolumetricLights = false;
        for (uint32 Index = 0; Index < NumLiveLights; ++Index)
        {
            const ELightFlags Flags = Frame.Lighting.Lights[Index].Flags;
            if (EnumHasAnyFlags(Flags, ELightFlags::Volumetric) && !EnumHasAnyFlags(Flags, ELightFlags::Directional))
            {
                Frame.Volumetrics.bLocalVolumetricLights = true;
                break;
            }
        }

        // Serial fit/allocate after parallel light pass; shrinks when sum(area) exceeds atlas budget.
        AllocateShadowTiles();
        ResolveLightFunctions();

        // Same overshoot as LightCount, and this is the last writer of the shadow counter.
        NumLiveShadows = Math::Min(Frame.Lighting.ShadowDataCount.load(std::memory_order_acquire),
                                   (uint32)MAX_SHADOWS);
        LUMINA_PROFILE_VALUE("Shadows/LocalShadowed", (int64)NumLiveShadows);
        LUMINA_PROFILE_VALUE("Lights/Live", (int64)NumLiveLights);

        // Serial after the light tasks, since the skylight reads the sun direction ProcessDirectionalLight wrote.
        const SEnvironmentComponent* ActiveEnv = ExtractEnvironment(Registry, Frame);
        ExtractSkyLight(Registry, Frame, ActiveEnv);
        ExtractFog(Registry, Frame);

        FSceneGlobalData& SceneGlobalData = Frame.SceneGlobalData;
        if (Frame.Lighting.LightData.bHasSun)
        {
            const FVector3 SunDir = Math::Normalize(Frame.Lighting.LightData.SunDirection);
            constexpr float ShadowSweepDistance = 2000.0f;
            SceneGlobalData.CullData.ShadowFrustum   = AsGPU(Frame.CameraFrustum.Extruded(SunDir, ShadowSweepDistance));
            SceneGlobalData.CullData.bHasDirectional = 1u;
        }
        else
        {
            SceneGlobalData.CullData.ShadowFrustum   = SceneGlobalData.CullData.Frustum;
            SceneGlobalData.CullData.bHasDirectional = 0u;
        }

        BuildCullViews(ExtractFrame->ViewVolume);

        // Last extract write, and on this thread, because Extract snapshots ImmediateLines a few lines later.
        ApplyCullFreeze(Frame);
    }

    void FDefaultSceneRenderer::PrepareGatherScratch(FFrameData& Frame)
    {
        auto& DrawCommands = Frame.Geometry.DrawCommands;

        // One command per pipeline batch, not per primitive; the merge emits exactly this many.
        DrawCommands.reserve(ScenePrimitives.GetBatches().Num());

        const uint32 NumThreads = GTaskSystem->GetNumTaskThreads();

        TVector<FThreadLocalDrawData>& ThreadLocal = ThreadLocalStorage;
        if (ThreadLocal.size() < NumThreads)
        {
            ThreadLocal.reserve(NumThreads);
            while (ThreadLocal.size() < NumThreads)
            {
                ThreadLocal.emplace_back();
            }
        }

        {
            LUMINA_PROFILE_SECTION("Thread Local Reset");
            // Every entry, not the first NumThreads, since the merge walks the whole container.
            for (FThreadLocalDrawData& Local : ThreadLocal)
            {
                Local.ResetForFrame();
            }
        }
        
        if (CFont* DefaultFont = CFontManager::Get().GetDefaultFont())
        {
            DefaultFont->GetAtlasResourceID();
        }
    }

    void FDefaultSceneRenderer::ScheduleSkinnedGather(FTaskGraph& Graph)
    {
        FTaskGraph::FNodeHandle MergeNode = Graph.Add([this]
        {
            MergeMeshDrawData(ThreadLocalStorage);
        }, ETaskPriority::High);

        if (ScenePrimitives.GetSkinnedPrimitiveCount() > 0)
        {
            // Rebuilt only on a shape change, so the cull walks skeletal, not every primitive.
            const TVector<uint32>& SkeletalList = ScenePrimitives.GetSkeletalIndices();
            SkeletalPrimitiveIndices = SkeletalList.data();
            const uint32 NumSkeletal = (uint32)SkeletalList.size();

            // Exact bound, since only a skeletal primitive can become a candidate.
            if ((uint32)SkinnedCandidates.size() < NumSkeletal)
            {
                SkinnedCandidates.resize(NumSkeletal);
                SkinnedCandidateBones.resize(NumSkeletal);
                PendingSliceAllocs.resize(NumSkeletal);
            }
            SkinnedCandidateCursor.store(0, std::memory_order_relaxed);

            FTaskGraph::FNodeHandle CullNode = Graph.AddParallelFor(NumSkeletal, GSkinnedCullGrain, [this](const Task::FParallelRange& Range)
            {
                LUMINA_PROFILE_SECTION("Cull Skinned Primitives");
                FThreadLocalDrawData& Local = AcquireThreadLocalDrawData(Range.Thread);
                CullSkinnedPrimitives(Range, Local);
            }, ETaskPriority::High); // critical path, MergeNode waits on this

            // Sizes the arena from what survived, which is what keeps it O(visible), not O(scene).
            FTaskGraph::FNodeHandle LayoutNode = Graph.Add([this]
            {
                LayoutSkinnedBoneSlices(ThreadLocalStorage);
            }, ETaskPriority::High);

            // A graph AddParallelFor needs its count at BUILD time, before the cull produces one.
            FTaskGraph::FNodeHandle EmitNode = Graph.Add([this]
            {
                LUMINA_PROFILE_SECTION("Emit Skinned Primitives");

                Task::ParallelFor(SkinnedCandidateCount,
                    [&](const Task::FParallelRange& Range)
                    {
                        FThreadLocalDrawData& Local = AcquireThreadLocalDrawData(Range.Thread);
                        EmitSkinnedPrimitives(Range, Local);
                    },
                    GSkinnedEmitGrain, ETaskPriority::High);
            }, ETaskPriority::High);

            Graph.AddDependency(LayoutNode, CullNode);
            Graph.AddDependency(EmitNode, LayoutNode);
            Graph.AddDependency(MergeNode, EmitNode);
        }
    }

    void FDefaultSceneRenderer::ScheduleLineBatching(FTaskGraph& EmitGraph, ECS::FRegistry& Registry)
    {
        auto LineBatcherView = Registry.View<FLineBatcherComponent>();

        FLineBatcherComponent* LineBatcher = nullptr;
        LineBatcherView.ForEach([&](FLineBatcherComponent& Batcher)
        {
            if (LineBatcher == nullptr)
            {
                LineBatcher = &Batcher;
            }
        });
        const uint32 LineChunkCount = (LineBatcher != nullptr) ? PrepareBatchedLines(*LineBatcher) : 0u;

        if (LineChunkCount > 0)
        {
            FTaskGraph::FNodeHandle LineBatchNode = EmitGraph.AddParallelFor(LineChunkCount, 1, [this](const Task::FParallelRange& Range)
            {
                BatchLineChunks(Range);
            });
            FTaskGraph::FNodeHandle LineFinalizeNode = EmitGraph.Add([this, LineBatcher]
            {
                FinalizeBatchedLines(*LineBatcher);
            });
            EmitGraph.AddDependency(LineFinalizeNode, LineBatchNode);
        }
    }

    void FDefaultSceneRenderer::ExtractTerrain(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Extract Terrain");

        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        auto TerrainAllView = Registry.View<STerrainComponent>();
        auto TerrainView = Registry.View<STerrainComponent>(ECS::TExclude<SDisabledTag>{});

        Frame.Extracts.LiveTerrainEntities.clear();

        for (ECS::FEntity Entity : TerrainAllView)
        {
            Frame.Extracts.LiveTerrainEntities.push_back(Entity);
        }

        SIZE_T TerrainCount = 0;
        for (ECS::FEntity Entity : TerrainView)
        {
            STerrainComponent& Terrain = TerrainView.Get<STerrainComponent>(Entity);

            FFrameData::FTerrainExtract& Item = ReuseAt(Frame.Extracts.TerrainExtracts, TerrainCount++);
            Item.Entity      = Entity;
            Item.WorldMatrix = TransformStorage.Get(Entity).GetWorldMatrix();
            PrepareTerrainExtract(Terrain, Item.WorldMatrix, Item);
            PrepareGrassExtract(Registry, Entity, Terrain, Item);
        }
        Frame.Extracts.TerrainExtracts.resize(TerrainCount);
    }

    static uint32 ResidentMeshletHeaderSlot(const CStaticMesh* Mesh)
    {
        return (Mesh != nullptr && Mesh->IsGeometryResident()) ? Mesh->GetMeshBuffers().MeshletHeaderSlot : 0u;
    }

    // Rigid, so the axes stay unit length and the entity's scale moves into the extent.
    static FParticleShapeGPU MakeParticleShape(const STransformComponent& Transform, EParticleShapeType Shape, const FVector3& Extent)
    {
        const FMatrix4 World = Transform.GetWorldMatrix();
        const FVector3 Scale = Transform.GetWorldScale();

        FParticleShapeGPU Out;
        Out.Center = FVector4(FVector3(World[3]), (float)Shape);
        Out.AxisX  = FVector4(Math::Normalize(FVector3(World[0])), 0.0f);
        Out.AxisY  = FVector4(Math::Normalize(FVector3(World[1])), 0.0f);
        Out.AxisZ  = FVector4(Math::Normalize(FVector3(World[2])), 0.0f);
        Out.Extent = Shape == EParticleShapeType::Sphere
            ? FVector4(Extent.x * Math::Max(Math::Max(Scale.x, Scale.y), Scale.z), 0.0f, 0.0f, 0.0f)
            : FVector4(Extent * Scale, 0.0f);
        Out.Params = FVector4(0.0f);
        return Out;
    }

    void FDefaultSceneRenderer::ExtractParticles(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Extract Particles");

        auto TransformStorage = Registry.GetStorage<STransformComponent>();

        Frame.Extracts.ParticleAttractors.clear();
        Registry.View<SParticleAttractorComponent>(ECS::TExclude<SDisabledTag>{}).ForEach([&](ECS::FEntity Entity, SParticleAttractorComponent& Attractor)
        {
            if (Attractor.bEnabled && Attractor.Strength != 0.0f)
            {
                FParticleShapeGPU& Shape = Frame.Extracts.ParticleAttractors.emplace_back(MakeParticleShape(TransformStorage.Get(Entity), Attractor.Shape, Attractor.Extent));
                Shape.Params = FVector4(Attractor.Strength, Attractor.Attenuation, Attractor.Directionality, 0.0f);
            }
        });

        Frame.Extracts.ParticleColliders.clear();
        Registry.View<SParticleColliderComponent>(ECS::TExclude<SDisabledTag>{}).ForEach([&](ECS::FEntity Entity, SParticleColliderComponent& Collider)
        {
            if (Collider.bEnabled)
            {
                Frame.Extracts.ParticleColliders.push_back(MakeParticleShape(TransformStorage.Get(Entity), Collider.Shape, Collider.Extent));
            }
        });
        auto ParticleAllView = Registry.View<SParticleSystemComponent>();
        auto ParticleView = Registry.View<SParticleSystemComponent>(ECS::TExclude<SDisabledTag>{});

        Frame.Extracts.LiveParticleEntities.clear();
        SIZE_T ParticleCount = 0;

        for (ECS::FEntity Entity : ParticleAllView)
        {
            Frame.Extracts.LiveParticleEntities.push_back(Entity);
        }

        ParticleView.ForEach([&](ECS::FEntity Entity, SParticleSystemComponent& Component)
        {
            Component.Collisions.clear();
            if (auto Hits = ParticleCollisionResults.find(Entity); Hits != ParticleCollisionResults.end())
            {
                Component.Collisions = Move(Hits->second);
                ParticleCollisionResults.erase(Hits);
            }

            CParticleSystem* PS = Component.ParticleSystem.Get();

            const bool bForceBurst = Component.bForceBurst;
            const bool bForceReset = Component.bForceReset;
            Component.bForceBurst = false;
            Component.bForceReset = false;

            if (PS == nullptr)
            {
                return;
            }

            const FMatrix4 WorldMatrix = TransformStorage.Get(Entity).GetWorldMatrix();
            const int32    EmitterCount = (int32)PS->Emitters.size();

            // A sub-emitter names its parent, so the parent learns here which event lists it has to raise.
            TFixedVector<int32, 8>  SourceIndices;
            TFixedVector<uint32, 8> RaisedMasks;
            TFixedVector<float, 8>  RaisedRates;
            SourceIndices.resize(EmitterCount, -1);
            RaisedMasks.resize(EmitterCount, 0u);
            RaisedRates.resize(EmitterCount, 0.0f);
            for (int32 Child = 0; Child < EmitterCount; ++Child)
            {
                const CParticleEmitter* ChildEmitter = PS->Emitters[Child].Get();
                if (ChildEmitter == nullptr || !ChildEmitter->bEnabled || ChildEmitter->SpawnFromEmitter.empty())
                {
                    continue;
                }

                for (int32 Parent = 0; Parent < EmitterCount; ++Parent)
                {
                    const CParticleEmitter* ParentEmitter = PS->Emitters[Parent].Get();
                    if (Parent != Child && ParentEmitter != nullptr && ParentEmitter->EmitterName == ChildEmitter->SpawnFromEmitter)
                    {
                        SourceIndices[Child] = Parent;
                        RaisedMasks[Parent] |= 1u << (uint32)ChildEmitter->SpawnEvent;
                        if (ChildEmitter->SpawnEvent == EParticleEventType::Continuous)
                        {
                            RaisedRates[Parent] = Math::Max(RaisedRates[Parent], ChildEmitter->EventRate);
                        }
                        break;
                    }
                }
            }

            for (int32 EmitterIdx = 0; EmitterIdx < EmitterCount; ++EmitterIdx)
            {
                CParticleEmitter* Emitter = PS->Emitters[EmitterIdx].Get();
                if (Emitter == nullptr || !Emitter->bEnabled)
                {
                    continue;
                }

                FFrameData::FParticleExtract& Item =
                    ReuseAt(Frame.Extracts.ParticleExtracts, ParticleCount++);

                // The element is reused, so everything the fill path writes conditionally resets here.
                Item.bReady              = false;
                Item.bUsesCustomShader   = false;
                Item.CustomComputeShader = {};
                Item.TextureIndex        = 0u;
                Item.MaterialVertexShader = {};
                Item.MaterialPixelShader  = {};
                Item.MaterialIndex        = -1;
                Item.AttributeFloatCount = 1u;
                Item.Resolved            = FResolvedParticleParams{};
                Item.ModuleParamValues.clear();

                Item.Entity              = Entity;
                Item.EmitterIndex        = EmitterIdx;
                Item.EmitterCount        = EmitterCount;
                Item.WorldMatrix         = WorldMatrix;
                Item.EmitterOffset       = Component.EmitterOffset;
                Item.TimeScale           = Component.TimeScale;
                Item.SpawnRateMultiplier = Component.SpawnRateMultiplier;
                for (int32 A = 0; A < (int32)ParticleRenderAttribute::Count; ++A)
                {
                    Item.RenderAttrSlots[A] = -1;
                }
                Item.bEmit               = Component.bEmit;
                Item.bBurstOnSpawn       = Component.bBurstOnSpawn;
                Item.bForceBurst         = bForceBurst;
                Item.bForceReset         = bForceReset;
                Item.MeshletHeaderSlot   = Emitter->RenderMode == EParticleRenderMode::Mesh ? ResidentMeshletHeaderSlot(Emitter->Mesh.Get()) : 0u;
                Item.MeshletCount        = Item.MeshletHeaderSlot != 0u ? Emitter->Mesh->GetMeshBuffers().MeshletCount : 0u;
                Item.EmissionMeshSlot    = ResidentMeshletHeaderSlot(Emitter->EmissionMesh.Get());
                Item.SourceEmitterIndex  = SourceIndices[EmitterIdx];
                Item.bReportCollisions   = Emitter->bReportCollisions;
                Item.RaisedEventMask     = RaisedMasks[EmitterIdx] | (Emitter->bReportCollisions ? 1u << (uint32)EParticleEventType::Collision : 0u);
                Item.RaisedEventRate     = RaisedRates[EmitterIdx];
                Item.ScriptEmits.clear();
                for (const FParticleScriptEmit& Emit : Component.PendingEmits)
                {
                    if (Emit.EmitterIndex == EmitterIdx)
                    {
                        Item.ScriptEmits.push_back(FParticleEventGPU{ Emit.PositionSize, Emit.VelocityFlags, Emit.Color });
                    }
                }

                Item.bReady = Emitter->IsReadyForSimulation();
                if (Item.bReady)
                {
                    Item.Resolved          = ResolveParticleParams(*PS, *Emitter, Component);
                    Item.bUsesCustomShader = Emitter->UsesCustomShader();
                    if (Item.bUsesCustomShader)
                    {
                        Item.CustomComputeShader = Emitter->GetCustomComputeShader();
                        Item.ModuleParamValues   = Emitter->ModuleParamValues;
                        ApplyParticleParamBindings(*Emitter, Component, Item.ModuleParamValues);
                        Item.AttributeFloatCount = Math::Max(Emitter->AttributeFloatCount, 1u);
                        for (int32 A = 0; A < (int32)ParticleRenderAttribute::Count; ++A)
                        {
                            Item.RenderAttrSlots[A] = Emitter->GetRenderAttributeSlot((ParticleRenderAttribute::Type)A);
                        }
                    }
                    if (CTexture* Tex = Emitter->Texture.Get())
                    {
                        const int32 CacheIdx = Tex->GetResourceID();
                        if (CacheIdx > 0)
                        {
                            Item.TextureIndex = (uint32)CacheIdx;
                        }
                    }

                    // Through the component, so a script's dynamic instance beats the asset's material.
                    CMaterialInterface* Candidate = Component.GetMaterialForEmitter(EmitterIdx);
                    if (CMaterialInterface* SpriteMaterial = ResolveParticleSpriteMaterial(Candidate))
                    {
                        FShaderH SpriteVS;
                        FShaderH SpritePS;
                        if (SpriteMaterial->ResolveDomainShaders(EMaterialType::Particle, SpriteVS, SpritePS))
                        {
                            Item.MaterialVertexShader = SpriteVS;
                            Item.MaterialPixelShader  = SpritePS;
                            Item.MaterialIndex        = SpriteMaterial->GetMaterialIndex();
                            Item.MaterialBlendMode    = SpriteMaterial->GetBlendMode();
                            Item.bMaterialWritesDepth = SpriteMaterial->WritesDepth();

                            // Demand only; an unresolved slot reads the placeholder rather than popping paths.
                            SpriteMaterial->RequestTexturesResolved();
                        }
                    }
                }
            }

            Component.PendingEmits.clear();
        });
        Frame.Extracts.ParticleExtracts.resize(ParticleCount);

        // Whatever no component claimed belongs to one that is gone.
        ParticleCollisionResults.clear();
    }

    const SEnvironmentComponent* FDefaultSceneRenderer::ExtractEnvironment(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Environment Processing");

        auto& EnvironmentParams = Frame.Volumetrics.EnvironmentParams;
        auto EnvironmentView = Registry.View<SEnvironmentComponent>(ECS::TExclude<SDisabledTag>{});
        const SEnvironmentComponent* ActiveEnv = nullptr;

        bool bHasEnvironment           = false;
        FrameFlags.bGTAO           = false;
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
            float SkyModeAsFloat;
            std::memcpy(&SkyModeAsFloat, &SkyModeBits, sizeof(float));

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
        auto SkyLightView = Registry.View<SSkyLightComponent>(ECS::TExclude<SDisabledTag>{});

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

    void FDefaultSceneRenderer::ExtractFog(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Fog Processing");

        auto TransformStorage = Registry.GetStorage<STransformComponent>();
        auto CloudView = Registry.View<SCloudComponent>(ECS::TExclude<SDisabledTag>{});
        auto FogView = Registry.View<SExponentialHeightFogComponent>(ECS::TExclude<SDisabledTag>{});
        auto FogVolumeView = Registry.View<SLocalFogVolumeComponent>(ECS::TExclude<SDisabledTag>{});

        Frame.Volumetrics.bHasFog        = false;
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

        FogView.ForEach([&Frame, &Registry] (ECS::FEntity Entity, const SExponentialHeightFogComponent& Fog)
        {
            if (!Fog.bEnabled || Fog.FogVisibilityDistance <= 0.0f)
            {
                return;
            }

            // Koschmieder extinction ln(50)/V, where contrast falls to 2%.
            constexpr float ContrastThreshold = 3.912f;
            const float FogDensity = ContrastThreshold / Math::Max(Fog.FogVisibilityDistance, 1.0f);

            float BaseHeight = Fog.FogBaseHeight;
            if (const STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity))
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

        Frame.Volumetrics.FogVolumes.clear();
        if (Frame.Volumetrics.bHasFog && Frame.Volumetrics.bVolumetricFog)
        {
            FogVolumeView.ForEach([&](ECS::FEntity Entity, const SLocalFogVolumeComponent& Volume)
            {
                if (!Volume.bEnabled || Frame.Volumetrics.FogVolumes.size() >= GFogMaxVolumes)
                {
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

                FGPUFogVolume Item;
                Item.WorldToVolume = Math::Inverse(Math::Scale(TransformStorage.Get(Entity).GetWorldMatrix(), Extent));

                constexpr float ContrastThreshold = 3.912f;
                const float Density = ContrastThreshold / Math::Max(Volume.VisibilityDistance, 1.0f);

                Item.Albedo   = FVector4(Volume.Albedo, Density);
                Item.Emissive = FVector4(Volume.EmissiveColor * Math::Max(Volume.EmissiveIntensity, 0.0f), 0.0f);
                Item.Params   = FVector4(Volume.bSphere ? 1.0f : 0.0f,
                                         Math::Clamp(Volume.EdgeSoftness, 0.001f, 1.0f),
                                         Math::Max(Volume.ScatteringIntensity, 0.0f),
                                         0.0f);

                Frame.Volumetrics.FogVolumes.push_back(Item);
            });
        }
    }

    void FDefaultSceneRenderer::ApplyCullFreeze(FFrameData& Frame)
    {
        FCullData& Cull = Frame.SceneGlobalData.CullData;

        if (!FrameSettings.bFreezeCulling)
        {
            FrozenCull.bValid = false;
            return;
        }

        if (!FrozenCull.bValid)
        {
            FrozenCull.Views            = Frame.Views.CullViews;
            FrozenCull.CameraPosition   = Cull.CullCameraPosition;
            FrozenCull.CameraView       = Cull.CullCameraView;
            FrozenCull.CameraProjection = Cull.CullCameraProjection;
            FrozenCull.NearPlane        = Cull.CullNearPlane;
            FrozenCull.FarPlane         = Cull.CullFarPlane;
            FrozenCull.Frustum          = Cull.Frustum;
            FrozenCull.ShadowFrustum    = Cull.ShadowFrustum;
            for (int32 c = 0; c < NumCascades; ++c)
            {
                FrozenCull.CascadeFrustum[c]              = Cull.CascadeFrustum[c];
                FrozenCull.CascadeHZBViewProjection[c]    = Cull.CascadeHZBViewProjection[c];
                FrozenCull.CascadeHZBNdcScale[c]          = Cull.CascadeHZBNdcScale[c];
                FrozenCull.CascadeHZBViewProjectionMid[c] = Cull.CascadeHZBViewProjectionMid[c];
                FrozenCull.CascadeHZBNdcScaleMid[c]       = Cull.CascadeHZBNdcScaleMid[c];
            }

            FrozenCull.bHasCascadeShadow = CaptureCascadeShadowFit(Frame);
            FrozenCull.CascadeViewBase   = Frame.Views.CascadeViewBase;
            FrozenCull.NumCascadeViews   = Frame.Views.NumCascadeViews;
            FrozenCull.NumNearCascadeViews = Frame.Views.NumNearCascadeViews;
            FrozenCull.bValid            = true;
            return;
        }

        // The view COUNT freezes too, since bucket indices and the dispatch grid all derive from it.
        Frame.Views.CullViews       = FrozenCull.Views;
        Frame.Views.CascadeViewBase = FrozenCull.CascadeViewBase;
        Frame.Views.NumCascadeViews = FrozenCull.NumCascadeViews;
        Frame.Views.NumNearCascadeViews = FrozenCull.NumNearCascadeViews;

        Cull.CullCameraPosition   = FrozenCull.CameraPosition;
        Cull.CullCameraView       = FrozenCull.CameraView;
        Cull.CullCameraProjection = FrozenCull.CameraProjection;
        Cull.CullNearPlane        = FrozenCull.NearPlane;
        Cull.CullFarPlane         = FrozenCull.FarPlane;
        Cull.Frustum              = FrozenCull.Frustum;
        Cull.ShadowFrustum        = FrozenCull.ShadowFrustum;
        for (int32 c = 0; c < NumCascades; ++c)
        {
            Cull.CascadeFrustum[c]              = FrozenCull.CascadeFrustum[c];
            Cull.CascadeHZBViewProjection[c]    = FrozenCull.CascadeHZBViewProjection[c];
            Cull.CascadeHZBNdcScale[c]          = FrozenCull.CascadeHZBNdcScale[c];
            Cull.CascadeHZBViewProjectionMid[c] = FrozenCull.CascadeHZBViewProjectionMid[c];
            Cull.CascadeHZBNdcScaleMid[c]       = FrozenCull.CascadeHZBNdcScaleMid[c];
        }

        RestoreCascadeShadowFit(Frame);

        DrawFrozenCullFrustum(Frame);
    }

    // The sun's cascade fit, which the shadow raster and the shading both read but the freeze did not reach.
    bool FDefaultSceneRenderer::CaptureCascadeShadowFit(const FFrameData& Frame)
    {
        // Same two gates CascadedShowPass uses, since the sun is what owns slot 0 and the cascade tiles.
        const int32 Slot = Frame.Lighting.LightData.bHasSun ? Frame.Lighting.Lights[0].ShadowDataIndex : Constants::kIndexNone;
        if (Slot == Constants::kIndexNone || Slot >= (int32)MAX_SHADOWS)
        {
            return false;
        }

        const FLightShadowData& Sun = Frame.Lighting.Shadows[Slot];
        for (int32 c = 0; c < NumCascades; ++c)
        {
            FrozenCull.CascadeShadowViewProjection[c] = Sun.ViewProjection[c];
        }

        FrozenCull.CascadeRadii        = Frame.Lighting.LightData.CascadeRadii;
        FrozenCull.CascadeDepthRanges  = Frame.Lighting.LightData.CascadeDepthRanges;
        return true;
    }

    // Written into whatever slot this frame assigned the sun, since the slot order is not itself frozen.
    void FDefaultSceneRenderer::RestoreCascadeShadowFit(FFrameData& Frame) const
    {
        const int32 Slot = Frame.Lighting.LightData.bHasSun ? Frame.Lighting.Lights[0].ShadowDataIndex : Constants::kIndexNone;
        if (!FrozenCull.bHasCascadeShadow || Slot == Constants::kIndexNone || Slot >= (int32)MAX_SHADOWS)
        {
            return;
        }

        FLightShadowData& Sun = Frame.Lighting.Shadows[Slot];
        for (int32 c = 0; c < NumCascades; ++c)
        {
            Sun.ViewProjection[c] = FrozenCull.CascadeShadowViewProjection[c];
        }

        Frame.Lighting.LightData.CascadeRadii       = FrozenCull.CascadeRadii;
        Frame.Lighting.LightData.CascadeDepthRanges = FrozenCull.CascadeDepthRanges;
    }

    // Retracted here, because a resize discarding the pyramid is only known after the render thread rebuilds.
    void FDefaultSceneRenderer::DropStaleFrozenOcclusion(FFrameData& Frame) const
    {
        if (!FrameSettings.bFreezeCulling || bDepthPyramidValid.load(std::memory_order_acquire))
        {
            return;
        }

        constexpr uint32 HiZFlags = (uint32)ECullViewFlags::Occlusion | (uint32)ECullViewFlags::MeshletHiZ;

        for (FCullView& View : Frame.Views.CullViews)
        {
            const uint32 Flags = GetCullViewFlags(View) & ~HiZFlags;
            std::memcpy(&View.ViewOriginAndFlags.w, &Flags, sizeof(Flags));
        }
    }

    // Without it a frozen cull is indistinguishable from geometry going missing for a real reason.
    void FDefaultSceneRenderer::DrawFrozenCullFrustum(const FFrameData& Frame)
    {
        const FMatrix4 InvViewProj = Math::Inverse(FrozenCull.CameraProjection * FrozenCull.CameraView);

        FVector3 Corner[8];
        for (int32 i = 0; i < 8; ++i)
        {
            // Vulkan clip volume has xy in [-1, 1] and z in [0, 1], with reverse-Z near at w = 1.
            const FVector4 Clip((i & 1) ? 1.0f : -1.0f,
                                (i & 2) ? 1.0f : -1.0f,
                                (i & 4) ? 1.0f :  0.0f,
                                1.0f);
            const FVector4 WorldH = InvViewProj * Clip;
            Corner[i] = FVector3(WorldH.x, WorldH.y, WorldH.z) / Math::Max(WorldH.w, 1e-6f);
        }

        static constexpr int32 kEdges[12][2] =
        {
            {0,1},{2,3},{0,2},{1,3},   // near face
            {4,5},{6,7},{4,6},{5,7},   // far face
            {0,4},{1,5},{2,6},{3,7},   // connecting
        };

        const FVector4 Color(1.0f, 0.55f, 0.1f, 1.0f);
        for (const auto& E : kEdges)
        {
            ImmediateLines.Line(Corner[E[0]], Corner[E[1]], Color, FImmediateLineRenderer::XRay);
        }
    }

    static float TransformMaxScale(const FMatrix4& Transform)
    {
        const FVector3 AxisX = FVector3(Transform[0]);
        const FVector3 AxisY = FVector3(Transform[1]);
        const FVector3 AxisZ = FVector3(Transform[2]);
        return Math::Sqrt(Math::Max(Math::Dot(AxisX, AxisX), Math::Max(Math::Dot(AxisY, AxisY), Math::Dot(AxisZ, AxisZ))));
    }

    // Mirrors SelectLODForView in Culling.slang.
    static uint32 SelectLODForView(const FSurfaceDescGPU& Desc, const FCullView& View, const FVector4& Sphere, float MaxScale)
    {
        const uint32 NumLODs = Math::Min(Desc.NumLODs, (uint32)MAX_MESH_LODS);
        if (NumLODs <= 1u || View.LODErrorScale <= 0.0f)
        {
            return 0u;
        }

        const bool  bOrthographic = (GetCullViewFlags(View) & ECullViewFlags::OrthographicLOD) != 0u;
        const float Reach         = bOrthographic
                                  ? 1.0f
                                  : Math::Max(Math::Length(FVector3(Sphere) - FVector3(View.ViewOriginAndFlags)) - Sphere.w, 0.0f);
        const float ErrorScale    = View.LODErrorScale * MaxScale;

        uint32 Picked = 0;
        for (uint32 i = 1; i < NumLODs; ++i)
        {
            if (Desc.LODError[i] * ErrorScale > Reach)
            {
                break;
            }
            Picked = i;
        }
        return Picked;
    }

    // Component override beats global setting; both clamped to surface NumLODs.
    static uint32 ResolveSurfaceLOD(const FSurfaceDescGPU& Desc, int32 ForcedLODIndex, bool bUseLODs,
                                    const FCullView* CameraView, const FVector4& Sphere, float MaxScale)
    {
        if (Desc.NumLODs <= 1)
        {
            return 0u;
        }
        if (ForcedLODIndex >= 0)
        {
            return (uint32)Math::Min((int32)Desc.NumLODs - 1, ForcedLODIndex);
        }
        if (bUseLODs && CameraView != nullptr)
        {
            return SelectLODForView(Desc, *CameraView, Sphere, MaxScale);
        }
        return 0u;
    }

    // Leaves FrameTag and SkinnedBoundsBase alone, since UploadSkinnedFrameData stamps both on the render thread.
    static void PublishSkinnedFrameData(FSkinnedFrameData& Out, const FSkinnedFrameData& In)
    {
        Out.SurfaceMeshletOffset    = In.SurfaceMeshletOffset;
        Out.SurfaceMeshletCount     = In.SurfaceMeshletCount;
        Out.MeshletTotalCount       = In.MeshletTotalCount;
        Out.SkinnedVertexBase       = In.SkinnedVertexBase;
        Out.BoneOffset              = In.BoneOffset;
    }

    struct FSkinnedFrameDataTarget
    {
        FSkinnedFrameData*  Data;
        uint32              Num;
        uint32              RetainedSlots;
    };

    // The LOD table comes from the interned FSurfaceDescGPU the binding already names, not from the
    // 304-byte FResolvedSurface behind Prim.Surfaces. Identical tables collapse to one entry there, so many
    // instances of few meshes read a handful of descs instead of one pointer chase per primitive.
    static void EmitPrimitiveSurfaces(FDefaultSceneRenderer::FThreadLocalDrawData& Local,
                                      const FScenePrimitive& Prim,
                                      const FSurfaceBinding* Bindings,
                                      const FSurfaceDescGPU* SurfaceDescs,
                                      uint32 NumSurfaceDescs,
                                      uint32 BoneArenaBase,
                                      const FSkinnedFrameDataTarget& Target,
                                      const FSceneRenderSettings& Settings,
                                      const FCullView* CameraView,
                                      const FVector4& Sphere,
                                      float MaxScale)
    {
        const uint32 MeshletHeaderSlot = Prim.MeshletHeaderSlot;

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[s];
            const uint32 InstanceSlot = Binding.InstanceSlot;
            if (Binding.SurfaceDescIndex >= NumSurfaceDescs || InstanceSlot >= Target.RetainedSlots)
            {
                continue;
            }
            const FSurfaceDescGPU& Desc = SurfaceDescs[Binding.SurfaceDescIndex];

            // Shadows take the camera's pick too, so both views share one pre-skin slice.
            const uint32 LODIndex = ResolveSurfaceLOD(Desc, Prim.ForcedLODIndex, Settings.bUseLODs, CameraView, Sphere, MaxScale);

            const uint32 LastLOD = Desc.NumLODs > 0u ? Math::Min(Desc.NumLODs, (uint32)MAX_MESH_LODS) - 1u : 0u;

            FSkinnedFrameData Data = {};
            // Zero meshlet count gates the cull shader's MeshletHeader deref.
            Data.SurfaceMeshletCount     = MeshletHeaderSlot ? Desc.LODMeshletCount[LODIndex] : 0u;
            Data.SurfaceMeshletOffset    = Desc.LODMeshletOffset[LODIndex];
            Data.MeshletTotalCount       = MeshletHeaderSlot
                                         ? Desc.LODMeshletOffset[LastLOD] + Desc.LODMeshletCount[LastLOD]
                                         : 0u;
            // Seeded to the sentinel so a rejected slot keeps no slice rather than last frame's base.
            Data.SkinnedVertexBase       = kNoPreSkinBase;
            Data.BoneOffset              = BoneArenaBase;

            // Slots are disjoint across primitives, and the array only grows in the merge, after every emit.
            if (InstanceSlot < Target.Num)
            {
                PublishSkinnedFrameData(Target.Data[InstanceSlot], Data);
                Local.SkinnedSlots.push_back(InstanceSlot);
            }
            else
            {
                Local.DeferredSlots.push_back({ InstanceSlot, Data });
            }
        }
    }

    void FDefaultSceneRenderer::CullSkinnedPrimitives(const Task::FParallelRange& Range, FThreadLocalDrawData& Local)
    {
        const FFrameData&        Frame     = *ExtractFrame;
        const FSceneCullContext& SceneCull = Frame.Geometry.SceneCullContext;
        const FVector3           CameraPos = FVector3(Frame.SceneGlobalData.CameraData.Location);

        const FScenePrimitive*    Prims   = ScenePrimitives.GetPrimitives();
        const FVector4*           Spheres = ScenePrimitives.GetBounds();
        const FPrimitiveCullData* Culls   = ScenePrimitives.GetCullData();

        // Dense, since the range indexes skeletal primitives rather than the whole table.
        const uint32* RESTRICT Skeletal = SkeletalPrimitiveIndices;

        // Claimed from the shared cursor a batch at a time, since every worker bumping it per primitive fights over one line.
        constexpr uint32 AcceptBatch = 64;
        uint32 Accepted[AcceptBatch];
        uint32 NumAccepted = 0;

        const auto Flush = [&]()
        {
            if (NumAccepted == 0u)
            {
                return;
            }
            const uint32 Base = SkinnedCandidateCursor.fetch_add(NumAccepted, std::memory_order_relaxed);
            const uint32 Capacity = (uint32)SkinnedCandidates.size();
            for (uint32 i = 0; i < NumAccepted && Base + i < Capacity; ++i)
            {
                SkinnedCandidates[Base + i]     = Accepted[i];
                SkinnedCandidateBones[Base + i] = Prims[Accepted[i]].BoneCount;
            }
            NumAccepted = 0;
        };

        const auto Accept = [&](uint32 PrimIndex)
        {
            const FScenePrimitive& Prim = Prims[PrimIndex];
            if (Prim.Surfaces == nullptr || Prim.SurfaceCount == 0u || Prim.BoneCount == 0u)
            {
                return;   // parked awaiting resolve or a skeleton; the sync pass retries it
            }

            // A superset of what the emit pass accepts; anything it rejects just leaves an unused hole.
            Accepted[NumAccepted++] = PrimIndex;
            if (NumAccepted == AcceptBatch)
            {
                Flush();
            }
        };

        if (!SceneCull.bEnabled)
        {
            for (uint32 d = Range.Start; d < Range.End; ++d)
            {
                Accept(Skeletal[d]);
            }
            Flush();
            return;
        }

        alignas(32) float CX[8], CY[8], CZ[8], RR[8], MD[8];

        uint32 d = Range.Start;
        for (; d + 8u <= Range.End; d += 8u)
        {
            uint32 CasterMask = 0u;
            for (uint32 n = 0; n < 8u; ++n)
            {
                const uint32 PrimIndex = Skeletal[d + n];
                const FVector4& S = Spheres[PrimIndex];
                CX[n] = S.x;
                CY[n] = S.y;
                CZ[n] = S.z;
                RR[n] = S.w;
                MD[n] = Culls[PrimIndex].MaxDrawDistance;
                CasterMask |= (Culls[PrimIndex].bCastShadow != 0u) ? (1u << n) : 0u;
            }

            uint32 KeepMask = 0u;
            uint32 MaybeMask = 0u;
            SceneCull.ShouldKeepBatch8(CX, CY, CZ, RR, MD, CasterMask, CameraPos, KeepMask, MaybeMask);

            for (uint32 n = 0; n < 8u; ++n)
            {
                const uint32 Bit = 1u << n;
                bool bKeep = (KeepMask & Bit) != 0u;

                if (!bKeep && (MaybeMask & Bit) != 0u)
                {
                    const uint32 PrimIndex = Skeletal[d + n];
                    bKeep = SceneCull.ShouldKeep(FVector3(CX[n], CY[n], CZ[n]), RR[n],
                                                 Culls[PrimIndex].bCastShadow != 0u, MD[n], CameraPos);
                }

                if (bKeep)
                {
                    Accept(Skeletal[d + n]);
                }
                else
                {
                    ++Local.Stats.NumInstancesCulled;
                }
            }
        }

        for (; d < Range.End; ++d)
        {
            const uint32 PrimIndex = Skeletal[d];
            const FVector4& S = Spheres[PrimIndex];

            if (!SceneCull.ShouldKeep(FVector3(S), S.w, Culls[PrimIndex].bCastShadow != 0u,
                                      Culls[PrimIndex].MaxDrawDistance, CameraPos))
            {
                ++Local.Stats.NumInstancesCulled;
                continue;
            }

            Accept(PrimIndex);
        }
        Flush();
    }

    void FDefaultSceneRenderer::LayoutSkinnedBoneSlices(TVector<FThreadLocalDrawData>& ThreadLocal)
    {
        LUMINA_PROFILE_SECTION("Layout Skinned Bone Slices");

        const uint32 NumPrimitives = ScenePrimitives.Num();

        // Grown, never cleared, since only entries this frame wrote are ever read.
        if ((uint32)BoneSliceByPrimitive.size() < NumPrimitives)
        {
            BoneSliceByPrimitive.resize(NumPrimitives, kNoBoneSlice);
        }

        SkinnedCandidateCount = Math::Min(SkinnedCandidateCursor.load(std::memory_order_relaxed),
                                          (uint32)SkinnedCandidates.size());
        PendingSliceCursor.store(0, std::memory_order_relaxed);

        ++BoneSliceFrameNumber;

        // Reuse needs nothing shared; only the misses fall through to the serial allocator below.
        Task::ParallelFor(SkinnedCandidateCount,
            [&](const Task::FParallelRange& Range)
            {
                for (uint32 c = Range.Start; c < Range.End; ++c)
                {
                    const uint32 Index = SkinnedCandidates[c];
                    if (Index >= NumPrimitives)
                    {
                        continue;
                    }

                    const uint32 Base = ScenePrimitives.TouchBoneSlice(Index, SkinnedCandidateBones[c],
                                                                      BoneSliceFrameNumber);
                    BoneSliceByPrimitive[Index] = Base;

                    if (Base == kNoBoneSlice)
                    {
                        const uint32 Slot = PendingSliceCursor.fetch_add(1, std::memory_order_relaxed);
                        if (Slot < (uint32)PendingSliceAllocs.size())
                        {
                            PendingSliceAllocs[Slot] = c;
                        }
                    }
                }
            },
            GSkinnedLayoutGrain, ETaskPriority::High);

        const uint32 NumPending = Math::Min(PendingSliceCursor.load(std::memory_order_relaxed),
                                            (uint32)PendingSliceAllocs.size());
        for (uint32 n = 0; n < NumPending; ++n)
        {
            const uint32 c     = PendingSliceAllocs[n];
            const uint32 Index = SkinnedCandidates[c];
            BoneSliceByPrimitive[Index] =
                ScenePrimitives.AcquireBoneSlice(Index, SkinnedCandidateBones[c], BoneSliceFrameNumber);
        }

        ScenePrimitives.ReleaseStaleBoneSlices(BoneSliceFrameNumber, kBoneSliceGraceFrames);

        ExtractFrame->Geometry.BoneCount = ScenePrimitives.GetBoneSliceExtent();
        ExtractFrame->Geometry.BonesData.resize_uninitialized(ExtractFrame->Geometry.BoneCount);
    }

    void FDefaultSceneRenderer::EmitSkinnedPrimitives(const Task::FParallelRange& Range, FThreadLocalDrawData& Local)
    {
        const FFrameData&        Frame           = *ExtractFrame;
        const FSceneCullContext& SceneCull       = Frame.Geometry.SceneCullContext;
        const FSceneGlobalData&  SceneGlobalData = Frame.SceneGlobalData;
        const FVector3           CameraPos       = FVector3(SceneGlobalData.CameraData.Location);
        const FCullView*         CameraView      = &Frame.Views.CameraLODView;

        const FScenePrimitive*      Prims    = ScenePrimitives.GetPrimitives();
        const FVector4*             Spheres  = ScenePrimitives.GetBounds();
        const FPrimitiveCullData*   Culls    = ScenePrimitives.GetCullData();
        const FSurfaceBinding*      Bindings = ScenePrimitives.GetBindings();
        const FSurfaceDescGPU*      SurfaceDescs     = ScenePrimitives.GetSurfaceDescs();
        const uint32                NumSurfaceDescs  = ScenePrimitives.GetSurfaceDescCount();
        FScenePrimitive*            MutablePrims = ScenePrimitives.GetMutablePrimitives();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        auto SkeletalStorage = Registry.GetStorage<SSkeletalMeshComponent>();

        // Sized by LayoutSkinnedBoneSlices before this pass runs, and every slice below is disjoint.
        TVector<FBoneTransform>& ArenaMirror = ExtractFrame->Geometry.BonesData;
        const uint32 ArenaCount = (uint32)ArenaMirror.size();

        const double WorldTime = World->GetTimeSinceWorldCreation();

        TVector<FSkinnedFrameData>& SkinnedData = ExtractFrame->Geometry.SkinnedFrameData;
        const FSkinnedFrameDataTarget Target{ SkinnedData.data(), (uint32)SkinnedData.size(), ScenePrimitives.GetRetainedSlotCount() };

        for (uint32 c = Range.Start; c < Range.End; ++c)
        {
            const uint32 i = SkinnedCandidates[c];
            const uint32 BoneSlice = BoneSliceByPrimitive[i];
            if (BoneSlice == kNoBoneSlice)
            {
                continue;   // its slice request failed; nothing to emit
            }

            const FScenePrimitive& Prim = Prims[i];

            const FVector4&           Sphere = Spheres[i];
            const FPrimitiveCullData& Cull   = Culls[i];

            const FVector3 Center = FVector3(Sphere);
            const float    Radius = Sphere.w;

            const FVector3 ToCamera = Center - CameraPos;
            const float    DistSq   = Math::Dot(ToCamera, ToCamera);

            if (!SkeletalStorage.Contains(Prim.Entity))
            {
                continue;
            }
            SSkeletalMeshComponent& MeshComponent = SkeletalStorage.Get(Prim.Entity);

            if (SceneCull.IsCameraVisible(Center, Radius, Cull.MaxDrawDistance, CameraPos))
            {
                MeshComponent.LastRenderedTime = WorldTime;
            }

            // Both counts were cached at sync; a skeleton swap re-syncs the primitive before it is gathered again.
            const uint32 SkeletonBoneCount = Prim.BoneCount;

            uint32 BoneArenaBase = kNoBoneSlice;
            if (SkeletonBoneCount > 0 && (SIZE_T)BoneSlice + SkeletonBoneCount <= ArenaCount)
            {
                BoneArenaBase = BoneSlice;

                // A partial pose cannot be packed against this slice, so it falls through to identity.
                const bool bHasFullPose = (uint32)MeshComponent.BoneTransforms.size() == SkeletonBoneCount;

                // A writer that moved the pose without bumping the serial gets folded in here, so the
                // residency test below is the single gate on whether this slice is already correct.
                if (MeshComponent.bRenderBonesDirty)
                {
                    MeshComponent.bRenderBonesDirty = false;
                    ++MeshComponent.PoseSerial;
                }

                // Disjoint per primitive, so workers stamp this without synchronizing.
                FScenePrimitive& Tracked = MutablePrims[i];
                const bool bResident = Tracked.UploadedSliceBase == BoneSlice
                                    && Tracked.UploadedPoseSerial == MeshComponent.PoseSerial;

                if (!bResident)
                {
                    FBoneTransform* Dst = ArenaMirror.data() + BoneSlice;

                    if (bHasFullPose)
                    {
                        SkeletalUtils::PackRenderBones(MeshComponent.BoneTransforms.data(), SkeletonBoneCount, Dst);
                    }
                    else
                    {
                        // With no pose, BoneWorld * InvBindMatrix collapses to identity for every bone.
                        const FBoneTransform IdentityBone = IdentityBoneTransform();
                        for (uint32 b = 0; b < SkeletonBoneCount; ++b)
                        {
                            Dst[b] = IdentityBone;
                        }
                    }

                    Tracked.UploadedSliceBase  = BoneSlice;
                    Tracked.UploadedPoseSerial = MeshComponent.PoseSerial;
                    Local.BoneUploadRanges.push_back(FUIntVector2{ BoneSlice, SkeletonBoneCount });
                }
            }

            MeshComponent.LastDistanceOverRadius = (Radius > 0.0f) ? (Math::Sqrt(DistSq) / Radius) : 0.0f;

            EmitPrimitiveSurfaces(Local, Prim, Bindings + Prim.BindingBase,
                                  SurfaceDescs, NumSurfaceDescs,
                                  BoneArenaBase, Target, FrameSettings, CameraView, Sphere, TransformMaxScale(Prim.Transform));
        }
    }

    void FDefaultSceneRenderer::MergeMeshDrawData(TVector<FThreadLocalDrawData>& ThreadLocal)
    {
        LUMINA_PROFILE_SECTION("Merge Mesh Draw Data");

        FFrameData& Frame               = *ExtractFrame;
        auto& DrawCommands              = Frame.Geometry.DrawCommands;
        auto& OpaqueDrawList            = Frame.Geometry.OpaqueDrawList;
        auto& TranslucentDrawList       = Frame.Geometry.TranslucentDrawList;
        auto& DeferredMaterials         = Frame.Geometry.DeferredMaterials;
        auto& FrameStats                = Frame.FrameStats;
        uint32& NumDrawsPerView         = Frame.Views.NumDrawsPerView;

        const uint32 NumThreads = (uint32)ThreadLocal.size();

        DeferredMaterials.clear();

        TVector<FUIntVector2>& BoneUploadRanges = Frame.Geometry.BoneUploadRanges;
        BoneUploadRanges.clear();

        uint64 TotalInstancesCulled = 0;
        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FThreadLocalDrawData& Local = ThreadLocal[t];
            TotalInstancesCulled += Local.Stats.NumInstancesCulled;

            BoneUploadRanges.insert(BoneUploadRanges.end(),
                                    Local.BoneUploadRanges.begin(), Local.BoneUploadRanges.end());
        }
        FrameStats.NumInstancesCulled += TotalInstancesCulled;

        FSceneBatchRegistry& Registry = ScenePrimitives.GetBatches();
        const uint32 NumBatches = Registry.Num();

        DrawCommands.clear();

        for (uint32 b = 0; b < NumBatches; ++b)
        {
            const FSceneBatchRegistry::FBatch& Batch = Registry.Get(b);

            FMeshDrawCommand& Cmd = DrawCommands.emplace_back();
            Cmd.VertexShader                   = Batch.VertexShader;
            Cmd.MeshShaderShadow               = Batch.MeshShaderShadow;
            Cmd.MeshShaderBase                 = Batch.MeshShaderBase;
            Cmd.PixelShader                    = Batch.PixelShader;
            Cmd.VisBufferMeshShader            = Batch.VisBufferMeshShader;
            Cmd.VisBufferMeshShaderMasked      = Batch.VisBufferMeshShaderMasked;
            Cmd.MaskedVisBufferPixelShader     = Batch.MaskedVisBufferPixelShader;
            Cmd.MeshShaderShadowMasked         = Batch.MeshShaderShadowMasked;
            Cmd.ShadowMaskedPixelShader        = Batch.ShadowMaskedPixelShader;
            Cmd.IndirectDrawOffset             = b;
            Cmd.DrawCount                      = 1u;
            Cmd.bTranslucent                   = Batch.Key.bTranslucent;
            Cmd.bMasked                        = Batch.Key.bMasked;
            Cmd.bAdditive                      = Batch.Key.bAdditive;
            Cmd.bModulate                      = Batch.Key.bModulate;
            Cmd.bWriteDepth                    = Batch.Key.bWriteDepth;
            Cmd.bTwoSided                      = Batch.Key.bTwoSided;
            // From the bindings, not this frame's gather: the GPU cull emits from the retained set, which
            // is a superset of what the CPU culled to, and a wrong variant reads skinned vertices at the
            // static stride.
            Cmd.bAnySkinned                    = (Batch.SkinnedRefCount != 0u) ? 1u : 0u;
            Cmd.bAnyStatic                     = (Batch.StaticRefCount != 0u) ? 1u : 0u;

            if (!Batch.Key.bTranslucent && Batch.RefCount != 0u)
            {
                for (const FSceneBatchRegistry::FDeferredMaterialSlot& MatSlot : Batch.DeferredMaterials)
                {
                    const uint8 Kinds = (uint8)((Batch.StaticRefCount != 0u ? 1u : 0u) | (Batch.SkinnedRefCount != 0u ? 2u : 0u));
                    DeferredMaterials.push_back({ (uint32)MatSlot.MaterialIndex, MatSlot.DeferredShader, Kinds });
                }
            }
        }

        // The per-draw meshlet prefix is GPU-built by BuildDrawPrefix; the CPU only publishes the count.
        NumDrawsPerView   = NumBatches;

        // CullInstances emits skinned instances too; the CPU still owns their per-frame payload.
        {
            LUMINA_PROFILE_SECTION("Publish Skinned Frame Data");

            TVector<FSkinnedFrameData>& SkinnedData = Frame.Geometry.SkinnedFrameData;
            TVector<uint32>&            SkinnedSlots = Frame.Geometry.SkinnedSlots;

            SkinnedSlots.clear();

            // Indexed by retained slot; grown, never cleared, and only as long as the highest skinned slot.
            uint32 SkinnedExtent = (uint32)SkinnedData.size();
            for (uint32 t = 0; t < NumThreads; ++t)
            {
                for (const FDeferredSkinnedSlot& Deferred : ThreadLocal[t].DeferredSlots)
                {
                    SkinnedExtent = Math::Max(SkinnedExtent, Deferred.InstanceSlot + 1u);
                }
            }
            if ((uint32)SkinnedData.size() < SkinnedExtent)
            {
                SkinnedData.resize(SkinnedExtent);
            }

            for (uint32 t = 0; t < NumThreads; ++t)
            {
                FThreadLocalDrawData& Local = ThreadLocal[t];
                if (!Local.bTouched)
                {
                    continue;
                }

                SkinnedSlots.insert(SkinnedSlots.end(), Local.SkinnedSlots.begin(), Local.SkinnedSlots.end());
                for (const FDeferredSkinnedSlot& Deferred : Local.DeferredSlots)
                {
                    PublishSkinnedFrameData(SkinnedData[Deferred.InstanceSlot], Deferred.Data);
                    SkinnedSlots.push_back(Deferred.InstanceSlot);
                }
            }
        }

        // Every slot keeps a command so IndirectDrawOffset stays a slot index, but only bound batches draw.
        const uint32 NumEmittedBatches = (uint32)DrawCommands.size();
        uint32 NumLiveBatches = 0;
        OpaqueDrawList.reserve(NumEmittedBatches);
        TranslucentDrawList.reserve(NumEmittedBatches);
        for (uint32 i = 0; i < NumEmittedBatches; ++i)
        {
            if (!Registry.IsLive(i))
            {
                continue;
            }
            ++NumLiveBatches;

            if (DrawCommands[i].bTranslucent)
            {
                TranslucentDrawList.push_back(i);
            }
            else if (!DrawCommands[i].bMasked)
            {
                OpaqueDrawList.push_back(i);
            }
        }

        // Masked batches go last, so the depth opaque geometry already wrote rejects their clip shader before it runs.
        for (uint32 i = 0; i < NumEmittedBatches; ++i)
        {
            if (Registry.IsLive(i) && !DrawCommands[i].bTranslucent && DrawCommands[i].bMasked)
            {
                OpaqueDrawList.push_back(i);
            }
        }
        FrameStats.NumBatches = NumLiveBatches;
    }

    uint32 FDefaultSceneRenderer::VisibleCubeFaces(const FVector3& Position, float Radius) const
    {
        const TVector<FFrustum>& Volumes = ExtractFrame->Lighting.RelevanceFrusta;
        if (Volumes.empty())
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

    bool FDefaultSceneRenderer::ShouldRequestShadow(const FVector3& LightPosition, float LightRadius) const
    {
        return ExtractFrame->CameraFrustum.IntersectsSphere(LightPosition, LightRadius);
    }

    void FDefaultSceneRenderer::BuildLightRelevanceVolumes()
    {
        FFrameData& Frame = *ExtractFrame;
        TVector<FFrustum>& Volumes = Frame.Lighting.RelevanceFrusta;

        Volumes.clear();

        if (!FrameSettings.bCullLights)
        {
            return;
        }

        Volumes.push_back(Frame.CameraFrustum);

        // Captures shade through the same light buffer via MakeSecondaryViewGlobals, so they vote too.
        for (const FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            Volumes.push_back(Capture.ViewVolume.GetFrustum());
        }

        if (Frame.ReflectionProbes.BakingProbe >= 0)
        {
            for (int32 Face = 0; Face < 6; ++Face)
            {
                Volumes.push_back(Frame.ReflectionProbes.FaceVolumes[Face].GetFrustum());
            }
        }
    }

    bool FDefaultSceneRenderer::IsLightRelevant(const FVector3& LightPosition, float LightRadius) const
    {
        if (!FrameSettings.bCullLights)
        {
            return true;
        }

        if (LightRadius <= 0.0f)
        {
            return false;
        }

        for (const FFrustum& Volume : ExtractFrame->Lighting.RelevanceFrusta)
        {
            if (Volume.IntersectsSphere(LightPosition, LightRadius))
            {
                return true;
            }
        }

        return false;
    }

    void FDefaultSceneRenderer::BuildSceneCullContext()
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
            SceneCullContext.CaptureFrusta.push_back(Capture.ViewVolume.GetFrustum());
        }

        if (Frame.ReflectionProbes.BakingProbe >= 0)
        {
            for (int32 Face = 0; Face < 6; ++Face)
            {
                SceneCullContext.CaptureFrusta.push_back(Frame.ReflectionProbes.FaceVolumes[Face].GetFrustum());
            }
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        // First enabled directional light wins (matches ProcessDirectionalLight).
        auto DirectionalView = Registry.View<SDirectionalLightComponent>(ECS::TExclude<SDisabledTag>{});
        for (ECS::FEntity Entity : DirectionalView)
        {
            const SDirectionalLightComponent& Light = DirectionalView.Get<SDirectionalLightComponent>(Entity);
            const float DirLenSq = Math::Dot(Light.Direction, Light.Direction);
            if (DirLenSq > 0.0001f)
            {
                SceneCullContext.SunDirection = Light.GetLightingDirection();
                SceneCullContext.bHasSun      = true;
                break;
            }
        }

        if (SceneCullContext.bHasSun)
        {
            constexpr float ShadowSweepDistance = 2000.0f;
            SceneCullContext.SunShadowFrustum = SceneCullContext.Frustum.Extruded(SceneCullContext.SunDirection, ShadowSweepDistance);
        }

        // Shadow-casting locals, keeping only lights whose attenuation sphere meets the frustum.
        auto PointView = Registry.View<SPointLightComponent, STransformComponent>(ECS::TExclude<SDisabledTag>{});
        for (ECS::FEntity Entity : PointView)
        {
            const SPointLightComponent& Light = PointView.Get<SPointLightComponent>(Entity);
            if (!Light.bCastShadows || Light.Intensity <= 0.0f)
            {
                continue;
            }
            const STransformComponent&  Transform = PointView.Get<STransformComponent>(Entity);
            const float     Radius   = Light.Attenuation;
            // Test with the location already resident in the transform's SIMD lanes (no scalar round-trip).
            if (!SceneCullContext.Frustum.IntersectsSphere(Transform.GetWorldTransformCached().Location, Radius))
            {
                continue;
            }
            SceneCullContext.ShadowLights.push_back({ Transform.GetWorldLocationCached(), Radius });
        }

        auto SpotView = Registry.View<SSpotLightComponent, STransformComponent>(ECS::TExclude<SDisabledTag>{});
        for (ECS::FEntity Entity : SpotView)
        {
            const SSpotLightComponent& Light    = SpotView.Get<SSpotLightComponent>(Entity);
            if (!Light.bCastShadows || Light.Intensity <= 0.0f)
            {
                continue;
            }
            const STransformComponent& Transform = SpotView.Get<STransformComponent>(Entity);
            const float     Radius   = Light.Attenuation;
            if (!SceneCullContext.Frustum.IntersectsSphere(Transform.GetWorldTransformCached().Location, Radius))
            {
                continue;
            }
            SceneCullContext.ShadowLights.push_back({ Transform.GetWorldLocationCached(), Radius });
        }
    }

    void FDefaultSceneRenderer::FlushLightBatch(FLightBatch& Batch, TAtomic<uint32>& LightCount)
    {
        if (Batch.NumLights == 0)
        {
            return;
        }

        FFrameData& Frame = *ExtractFrame;
        const uint32 Base = LightCount.fetch_add(Batch.NumLights, std::memory_order_acq_rel);
        const uint32 Fits = Base >= (uint32)MAX_LIGHTS ? 0u : Math::Min(Batch.NumLights, (uint32)MAX_LIGHTS - Base);
        if (Fits < Batch.NumLights)
        {
            NotifyMaxLightsHit();
        }

        for (uint32 Slot = 0; Slot < Fits; ++Slot)
        {
            Frame.Lighting.Lights[Base + Slot] = Batch.Lights[Slot];
        }

        if (Batch.NumShadows != 0)
        {
            FScopeLock Lock(Frame.Lighting.ShadowRequestMutex);
            for (uint32 Index = 0; Index < Batch.NumShadows; ++Index)
            {
                FShadowRequest& Request = Batch.Shadows[Index];
                if (Request.LightIndex < Fits)
                {
                    Request.LightIndex += Base;
                    Frame.Lighting.ShadowRequests.push_back(Request);
                }
            }
        }

        if (Batch.NumLightFunctions != 0)
        {
            FScopeLock Lock(Frame.Lighting.LightFunctionRequestMutex);
            for (uint32 Index = 0; Index < Batch.NumLightFunctions; ++Index)
            {
                FLightFunctionRequest& Request = Batch.LightFunctions[Index];
                if (Request.LightIndex < Fits)
                {
                    Request.LightIndex += Base;
                    Frame.Lighting.LightFunctionRequests.push_back(Request);
                }
            }
        }

        Batch.NumLights  = 0;
        Batch.NumShadows = 0;
        Batch.NumLightFunctions = 0;
    }

    void FDefaultSceneRenderer::ProcessPointLight(const SPointLightComponent& PointLight, const STransformComponent& TransformComponent, FLightBatch& Batch, TAtomic<uint32>& LightCount)
    {
        // A switched-off light adds nothing to the image, so it should not take a slot or a cluster test either.
        if (PointLight.Intensity <= 0.0f)
        {
            return;
        }

        const FVector3 Position = TransformComponent.GetWorldLocationCached();

        // Ahead of the slot handout, so an unreachable light costs neither a light index nor a cluster test.
        if (!IsLightRelevant(Position, PointLight.Attenuation))
        {
            return;
        }

        const uint32 Lights = Batch.NumLights;

        FLight Light                = {};
        Light.Flags                 = ELightFlags::Point;
        Light.Falloff               = PointLight.Falloff;
        Light.Color                 = PackColor(FVector4(PointLight.LightColor, 1.0));
        Light.Intensity             = PointLight.Intensity;
        Light.Radius                = PointLight.Attenuation;
        Light.Position              = Position;
        Light.ShadowDataIndex       = Constants::kIndexNone;
        if (PointLight.bVolumetric)
        {
            Light.Flags             |= ELightFlags::Volumetric;
            Light.VolumetricIntensity = PointLight.VolumetricIntensity;
            Light.VolumetricScatteringRadius = PointLight.VolumetricScatteringRadius;
        }
        if (PointLight.bContactShadows)
        {
            Light.Flags |= ELightFlags::ContactShadow;
        }
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

        if (PointLight.bCastShadows && ShouldRequestShadow(Light.Position, Light.Radius))
        {
            const FVector3 CamPos = ExtractFrame->ViewVolume.GetViewPosition();
            const float Dist = Math::Distance(CamPos, Light.Position);
            constexpr float ResolutionScale = 2048.0f;
            const uint32 DesiredPixels = (uint32)((Light.Radius / Math::Max(Dist, 0.01f)) * ResolutionScale);

            const uint32 FaceMask = VisibleCubeFaces(Light.Position, Light.Radius);
            if (FaceMask == 0u)
            {
                Batch.Lights[Batch.NumLights++] = Light;
                if (Batch.NumLights == FLightBatch::Capacity)
                {
                    FlushLightBatch(Batch, LightCount);
                }
                return;
            }

            FShadowRequest& Req = Batch.Shadows[Batch.NumShadows++];
            Req.FaceMask        = FaceMask;
            Req.LightIndex      = Lights;
            Req.Type            = ELightType::Point;
            Req.DesiredPixels   = DesiredPixels;
            Req.DistanceToCamera = Dist;
            Req.Position        = Light.Position;
            Req.Direction       = FVector3(0.0f);
            Req.Up              = FVector3(0.0f);
            Req.Attenuation     = Light.Radius;
            Req.OuterFOVDegrees = 0.0f;
        }

        Batch.Lights[Batch.NumLights++] = Light;
        if (Batch.NumLights == FLightBatch::Capacity)
        {
            FlushLightBatch(Batch, LightCount);
        }
    }

    void FDefaultSceneRenderer::ProcessSpotLight(const SSpotLightComponent& SpotLight, const STransformComponent& TransformComponent, FLightBatch& Batch, TAtomic<uint32>& LightCount)
    {
        if (SpotLight.Intensity <= 0.0f)
        {
            return;
        }

        const FVector3 Position = TransformComponent.GetWorldLocationCached();

        // Tested as the full attenuation sphere; the cone would reject more but needs the rotation first.
        if (!IsLightRelevant(Position, SpotLight.Attenuation))
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
        Light.Radius                = SpotLight.Attenuation;
        Light.Angles                = FVector2(InnerCos, OuterCos);
        Light.ShadowDataIndex       = Constants::kIndexNone;
        if (SpotLight.bVolumetric)
        {
            Light.Flags             |= ELightFlags::Volumetric;
            Light.VolumetricIntensity = SpotLight.VolumetricIntensity;
            Light.VolumetricScatteringRadius = SpotLight.VolumetricScatteringRadius;
        }
        if (SpotLight.bContactShadows)
        {
            Light.Flags |= ELightFlags::ContactShadow;
        }
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

        if (SpotLight.bCastShadows && ShouldRequestShadow(Light.Position, Light.Radius))
        {
            const FVector3 CamPos = ExtractFrame->ViewVolume.GetViewPosition();
            const float Dist = Math::Distance(CamPos, Light.Position);
            constexpr float ResolutionScale = 2048.0f;
            const uint32 DesiredPixels = (uint32)((Light.Radius / Math::Max(Dist, 0.01f)) * ResolutionScale);

            FShadowRequest& Req = Batch.Shadows[Batch.NumShadows++];
            Req.LightIndex      = Lights;
            Req.Type            = ELightType::Spot;
            Req.DesiredPixels   = DesiredPixels;
            Req.DistanceToCamera = Dist;
            Req.Position        = Light.Position;
            Req.Direction       = UpdatedForward;   // shadow camera looks along the aim (where the spot lights)
            Req.Up              = UpdatedUp;
            Req.Attenuation     = SpotLight.Attenuation;
            Req.OuterFOVDegrees = OuterDegrees;
        }

        Batch.Lights[Batch.NumLights++] = Light;
        if (Batch.NumLights == FLightBatch::Capacity)
        {
            FlushLightBatch(Batch, LightCount);
        }
    }

    void FDefaultSceneRenderer::ProcessAreaLight(const SAreaLightComponent& AreaLight, const STransformComponent& TransformComponent, FLightBatch& Batch, TAtomic<uint32>& LightCount)
    {
        if (AreaLight.Intensity <= 0.0f)
        {
            return;
        }

        const FVector2 HalfSize    = FVector2(Math::Max(AreaLight.Width, 0.01f), Math::Max(AreaLight.Height, 0.01f)) * 0.5f;
        const FVector3 Position    = TransformComponent.GetWorldLocationCached();
        // Padded by the half diagonal, so the cluster sphere still covers the reach past the rectangle's corners.
        const float    Radius      = Math::Max(AreaLight.Attenuation, 0.01f) + Math::Length(HalfSize);
        if (!IsLightRelevant(Position, Radius))
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
        if (Batch.NumLights == FLightBatch::Capacity)
        {
            FlushLightBatch(Batch, LightCount);
        }
    }

    void FDefaultSceneRenderer::ResolveLightFunctions()
    {
        auto& Lighting = ExtractFrame->Lighting;
        Lighting.LightFunctionDraws.clear();

        // The tasks pushed in any order, so sorting keeps the sun first and a capped frame stable.
        std::sort(Lighting.LightFunctionRequests.begin(), Lighting.LightFunctionRequests.end(),
            [](const FLightFunctionRequest& A, const FLightFunctionRequest& B) { return A.LightIndex < B.LightIndex; });

        for (const FLightFunctionRequest& Request : Lighting.LightFunctionRequests)
        {
            if (Lighting.LightFunctionDraws.size() == MAX_LIGHT_FUNCTIONS)
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

            const uint32 Slot = (uint32)Lighting.LightFunctionDraws.size();
            FFrameData::FLighting::FLightFunctionDraw& Draw = Lighting.LightFunctionDraws.emplace_back();
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

        Lighting.LightFunctionRequests.clear();
    }

    void FDefaultSceneRenderer::AllocateShadowTiles()
    {
        FFrameData& Frame = *ExtractFrame;
        auto& LightData       = Frame.Lighting.LightData;
        auto& ShadowRequests  = Frame.Lighting.ShadowRequests;
        auto& ShadowDataCount = Frame.Lighting.ShadowDataCount;
        auto& PackedShadows   = Frame.Lighting.PackedShadows;

        LUMINA_PROFILE_VALUE("Shadows/Requested", (int64)ShadowRequests.size());
        if (ShadowRequests.empty())
        {
            return;
        }

        {
            const uint32 SunViews        = LightData.bHasSun ? (uint32)ActiveCascadeCount : 0u;
            const uint32 ReservedViews   = 1u                                     // Camera (early)
                                         + SunViews                               // CSM cascades
                                         + SunViews                               // CSM cascades (late, phase 2)
                                         + 1u                                     // Camera (late, phase 1)
                                         + (uint32)Frame.Views.CaptureViews.size()
                                         + (Frame.ReflectionProbes.BakingProbe >= 0 ? 6u : 0u);
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
                Order.resize_uninitialized(ShadowRequests.size());
                for (uint32 i = 0; i < (uint32)ShadowRequests.size(); ++i)
                {
                    Order[i] = i;
                }
                Algo::StableSort(Order,
                    [&](uint32 A, uint32 B)
                    {
                        return ShadowRequests[A].DistanceToCamera > ShadowRequests[B].DistanceToCamera;
                    });

                TVector<bool>& Drop = ShadowDropScratch;
                Drop.assign(ShadowRequests.size(), false);
                for (uint32 i = 0; i < (uint32)Order.size() && UsedViews > AvailableViews; ++i)
                {
                    const uint32 Idx = Order[i];
                    Drop[Idx] = true;
                    UsedViews -= ViewCost(ShadowRequests[Idx]);
                }

                // Compacted in place; moving a fresh vector in would drop the request buffer's capacity.
                SIZE_T Write = 0;
                for (SIZE_T i = 0; i < ShadowRequests.size(); ++i)
                {
                    if (!Drop[i])
                    {
                        ShadowRequests[Write++] = ShadowRequests[i];
                    }
                }
                ShadowRequests.resize(Write);
            }
        }

        if (ShadowRequests.empty())
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

        const uint32 NumRequests = (uint32)ShadowRequests.size();

        TVector<uint32>& Sizes = ShadowSizeScratch;
        Sizes.resize_uninitialized(NumRequests);
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
        SortedIndices.resize_uninitialized(NumRequests);
        for (uint32 i = 0; i < NumRequests; ++i)
        {
            SortedIndices[i] = i;
        }
        Algo::Sort(SortedIndices,
            [&](uint32 A, uint32 B) { return Sizes[A] > Sizes[B]; });

        for (uint32 SortedI = 0; SortedI < NumRequests; ++SortedI)
        {
            const uint32 ReqIdx       = SortedIndices[SortedI];
            const FShadowRequest& Req = ShadowRequests[ReqIdx];
            const uint32 TileSize     = Sizes[ReqIdx];

            if (Req.Type == ELightType::Point)
            {
                int32 FaceTileIndices[6] = { Constants::kIndexNone, Constants::kIndexNone, Constants::kIndexNone, Constants::kIndexNone, Constants::kIndexNone, Constants::kIndexNone };
                bool  bAllAllocated = true;
                for (uint32 Face = 0; Face < 6; ++Face)
                {
                    if ((Req.FaceMask & (1u << Face)) == 0u)
                    {
                        continue;
                    }
                    FaceTileIndices[Face] = ShadowAtlas.AllocateTile(TileSize);
                    if (FaceTileIndices[Face] == Constants::kIndexNone)
                    {
                        bAllAllocated = false;
                        break;
                    }
                }
                if (!bAllAllocated)
                {
                    continue;
                }

                const uint32 ShadowSlot = ShadowDataCount.fetch_add(1, std::memory_order_acquire);
                if (ShadowSlot >= (uint32)MAX_SHADOWS)
                {
                    continue;
                }

                Frame.Lighting.Lights[Req.LightIndex].ShadowDataIndex = (int32)ShadowSlot;
                FLightShadowData& ShadowData = Frame.Lighting.Shadows[ShadowSlot];

                const float ShadowNear = Math::Max(Req.Attenuation * 0.01f, 0.1f);
                FViewVolume LightView(90.0f, 1.0f, ShadowNear, Req.Attenuation);

                auto SetFace = [&](uint32 Face)
                {
                    switch (Face)
                    {
                        case 0: LightView.SetView(Req.Position, FViewVolume::RightAxis,    FViewVolume::DownAxis);     break;
                        case 1: LightView.SetView(Req.Position, FViewVolume::LeftAxis,     FViewVolume::DownAxis);     break;
                        case 2: LightView.SetView(Req.Position, FViewVolume::UpAxis,       FViewVolume::ForwardAxis);  break;
                        case 3: LightView.SetView(Req.Position, FViewVolume::DownAxis,     FViewVolume::BackwardAxis); break;
                        case 4: LightView.SetView(Req.Position, FViewVolume::ForwardAxis,  FViewVolume::DownAxis);     break;
                        case 5: LightView.SetView(Req.Position, FViewVolume::BackwardAxis, FViewVolume::DownAxis);     break;
                    }
                };

                for (uint32 Face = 0; Face < 6; ++Face)
                {
                    SetFace(Face);
                    ShadowData.ViewProjection[Face] = LightView.ToReverseDepthViewProjectionMatrix();

                    // A face no view reaches keeps no tile, which the shaders read as unshadowed.
                    FLightShadow& Shadow   = ShadowData.Shadow[Face];
                    if (FaceTileIndices[Face] == Constants::kIndexNone)
                    {
                        Shadow = FLightShadow{};
                        Shadow.ShadowMapIndex  = Constants::kIndexNone;
                        Shadow.LightIndex      = (int32)Req.LightIndex;
                        Shadow.ShadowDataIndex = (int32)ShadowSlot;
                        continue;
                    }

                    const FShadowTile& FaceTile = ShadowAtlas.GetTile(FaceTileIndices[Face]);
                    Shadow.AtlasUVOffset   = FaceTile.UVOffset;
                    Shadow.AtlasUVScale    = FaceTile.UVScale;
                    Shadow.ShadowMapIndex  = FaceTileIndices[Face];
                    Shadow.LightIndex      = (int32)Req.LightIndex;
                    Shadow.ShadowDataIndex = (int32)ShadowSlot;
                    Shadow._Padding        = 0;
                }

                PackedShadows[(uint32)ELightType::Point].push_back(ShadowData.Shadow[0]);
            }
            else // Spot
            {
                const int32 TileIndex = ShadowAtlas.AllocateTile(TileSize);
                if (TileIndex == Constants::kIndexNone)
                {
                    continue;
                }

                const uint32 ShadowSlot = ShadowDataCount.fetch_add(1, std::memory_order_acquire);
                if (ShadowSlot >= (uint32)MAX_SHADOWS)
                {
                    continue;
                }

                Frame.Lighting.Lights[Req.LightIndex].ShadowDataIndex = (int32)ShadowSlot;
                FLightShadowData& ShadowData = Frame.Lighting.Shadows[ShadowSlot];
                const FShadowTile& Tile      = ShadowAtlas.GetTile(TileIndex);

                const float ShadowNear = Math::Max(Req.Attenuation * 0.01f, 0.1f);
                FViewVolume ViewVolume(Req.OuterFOVDegrees * 2.0f, 1.0f, ShadowNear, Req.Attenuation);
                ViewVolume.SetView(Req.Position, Req.Direction, Req.Up);
                ShadowData.ViewProjection[0] = ViewVolume.ToReverseDepthViewProjectionMatrix();

                FLightShadow& Shadow   = ShadowData.Shadow[0];
                Shadow.AtlasUVOffset   = Tile.UVOffset;
                Shadow.AtlasUVScale    = Tile.UVScale;
                Shadow.ShadowMapIndex  = TileIndex;
                Shadow.LightIndex      = (int32)Req.LightIndex;
                Shadow.ShadowDataIndex = (int32)ShadowSlot;
                Shadow._Padding        = 0;

                PackedShadows[(uint32)ELightType::Spot].push_back(Shadow);
            }
        }
    }

    void FDefaultSceneRenderer::BuildCullViews(const FViewVolume& ViewVolume)
    {
        FFrameData& Frame = *ExtractFrame;
        auto& CullViews                = Frame.Views.CullViews;
        auto& LightData                = Frame.Lighting.LightData;
        auto& PackedShadows            = Frame.Lighting.PackedShadows;
        auto& PointShadowCullViewBases = Frame.Views.PointShadowCullViewBases;
        auto& SpotShadowCullViewBases  = Frame.Views.SpotShadowCullViewBases;
        uint32& CascadeViewBase        = Frame.Views.CascadeViewBase;
        Frame.Views.NumCascadeViews    = 0;
        Frame.Views.NumNearCascadeViews = 0;

        const float CameraTolerance   = CameraLODPixels();
        const float ShadowTolerance   = ShadowLODTexels();
        const float ShadowAtlasTexels = (float)ShadowAtlas.GetConfig().AtlasResolution;

        auto PushView = [&](const FMatrix4& ViewProjection, const FVector3& Origin, uint32 Flags, float LODErrorScale,
                            uint32 CascadeIndex = Constants::kIndexNoneU32, float MinBoundsDiameter = 0.0f)
        {
            const uint32 ViewIndex = (uint32)CullViews.size();
            FFrustum Frustum = FFrustum::FromViewProjection(ViewProjection);

            FCullView View = {};
            for (int p = 0; p < 6; ++p)
            {
                View.FrustumPlanes[p] = Frustum.Planes[p];
            }
            float FlagsAsFloat;
            std::memcpy(&FlagsAsFloat, &Flags, sizeof(float));
            View.ViewOriginAndFlags = FVector4(Origin, FlagsAsFloat);
            View.CascadeIndex       = CascadeIndex;
            View.MinBoundsDiameter  = MinBoundsDiameter;
            View.LODErrorScale      = LODErrorScale;
            CullViews.push_back(View);

            return ViewIndex;
        };

        uint32 NumPointFaceViews = 0u;
        for (const FLightShadow& PointShadow : PackedShadows[(uint32)ELightType::Point])
        {
            if (PointShadow.ShadowDataIndex < 0)
            {
                continue;
            }
            for (const FLightShadow& Face : Frame.Lighting.Shadows[PointShadow.ShadowDataIndex].Shadow)
            {
                NumPointFaceViews += Face.ShadowMapIndex != Constants::kIndexNone ? 1u : 0u;
            }
        }

        // Both VisBuffer phases rasterize the SAME camera view, so it contributes one entry.
        const uint32 NumViews =
            1u +                                                        // Camera
            (LightData.bHasSun ? (uint32)ActiveCascadeCount : 0u) +     // CSM cascades
            NumPointFaceViews +
            (uint32)PackedShadows[(uint32)ELightType::Spot].size() +
            (uint32)Frame.Views.CaptureViews.size() +                         // Capture cameras (frustum-only)
            (Frame.ReflectionProbes.BakingProbe >= 0 ? 6u : 0u);              // Reflection-probe cube faces

        ASSERT(NumViews <= (uint32)GMaxCullViews);
        LUMINA_PROFILE_VALUE("Shadows/PointFaceViews", (int64)NumPointFaceViews);
        LUMINA_PROFILE_VALUE("Cull/Views", (int64)NumViews);

        CullViews.reserve(NumViews);

        CascadeViewBase = Constants::kIndexNoneU32;
        PointShadowCullViewBases.clear();
        PointShadowCullViewBases.reserve(PackedShadows[(uint32)ELightType::Point].size());
        SpotShadowCullViewBases.clear();
        SpotShadowCullViewBases.reserve(PackedShadows[(uint32)ELightType::Spot].size());
        
        const uint32 ConeFlag = FrameSettings.bConeCull ? (uint32)ECullViewFlags::Cone : 0u;
        
        {
            const FMatrix4 CameraVP = ViewVolume.GetProjectionMatrix() * ViewVolume.GetViewMatrix();
            uint32 CameraFlags = ConeFlag;
            if (FrameSettings.bFrustumCull)
            {
                CameraFlags |= ECullViewFlags::Frustum;
            }
            if (FrameSettings.bOcclusionCull && bDepthPyramidValid.load(std::memory_order_acquire))
            {
                CameraFlags |= ECullViewFlags::Occlusion;
            }
            // Primary camera only, since any other view is emitted entirely by the early dispatch.
            if (FrameSettings.bMeshletOcclusionCull && bDepthPyramidValid.load(std::memory_order_acquire))
            {
                CameraFlags |= ECullViewFlags::MeshletHiZ;
            }
            // The same view the skinned gather picked against, so camera picks agree on the CPU and the GPU.
            const FCullView& CameraLOD = Frame.Views.CameraLODView;
            CameraFlags |= GetCullViewFlags(CameraLOD) & ECullViewFlags::OrthographicLOD;
            PushView(CameraVP, ViewVolume.GetViewPosition(), CameraFlags, CameraLOD.LODErrorScale);
        }
        
        if (LightData.bHasSun)
        {
            const int32 SunShadowIndex = Frame.Lighting.Lights[0].ShadowDataIndex;
            if (SunShadowIndex != Constants::kIndexNone)
            {
                const FLightShadowData& SunShadow = Frame.Lighting.Shadows[SunShadowIndex];
                const uint32 CascadeFlags =
                    (FrameSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
                    ConeFlag |
                    ECullViewFlags::SunAligned |
                    ECullViewFlags::CastShadowOnly |
                    ECullViewFlags::Distance |
                    ECullViewFlags::Cascade |
                    ECullViewFlags::OrthographicLOD;

                // Published by ProcessDirectionalLight from the active sun, already clamped.
                const float MinTexels = CascadeMinTexels;

                CascadeViewBase = (uint32)CullViews.size();
                for (int32 c = 0; c < ActiveCascadeCount; ++c)
                {
                    // Micro-poly threshold in world units, from this cascade's own texel pitch.
                    const float Radius     = LightData.CascadeRadii[c];
                    const float Resolution = Math::Max(LightData.CascadeResolutions[c], 1.0f);
                    const float TexelWorld = (Radius * 2.0f) / Resolution;

                    // Unreal's screen-size rule taken at the cascade's near edge, so it never drops a caster Unreal would keep.
                    const float CascadeNear    = (c == 0) ? 0.0f : LightData.CascadeSplits[c - 1];
                    const float ScreenDiameter = 2.0f * CasterMinScreenRadius * CascadeNear;

                    // The far cascade lies wholly past ShadowMaxDistance, which the distance reject would empty.
                    const uint32 Flags = (c < NearCascadeCount)
                        ? CascadeFlags
                        : ((CascadeFlags & ~(uint32)ECullViewFlags::Distance) | ECullViewFlags::FarCascade);

                    PushView(SunShadow.ViewProjection[c], ViewVolume.GetViewPosition(), Flags,
                             MakeLODErrorScale(SunShadow.ViewProjection[c], Resolution, ShadowTolerance),
                             (uint32)c, Math::Max(MinTexels * TexelWorld, ScreenDiameter));
                }
                Frame.Views.NumCascadeViews     = (uint32)ActiveCascadeCount;
                Frame.Views.NumNearCascadeViews = (uint32)NearCascadeCount;

            }
        }

        for (const FLightShadow& PointShadow : PackedShadows[(uint32)ELightType::Point])
        {
            if (PointShadow.ShadowDataIndex < 0)
            {
                PointShadowCullViewBases.push_back(Constants::kIndexNoneU32);
                continue;
            }

            const FLightShadowData& ShadowData = Frame.Lighting.Shadows[PointShadow.ShadowDataIndex];
            const FLight& Light = Frame.Lighting.Lights[PointShadow.LightIndex];
            const uint32 FaceFlags =
                (FrameSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
                ConeFlag |
                ECullViewFlags::CastShadowOnly;

            // Only faces holding a tile get a view, in face order, which is how the shadow pass walks them.
            PointShadowCullViewBases.push_back((uint32)CullViews.size());
            for (int32 Face = 0; Face < 6; ++Face)
            {
                if (ShadowData.Shadow[Face].ShadowMapIndex != Constants::kIndexNone)
                {
                    const float FaceTexels = ShadowData.Shadow[Face].AtlasUVScale.y * ShadowAtlasTexels;
                    PushView(ShadowData.ViewProjection[Face], Light.Position, FaceFlags,
                             MakeLODErrorScale(ShadowData.ViewProjection[Face], FaceTexels, ShadowTolerance));
                }
            }
        }

        // One view per spotlight.
        for (const FLightShadow& SpotShadow : PackedShadows[(uint32)ELightType::Spot])
        {
            if (SpotShadow.ShadowDataIndex < 0)
            {
                SpotShadowCullViewBases.push_back(Constants::kIndexNoneU32);
                continue;
            }

            const FLightShadowData& ShadowData = Frame.Lighting.Shadows[SpotShadow.ShadowDataIndex];
            const FLight& Light = Frame.Lighting.Lights[SpotShadow.LightIndex];
            const uint32 SpotFlags =
                (FrameSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
                ConeFlag |
                ECullViewFlags::CastShadowOnly;

            SpotShadowCullViewBases.push_back((uint32)CullViews.size());
            const float SpotTexels = ShadowData.Shadow[0].AtlasUVScale.y * ShadowAtlasTexels;
            PushView(ShadowData.ViewProjection[0], Light.Position, SpotFlags,
                     MakeLODErrorScale(ShadowData.ViewProjection[0], SpotTexels, ShadowTolerance));
        }

        for (FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            const FMatrix4 CaptureVP = Capture.ViewVolume.GetProjectionMatrix() * Capture.ViewVolume.GetViewMatrix();
            const uint32 CaptureFlags = ECullViewFlags::Frustum | ConeFlag
                                      | (Capture.ViewVolume.IsOrthographic() ? (uint32)ECullViewFlags::OrthographicLOD : 0u);
            const float  CaptureHeight = (float)Capture.SceneGlobalData.ScreenSize.w;
            Capture.CameraViewIndex = PushView(CaptureVP, Capture.ViewVolume.GetViewPosition(), CaptureFlags,
                                               MakeLODErrorScale(CaptureVP, CaptureHeight, CameraTolerance));
        }

        if (Frame.ReflectionProbes.BakingProbe >= 0)
        {
            const uint32 FaceFlags = ECullViewFlags::Frustum | ConeFlag;
            for (int32 Face = 0; Face < 6; ++Face)
            {
                const FViewVolume& Volume = Frame.ReflectionProbes.FaceVolumes[Face];
                const FMatrix4 FaceVP = Volume.GetProjectionMatrix() * Volume.GetViewMatrix();
                Frame.ReflectionProbes.FaceCullViews[Face] = PushView(FaceVP, Volume.GetViewPosition(), FaceFlags,
                    MakeLODErrorScale(FaceVP, (float)Frame.ReflectionProbes.BakeFaceSize, CameraTolerance));
            }
        }
    }

    static FVector3 ColorTemperatureToRGB(float Kelvin)
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

    void FDefaultSceneRenderer::ProcessDirectionalLight(const SDirectionalLightComponent& DirectionalLight)
    {
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
        const float ClipRange  = ShadowFar - ShadowNear;

        // The far cascade takes the last atlas tile, so it costs the near split one cascade when all four are asked for.
        const float FarCascadeDistance = Math::Min(FarClip, DirectionalLight.FarShadowDistance);
        const bool  bFarCascade        = DirectionalLight.bFarShadowCascade && FarCascadeDistance > ShadowFar;
        NearCascadeCount   = Math::Clamp(DirectionalLight.CascadeCount, 1, bFarCascade ? NumCascades - 1 : NumCascades);
        ActiveCascadeCount = NearCascadeCount + (bFarCascade ? 1 : 0);

        // A hard edge needs only the center tap, so a blur of zero drops the rest.
        const FSunShadowFilter Filter       = GetSunShadowFilter();
        const float            FilterRadius = Filter.RadiusTexels * Math::Max(DirectionalLight.ShadowBlur, 0.0f);
        const uint32           FilterTaps   = FilterRadius > 0.0f ? Filter.Taps : 1u;

        // Shadow tuning forwarded to the lit pixel shaders via the light buffer.
        LightData.ShadowParams  = FVector4(DirectionalLight.ShadowNormalBias,
                                            DirectionalLight.ShadowDepthBias,
                                            FilterRadius,
                                            DirectionalLight.CascadeBlend);
        LightData.ShadowParams2 = FVector4(DirectionalLight.ShadowDistanceFade,
                                            float(FilterTaps),
                                            float(ActiveCascadeCount),
                                            bFarCascade ? 1.0f : 0.0f);

        const float DistributionExponent = Math::Clamp(DirectionalLight.CascadeDistributionExponent, 1.0f, 10.0f);

        float CascadeFarDistances[NumCascades];
        SunShadow::ComputeCascadeSplits(ShadowNear, ShadowFar, NearCascadeCount, DistributionExponent, bFarCascade,
                                        FarCascadeDistance, CascadeFarDistances);
        for (int i = 0; i < NumCascades; ++i)
        {
            LightData.CascadeSplits[i] = CascadeFarDistances[i]; // World-distance, view-space Z.
        }
        
        const FMatrix4& CamView   = ViewVolume.GetViewMatrix();
        const float      CamFOV    = ViewVolume.GetFOV();
        const float      CamAspect = ViewVolume.GetAspectRatio();
        const FVector3  LightDir  = Light.Direction; // Toward the sun.

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

            const int   CascadeRes        = GCSMCascadeSizes[i];
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
                CascadeTile.AtlasUVOffset = FVector2(
                    (float)GCSMCascadeOriginX[i] / (float)GCSMAtlasWidth,
                    (float)GCSMCascadeOriginY[i] / (float)GCSMAtlasHeight);
                CascadeTile.AtlasUVScale = FVector2(
                    (float)GCSMCascadeSizes[i]  / (float)GCSMAtlasWidth,
                    (float)GCSMCascadeSizes[i]  / (float)GCSMAtlasHeight);
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

        if (DirectionalLight.LightFunctionMaterial.Get() != nullptr)
        {
            const FVector3 Forward   = -Math::Normalize(Light.Direction);
            const FVector3 Reference = Math::Abs(Forward.y) < 0.99f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
            const FVector3 Right     = Math::Normalize(Math::Cross(Forward, Reference));

            FLightFunctionRequest Request;
            Request.Material        = DirectionalLight.LightFunctionMaterial.Get();
            Request.LightIndex      = 0;
            Request.Type            = ELightFlags::Directional;
            Request.Forward         = Forward;
            Request.Right           = Right;
            Request.Up              = Math::Cross(Right, Forward);
            Request.ProjectionScale = 1.0f / Math::Max(DirectionalLight.LightFunctionScale, 0.01f);

            FScopeLock Lock(Frame.Lighting.LightFunctionRequestMutex);
            Frame.Lighting.LightFunctionRequests.push_back(Request);
        }

        // Slot 0 is reserved for the sun by CompileDrawCommands_Extract, which is why no index is taken here.
        Frame.Lighting.Lights[0] = Light;
    }

    uint32 FDefaultSceneRenderer::PrepareBatchedLines(FLineBatcherComponent& Batcher)
    {
        using FLineInstance = FLineBatcherComponent::FLineInstance;
        constexpr uint32 kMaxBuckets = FLineBatchScratch::kMaxBuckets;
        constexpr uint32 kChunkLines = 4096;

        LineChunkScratch.clear();
        uint32 LineCount = 0;
        auto AddSource = [&](const FLineInstance* Data, uint32 Num)
        {
            for (uint32 Off = 0; Off < Num; Off += kChunkLines)
            {
                LineChunkScratch.push_back(FLineChunk{ Data + Off, Math::Min(kChunkLines, Num - Off) });
            }
            LineCount += Num;
        };

        if (!Batcher.Lines.empty())
        {
            AddSource(Batcher.Lines.data(), (uint32)Batcher.Lines.size());
        }
        for (TVector<FLineInstance>& Buffer : Batcher.ThreadBuffers)
        {
            if (!Buffer.empty())
            {
                AddSource(Buffer.data(), (uint32)Buffer.size());
            }
        }

        if (LineCount == 0)
        {
            return 0;
        }

        const uint32 NumThreads = GTaskSystem->GetNumTaskThreads();
        if (LineBatchScratch.size() < NumThreads)
        {
            LineBatchScratch.resize(NumThreads);
        }
        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FLineBatchScratch& S = LineBatchScratch[t];
            S.NumBuckets = 0;
            S.Survivors.clear();
            for (uint32 b = 0; b < kMaxBuckets; ++b)
            {
                S.BucketVerts[b].clear();
            }
        }

        return (uint32)LineChunkScratch.size();
    }
    
    void FDefaultSceneRenderer::BatchLineChunks(const Task::FParallelRange& Range)
    {
        LUMINA_PROFILE_SECTION("Batch Lines");

        using FLineInstance = FLineBatcherComponent::FLineInstance;
        constexpr uint32 kMaxBuckets = FLineBatchScratch::kMaxBuckets;

        const float     Dt      = ExtractFrame->SceneGlobalData.DeltaTime;
        const FFrustum& Frustum = ExtractFrame->CameraFrustum;
        FLineBatchScratch&     S      = LineBatchScratch[Range.Thread];
        const FLineChunk* const Chunks = LineChunkScratch.data();

        for (uint32 c = Range.Start; c < Range.End; ++c)
        {
            const FLineChunk& Chunk = Chunks[c];
            for (uint32 i = 0; i < Chunk.Count; ++i)
            {
                FLineInstance Line = Chunk.Data[i];

                const FAABB LineBounds(Math::Min(Line.Start, Line.End), Math::Max(Line.Start, Line.End));
                if (Frustum.IsInside(LineBounds))
                {
                    uint32 Idx = Constants::kIndexNoneU32;
                    for (uint32 b = 0; b < S.NumBuckets; ++b)
                    {
                        if (S.BucketDepthTest[b] == Line.bDepthTest &&
                            Math::EpsilonEqual(S.BucketThickness[b], Line.Thickness, Math::kSmallNumber))
                        {
                            Idx = b;
                            break;
                        }
                    }
                    if (Idx == Constants::kIndexNoneU32)
                    {
                        Idx = (S.NumBuckets < kMaxBuckets) ? S.NumBuckets++ : (kMaxBuckets - 1);
                        S.BucketThickness[Idx] = Line.Thickness;
                        S.BucketDepthTest[Idx] = Line.bDepthTest;
                    }

                    TVector<FSimpleElementVertex>& V = S.BucketVerts[Idx];
                    V.push_back({ Line.Start, Line.ColorPacked });
                    V.push_back({ Line.End,   Line.ColorPacked });
                }

                if (Line.bSingleFrame)
                {
                    continue;
                }

                Line.RemainingLifetime -= Dt;
                if (Line.RemainingLifetime > 0.0f)
                {
                    S.Survivors.push_back(Line);
                }
            }
        }
    }

    void FDefaultSceneRenderer::FinalizeBatchedLines(FLineBatcherComponent& Batcher)
    {
        using FLineInstance = FLineBatcherComponent::FLineInstance;
        constexpr uint32 kMaxBuckets = FLineBatchScratch::kMaxBuckets;

        FFrameData& Frame       = *ExtractFrame;
        auto& SimpleVertices    = Frame.Primitives.SimpleVertices;
        auto& LineBatches       = Frame.Primitives.LineBatches;

        TVector<FLineInstance>& Lines = Batcher.Lines;
        auto& ThreadBuffers           = Batcher.ThreadBuffers;

        const uint32 NumThreads = GTaskSystem->GetNumTaskThreads();

        struct FGlobalBucket
        {
            float   Thickness;
            uint8   bDepthTest;
            uint32  VertexCount;
            uint32  StartVertex;
        };
        TFixedVector<FGlobalBucket, kMaxBuckets> Global;

        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FLineBatchScratch& S = LineBatchScratch[t];
            for (uint32 b = 0; b < S.NumBuckets; ++b)
            {
                const uint32 VC = (uint32)S.BucketVerts[b].size();
                if (VC == 0)
                {
                    S.GlobalBucket[b] = Constants::kIndexNoneU32;
                    continue;
                }

                uint32 G = Constants::kIndexNoneU32;
                for (uint32 k = 0, n = (uint32)Global.size(); k < n; ++k)
                {
                    if (Global[k].bDepthTest == S.BucketDepthTest[b] &&
                        Math::EpsilonEqual(Global[k].Thickness, S.BucketThickness[b], Math::kSmallNumber))
                    {
                        G = k;
                        break;
                    }
                }
                if (G == Constants::kIndexNoneU32)
                {
                    G = (Global.size() < kMaxBuckets) ? (uint32)Global.size() : (kMaxBuckets - 1);
                    if (G == (uint32)Global.size())
                    {
                        Global.emplace_back(FGlobalBucket{ S.BucketThickness[b], S.BucketDepthTest[b], 0u, 0u });
                    }
                }
                Global[G].VertexCount += VC;
                S.GlobalBucket[b] = G;
            }
        }

        // Prefix sum to give each global bucket a contiguous range in SimpleVertices.
        const uint32 BaseVertex = (uint32)SimpleVertices.size();
        uint32 Cursor = BaseVertex;
        for (FGlobalBucket& B : Global)
        {
            B.StartVertex = Cursor;
            Cursor += B.VertexCount;
        }
        SimpleVertices.resize(Cursor);

        const bool bParallel = (Cursor - BaseVertex) > 4096;

        // Hand each (worker, bucket) a disjoint sub-range within its global bucket so the copy is race-free.
        TFixedVector<uint32, kMaxBuckets> GlobalWrite;
        GlobalWrite.resize(Global.size());
        for (uint32 k = 0, n = (uint32)Global.size(); k < n; ++k)
        {
            GlobalWrite[k] = Global[k].StartVertex;
        }
        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FLineBatchScratch& S = LineBatchScratch[t];
            for (uint32 b = 0; b < S.NumBuckets; ++b)
            {
                const uint32 VC = (uint32)S.BucketVerts[b].size();
                if (VC == 0)
                {
                    continue;
                }
                const uint32 G = S.GlobalBucket[b];
                S.WriteCursor[b] = GlobalWrite[G];
                GlobalWrite[G] += VC;
            }
        }

        // Parallel scatter, where each worker copies its buckets into their reserved slices.
        FSimpleElementVertex* const Dst = SimpleVertices.data();
        auto CopyBody = [&](const Task::FParallelRange& Range)
        {
            LUMINA_PROFILE_SECTION("Copy Batched Lines");
            for (uint32 t = Range.Start; t < Range.End; ++t)
            {
                FLineBatchScratch& S = LineBatchScratch[t];
                for (uint32 b = 0; b < S.NumBuckets; ++b)
                {
                    const TVector<FSimpleElementVertex>& V = S.BucketVerts[b];
                    if (V.empty())
                    {
                        continue;
                    }
                    std::memcpy(Dst + S.WriteCursor[b], V.data(), V.size() * sizeof(FSimpleElementVertex));
                }
            }
        };
        if (bParallel) { Task::ParallelFor(NumThreads, CopyBody, 1); }
        else           { CopyBody(Task::FParallelRange{ 0u, NumThreads, 0u }); }

        LineBatches.reserve(LineBatches.size() + Global.size());
        for (const FGlobalBucket& B : Global)
        {
            LineBatches.emplace_back(B.StartVertex, B.VertexCount, B.Thickness, (bool)B.bDepthTest);
        }

        // Rebuild the persistent line list from surviving (non-single-frame) lines; order is irrelevant.
        uint32 SurvivorTotal = 0;
        for (uint32 t = 0; t < NumThreads; ++t)
        {
            SurvivorTotal += (uint32)LineBatchScratch[t].Survivors.size();
        }
        if (SurvivorTotal == 0)
        {
            Lines.clear();
        }
        else
        {
            LineCompactScratch.clear();
            LineCompactScratch.reserve(SurvivorTotal);
            for (uint32 t = 0; t < NumThreads; ++t)
            {
                const TVector<FLineInstance>& Sv = LineBatchScratch[t].Survivors;
                LineCompactScratch.insert(LineCompactScratch.end(), Sv.begin(), Sv.end());
            }
            Lines.swap(LineCompactScratch);
        }

        // Reset the per-worker produce buffers for next frame (capacity retained, so no per-frame realloc).
        for (TVector<FLineInstance>& Buffer : ThreadBuffers)
        {
            Buffer.clear();
        }
    }

    void FDefaultSceneRenderer::ProcessBatchedTriangles(FTriangleBatcherComponent& Batcher)
    {
        FFrameData& Frame       = *ExtractFrame;
        auto& SceneGlobalData   = Frame.SceneGlobalData;
        auto& SolidVertices     = Frame.Primitives.SolidVertices;
        auto& SolidBatches      = Frame.Primitives.SolidBatches;

        Batcher.DrainQueue();

        TVector<FTriangleBatcherComponent::FBatchInstance>& Batches = Batcher.Batches;
        if (Batches.empty())
        {
            return;
        }

        const float Dt = SceneGlobalData.DeltaTime;
        SIZE_T WriteIdx = 0;
        for (SIZE_T i = 0, N = Batches.size(); i < N; ++i)
        {
            FTriangleBatcherComponent::FBatchInstance& Batch = Batches[i];
            if (!Batch.Vertices.empty())
            {
                const uint32 Start = (uint32)SolidVertices.size();
                SolidVertices.insert(SolidVertices.end(), Batch.Vertices.begin(), Batch.Vertices.end());
                SolidBatches.emplace_back(Start, (uint32)Batch.Vertices.size(), Batch.Mode);
            }

            if (Batch.bSingleFrame)
            {
                continue;
            }

            Batch.RemainingLifetime -= Dt;
            if (Batch.RemainingLifetime > 0.0f)
            {
                if (WriteIdx != i)
                {
                    Batches[WriteIdx] = std::move(Batch);
                }
                ++WriteIdx;
            }
        }
        Batches.resize(WriteIdx);
    }

    void FDefaultSceneRenderer::NotifyMaxLightsHit()
    {
        // Every overflowing batch lands here every frame, so the log hears about it once.
        static TAtomic<bool> bWarned{false};
        if (!bWarned.exchange(true, std::memory_order_relaxed))
        {
            LOG_WARN("[Rendering] More than {} lights reach the view, so the rest are left out.", MAX_LIGHTS);
        }
    }

    void FDefaultSceneRenderer::DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale)
    {
        if (ResourceID < 0 || ExtractFrame == nullptr)
        {
            return;
        }

        FBillboardInstance& Billboard   = ExtractFrame->Primitives.BillboardInstances.emplace_back();
        Billboard.TextureIndex          = (uint32)ResourceID;
        Billboard.Position              = Location;
        Billboard.Size                  = Scale;
        Billboard.EntityID              = ECS::NullEntity.Value;
    }

    void FDefaultSceneRenderer::ResetPass_Extract()
    {
        FFrameData& Frame = *ExtractFrame;

        Frame.Primitives.SimpleVertices.clear();
        Frame.Primitives.LineBatches.clear();
        for (FImmediateLineRenderer::FDrawRange& Range : Frame.Primitives.ImmediateLines)
        {
            Range = {};
        }
        Frame.Primitives.SolidVertices.clear();
        Frame.Primitives.SolidBatches.clear();
        Frame.Views.CullViews.clear();
        Frame.Views.CaptureViews.clear();
        Memory::Memzero(&Frame.Lighting.LightData, sizeof(Frame.Lighting.LightData));
        Frame.Lighting.ShadowDataCount.store(0, std::memory_order_release);
        ShadowAtlas.FreeTiles();
        Frame.Lighting.ShadowRequests.clear();
        Frame.Lighting.AtlasTiles.clear();
        Frame.Lighting.LightFunctionRequests.clear();
        Frame.Lighting.LightFunctionDraws.clear();
        Frame.Primitives.BillboardInstances.clear();
        Frame.Primitives.WidgetInstances.clear();
        Frame.Primitives.GlyphInstances.clear();
        Frame.Primitives.SpriteInstances.clear();
        Frame.Primitives.SpriteBatches.clear();
        Frame.Primitives.TextBatches.clear();
        Frame.FrameStats = {};

        for (int i = 0; i < (int)ELightType::Num; ++i)
        {
            Frame.Lighting.PackedShadows[i].clear();
        }
    }

    // Split out of ResetPass so it can be ordered against SyncScenePrimitives.
    void FDefaultSceneRenderer::ResetGeometry_Extract()
    {
        LUMINA_PROFILE_SCOPE();

        FFrameData& Frame = *ExtractFrame;

        Frame.Geometry.DrawCommands.clear();
        Frame.Geometry.OpaqueDrawList.clear();
        Frame.Geometry.TranslucentDrawList.clear();
        // BonesData keeps its capacity; the layout pass resizes it to the live arena extent.
        Frame.Geometry.BoneCount = 0;
        Frame.Geometry.BoneUploadRanges.clear();
        Frame.Views.NumDrawsPerView   = 0;
    }

    namespace
    {
        // Bilinear resample of a square row-major grid from OldRes^2 to NewRes^2.
        template <typename T>
        static void ResampleGrid(const T* Src, int32 OldRes, T* Dst, int32 NewRes)
        {
            for (int32 Y = 0; Y < NewRes; ++Y)
            {
                const float Fy = (NewRes > 1) ? float(Y) / float(NewRes - 1) * float(OldRes - 1) : 0.0f;
                const int32 Y0 = int(Fy);
                const int32 Y1 = Math::Min(Y0 + 1, OldRes - 1);
                const float Ty = Fy - float(Y0);
                for (int32 X = 0; X < NewRes; ++X)
                {
                    const float Fx = (NewRes > 1) ? float(X) / float(NewRes - 1) * float(OldRes - 1) : 0.0f;
                    const int32 X0 = int(Fx);
                    const int32 X1 = Math::Min(X0 + 1, OldRes - 1);
                    const float Tx = Fx - float(X0);
                    const float V00 = float(Src[size_t(Y0) * OldRes + X0]);
                    const float V10 = float(Src[size_t(Y0) * OldRes + X1]);
                    const float V01 = float(Src[size_t(Y1) * OldRes + X0]);
                    const float V11 = float(Src[size_t(Y1) * OldRes + X1]);
                    const float V   = Math::Mix(Math::Mix(V00, V10, Tx), Math::Mix(V01, V11, Tx), Ty);
                    if constexpr (std::is_integral_v<T>)
                    {
                        Dst[size_t(Y) * NewRes + X] = T(Math::Clamp(V + 0.5f, 0.0f, 255.0f));
                    }
                    else
                    {
                        Dst[size_t(Y) * NewRes + X] = T(V);
                    }
                }
            }
        }

        static void EnsureTerrainCpuBuffers(STerrainComponent& Terrain)
        {
            const int32  NewRes        = Terrain.Resolution;
            const size_t NeededHeights = size_t(NewRes) * size_t(NewRes);
            if (Terrain.Heightmap.size() != NeededHeights)
            {
                const size_t OldCount = Terrain.Heightmap.size();
                const int32  OldRes   = (int32)std::llround(std::sqrt((double)OldCount));
                const bool   bResample = !Terrain.Heightmap.empty() && NewRes >= 2 && OldRes >= 2
                                       && size_t(OldRes) * size_t(OldRes) == OldCount;
                if (bResample)
                {
                    TVector<float> Resampled(NeededHeights);
                    ResampleGrid(Terrain.Heightmap.data(), OldRes, Resampled.data(), NewRes);
                    Terrain.Heightmap = std::move(Resampled);

                    const int32 LayerCount = (int32)Terrain.Layers.size();
                    if (LayerCount > 0 && Terrain.LayerWeights.size() == size_t(LayerCount) * OldCount)
                    {
                        TVector<uint8> NewWeights(size_t(LayerCount) * NeededHeights);
                        for (int32 L = 0; L < LayerCount; ++L)
                        {
                            ResampleGrid(Terrain.LayerWeights.data() + size_t(L) * OldCount, OldRes,
                                         NewWeights.data() + size_t(L) * NeededHeights, NewRes);
                        }
                        Terrain.LayerWeights = std::move(NewWeights);
                        Terrain.CPUState.bFullWeightsDirty = true;
                    }
                }
                else
                {
                    Terrain.Heightmap.assign(NeededHeights, 0.0f);
                }
                Terrain.CPUState.bFullHeightmapDirty = true;
            }
            const size_t NeededWeights = size_t(Terrain.Layers.size()) * NeededHeights;
            if (Terrain.LayerWeights.size() != NeededWeights)
            {
                Terrain.LayerWeights.resize(NeededWeights, 0u);
                Terrain.CPUState.bFullWeightsDirty = true;
            }
        }

        void PrepareGrassExtract(ECS::FRegistry& Registry, ECS::FEntity Entity, const STerrainComponent& Terrain,
                                 FDefaultSceneRenderer::FFrameData::FTerrainExtract& Out)
        {
            Out.Grass.clear();
            Out.GrassMaxInstances = 0;

            const SGrassComponent* Grass = Registry.TryGet<SGrassComponent>(Entity);
            if (Grass == nullptr || !Grass->bEnabled)
            {
                return;
            }

            CMaterialInterface* MaterialInterface = Terrain.Material.Get();
            CMaterial* Material = MaterialInterface != nullptr ? MaterialInterface->GetMaterial() : nullptr;
            if (Material == nullptr || Material->GrassOutputs.empty())
            {
                return;
            }

            Out.GrassMaxInstances    = Grass->MaxInstancesPerSpecies;

            for (const FGrassOutput& Output : Material->GrassOutputs)
            {
                const CGrassType* Type = Output.GrassType.Get();
                if (Type == nullptr || Type->Mesh == nullptr)
                {
                    continue;
                }

                // A layer the terrain does not have would sample a slice past the array's end.
                if (Output.LayerIndex >= (uint32)Terrain.Layers.size())
                {
                    continue;
                }

                const float Density = Type->Density * Output.DensityScale;
                if (Density <= 0.0f)
                {
                    continue;
                }

                FDefaultSceneRenderer::FFrameData::FGrassSpeciesExtract Species;
                Species.Mesh          = Type->Mesh.Get();
                Species.ResolveHandle = FMeshResolveCache::Get().Resolve(Species.Mesh, {});
                Species.TypeName      = Type->GetName();
                Species.LayerIndex = Output.LayerIndex;

                // Density is instances per square meter and a world unit is a meter, so the spacing is its inverse root.
                Species.CellSize = Math::Max(0.01f, 1.0f / Math::Sqrt(Density));

                Species.MinWeight     = Type->MinWeight;
                Species.ScaleMin      = Type->ScaleMin;
                Species.ScaleMax      = Type->ScaleMax;
                Species.ZOffset       = Type->ZOffset;
                Species.AlignToNormal = Type->AlignToNormal;
                Species.MaxSlopeCos   = Math::Cos(Type->MaxSlopeDegrees * (3.14159265358979f / 180.0f));
                Species.CullDistance  = Type->CullDistance;
                Species.Seed          = Type->Seed;
                Species.bReceiveShadow = Type->bReceiveShadow;
                Species.bContactShadows = Type->bContactShadows;
                Species.FullDensityDistance = Type->FullDensityDistance;
                Species.bRandomYaw    = Type->bRandomYaw;

                Out.Grass.push_back(Species);
            }
        }

        void PrepareTerrainExtract(STerrainComponent& Terrain, const FMatrix4& WorldMatrix,
                                   FDefaultSceneRenderer::FFrameData::FTerrainExtract& Out)
        {
            EnsureTerrainCpuBuffers(Terrain);

            FTerrainCPUState& CPU = Terrain.CPUState;

            const int32  Res        = Terrain.Resolution;
            const int32  LayerCount = (int32)std::max<size_t>(Terrain.Layers.size(), 1u);
            const size_t SlicePixels = size_t(Res) * size_t(Res);

            // Snapshot the scalar params (render passes read these, not the component).
            Out.Resolution      = Res;
            Out.ChunkResolution = Terrain.ChunkResolution;
            Out.TileWorldSize   = Terrain.TileWorldSize;
            Out.MaxHeight       = Terrain.MaxHeight;
            Out.LayerCount      = (int32)Terrain.Layers.size();
            Out.bCastShadow     = Terrain.bCastShadow;
            Out.bReceiveShadow  = Terrain.bReceiveShadow;

            // Cleared up front because the element is reused across frames and these are written conditionally.
            Out.Shaders       = FRenderMaterialShaders{};
            Out.MaterialIndex = 0u;
            Out.bMasked       = false;

            // Anything that is not a ready terrain material, wrong domain included, falls back to the default.
            CMaterialInterface* TerrainMaterial = Terrain.Material.Get();
            FShaderH TerrainVS;
            FShaderH TerrainPS;
            if (TerrainMaterial == nullptr || !TerrainMaterial->ResolveDomainShaders(EMaterialType::Terrain, TerrainVS, TerrainPS))
            {
                TerrainMaterial = CMaterial::GetDefaultTerrainMaterial();
            }
            if (TerrainMaterial != nullptr && TerrainMaterial->ResolveDomainShaders(EMaterialType::Terrain, TerrainVS, TerrainPS))
            {
                Out.Shaders.VertexShader = TerrainVS;
                Out.Shaders.PixelShader  = TerrainPS;
                Out.MaterialIndex        = (uint32)Math::Max(TerrainMaterial->GetMaterialIndex(), 0);
                Out.bMasked              = TerrainMaterial->GetBlendMode() == EBlendMode::Masked;

                // Demand only, like particles, since terrain cannot simply disappear while its textures load.
                TerrainMaterial->RequestTexturesResolved();
            }

            Out.HeightUpload      = 0;
            Out.WeightUpload      = 0;
            Out.WeightSliceMask   = 0u;
            Out.bGeometryRebuilt  = false;
            Out.bStructuralChange = false;
            Out.HeightBytes.clear();
            Out.WeightBytes.clear();
            Out.Chunks.clear();
            Out.Meshlets.clear();

            if (Res < 2 || Terrain.ChunkResolution < 2)
            {
                return;
            }

            const bool bStructural = CPU.PreparedResolution      != Res
                                  || CPU.PreparedChunkResolution != Terrain.ChunkResolution
                                  || CPU.PreparedLayerCount      != Out.LayerCount;
            Out.bStructuralChange = bStructural;

            // Height upload
            const bool bFullHeight = CPU.bFullHeightmapDirty || bStructural;
            const bool bRectHeight = !bFullHeight && (CPU.HeightDirtyMax.x >= CPU.HeightDirtyMin.x);

            FIntVector2 RectMin = FIntVector2(0);
            FIntVector2 RectMax = FIntVector2(Res - 1);
            if (bFullHeight && Terrain.Heightmap.size() == SlicePixels)
            {
                Out.HeightUpload = 1;
                Out.HeightRectMin = FIntVector2(0);
                Out.HeightRectMax = FIntVector2(Res - 1);
                Out.HeightBytes.assign(Terrain.Heightmap.begin(), Terrain.Heightmap.end());
            }
            else if (bRectHeight && Terrain.Heightmap.size() == SlicePixels)
            {
                RectMin = Math::Clamp(CPU.HeightDirtyMin, FIntVector2(0), FIntVector2(Res - 1));
                RectMax = Math::Clamp(CPU.HeightDirtyMax, FIntVector2(0), FIntVector2(Res - 1));
                const int32 RegionW = RectMax.x - RectMin.x + 1;
                const int32 RegionH = RectMax.y - RectMin.y + 1;
                Out.HeightUpload  = 2;
                Out.HeightRectMin = RectMin;
                Out.HeightRectMax = RectMax;
                Out.HeightBytes.resize(size_t(RegionW) * size_t(RegionH));
                for (int32 Row = 0; Row < RegionH; ++Row)
                {
                    const float* Src = Terrain.Heightmap.data() + size_t(RectMin.y + Row) * Res + RectMin.x;
                    std::memcpy(Out.HeightBytes.data() + size_t(Row) * RegionW, Src, size_t(RegionW) * sizeof(float));
                }
            }

            // Weights upload as whole slices, matching the GPU upload granularity.
            const bool bFullWeights = CPU.bFullWeightsDirty || bStructural;
            if (!Terrain.LayerWeights.empty())
            {
                if (bFullWeights)
                {
                    uint32 Mask = 0u;
                    for (int32 L = 0; L < LayerCount && (size_t(L + 1) * SlicePixels) <= Terrain.LayerWeights.size(); ++L)
                    {
                        Mask |= (1u << L);
                    }
                    if (Mask != 0u)
                    {
                        Out.WeightUpload    = 1;
                        Out.WeightSliceMask = Mask;
                        // Pack only the present slices back-to-back, in ascending slice order.
                        for (int32 L = 0; L < LayerCount; ++L)
                        {
                            if ((Mask & (1u << L)) == 0u) continue;
                            const uint8* Slice = Terrain.LayerWeights.data() + size_t(L) * SlicePixels;
                            Out.WeightBytes.insert(Out.WeightBytes.end(), Slice, Slice + SlicePixels);
                        }
                    }
                }
                else if (CPU.WeightDirtyLayerMask != 0u)
                {
                    uint32 Mask = 0u;
                    for (int32 L = 0; L < LayerCount; ++L)
                    {
                        if ((CPU.WeightDirtyLayerMask & (1u << L)) == 0u) continue;
                        if ((size_t(L + 1) * SlicePixels) > Terrain.LayerWeights.size()) continue;
                        Mask |= (1u << L);
                        const uint8* Slice = Terrain.LayerWeights.data() + size_t(L) * SlicePixels;
                        Out.WeightBytes.insert(Out.WeightBytes.end(), Slice, Slice + SlicePixels);
                    }
                    if (Mask != 0u)
                    {
                        Out.WeightUpload    = 2;
                        Out.WeightSliceMask = Mask;
                    }
                }
            }

            // Chunk / meshlet metadata
            if (CPU.bChunksDirty || bStructural)
            {
                const FVector3 WorldOrigin = FVector3(WorldMatrix[3]);
                const bool bFullRebuild = bStructural || !bRectHeight || CPU.Chunks.empty();
                if (bFullRebuild)
                {
                    TerrainMeshletBuilder::Build(Terrain, WorldOrigin);
                }
                else
                {
                    TerrainMeshletBuilder::UpdateRegion(Terrain, WorldOrigin, RectMin, RectMax);
                }

                if (!CPU.Chunks.empty() && !CPU.Meshlets.empty())
                {
                    Out.bGeometryRebuilt = true;
                    Out.Chunks.assign(CPU.Chunks.begin(), CPU.Chunks.end());
                    Out.Meshlets.assign(CPU.Meshlets.begin(), CPU.Meshlets.end());
                }
            }

            // Consume the dirty state now that it's captured for this frame.
            CPU.bFullHeightmapDirty = false;
            CPU.bFullWeightsDirty   = false;
            CPU.bChunksDirty        = false;
            CPU.HeightDirtyMin      = FIntVector2(INT32_MAX);
            CPU.HeightDirtyMax      = FIntVector2(INT32_MIN);
            CPU.WeightDirtyMin      = FIntVector2(INT32_MAX);
            CPU.WeightDirtyMax      = FIntVector2(INT32_MIN);
            CPU.WeightDirtyLayerMask = 0u;
            CPU.PreparedResolution      = Res;
            CPU.PreparedChunkResolution = Terrain.ChunkResolution;
            CPU.PreparedLayerCount      = Out.LayerCount;
        }

    }
}
