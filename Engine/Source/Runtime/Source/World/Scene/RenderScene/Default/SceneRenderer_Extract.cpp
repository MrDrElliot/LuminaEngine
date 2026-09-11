#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
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
            if (IsTemporalAAEnabledFor(PrimaryView))
            {
                // A bailed extract must not flip parity, or the resolve blends a slot against itself.
                PrimaryView.PendingTemporalFrameIndex = PrimaryView.TemporalFrameIndex + 1;

                // Column-major, so the mathematical row-0/row-1 of column 2 are these two elements.
                const FVector2 Jitter = GetTemporalJitterNDC(PrimaryView, PrimaryView.PendingTemporalFrameIndex);
                SceneGlobalData.CameraData.Projection[2][0] += Jitter.x;
                SceneGlobalData.CameraData.Projection[2][1] += Jitter.y;

                // Every depth-reconstructing pass reads this, so it has to match the jitter that was rendered.
                SceneGlobalData.CameraData.InverseProjection = Math::Inverse(SceneGlobalData.CameraData.Projection);

                // Same period as the jitter, or the pair the resolve averages never repeats and never settles.
                SceneGlobalData.TemporalPhase = PrimaryView.PendingTemporalFrameIndex & 1u;
            }
            else
            {
                PrimaryView.bTemporalHistoryValid = false;
                SceneGlobalData.TemporalPhase    = 0u;
            }

            SceneGlobalData.CameraData.PrevViewProjection = PrimaryView.PrevViewProjection;
            PrimaryView.PendingViewProjection = SceneGlobalData.CameraData.Projection * SceneGlobalData.CameraData.View;
        }
        SceneGlobalData.ScreenSize                      = FUIntVector4(PrimarySize.x, PrimarySize.y, 0, 0);
        SceneGlobalData.GridSize                        = FUIntVector4(ClusterGridSizeX, ClusterGridSizeY, ClusterGridSizeZ, 0);
        SceneGlobalData.Time                            = (float)World->GetTimeSinceWorldCreation();
        SceneGlobalData.DeltaTime                       = Frame.CachedWorldDeltaTime;
        // Derived rather than remembered, so it cannot drift out of step with the clock the graph reads.
        SceneGlobalData.PrevTime                        = SceneGlobalData.Time - SceneGlobalData.DeltaTime;
        SceneGlobalData.FarPlane                        = ViewVolume.GetFar();
        SceneGlobalData.NearPlane                       = ViewVolume.GetNear();
        SceneGlobalData.GTAOSettings                    = FGTAOSettings{};
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
        SceneGlobalData.CullData.ShadowLODBias          = FrameSettings.ShadowLODBias;
        SceneGlobalData.CullData.ShadowCoarseLODDistSq  = FrameSettings.ShadowCoarseLODDistance
                                                        * FrameSettings.ShadowCoarseLODDistance;
        CascadeMinTexels                                = 1.0f;
        SceneGlobalData.CullData.DebugMode              = (uint32)FrameSettings.Flags;
        SceneGlobalData.CullData.bCascadeHZBValid       = 0u;
        SceneGlobalData.CullData.bCascadeHZBMidValid    = 0u;

        CMaterial* FallbackMaterial = CMaterial::GetDefaultMaterial();
        if (!IsValid(FallbackMaterial) || !FallbackMaterial->IsReadyForRender())
        {
            ExtractFrame = nullptr;

            // Nothing rendered, so next frame's HZB is not this frame's depth.
            bDepthPyramidValid.store(false, std::memory_order_release);
            return;
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
            Data.ScreenSize                   = FUIntVector4(CaptureSize.x, CaptureSize.y, 0, 0);
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
                Data.ScreenSize                   = FUIntVector4(FaceSize, FaceSize, 0, 0);
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
                                                 Math::Clamp(Probe.BlendDistance, 0.0f, 1.0f));

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

        uint32 ProbeIndex = ~0u;
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

        if (ProbeIndex == ~0u)
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

        if (ProbeIndex == ~0u)
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
            Bake.FaceCullViews[Face] = ~0u;   // filled by BuildCullViews
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
        FThreadLocalDrawData& Local = ThreadLocalStorage[Slot];

        if (!Local.bTouched)
        {
            Local.ResetForFrame();
            Local.Items.reserve(CurrentReservePerThread);

            Local.PrepareCounters(ScenePrimitives.GetBatches().Num());
            Local.bTouched = true;
        }
        return Local;
    }

    // Routes this frame's transform + component changes into the persistent primitive table.
    void FDefaultSceneRenderer::SyncScenePrimitives()
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

        MovedTransformScratch.clear();
        if (ECS::Utils::DrainMovedTransforms(Registry, MovedTransformScratch))
        {
            FRenderDirtyTracker& Tracker = FRenderDirtyTracker::Ensure(Registry);
            for (ECS::FEntity Entity : MovedTransformScratch)
            {
                Tracker.MarkAllSources(Entity, EPrimitiveDirty::Transform);
            }
        }

        ScenePrimitives.Sync(*World);

        PublishRetainedUpload();
    }

    // Collects what changed in the retained scene, before the render phase uploads it.
    void FDefaultSceneRenderer::PublishRetainedUpload()
    {
        FFrameData::FGeometry::FRetainedUpload& Out = ExtractFrame->Geometry.RetainedUpload;

        const uint32 SlotCount = ScenePrimitives.GetRetainedSlotCount();
        Out.SlotCount = SlotCount;

        Out.DirtySlots.clear();
        Out.DirtyStaticSlots.clear();

        const uint32 DeviceCapacity = RetainedDeviceCapacity.load(std::memory_order_acquire);
        Out.bFull = ScenePrimitives.NeedsFullInstanceUpload() || SlotCount > DeviceCapacity;

        if (!Out.bFull
            && (ScenePrimitives.GetDirtyInstanceSlots().size() * 4 >= (SIZE_T)SlotCount
                || ScenePrimitives.GetDirtyStaticSlots().size() * 4 >= (SIZE_T)SlotCount))
        {
            Out.bFull = true;
        }

        if (!Out.bFull)
        {
            auto Collect = [SlotCount](const TVector<uint32>& In, TVector<uint32>& OutList)
            {
                for (uint32 DirtySlot : In)
                {
                    if (DirtySlot < SlotCount)   // a slot freed after being marked is simply dropped
                    {
                        OutList.push_back(DirtySlot);
                    }
                }
                Algo::Sort(OutList);
                OutList.erase(Algo::Unique(OutList), OutList.end());
            };

            Collect(ScenePrimitives.GetDirtyInstanceSlots(), Out.DirtySlots);
            Collect(ScenePrimitives.GetDirtyStaticSlots(),   Out.DirtyStaticSlots);

            if (Out.DirtySlots.size() * 4 >= (SIZE_T)SlotCount
                || Out.DirtyStaticSlots.size() * 4 >= (SIZE_T)SlotCount)
            {
                Out.bFull = true;
                Out.DirtySlots.clear();
                Out.DirtyStaticSlots.clear();
            }
        }

        // Interned; a full instance re-send also re-sends these, since a replaced device buffer loses them.
        Out.SurfaceDescCount        = ScenePrimitives.GetSurfaceDescCount();
        Out.bSurfaceDescsChanged    = ScenePrimitives.AreSurfaceDescsDirty() || Out.bFull;
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
            for (const TObjectPtr<CMaterialInterface>& Override : Component.MaterialOverrides)
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

        // Copies the mesh-level values into the component so the cull path never reaches the asset.
        template <typename TComponent>
        void ResolveMeshComponent(TComponent& Component, CMesh* Mesh,
                                  EInstanceFlags SeedFlags, TVector<CMaterialInterface*>& OverrideScratch)
        {
            OverrideScratch.clear();
            OverrideScratch.reserve(Component.MaterialOverrides.size());
            for (const TObjectPtr<CMaterialInterface>& Override : Component.MaterialOverrides)
            {
                OverrideScratch.push_back(Override.Get());
            }
            Component.CachedMaterialHash = HashOverrides(Component);

            FMeshResolveCache& Cache = FMeshResolveCache::Get();

            const uint32 Handle = Cache.Resolve(Mesh, OverrideScratch);
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
                return;
            }

            const FResolvedMesh& Entry = Cache.GetEntry(Handle);
            Component.CachedLocalCenter          = Entry.LocalCenter;
            Component.CachedLocalRadius          = Entry.LocalRadius;
            Component.CachedMeshletHeaderSlot = Entry.MeshletHeaderSlot;

            EInstanceFlags BaseFlags = SeedFlags;
            if (Component.bReceiveShadow)          { BaseFlags |= EInstanceFlags::ReceiveShadow; }
            if (Component.bIgnoreOcclusionCulling) { BaseFlags |= EInstanceFlags::IgnoreOcclusionCulling; }
            Component.CachedBaseFlags = BaseFlags;

            // An unready entry carries its own token too; the asset that completes it wakes the pass.
            Component.CachedEntryState = Cache.GetEntryState(Handle);
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

                ResolveMeshComponent(Component, Mesh, SeedFlags, OverrideScratch);

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
            [](const SStaticMeshComponent& C) -> CMesh* { return C.StaticMesh; },
            Tracker, EPrimitiveSource::StaticMesh);

        // Skeletal assets always carry FMeshletSkinnedVertex, so Skinned is unconditional here.
        Refreshed += ResolveMeshPool(Registry.GetStorage<SSkeletalMeshComponent>(), EInstanceFlags::Skinned, Scratch,
            [](const SSkeletalMeshComponent& C) -> CMesh* { return C.SkeletalMesh; },
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

        FFrameData& Frame = *ExtractFrame;
        auto& DrawCommands           = Frame.Geometry.DrawCommands;
        auto& LightData              = Frame.Lighting.LightData;
        auto& EnvironmentParams      = Frame.Volumetrics.EnvironmentParams;
        auto& SceneGlobalData        = Frame.SceneGlobalData;
        auto& BillboardInstances     = Frame.Primitives.BillboardInstances;
        auto& WidgetInstances        = Frame.Primitives.WidgetInstances;
        auto& GlyphInstances         = Frame.Primitives.GlyphInstances;
        auto& TextBatches            = Frame.Primitives.TextBatches;
        auto& SpriteInstances        = Frame.Primitives.SpriteInstances;
        auto& SpriteBatches          = Frame.Primitives.SpriteBatches;

        {
            LUMINA_PROFILE_SECTION("Compile Draw Commands");
            ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
            TAtomic<uint32> LightCount{0};

            auto DirectionalView     = Registry.View<SDirectionalLightComponent>(ECS::TExclude<SDisabledTag>{});
            auto SpotLightView       = Registry.View<SSpotLightComponent>(ECS::TExclude<SDisabledTag>{});
            auto PointLightView      = Registry.View<SPointLightComponent>(ECS::TExclude<SDisabledTag>{});
            auto CharacterView       = Registry.View<SCharacterControllerComponent>(ECS::TExclude<SDisabledTag>{});
            auto CameraView          = Registry.View<SCameraComponent>(ECS::TExclude<SDisabledTag>{});
            auto BillboardView       = Registry.View<SBillboardComponent>(ECS::TExclude<SDisabledTag>{});
            auto WidgetView          = Registry.View<SWidgetComponent>(ECS::TExclude<SDisabledTag>{});
            auto TextView            = Registry.View<STextComponent>(ECS::TExclude<SDisabledTag>{});
            auto SpriteView          = Registry.View<SSprite3DComponent>(ECS::TExclude<SDisabledTag>{});
            auto LineBatcherView     = Registry.View<FLineBatcherComponent>();
            auto TriangleBatcherView = Registry.View<FTriangleBatcherComponent>();
            auto EnvironmentView     = Registry.View<SEnvironmentComponent>(ECS::TExclude<SDisabledTag>{});
            auto SkyLightView        = Registry.View<SSkyLightComponent>(ECS::TExclude<SDisabledTag>{});
            auto FogView             = Registry.View<SExponentialHeightFogComponent>(ECS::TExclude<SDisabledTag>{});
            auto FogVolumeView       = Registry.View<SLocalFogVolumeComponent>(ECS::TExclude<SDisabledTag>{});
            auto CloudView           = Registry.View<SCloudComponent>(ECS::TExclude<SDisabledTag>{});
            auto TerrainAllView      = Registry.View<STerrainComponent>();
            auto TerrainView         = Registry.View<STerrainComponent>(ECS::TExclude<SDisabledTag>{});
            auto ParticleAllView     = Registry.View<SParticleSystemComponent>();
            auto ParticleView        = Registry.View<SParticleSystemComponent>(ECS::TExclude<SDisabledTag>{});
            auto DecalView           = Registry.View<SDecalComponent>(ECS::TExclude<SDisabledTag>{});
            auto AudioSourceView     = Registry.View<SAudioSourceComponent>(ECS::TExclude<SDisabledTag>{});
            auto AudioListenerView   = Registry.View<SAudioListenerComponent>(ECS::TExclude<SDisabledTag>{});
            auto WaterView           = Registry.View<SWaterComponent>(ECS::TExclude<SDisabledTag>{});
            auto TransformStorage  = Registry.GetStorage<STransformComponent>();

            ECS::Utils::ResolveAllDirtyTransforms(Registry);

        {
            ECS::FRegistry& DynRegistry = ECS::GetWorldRegistry(*World);
            ResolveDynamicMeshMaterials(DynRegistry, FRenderDirtyTracker::Ensure(DynRegistry));
        }

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

            // The gather emits skeletal items only; statics are culled on the GPU from the retained set.
            const size_t EstimatedProxies  = (size_t)ScenePrimitives.GetSkinnedPrimitiveCount() * 2;

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
            CurrentReservePerThread = (uint32)((EstimatedProxies + NumThreads - 1) / Math::Max(1u, NumThreads));
            
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

            DrawTaskGraph.Reset();   // reuse the persistent graph (allocator block + capacity)
            FTaskGraph& Graph = DrawTaskGraph;

            {
                FTaskGraph::FNodeHandle MergeNode = Graph.Add([&]
                {
                    MergeMeshDrawData(ThreadLocal);
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

                    FTaskGraph::FNodeHandle CullNode = Graph.AddParallelFor(NumSkeletal, GSkinnedCullGrain, [&](const Task::FParallelRange& Range)
                    {
                        LUMINA_PROFILE_SECTION("Cull Skinned Primitives");
                        FThreadLocalDrawData& Local = AcquireThreadLocalDrawData(Range.Thread);
                        CullSkinnedPrimitives(Range, Local);
                    }, ETaskPriority::High); // critical path, MergeNode waits on this

                    // Sizes the arena from what survived, which is what keeps it O(visible), not O(scene).
                    FTaskGraph::FNodeHandle LayoutNode = Graph.Add([&]
                    {
                        LayoutSkinnedBoneSlices(ThreadLocal);
                    }, ETaskPriority::High);

                    // A graph AddParallelFor needs its count at BUILD time, before the cull produces one.
                    FTaskGraph::FNodeHandle EmitNode = Graph.Add([&]
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

            Graph.Dispatch();

            EmitTaskGraph.Reset();
            FTaskGraph& EmitGraph = EmitTaskGraph;

            FLineBatcherComponent* LineBatcher = nullptr;
            LineBatcherView.ForEach([&](FLineBatcherComponent& C) { if (LineBatcher == nullptr)
                    {
                        LineBatcher = &C;
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

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Batched Triangle Processing");

                TriangleBatcherView.ForEach([&](FTriangleBatcherComponent& TriangleBatcherComponent)
                {
                    ProcessBatchedTriangles(TriangleBatcherComponent);
                });
            }, ETaskPriority::Medium);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Process Widget Primitives");

                const FFrustum& WidgetFrustum = Frame.CameraFrustum;
                const bool      bCullWidgets   = SceneGlobalData.CullData.bFrustumCull != 0u;

                WidgetView.ForEach([&](ECS::FEntity Entity, SWidgetComponent& WidgetComponent)
                {
                    FWidgetRuntime& Runtime = WidgetComponent.Runtime;

                    const FMatrix4 WorldMatrix = TransformStorage.Get(Entity).GetWorldMatrix();
                    const FVector3 Center = FVector3(WorldMatrix[3]);
                    const float ScaleXY = Math::Max(Math::Length(FVector3(WorldMatrix[0])), Math::Length(FVector3(WorldMatrix[1])));
                    const float Radius  = 0.5f * Math::Length(WidgetComponent.WorldSize) * Math::Max(1.0f, ScaleXY);

                    Runtime.bVisible = !bCullWidgets || WidgetFrustum.IntersectsSphere(Center, Radius);

                    if (!Runtime.bVisible || Runtime.ResourceID < 0)
                    {
                        return;
                    }

                    FWidgetInstance& Inst = WidgetInstances.emplace_back();
                    Inst.Transform    = WorldMatrix;
                    Inst.WorldSize    = WidgetComponent.WorldSize;
                    Inst.TextureIndex = (uint32)Runtime.ResourceID;
                    Inst.Flags        = WidgetComponent.bBillboard ? WIDGET_FLAG_BILLBOARD : 0u;
                    Inst.ColorPack    = PackColor(WidgetComponent.Tint);
                    Inst.EntityID     = (Entity).Value;
                    Inst.Pad0         = 0u;
                    Inst.Pad1         = 0u;
                });
            }, ETaskPriority::Medium); // emitter, not on the mesh critical path

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Process Text Primitives");

                const FFrustum& TextFrustum = Frame.CameraFrustum;
                const bool      bCullText   = SceneGlobalData.CullData.bFrustumCull != 0u;
                const FVector3  CamRight    = FVector3(SceneGlobalData.CameraData.Right);
                const FVector3  CamUp       = FVector3(SceneGlobalData.CameraData.Up);

                TextView.ForEach([&](ECS::FEntity Entity, STextComponent& TextComponent)
                {
                    if (TextComponent.Text.empty())
                    {
                        return;
                    }

                    // Fall back to the engine default font when none is set, or its atlas failed to bake.
                    CFont* Font = TextComponent.Font.Get();
                    if (Font == nullptr || !Font->HasAtlas())
                    {
                        Font = CFontManager::Get().GetDefaultFont();
                    }
                    if (Font == nullptr || !Font->HasAtlas())
                    {
                        return;
                    }

                    const int32 AtlasID = Font->GetAtlasResourceID();
                    if (AtlasID < 0)
                    {
                        return;
                    }

                    const FMatrix4 WorldMatrix = TransformStorage.Get(Entity).GetWorldMatrix();
                    const FVector3 Origin = FVector3(WorldMatrix[3]);

                    const float HAlign = (TextComponent.HorizontalAlign == ETextHorizontalAlign::Left)   ? 0.0f
                                       : (TextComponent.HorizontalAlign == ETextHorizontalAlign::Center) ? 0.5f : 1.0f;
                    // Top places the text above the origin, Bottom below (block bottom/top anchored at origin).
                    const float VAlign = (TextComponent.VerticalAlign == ETextVerticalAlign::Top)        ? 1.0f
                                       : (TextComponent.VerticalAlign == ETextVerticalAlign::Middle)     ? 0.5f : 0.0f;

                    FTextRenderCache& Cache = TextComponent.RenderCache;
                    const uint64      TextHash = Hash::GetHash64(TextComponent.Text);

                    const bool bCacheValid =
                           Cache.bValid
                        && Cache.Font        == Font
                        && Cache.FontVersion == Font->GetShapeVersion()
                        && Cache.TextHash    == TextHash
                        && Cache.TextLength  == (uint32)TextComponent.Text.size()
                        && Cache.HAlign      == TextComponent.HorizontalAlign
                        && Cache.VAlign      == TextComponent.VerticalAlign
                        && Cache.LineSpacing == TextComponent.LineSpacing;

                    if (!bCacheValid)
                    {
                        if (!Font->ShapeText(TextComponent.Text, HAlign, VAlign, TextComponent.LineSpacing, Cache.Glyphs))
                        {
                            return;
                        }

                        float EmExtent = 0.0f;
                        for (const FShapedGlyph& S : Cache.Glyphs)
                        {
                            EmExtent = Math::Max(EmExtent, Math::Max(Math::Abs(S.Min.x), Math::Abs(S.Max.x)));
                            EmExtent = Math::Max(EmExtent, Math::Max(Math::Abs(S.Min.y), Math::Abs(S.Max.y)));
                        }

                        Cache.EmExtent    = EmExtent;
                        Cache.TextHash    = TextHash;
                        Cache.TextLength  = (uint32)TextComponent.Text.size();
                        Cache.Font        = Font;
                        Cache.FontVersion = Font->GetShapeVersion();
                        Cache.HAlign      = TextComponent.HorizontalAlign;
                        Cache.VAlign      = TextComponent.VerticalAlign;
                        Cache.LineSpacing = TextComponent.LineSpacing;
                        Cache.bValid      = true;
                    }

                    const TVector<FShapedGlyph>& Shaped = Cache.Glyphs;
                    if (Shaped.empty())
                    {
                        return;
                    }

                    if (bCullText && !TextFrustum.IntersectsSphere(Origin, Cache.EmExtent * TextComponent.WorldSize * 1.5f))
                    {
                        return;
                    }

                    FVector3 RightDir, UpDir;
                    if (TextComponent.bBillboard)
                    {
                        RightDir = CamRight;
                        UpDir    = CamUp;
                    }
                    else
                    {
                        RightDir = Math::Normalize(FVector3(WorldMatrix[0]));
                        UpDir    = Math::Normalize(FVector3(WorldMatrix[1]));
                    }

                    const FVector3 RightScaled = RightDir * TextComponent.WorldSize;
                    const FVector3 UpScaled    = UpDir    * TextComponent.WorldSize;
                    const uint32   Color       = PackColor(TextComponent.Color);
                    const uint32   First       = (uint32)GlyphInstances.size();

                    for (const FShapedGlyph& S : Shaped)
                    {
                        FGPUGlyph& G = GlyphInstances.emplace_back();
                        G.Origin    = Origin;
                        G.Pad0      = 0.0f;
                        G.Right     = RightScaled;
                        G.Pad1      = 0.0f;
                        G.Up        = UpScaled;
                        G.Pad2      = 0.0f;
                        G.UVRect    = S.UV;
                        G.PlaneMin  = S.Min;
                        G.PlaneMax  = S.Max;
                        G.ColorPack = Color;
                        G.EntityID  = (Entity).Value;
                    }

                    const uint32 GlyphCount = (uint32)GlyphInstances.size() - First;

                    // Extending the open batch never reorders anything, and same-font neighbors are common.
                    if (!TextBatches.empty())
                    {
                        FFrameData::FTextBatch& Last = TextBatches.back();
                        if (Last.AtlasIndex == (uint32)AtlasID
                            && Last.bDepthTest == TextComponent.bDepthTest
                            && Last.FirstInstance + Last.Count == First)
                        {
                            Last.Count += GlyphCount;
                            return;
                        }
                    }

                    FFrameData::FTextBatch& Batch = TextBatches.emplace_back();
                    Batch.AtlasIndex    = (uint32)AtlasID;
                    Batch.AtlasWidth    = Font->GetAtlasWidth();
                    Batch.AtlasHeight   = Font->GetAtlasHeight();
                    Batch.DistanceRange = Font->GetDistanceRange();
                    Batch.FirstInstance = First;
                    Batch.Count         = GlyphCount;
                    Batch.bDepthTest    = TextComponent.bDepthTest;
                });
            }, ETaskPriority::Medium); // emitter, so it must not outrank the mesh critical path

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Process Sprite Primitives");

                const FFrustum& SpriteFrustum  = Frame.CameraFrustum;
                const bool      bCullSprites   = SceneGlobalData.CullData.bFrustumCull != 0u;
                const FVector3  SpriteCamRight = FVector3(SceneGlobalData.CameraData.Right);
                const FVector3  SpriteCamUp    = FVector3(SceneGlobalData.CameraData.Up);
                const FVector3  SpriteCamPos   = FVector3(SceneGlobalData.CameraData.Location);

                SpriteSortScratch.clear();

                SpriteView.ForEach([&](ECS::FEntity Entity, const SSprite3DComponent& Sprite)
                {
                    CTexture* Texture = Sprite.Texture.Get();
                    if (Texture == nullptr || Texture->GetResourceID() < 0 || Texture->GetNumMips() == 0)
                    {
                        return;
                    }

                    const FTextureResource::FMip& Base = Texture->GetTextureResource().Mips[0];
                    const float TexW = (float)Base.Width;
                    const float TexH = (float)Base.Height;
                    if (TexW <= 0.0f || TexH <= 0.0f)
                    {
                        return;
                    }

                    float U0, V0, U1, V1, FrameW, FrameH;
                    if (Sprite.bRegionEnabled)
                    {
                        FrameW = Sprite.RegionRect.z;
                        FrameH = Sprite.RegionRect.w;
                        if (FrameW <= 0.0f || FrameH <= 0.0f)
                        {
                            return;
                        }
                        U0 = Sprite.RegionRect.x / TexW;
                        V0 = Sprite.RegionRect.y / TexH;
                        U1 = (Sprite.RegionRect.x + FrameW) / TexW;
                        V1 = (Sprite.RegionRect.y + FrameH) / TexH;
                    }
                    else
                    {
                        const int32 HF    = Math::Max(Sprite.HFrames, 1);
                        const int32 VF    = Math::Max(Sprite.VFrames, 1);
                        const int32 Index = Math::Clamp(Sprite.Frame, 0, HF * VF - 1);
                        const int32 Cx    = Index % HF;
                        const int32 Cy    = Index / HF;

                        FrameW = TexW / (float)HF;
                        FrameH = TexH / (float)VF;
                        U0 = (float)Cx / (float)HF;
                        V0 = (float)Cy / (float)VF;
                        U1 = (float)(Cx + 1) / (float)HF;
                        V1 = (float)(Cy + 1) / (float)VF;
                    }

                    if (Sprite.bFlipH)
                    {
                        const float SwapU = U0; U0 = U1; U1 = SwapU;
                    }
                    if (Sprite.bFlipV)
                    {
                        const float SwapV = V0; V0 = V1; V1 = SwapV;
                    }

                    const FMatrix4 WorldMatrix = TransformStorage.Get(Entity).GetWorldMatrix();
                    const FVector3 Origin      = FVector3(WorldMatrix[3]);

                    const float ScaleX = Math::Length(FVector3(WorldMatrix[0]));
                    const float ScaleY = Math::Length(FVector3(WorldMatrix[1]));
                    const float QuadW  = FrameW * Sprite.PixelSize * ScaleX;
                    const float QuadH  = FrameH * Sprite.PixelSize * ScaleY;

                    // Uncentered anchors the frame's top-left at the origin, so it hangs right and down.
                    FVector2 PlaneMin = Sprite.bCentered ? FVector2(-QuadW * 0.5f, -QuadH * 0.5f) : FVector2(0.0f, -QuadH);
                    FVector2 PlaneMax = Sprite.bCentered ? FVector2( QuadW * 0.5f,  QuadH * 0.5f) : FVector2(QuadW, 0.0f);

                    // Offset is authored in texture pixels with y running down, as the 2D sprite editors do.
                    const FVector2 OffsetWorld( Sprite.Offset.x * Sprite.PixelSize * ScaleX,
                                               -Sprite.Offset.y * Sprite.PixelSize * ScaleY);
                    PlaneMin += OffsetWorld;
                    PlaneMax += OffsetWorld;

                    const float Radius = Math::Max(Math::Abs(PlaneMin.x), Math::Abs(PlaneMax.x))
                                       + Math::Max(Math::Abs(PlaneMin.y), Math::Abs(PlaneMax.y));
                    if (bCullSprites && !SpriteFrustum.IntersectsSphere(Origin, Radius))
                    {
                        return;
                    }

                    FVector3 RightDir;
                    FVector3 UpDir;
                    switch (Sprite.BillboardMode)
                    {
                    case ESpriteBillboardMode::Enabled:
                        RightDir = SpriteCamRight;
                        UpDir    = SpriteCamUp;
                        break;

                    case ESpriteBillboardMode::YBillboard:
                    {
                        UpDir = FVector3(0.0f, 1.0f, 0.0f);
                        FVector3 ToCamera = SpriteCamPos - Origin;
                        ToCamera.y = 0.0f;
                        // Directly overhead leaves no yaw to resolve, so any stable axis will do.
                        RightDir = Math::LengthSquared(ToCamera) > 1e-8f
                                 ? Math::Normalize(Math::Cross(UpDir, ToCamera))
                                 : SpriteCamRight;
                        break;
                    }

                    default:
                        RightDir = Math::Normalize(FVector3(WorldMatrix[0]));
                        UpDir    = Math::Normalize(FVector3(WorldMatrix[1]));
                        break;
                    }

                    FSpriteSortEntry& Entry = SpriteSortScratch.emplace_back();
                    Entry.TextureIndex = (uint32)Texture->GetResourceID();
                    Entry.SortOrder    = Sprite.SortOrder;
                    Entry.ViewDepthSq  = Math::LengthSquared(Origin - SpriteCamPos);
                    Entry.bDepthTest   = Sprite.bDepthTest;
                    Entry.bDoubleSided = Sprite.bDoubleSided;

                    FGPUSprite& Out = Entry.Gpu;
                    Out.Origin   = Origin;   Out.Pad0 = 0.0f;
                    Out.Right    = RightDir; Out.Pad1 = 0.0f;
                    Out.Up       = UpDir;    Out.Pad2 = 0.0f;
                    Out.UVRect   = FVector4(U0, V0, U1, V1);
                    Out.PlaneMin = PlaneMin;
                    Out.PlaneMax = PlaneMax;
                    Out.ColorPack = PackColor(Sprite.Modulate);
                    Out.EntityID  = (Entity).Value;
                    Out.Flags     = (Sprite.AlphaCut == ESpriteAlphaCut::Discard) ? SPRITE_FLAG_ALPHA_CUT : 0u;
                    Out.AlphaCutThreshold = Sprite.AlphaCutThreshold;
                });

                if (SpriteSortScratch.empty())
                {
                    return;
                }

                Algo::StableSort(SpriteSortScratch, [](const FSpriteSortEntry& A, const FSpriteSortEntry& B)
                {
                    if (A.SortOrder != B.SortOrder)
                    {
                        return A.SortOrder < B.SortOrder;
                    }
                    return A.ViewDepthSq > B.ViewDepthSq;
                });

                SpriteInstances.reserve(SpriteSortScratch.size());
                for (const FSpriteSortEntry& Entry : SpriteSortScratch)
                {
                    const uint32 First = (uint32)SpriteInstances.size();
                    SpriteInstances.push_back(Entry.Gpu);

                    if (!SpriteBatches.empty())
                    {
                        FFrameData::FSpriteBatch& Last = SpriteBatches.back();
                        if (Last.TextureIndex == Entry.TextureIndex
                            && Last.bDepthTest == Entry.bDepthTest
                            && Last.bDoubleSided == Entry.bDoubleSided
                            && Last.FirstInstance + Last.Count == First)
                        {
                            ++Last.Count;
                            continue;
                        }
                    }

                    FFrameData::FSpriteBatch& Batch = SpriteBatches.emplace_back();
                    Batch.TextureIndex  = Entry.TextureIndex;
                    Batch.FirstInstance = First;
                    Batch.Count         = 1;
                    Batch.bDepthTest    = Entry.bDepthTest;
                    Batch.bDoubleSided  = Entry.bDoubleSided;
                }
            }, ETaskPriority::Medium);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Process Billboard Primitives");

                BillboardView.ForEach([this, &BillboardInstances, &TransformStorage](ECS::FEntity Entity, const SBillboardComponent& BillboardComponent)
                {
                    if (!BillboardComponent.Texture.IsValid() || BillboardComponent.Texture->GetResourceID() < 0)
                    {
                        return;
                    }

                    FBillboardInstance& Billboard   = BillboardInstances.emplace_back();
                    Billboard.TextureIndex          = BillboardComponent.Texture->GetResourceID();
                    Billboard.Position              = TransformStorage.Get(Entity).GetWorldLocationCached();
                    Billboard.Size                  = BillboardComponent.Scale;
                    Billboard.EntityID              = (Entity).Value;
                });
                
                #if USING(WITH_EDITOR)
                // Editor visualizer billboards, skipped in game and thumbnail worlds.
                if (!World->IsGameWorld())
                {
                    auto EmplaceVisualizer = [this, &BillboardInstances](ECS::FEntity Entity, const FVector3& Position, ENamedImage Icon, const FVector4& Color, float Size = 0.20f)
                    {
                        FBillboardInstance& Billboard = BillboardInstances.emplace_back();
                        Billboard.TextureIndex        = (uint32)GetNamedImage(Icon).GetResourceID();
                        Billboard.ColorPack           = PackColor(Color);
                        Billboard.Position            = Position;
                        Billboard.Size                = Size;
                        Billboard.EntityID            = (Entity).Value;
                    };

                    // Skip editor viewport camera so the billboard doesn't sit on the user's view.
                    CameraView.ForEach([&](ECS::FEntity Entity, SCameraComponent&)
                    {
                        if (Registry.HasAll<FEditorComponent>(Entity))
                        {
                            return;
                        }
                        EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::CameraIcon, FColor::White);
                    });

                    CharacterView.ForEach([&](ECS::FEntity Entity, SCharacterControllerComponent&)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::CharacterIcon, FColor::White);
                    });

                    PointLightView.ForEach([&](ECS::FEntity Entity, const SPointLightComponent& Light)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::PointLightIcon, FVector4(Light.LightColor, 1.0f));
                    });

                    SpotLightView.ForEach([&](ECS::FEntity Entity, const SSpotLightComponent& Light)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::SpotLightIcon, FVector4(Light.LightColor, 1.0f));
                    });

                    DirectionalView.ForEach([&](ECS::FEntity Entity, const SDirectionalLightComponent& Light)
                    {
                        const auto& Transform = Registry.Get<STransformComponent>(Entity);
                        EmplaceVisualizer(Entity, Transform.GetWorldLocationCached(), ENamedImage::DirectionalLightIcon, FVector4(Light.Color, 1.0f));
                    });

                    SkyLightView.ForEach([&](ECS::FEntity Entity, const SSkyLightComponent&)
                    {
                        const auto& Transform = Registry.Get<STransformComponent>(Entity);
                        EmplaceVisualizer(Entity, Transform.GetWorldLocationCached(), ENamedImage::SkyLightIcon, FVector4(1.0f));
                    });

                    ParticleView.ForEach([&](ECS::FEntity Entity, const SParticleSystemComponent&)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::ParticleSystemIcon, FVector4(1.0f));
                    });

                    AudioSourceView.ForEach([&](ECS::FEntity Entity, const SAudioSourceComponent& Source)
                    {
                        // A live voice tints cyan, so an audible emitter is obvious without selecting it.
                        const FVector4 Tint = Source.bPlaying ? FVector4(0.35f, 0.95f, 1.0f, 1.0f) : FVector4(1.0f);
                        EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::AudioSourceIcon, Tint);
                    });

                    AudioListenerView.ForEach([&](ECS::FEntity Entity, const SAudioListenerComponent&)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.Get(Entity).GetWorldLocationCached(), ENamedImage::AudioListenerIcon, FVector4(1.0f));
                    });
                }
                #endif
            }, ETaskPriority::Medium);

            #if USING(WITH_EDITOR)
            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Selection");

                TVector<uint32>& Bits = Frame.Extracts.SelectionBits;
                Bits.clear();

                // Sized to the highest selected slot, since an entity above the top bit reads as unselected.
                uint32 HighestWord = 0u;
                bool   bAnySelected = false;
                auto   Selected = Registry.View<FSelectedInEditorComponent>();
                Selected.ForEach([&](ECS::FEntity Entity)
                {
                    HighestWord  = Math::Max(HighestWord, (uint32)(Entity).GetIndex() >> 5u);
                    bAnySelected = true;
                });

                // Sized once, since the view yields entities in no particular order.
                if (bAnySelected)
                {
                    Bits.resize(HighestWord + 1u, 0u);
                    Selected.ForEach([&](ECS::FEntity Entity)
                    {
                        const uint32 Index = (uint32)(Entity).GetIndex();
                        Bits[Index >> 5u] |= (1u << (Index & 31u));
                    });
                }
            }, ETaskPriority::Medium);
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

                PointLightView.ForEachInRange(Range.Start, Range.End,
                    [&](ECS::FEntity Entity, SPointLightComponent& PointLight)
                {
                    ProcessPointLight(PointLight, TransformStorage.Get(Entity), LightCount);
                });
            });
            
            auto SpotLightTask = EmitGraph.AddParallelFor((uint32)SpotLightView.NumDenseSlots(), 32, [&](Task::FParallelRange Range)
            {
                LUMINA_PROFILE_SECTION("Process Spot Light Range");

                SpotLightView.ForEachInRange(Range.Start, Range.End,
                    [&](ECS::FEntity Entity, SSpotLightComponent& SpotLight)
                {
                    ProcessSpotLight(SpotLight, TransformStorage.Get(Entity), LightCount);
                });
            });
            
            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Terrain");

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
            }, ETaskPriority::Medium);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Particles");

                Frame.Extracts.LiveParticleEntities.clear();
                SIZE_T ParticleCount = 0;

                for (ECS::FEntity Entity : ParticleAllView)
                {
                    Frame.Extracts.LiveParticleEntities.push_back(Entity);
                }

                ParticleView.ForEach([&](ECS::FEntity Entity, SParticleSystemComponent& Component)
                {
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
                });
                Frame.Extracts.ParticleExtracts.resize(ParticleCount);
            }, ETaskPriority::Medium);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Decals");

                Frame.Primitives.DecalExtracts.clear();
                Frame.Primitives.DecalBatches.clear();
                DecalSortScratch.clear();
                DecalGroupMinSort.clear();

                DecalView.ForEach([&](ECS::FEntity Entity, const SDecalComponent& Decal)
                {
                    CMaterialInterface* Material = Decal.DecalMaterial.Get();
                    FShaderH DecalVS;
                    FShaderH DecalPS;
                    if (Material == nullptr || !Material->ResolveDomainShaders(EMaterialType::Decal, DecalVS, DecalPS))
                    {
                        return;
                    }
                    CMaterial* ShaderOwner = Material->GetMaterial();
                    const int32 MaterialIndex = Material->GetMaterialIndex();
                    if (MaterialIndex < 0)
                    {
                        return;
                    }

                    FGPUDecal Item;
                    Item.DecalToWorld  = Math::Scale(TransformStorage.Get(Entity).GetWorldMatrix(), Decal.Size);
                    Item.WorldToDecal  = Math::Inverse(Item.DecalToWorld);
                    Item.FadeAngleCos  = Math::Cos(Math::Radians(Math::Clamp(Decal.FadeAngle, 0.0f, 89.9f)));
                    Item.Opacity       = Math::Clamp(Decal.Opacity, 0.0f, 1.0f);
                    Item.MaterialIndex = (uint32)MaterialIndex;
                    Item.Flags         = 0;

                    DecalSortScratch.push_back({ ShaderOwner, Decal.SortOrder, Item });
                });

                for (const FDecalSortEntry& E : DecalSortScratch)
                {
                    auto It = DecalGroupMinSort.find(E.ShaderOwner);
                    if (It == DecalGroupMinSort.end() || E.SortOrder < It->second)
                    {
                        DecalGroupMinSort[E.ShaderOwner] = E.SortOrder;
                    }
                }
                Algo::StableSort(DecalSortScratch, [&](const FDecalSortEntry& A, const FDecalSortEntry& B)
                {
                    const int32 GA = DecalGroupMinSort[A.ShaderOwner];
                    const int32 GB = DecalGroupMinSort[B.ShaderOwner];
                    if (GA != GB)
                    {
                        return GA < GB;
                    }
                    if (A.ShaderOwner != B.ShaderOwner)
                    {
                        return A.ShaderOwner < B.ShaderOwner;
                    }
                    return A.SortOrder < B.SortOrder;
                });

                Frame.Primitives.DecalExtracts.reserve(DecalSortScratch.size());
                CMaterial* PrevOwner = nullptr;
                for (uint32 i = 0; i < (uint32)DecalSortScratch.size(); ++i)
                {
                    Frame.Primitives.DecalExtracts.push_back(DecalSortScratch[i].Gpu);

                    CMaterial* Owner = DecalSortScratch[i].ShaderOwner;
                    if (Owner == PrevOwner && !Frame.Primitives.DecalBatches.empty())
                    {
                        Frame.Primitives.DecalBatches.back().Count++;
                    }
                    else
                    {
                        FFrameData::FDecalBatch& Batch = Frame.Primitives.DecalBatches.emplace_back();
                        Batch.Shaders.VertexShader = Owner->GetVertexShader();
                        Batch.Shaders.PixelShader  = Owner->GetPixelShader();
                        Batch.FirstInstance        = i;
                        Batch.Count                = 1u;
                        PrevOwner                  = Owner;
                    }
                }
            }, ETaskPriority::Medium);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Water");

                Frame.Water.Surfaces.clear();
                Frame.Water.bUnderwaterActive = false;

                const FVector4& CamLoc = Frame.SceneGlobalData.CameraData.Location;
                const FVector3  CameraPos = FVector3(CamLoc.x, CamLoc.y, CamLoc.z);

                // Track the nearest water surface above the camera (largest local.y still below the plane).
                float BestUnderwaterLocalY = -1.0e30f;

                auto ResolveTexture = [](const TObjectPtr<CTexture>& Tex) -> uint32
                {
                    const CTexture* T = Tex.Get();
                    const int32 ID = T ? T->GetResourceID() : -1;
                    return ID >= 0 ? (uint32)ID : ~0u;
                };

                WaterView.ForEach([&](ECS::FEntity Entity, const SWaterComponent& Water)
                {
                    const FMatrix4 WorldMatrix = TransformStorage.Get(Entity).GetWorldMatrix();
                    const float ExtentX = Math::Max(Water.Extent.x, 0.01f);
                    const float ExtentZ = Math::Max(Water.Extent.y, 0.01f);

                    FGPUWater Item    = {};
                    Item.WaterToWorld = Math::Scale(WorldMatrix, FVector3(ExtentX, 1.0f, ExtentZ));
                    Item.WorldToWater = Math::Inverse(Item.WaterToWorld);

                    Item.ShallowColor = FVector4(Water.ShallowColor, 1.0f);
                    Item.DeepColor    = FVector4(Water.DeepColor, 1.0f);
                    Item.FoamColor    = FVector4(Water.FoamColor, 1.0f);

                    FVector2    Wind    = Water.WindDirection;
                    const float WindLen = Math::Sqrt(Wind.x * Wind.x + Wind.y * Wind.y);
                    Wind = (WindLen > 1e-4f) ? FVector2(Wind.x / WindLen, Wind.y / WindLen) : FVector2(1.0f, 0.0f);

                    Item.WindAndWave    = FVector4(Wind.x, Wind.y, Water.WindSpeed, Water.WaveAmplitude);
                    Item.WaveParams     = FVector4(Math::Clamp(Water.Choppiness, 0.0f, 1.0f),
                                                   Math::Max(Water.WaveLength, 0.5f),
                                                   (float)Math::Clamp(Water.WaveCount, 1, 8),
                                                   Math::Clamp(Water.DetailStrength, 0.0f, 1.0f));
                    Item.RefractReflect = FVector4(Math::Max(Water.RefractionStrength, 0.0f),
                                                   Math::Clamp(Water.ReflectionStrength, 0.0f, 1.0f),
                                                   Math::Clamp(Water.Roughness, 0.0f, 1.0f),
                                                   Math::Max(Water.FresnelPower, 1.0f));
                    Item.FoamAbsorb     = FVector4(Math::Max(Water.ShorelineFoamWidth, 0.0f),
                                                   Math::Clamp(Water.CrestFoamAmount, 0.0f, 1.0f),
                                                   Math::Max(Water.DepthFadeDistance, 0.01f),
                                                   Math::Max(Water.AbsorptionScale, 0.0f));
                    Item.SSRSpecOpacity = FVector4(Math::Max(Water.SSRMaxDistance, 1.0f),
                                                   (float)Math::Clamp(Water.SSRStepCount, 8, 128),
                                                   Math::Max(Water.SpecularIntensity, 0.0f),
                                                   Math::Clamp(Water.Opacity, 0.0f, 1.0f));
                    Item.DetailParams   = FVector4(Math::Max(Water.DetailTiling, 0.01f),
                                                   Math::Max(Water.DetailScrollSpeed, 0.0f),
                                                   Math::Max(Water.FoamTiling, 0.01f),
                                                   Math::Max(Water.FoamIntensity, 0.0f));

                    Item.DetailNormalIndex = ResolveTexture(Water.DetailNormalMap);
                    Item.FoamTextureIndex  = ResolveTexture(Water.FoamTexture);
                    Item.GridResolution    = (uint32)Math::Clamp(Water.GridResolution, 2, 512);
                    Item.Flags             = 0;

                    Frame.Water.Surfaces.push_back(Item);
                    const FVector4 LocalCam = Item.WorldToWater * FVector4(CameraPos, 1.0f);
                    if (Math::Abs(LocalCam.x) <= 0.5f && Math::Abs(LocalCam.z) <= 0.5f &&
                        LocalCam.y < 0.0f && LocalCam.y > BestUnderwaterLocalY)
                    {
                        BestUnderwaterLocalY = LocalCam.y;

                        const FVector3 SurfaceCenter = TransformStorage.Get(Entity).GetWorldLocation();
                        Frame.Water.bUnderwaterActive = true;
                        Frame.Water.Underwater.PlaneNormalAndHeight = FVector4(0.0f, 1.0f, 0.0f, SurfaceCenter.y);
                        Frame.Water.Underwater.FogColorDensity      = FVector4(Water.UnderwaterFogColor, Math::Max(Water.UnderwaterFogDensity, 0.0f));
                        Frame.Water.Underwater.TintDistortion       = FVector4(Water.UnderwaterTint, Math::Max(Water.UnderwaterDistortion, 0.0f));
                        Frame.Water.Underwater.DeepColor            = FVector4(Water.DeepColor, 0.0f);
                    }
                });
            }, ETaskPriority::Medium);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Paint Ops");
                Frame.Extracts.PaintOps.clear();
                World->DrainRenderTargetPaints(Frame.Extracts.PaintOps);
            }, ETaskPriority::Medium);

            EmitGraph.AddDependency(PointLightTask, DLightTask);
            EmitGraph.AddDependency(SpotLightTask, DLightTask);

            EmitGraph.Dispatch();

            Frame.Primitives.DebugTextGlyphs.clear();
            Frame.Primitives.DebugTextBatch = {};
#if !defined(LE_SHIPPING)
            {
                TVector<FDebugTextLine> DebugLines;
                World->DrainDebugTextLines(DebugLines);

                CFont* DebugFont = CFontManager::Get().GetDefaultFont();
                const int32 DebugAtlasID = DebugFont ? DebugFont->GetAtlasResourceID() : -1;
                if (!DebugLines.empty() && DebugFont && DebugFont->HasAtlas() && DebugAtlasID >= 0)
                {
                    const float PxSize = 32.0f;   // pixels per em
                    const float Margin = 12.0f;
                    float       PenY   = Margin;

                    TVector<FShapedGlyph> DebugShaped;
                    for (const FDebugTextLine& Line : DebugLines)
                    {
                        const uint32 Color = PackColor(Line.Color);
                        if (DebugFont->ShapeText(Line.Text, 0.0f /*left*/, 0.0f, 1.0f, DebugShaped))
                        {
                            for (const FShapedGlyph& S : DebugShaped)
                            {
                                FGPUGlyph& G = Frame.Primitives.DebugTextGlyphs.emplace_back();
                                G.PlaneMin  = FVector2(Margin + S.Min.x * PxSize, PenY - S.Max.y * PxSize);
                                G.PlaneMax  = FVector2(Margin + S.Max.x * PxSize, PenY - S.Min.y * PxSize);
                                G.UVRect    = S.UV;
                                G.ColorPack = Color;
                            }
                        }

                        int32 NumLines = 1;
                        for (const char C : Line.Text)
                        {
                            if (C == '\n') ++NumLines;
                        }
                        PenY += (float)NumLines * DebugFont->GetLineHeight() * PxSize;
                    }

                    if (!Frame.Primitives.DebugTextGlyphs.empty())
                    {
                        FFrameData::FTextBatch& Batch = Frame.Primitives.DebugTextBatch;
                        Batch.AtlasIndex    = (uint32)DebugAtlasID;
                        Batch.AtlasWidth    = DebugFont->GetAtlasWidth();
                        Batch.AtlasHeight   = DebugFont->GetAtlasHeight();
                        Batch.DistanceRange = DebugFont->GetDistanceRange();
                        Batch.FirstInstance = 0;
                        Batch.Count         = (uint32)Frame.Primitives.DebugTextGlyphs.size();
                    }
                }
            }
#endif

            {
                LUMINA_PROFILE_SECTION_COLORED("Wait Draw Graphs", tracy::Color::Crimson);
                Graph.Wait();
                EmitGraph.Wait();
            }

            // After the waits, so the streamer walks CMaterial state with no gather in flight.
            PublishStreamingFeedback();

            // LightCount can overshoot MAX_LIGHTS; clamp to match what Process*Light wrote.
            NumLiveLights = Math::Min(LightCount.load(std::memory_order_acquire), (uint32)MAX_LIGHTS);

            // Serial fit/allocate after parallel light pass; shrinks when sum(area) exceeds atlas budget.
            AllocateShadowTiles();

            // Same overshoot as LightCount, and this is the last writer of the shadow counter.
            NumLiveShadows = Math::Min(Frame.Lighting.ShadowDataCount.load(std::memory_order_acquire),
                                       (uint32)MAX_SHADOWS);

            // Serial after parallel light pass; skylight below reads ActiveEnv + LightData.SunDirection set by ProcessDirectionalLight.
            const SEnvironmentComponent* ActiveEnv = nullptr;
            {
                LUMINA_PROFILE_SECTION("Environment Processing");

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
            }

            {
                LUMINA_PROFILE_SECTION("Skylight Processing");

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
                    LightData.AmbientLight = FVector4(AmbientRGB, Sky.Intensity);
                });

                LightData.bHasIBL = FrameFlags.bHasEnvironment ? 1u : 0u;
            }

            // Exponential height fog. Last enabled component with density > 0 wins.
            {
                LUMINA_PROFILE_SECTION("Fog Processing");

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
        }

        if (LightData.bHasSun)
        {
            const FVector3 SunDir = Math::Normalize(LightData.SunDirection);
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
            FrozenCull.bValid            = true;
            return;
        }

        // The view COUNT freezes too, since bucket indices and the dispatch grid all derive from it.
        Frame.Views.CullViews       = FrozenCull.Views;
        Frame.Views.CascadeViewBase = FrozenCull.CascadeViewBase;

        // Batches are still live, so the only per-view field that tracks them has to be restamped.
        const uint32 NumDraws = Frame.Views.NumDrawsPerView;
        for (uint32 v = 0; v < (uint32)Frame.Views.CullViews.size(); ++v)
        {
            Frame.Views.CullViews[v].IndirectArgsOffset = v * NumDraws;
            Frame.Views.CullViews[v].NumDraws           = NumDraws;
        }

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
        const int32 Slot = Frame.Lighting.LightData.bHasSun ? Frame.Lighting.Lights[0].ShadowDataIndex : INDEX_NONE;
        if (Slot == INDEX_NONE || Slot >= (int32)MAX_SHADOWS)
        {
            return false;
        }

        const FLightShadowData& Sun = Frame.Lighting.Shadows[Slot];
        for (int32 c = 0; c < NumCascades; ++c)
        {
            FrozenCull.CascadeShadowViewProjection[c] = Sun.ViewProjection[c];
        }

        FrozenCull.CascadeRadii = Frame.Lighting.LightData.CascadeRadii;
        return true;
    }

    // Written into whatever slot this frame assigned the sun, since the slot order is not itself frozen.
    void FDefaultSceneRenderer::RestoreCascadeShadowFit(FFrameData& Frame) const
    {
        const int32 Slot = Frame.Lighting.LightData.bHasSun ? Frame.Lighting.Lights[0].ShadowDataIndex : INDEX_NONE;
        if (!FrozenCull.bHasCascadeShadow || Slot == INDEX_NONE || Slot >= (int32)MAX_SHADOWS)
        {
            return;
        }

        FLightShadowData& Sun = Frame.Lighting.Shadows[Slot];
        for (int32 c = 0; c < NumCascades; ++c)
        {
            Sun.ViewProjection[c] = FrozenCull.CascadeShadowViewProjection[c];
        }

        Frame.Lighting.LightData.CascadeRadii = FrozenCull.CascadeRadii;
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

    static uint32 SelectLODIndex(const FSurfaceDescGPU& Desc, float DistSq, float RadiusSq)
    {
        // A zero radius used to yield ratio 0; without this guard every threshold would pass.
        if (RadiusSq <= 0.0f)
        {
            return 0u;
        }

        uint32 Picked = 0;
        const uint32 LastLOD = Desc.NumLODs > 0 ? Desc.NumLODs - 1u : 0u;
        for (uint32 i = 1; i <= LastLOD; ++i)
        {
            if (DistSq >= Desc.LODScreenThresholdSq[i] * RadiusSq)
            {
                Picked = i;
            }
            else
            {
                break;
            }
        }
        return Picked;
    }

    // Component override beats global setting; both clamped to surface NumLODs.
    static uint32 ResolveSurfaceLOD(const FSurfaceDescGPU& Desc, int32 ForcedLODIndex, bool bUseLODs, float DistSq, float RadiusSq)
    {
        if (Desc.NumLODs <= 1)
        {
            return 0u;
        }
        if (ForcedLODIndex >= 0)
        {
            return (uint32)Math::Min((int32)Desc.NumLODs - 1, ForcedLODIndex);
        }
        if (bUseLODs)
        {
            return SelectLODIndex(Desc, DistSq, RadiusSq);
        }
        return 0u;
    }

    static uint32 ResolveShadowLOD(const FSurfaceDescGPU& Desc, uint32 CameraLOD, int32 ShadowLODBias,
                                   float DistSq, float CoarseDistSq)
    {
        if (Desc.NumLODs == 0)
        {
            return 0u;
        }
        const uint32 Cap   = (CoarseDistSq > 0.0f && DistSq >= CoarseDistSq) ? MAX_COARSE_SHADOW_LOD : MAX_SHADOW_LOD;
        const int32 Biased = (int32)CameraLOD + ShadowLODBias;
        const int32 MaxLOD = (int32)Math::Min<uint32>(Desc.NumLODs - 1u, Cap);
        return (uint32)Math::Clamp(Biased, 0, MaxLOD);
    }

    // The LOD table comes from the interned FSurfaceDescGPU the binding already names, not from the
    // 304-byte FResolvedSurface behind Prim.Surfaces. Identical tables collapse to one entry there, so many
    // instances of few meshes read a handful of descs instead of one pointer chase per primitive.
    static void EmitPrimitiveSurfaces(FDefaultSceneRenderer::FThreadLocalDrawData& Local,
                                      const FScenePrimitive& Prim,
                                      const FSurfaceBinding* Bindings,
                                      const FSurfaceDescGPU* SurfaceDescs,
                                      uint32 NumSurfaceDescs,
                                      uint32 EntityRecordIdx,
                                      const FSceneRenderSettings& Settings,
                                      float DistSq,
                                      float RadiusSq)
    {
        const uint32 MeshletHeaderSlot = Prim.MeshletHeaderSlot;

        for (uint32 s = 0; s < Prim.SurfaceCount; ++s)
        {
            const FSurfaceBinding& Binding = Bindings[s];
            if (Binding.SurfaceDescIndex >= NumSurfaceDescs)
            {
                continue;
            }
            const FSurfaceDescGPU& Desc = SurfaceDescs[Binding.SurfaceDescIndex];

            EInstanceFlags Flags = Prim.BaseFlags | Binding.MaterialFlags;
            if (Prim.bCastShadow && Binding.bMaterialCastsShadows)
            {
                Flags |= EInstanceFlags::CastShadow;
            }

            // CPU LOD pick replaces LOD 0; smaller ranges directly cut cull-pass cost.
            const uint32 LODIndex       = ResolveSurfaceLOD(Desc, Prim.ForcedLODIndex, Settings.bUseLODs, DistSq, RadiusSq);
            const uint32 ShadowLODIndex = ResolveShadowLOD(Desc, LODIndex, Settings.ShadowLODBias,
                                                           DistSq, Settings.ShadowCoarseLODDistance * Settings.ShadowCoarseLODDistance);

            // Zero meshlet count gates the cull shader's MeshletHeader deref.
            const uint32 SurfaceMeshletCount  = MeshletHeaderSlot ? Desc.LODMeshletCount[LODIndex]       : 0u;
            const uint32 SurfaceMeshletOffset = Desc.LODMeshletOffset[LODIndex];
            const uint32 ShadowMeshletCount   = MeshletHeaderSlot ? Desc.LODMeshletCount[ShadowLODIndex] : 0u;
            const uint32 ShadowMeshletOffset  = Desc.LODMeshletOffset[ShadowLODIndex];

            const uint32 LastLOD = Desc.NumLODs > 0u ? Math::Min(Desc.NumLODs, (uint32)MAX_MESH_LODS) - 1u : 0u;
            const uint32 MeshletTotalCount = MeshletHeaderSlot
                                           ? Desc.LODMeshletOffset[LastLOD] + Desc.LODMeshletCount[LastLOD]
                                           : 0u;

            const uint32 BatchIndex = Binding.BatchIndex;

            // Counted only to mark the batch touched, so PrepareCounters knows what to reset.
            if (Local.DrawInstanceCounts[BatchIndex]++ == 0u)
            {
                Local.TouchedSlots.push_back(BatchIndex);
            }
            FDefaultSceneRenderer::FProcessedDrawItem& Item = Local.Items.emplace_back();
            Item.EntityRecordIndex    = EntityRecordIdx;
            Item.BatchIndex           = BatchIndex;
            Item.InstanceSlot         = Binding.InstanceSlot;
            Item.SurfaceMeshletOffset = SurfaceMeshletOffset;
            Item.SurfaceMeshletCount  = SurfaceMeshletCount;
            Item.ShadowMeshletOffset  = ShadowMeshletOffset;
            Item.ShadowMeshletCount   = ShadowMeshletCount;
            Item.MeshletTotalCount    = MeshletTotalCount;
            Item.Flags                = Flags;
            Item.MaterialIndex        = Binding.MaterialIndex;
            Item._Pad                 = 0;

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

        const auto Accept = [&](uint32 PrimIndex)
        {
            const FScenePrimitive& Prim = Prims[PrimIndex];
            if (Prim.Surfaces == nullptr || Prim.SurfaceCount == 0u || Prim.BoneCount == 0u)
            {
                return;   // parked awaiting resolve or a skeleton; the sync pass retries it
            }

            // A SUPERSET of what the emit pass accepts; anything it rejects just leaves an unused hole.
            const uint32 Slot = SkinnedCandidateCursor.fetch_add(1, std::memory_order_relaxed);
            if (Slot < (uint32)SkinnedCandidates.size())
            {
                SkinnedCandidates[Slot]     = PrimIndex;
                SkinnedCandidateBones[Slot] = Prim.BoneCount;
            }
        };

        if (!SceneCull.bEnabled)
        {
            for (uint32 d = Range.Start; d < Range.End; ++d)
            {
                Accept(Skeletal[d]);
            }
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
            const float    RadiusSq = Radius * Radius;

            const uint32 EntityRecordIdx = (uint32)Local.EntityRecords.size();
            FEntityRecord& EntityRecord = Local.EntityRecords.emplace_back();
            EntityRecord.Transform            = Prim.Transform;
            EntityRecord.SphereBounds         = Sphere;
            EntityRecord.MeshletHeaderSlot    = Prim.MeshletHeaderSlot;
            EntityRecord.CustomData           = Prim.CustomData;
            EntityRecord.EntityID             = Prim.EntityID;
            EntityRecord.BoneArenaBase        = kNoBoneSlice;
            EntityRecord.BoneArenaCount       = 0u;

            //~ Everything below needs the live component.

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

            if (SkeletonBoneCount > 0 && (SIZE_T)BoneSlice + SkeletonBoneCount <= ArenaCount)
            {
                EntityRecord.BoneArenaBase  = BoneSlice;
                EntityRecord.BoneArenaCount = SkeletonBoneCount;

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
                                  EntityRecordIdx, FrameSettings, DistSq, RadiusSq);
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
            Cmd.MomentPixelShader              = Batch.MomentPixelShader;
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
                    DeferredMaterials.push_back({ (uint32)MatSlot.MaterialIndex, MatSlot.DeferredShader });
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
            // Indexed by retained slot; grown, never cleared. Ungathered slots are rejected by their tag.
            const uint32 RetainedSlots = ScenePrimitives.GetRetainedSlotCount();
            if ((uint32)SkinnedData.size() < RetainedSlots)
            {
                SkinnedData.resize(RetainedSlots);
            }

            for (uint32 t = 0; t < NumThreads; ++t)
            {
                FThreadLocalDrawData& Local = ThreadLocal[t];
                if (!Local.bTouched)
                {
                    continue;
                }

                for (const FProcessedDrawItem& Item : Local.Items)
                {
                    if (Item.InstanceSlot >= RetainedSlots)
                    {
                        continue;   // slot freed after the gather read it; the cull will reject it anyway
                    }

                    FSkinnedFrameData& Out = SkinnedData[Item.InstanceSlot];
                    Out.BoneOffset              = (Item.EntityRecordIndex < Local.EntityRecords.size())
                                                ? Local.EntityRecords[Item.EntityRecordIndex].BoneArenaBase
                                                : kNoBoneSlice;
                    // UploadSkinnedFrameData stamps FrameTag later, so the tag does not exist yet here.
                    Out.SurfaceMeshletOffset    = Item.SurfaceMeshletOffset;
                    Out.SurfaceMeshletCount     = Item.SurfaceMeshletCount;
                    Out.ShadowMeshletOffset     = Item.ShadowMeshletOffset;
                    Out.ShadowMeshletCount      = Item.ShadowMeshletCount;
                    Out.MeshletTotalCount       = Item.MeshletTotalCount;
                    // Seeded to the sentinel so a rejected slot keeps no slice rather than last frame's base.
                    Out.SkinnedVertexBase       = kNoPreSkinBase;
                    Out.ShadowSkinnedVertexBase = kNoPreSkinBase;

                    SkinnedSlots.push_back(Item.InstanceSlot);
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
            else
            {
                OpaqueDrawList.push_back(i);
            }
        }
        FrameStats.NumBatches = NumLiveBatches;
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
                SceneCullContext.SunDirection = Math::Normalize(Light.Direction);
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
            if (!Light.bCastShadows)
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
            if (!Light.bCastShadows)
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

    void FDefaultSceneRenderer::ProcessPointLight(const SPointLightComponent& PointLight, const STransformComponent& TransformComponent, TAtomic<uint32>& LightCount)
    {
        FFrameData& Frame                       = *ExtractFrame;
        TVector<FShadowRequest>& ShadowRequests = Frame.Lighting.ShadowRequests;
        FMutex& ShadowRequestMutex              = Frame.Lighting.ShadowRequestMutex;

        const FVector3 Position = TransformComponent.GetWorldLocationCached();

        // Ahead of the slot handout, so an unreachable light costs neither a light index nor a cluster test.
        if (!IsLightRelevant(Position, PointLight.Attenuation))
        {
            return;
        }

        auto Lights = LightCount.fetch_add(1, std::memory_order_acquire);
        if (Lights >= MAX_LIGHTS)
        {
            NotifyMaxLightsHit();
            return;
        }

        FLight Light                = {};
        Light.Flags                 = ELightFlags::Point;
        Light.Falloff               = PointLight.Falloff;
        Light.Color                 = PackColor(FVector4(PointLight.LightColor, 1.0));
        Light.Intensity             = PointLight.Intensity;
        Light.Radius                = PointLight.Attenuation;
        Light.Position              = Position;
        Light.ShadowDataIndex       = INDEX_NONE;
        if (PointLight.bVolumetric)
        {
            Light.Flags             |= ELightFlags::Volumetric;
            Light.VolumetricIntensity = PointLight.VolumetricIntensity;
            Light.VolumetricScatteringRadius = PointLight.VolumetricScatteringRadius;
        }

        if (PointLight.bCastShadows && ShouldRequestShadow(Light.Position, Light.Radius))
        {
            const FVector3 CamPos = ExtractFrame->ViewVolume.GetViewPosition();
            const float Dist = Math::Distance(CamPos, Light.Position);
            constexpr float ResolutionScale = 2048.0f;
            const uint32 DesiredPixels = (uint32)((Light.Radius / Math::Max(Dist, 0.01f)) * ResolutionScale);

            FShadowRequest Req;
            Req.LightIndex      = Lights;
            Req.Type            = ELightType::Point;
            Req.DesiredPixels   = DesiredPixels;
            Req.DistanceToCamera = Dist;
            Req.Position        = Light.Position;
            Req.Direction       = FVector3(0.0f);
            Req.Up              = FVector3(0.0f);
            Req.Attenuation     = Light.Radius;
            Req.OuterFOVDegrees = 0.0f;
            {
                FScopeLock Lock(ShadowRequestMutex);
                ShadowRequests.push_back(Req);
            }
        }

        Frame.Lighting.Lights[Lights] = Light;
    }

    void FDefaultSceneRenderer::ProcessSpotLight(const SSpotLightComponent& SpotLight, const STransformComponent& TransformComponent, TAtomic<uint32>& LightCount)
    {
        FFrameData& Frame = *ExtractFrame;
        auto& ShadowRequests    = Frame.Lighting.ShadowRequests;
        auto& ShadowRequestMutex= Frame.Lighting.ShadowRequestMutex;

        const FVector3 Position = TransformComponent.GetWorldLocationCached();

        // Tested as the full attenuation sphere; the cone would reject more but needs the rotation first.
        if (!IsLightRelevant(Position, SpotLight.Attenuation))
        {
            return;
        }

        auto Lights = LightCount.fetch_add(1, std::memory_order_acquire);
        if (Lights >= MAX_LIGHTS)
        {
            NotifyMaxLightsHit();
            return;
        }

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
        Light.ShadowDataIndex       = INDEX_NONE;
        if (SpotLight.bVolumetric)
        {
            Light.Flags             |= ELightFlags::Volumetric;
            Light.VolumetricIntensity = SpotLight.VolumetricIntensity;
            Light.VolumetricScatteringRadius = SpotLight.VolumetricScatteringRadius;
        }

        if (SpotLight.bCastShadows && ShouldRequestShadow(Light.Position, Light.Radius))
        {
            const FVector3 CamPos = ExtractFrame->ViewVolume.GetViewPosition();
            const float Dist = Math::Distance(CamPos, Light.Position);
            constexpr float ResolutionScale = 2048.0f;
            const uint32 DesiredPixels = (uint32)((Light.Radius / Math::Max(Dist, 0.01f)) * ResolutionScale);

            FShadowRequest Req;
            Req.LightIndex      = Lights;
            Req.Type            = ELightType::Spot;
            Req.DesiredPixels   = DesiredPixels;
            Req.DistanceToCamera = Dist;
            Req.Position        = Light.Position;
            Req.Direction       = UpdatedForward;   // shadow camera looks along the aim (where the spot lights)
            Req.Up              = UpdatedUp;
            Req.Attenuation     = SpotLight.Attenuation;
            Req.OuterFOVDegrees = OuterDegrees;
            {
                FScopeLock Lock(ShadowRequestMutex);
                ShadowRequests.push_back(Req);
            }
        }

        Frame.Lighting.Lights[Lights] = Light;
    }

    void FDefaultSceneRenderer::AllocateShadowTiles()
    {
        FFrameData& Frame = *ExtractFrame;
        auto& LightData       = Frame.Lighting.LightData;
        auto& ShadowRequests  = Frame.Lighting.ShadowRequests;
        auto& ShadowDataCount = Frame.Lighting.ShadowDataCount;
        auto& PackedShadows   = Frame.Lighting.PackedShadows;

        if (ShadowRequests.empty())
        {
            return;
        }

        {
            const uint32 SunViews        = LightData.bHasSun ? (uint32)NumCascades : 0u;
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
                return Req.Type == ELightType::Point ? 6u : 1u;
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
        }

        auto AreaCost = [&](uint32 i) -> uint64
        {
            const uint64 PerTile = (uint64)Sizes[i] * (uint64)Sizes[i];
            return ShadowRequests[i].Type == ELightType::Point ? PerTile * 6ull : PerTile;
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
                int32 FaceTileIndices[6];
                bool  bAllAllocated = true;
                for (uint32 Face = 0; Face < 6; ++Face)
                {
                    FaceTileIndices[Face] = ShadowAtlas.AllocateTile(TileSize);
                    if (FaceTileIndices[Face] == INDEX_NONE)
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

                    const FShadowTile& FaceTile = ShadowAtlas.GetTile(FaceTileIndices[Face]);

                    FLightShadow& Shadow   = ShadowData.Shadow[Face];
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
                if (TileIndex == INDEX_NONE)
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

        const uint32 NumDraws = Frame.Views.NumDrawsPerView;

        auto PushView = [&](const FMatrix4& ViewProjection, const FVector3& Origin, uint32 Flags,
                            uint32 CascadeIndex = ~0u, float MinBoundsDiameter = 0.0f)
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
            View.IndirectArgsOffset = ViewIndex * NumDraws;
            View.NumDraws           = NumDraws;
            CullViews.push_back(View);

            return ViewIndex;
        };

        // Both VisBuffer phases rasterize the SAME camera view, so it contributes one entry.
        const uint32 NumViews =
            1u +                                                        // Camera
            (LightData.bHasSun ? (uint32)NumCascades : 0u) +            // CSM cascades
            (uint32)PackedShadows[(uint32)ELightType::Point].size() * 6u +
            (uint32)PackedShadows[(uint32)ELightType::Spot].size() +
            (uint32)Frame.Views.CaptureViews.size() +                         // Capture cameras (frustum-only)
            (Frame.ReflectionProbes.BakingProbe >= 0 ? 6u : 0u);              // Reflection-probe cube faces

        ASSERT(NumViews <= (uint32)GMaxCullViews);

        CullViews.reserve(NumViews);

        CascadeViewBase = ~0u;
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
            PushView(CameraVP, ViewVolume.GetViewPosition(), CameraFlags);
        }
        
        if (LightData.bHasSun)
        {
            const int32 SunShadowIndex = Frame.Lighting.Lights[0].ShadowDataIndex;
            if (SunShadowIndex != INDEX_NONE)
            {
                const FLightShadowData& SunShadow = Frame.Lighting.Shadows[SunShadowIndex];
                const uint32 CascadeFlags =
                    (FrameSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
                    ConeFlag |
                    ECullViewFlags::SunAligned |
                    ECullViewFlags::CastShadowOnly |
                    ECullViewFlags::Distance |
                    ECullViewFlags::Cascade;

                // Published by ProcessDirectionalLight from the active sun, already clamped.
                const float MinTexels = CascadeMinTexels;

                CascadeViewBase = (uint32)CullViews.size();
                for (int32 c = 0; c < NumCascades; ++c)
                {
                    // Micro-poly threshold in world units, from this cascade's own texel pitch.
                    const float Radius     = LightData.CascadeRadii[c];
                    const float Resolution = Math::Max(LightData.CascadeResolutions[c], 1.0f);
                    const float TexelWorld = (Radius * 2.0f) / Resolution;

                    PushView(SunShadow.ViewProjection[c], ViewVolume.GetViewPosition(), CascadeFlags,
                             (uint32)c, MinTexels * TexelWorld);
                }

            }
        }

        for (const FLightShadow& PointShadow : PackedShadows[(uint32)ELightType::Point])
        {
            if (PointShadow.ShadowDataIndex < 0)
            {
                PointShadowCullViewBases.push_back(~0u);
                continue;
            }

            const FLightShadowData& ShadowData = Frame.Lighting.Shadows[PointShadow.ShadowDataIndex];
            const FLight& Light = Frame.Lighting.Lights[PointShadow.LightIndex];
            const uint32 FaceFlags =
                (FrameSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
                ConeFlag |
                ECullViewFlags::CastShadowOnly;

            PointShadowCullViewBases.push_back((uint32)CullViews.size());
            for (int32 Face = 0; Face < 6; ++Face)
            {
                PushView(ShadowData.ViewProjection[Face], Light.Position, FaceFlags);
            }
        }

        // One view per spotlight.
        for (const FLightShadow& SpotShadow : PackedShadows[(uint32)ELightType::Spot])
        {
            if (SpotShadow.ShadowDataIndex < 0)
            {
                SpotShadowCullViewBases.push_back(~0u);
                continue;
            }

            const FLightShadowData& ShadowData = Frame.Lighting.Shadows[SpotShadow.ShadowDataIndex];
            const FLight& Light = Frame.Lighting.Lights[SpotShadow.LightIndex];
            const uint32 SpotFlags =
                (FrameSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
                ConeFlag |
                ECullViewFlags::CastShadowOnly;

            SpotShadowCullViewBases.push_back((uint32)CullViews.size());
            PushView(ShadowData.ViewProjection[0], Light.Position, SpotFlags);
        }

        for (FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            const FMatrix4 CaptureVP = Capture.ViewVolume.GetProjectionMatrix() * Capture.ViewVolume.GetViewMatrix();
            const uint32 CaptureFlags = ECullViewFlags::Frustum | ConeFlag;
            Capture.CameraViewIndex = PushView(CaptureVP, Capture.ViewVolume.GetViewPosition(), CaptureFlags);
        }

        if (Frame.ReflectionProbes.BakingProbe >= 0)
        {
            const uint32 FaceFlags = ECullViewFlags::Frustum | ConeFlag;
            for (int32 Face = 0; Face < 6; ++Face)
            {
                const FViewVolume& Volume = Frame.ReflectionProbes.FaceVolumes[Face];
                const FMatrix4 FaceVP = Volume.GetProjectionMatrix() * Volume.GetViewMatrix();
                Frame.ReflectionProbes.FaceCullViews[Face] = PushView(FaceVP, Volume.GetViewPosition(), FaceFlags);
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

        FLight Light            = {};
        Light.Flags             = ELightFlags::Directional;
        Light.Color             = PackColor(FVector4(LightColor, 1.0));
        Light.Intensity         = DirectionalLight.Intensity;
        Light.Direction         = Math::Normalize(DirectionalLight.Direction);
        Light.ShadowDataIndex   = INDEX_NONE;
        LightData.SunDirection  = Light.Direction;
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

        if (!DirectionalLight.bCascadeOcclusionCull)
        {
            SceneGlobalData.CullData.bShadowOcclusionCull = 0u;
        }
        CascadeMinTexels = Math::Max(DirectionalLight.CascadeMinTexels, 0.0f);

        // Shadow tuning forwarded to the lit pixel shaders via the light buffer.
        LightData.ShadowParams  = FVector4(DirectionalLight.ShadowNormalBias,
                                            DirectionalLight.ShadowDepthBias,
                                            DirectionalLight.ShadowSoftness,
                                            DirectionalLight.CascadeBlend);
        LightData.ShadowParams2 = FVector4(DirectionalLight.ShadowDistanceFade,
                                            float(DirectionalLight.ShadowSampleCount),
                                            DirectionalLight.ContactShadowLength,
                                            float(DirectionalLight.ContactShadowSamples));

        const float CascadeSplitLambda = Math::Clamp(DirectionalLight.CascadeSplitLambda, 0.0f, 1.0f);

        constexpr float ShadowMinDistance   = 1.0f;

        const float ShadowFar  = Math::Min(FarClip, DirectionalLight.ShadowMaxDistance);
        const float ShadowNear = Math::Max(NearClip, ShadowMinDistance);
        const float ClipRange  = ShadowFar - ShadowNear;
        const float MinDepth   = ShadowNear;
        const float MaxDepth   = ShadowFar;
        const float DepthRatio = MaxDepth / Math::Max(MinDepth, 0.0001f);
        
        float CascadeFarDistances[NumCascades];
        for (int i = 0; i < NumCascades; ++i)
        {
            const float P       = (float)(i + 1) / (float)NumCascades;
            const float LogD    = MinDepth * Math::Pow(DepthRatio, P);
            const float UniD    = MinDepth + ClipRange * P;
            const float D       = CascadeSplitLambda * (LogD - UniD) + UniD;
            CascadeFarDistances[i]      = D;
            LightData.CascadeSplits[i]  = D; // World-distance, view-space Z.
        }
        
        const FMatrix4& CamView   = ViewVolume.GetViewMatrix();
        const float      CamFOV    = ViewVolume.GetFOV();
        const float      CamAspect = ViewVolume.GetAspectRatio();
        const FVector3  LightDir  = Light.Direction; // Toward the sun.

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
        for (int i = 0; i < NumCascades; ++i)
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
                FViewVolume::UpAxis);

            FVector4 CenterLS = LightRotation * FVector4(SphereCenter, 1.0f);
            CenterLS.x = std::round(CenterLS.x / TexelSize) * TexelSize;
            CenterLS.y = std::round(CenterLS.y / TexelSize) * TexelSize;
            const FVector3 SnappedCenter = FVector3(Math::Inverse(LightRotation) * CenterLS);

            const FMatrix4 LightView = Math::LookAt(
                SnappedCenter + LightDir * (Radius + BackDistance),
                SnappedCenter,
                FViewVolume::UpAxis);
            
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
                CascadeTile.ShadowMapIndex  = INDEX_NONE;
                CascadeTile.LightIndex      = 0;
                CascadeTile.ShadowDataIndex = (int32)ShadowSlot;
                CascadeTile._Padding        = 0;
            }

            LightData.CascadeRadii[i] = Radius;

            {
                const float PrevBandNear = (i >= 2) ? CascadeFarDistances[i - 2] : 0.0f;
                const float BlendBack    = (i == 0) ? 0.0f : CascadeBlendFraction * (SplitNear - PrevBandNear);

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
                    uint32 Idx = ~0u;
                    for (uint32 b = 0; b < S.NumBuckets; ++b)
                    {
                        if (S.BucketDepthTest[b] == Line.bDepthTest &&
                            Math::EpsilonEqual(S.BucketThickness[b], Line.Thickness, LE_SMALL_NUMBER))
                        {
                            Idx = b;
                            break;
                        }
                    }
                    if (Idx == ~0u)
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
                    S.GlobalBucket[b] = ~0u;
                    continue;
                }

                uint32 G = ~0u;
                for (uint32 k = 0, n = (uint32)Global.size(); k < n; ++k)
                {
                    if (Global[k].bDepthTest == S.BucketDepthTest[b] &&
                        Math::EpsilonEqual(Global[k].Thickness, S.BucketThickness[b], LE_SMALL_NUMBER))
                    {
                        G = k;
                        break;
                    }
                }
                if (G == ~0u)
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
        LOG_WARN("[Rendering] - Maximum Lights Hit! {}", MAX_LIGHTS);
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
            Out.GrassMaxDrawDistance = 0.0f;

            const SGrassComponent* Grass = Registry.TryGet<SGrassComponent>(Entity);
            if (Grass == nullptr || !Grass->bEnabled)
            {
                return;
            }

            CMaterialInterface* MaterialInterface = Terrain.Material;
            CMaterial* Material = MaterialInterface != nullptr ? MaterialInterface->GetMaterial() : nullptr;
            if (Material == nullptr || Material->GrassOutputs.empty())
            {
                return;
            }

            Out.GrassMaxInstances    = Grass->MaxInstancesPerSpecies;
            Out.GrassMaxDrawDistance = Grass->MaxDrawDistance;

            for (const FGrassOutput& Output : Material->GrassOutputs)
            {
                const CGrassType* Type = Output.GrassType;
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
                Species.Mesh       = Type->Mesh;
                Species.LayerIndex = Output.LayerIndex;

                // Density is instances per square metre, so the candidate grid spacing is its inverse root.
                // Converted to world units here, once, rather than per thread on the GPU.
                Species.CellSize = Math::Max(0.01f, 100.0f / Math::Sqrt(Density));

                Species.MinWeight     = Type->MinWeight;
                Species.ScaleMin      = Type->ScaleMin;
                Species.ScaleMax      = Type->ScaleMax;
                Species.ZOffset       = Type->ZOffset;
                Species.AlignToNormal = Type->AlignToNormal;
                Species.MaxSlopeCos   = Math::Cos(Type->MaxSlopeDegrees * (3.14159265358979f / 180.0f));
                Species.CullDistance  = Type->CullDistance;
                Species.Seed          = Type->Seed;
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
