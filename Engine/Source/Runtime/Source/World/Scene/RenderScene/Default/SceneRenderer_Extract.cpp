#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace SceneRendererExtractInternal
    {
        // Row 1 of a view-projection holds the pixel scale at unit distance whatever the rotation, perspective or orthographic.
        float MakeLODErrorScale(const FMatrix4& ViewProjection, float HeightPixels, float ToleratedPixels)
        {
            if (HeightPixels <= 0.0f || ToleratedPixels <= 0.0f)
            {
                return 0.0f;
            }
            const FVector3 Row1 = FVector3(ViewProjection[0][1], ViewProjection[1][1], ViewProjection[2][1]);
            return 0.5f * HeightPixels * Math::Length(Row1) / ToleratedPixels;
        }

        float CameraLODPixels()
        {
            const CRendererSettings* Settings = GetDefault<CRendererSettings>();
            return 1.0f / Math::Clamp(Settings != nullptr ? Settings->LODDistanceScale : 1.0f, 0.25f, 16.0f);
        }

        float ShadowLODTexels()
        {
            const CRendererSettings* Settings = GetDefault<CRendererSettings>();
            return 1.0f / Math::Clamp(Settings != nullptr ? Settings->ShadowLODScale : 1.0f, 0.25f, 16.0f);
        }

        FCullView MakeCameraLODView(const FViewVolume& ViewVolume, float DisplayHeight)
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

        void AimGlobalsAtView(FSceneGlobalData& Data, const FSceneGlobalData& Primary, const FViewVolume& View, const FUIntVector2& Size)
        {
            Data = Primary;
            Data.CameraData.Location          = FVector4(View.GetViewPosition(), 1.0f);
            Data.CameraData.Up                = FVector4(View.GetUpVector(), 1.0f);
            Data.CameraData.Right             = FVector4(View.GetRightVector(), 1.0f);
            Data.CameraData.Forward           = FVector4(View.GetForwardVector(), 1.0f);
            Data.CameraData.View              = View.GetViewMatrix();
            Data.CameraData.InverseView       = View.GetInverseViewMatrix();
            Data.CameraData.Projection        = View.GetProjectionMatrix();
            Data.CameraData.InverseProjection = View.GetInverseProjectionMatrix();
            Data.ScreenSize                   = FUIntVector4(Size.x, Size.y, Size.x, Size.y);
            Data.TextureMipBias               = 0.0f;
            Data.GridSize                     = ComputeClusterGrid(Size);
            Data.FarPlane                     = View.GetFar();
            Data.NearPlane                    = View.GetNear();
            Data.CullData.Frustum             = AsGPU(View.GetFrustum());
            // No resolve runs here, so a varying phase would just boil the view's screen-space noise.
            Data.TemporalPhase                = 0u;
        }

        #if !defined(LE_SHIPPING)
        // Scripts and agents have no viewport menu, so this forces a debug view mode by its ERenderSceneDebugFlags value.
        TConsoleVar<int32> CVarDebugViewOverride("r.DebugViewOverride", -1,
            "Forces the scene debug view to this ERenderSceneDebugFlags value in every view; -1 leaves each viewport's own.");
        #endif
    }

    void FDefaultSceneRenderer::Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess, const FViewLayerSettings& Layers)
    {
        using namespace SceneRendererExtractInternal;

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
        Frame.ViewLayers           = Layers;

        PreparePostProcess(Frame, PostProcess);
        PreparePrimaryGlobals(Frame, ViewVolume, Layers);

        if (!IsDefaultMaterialReady())
        {
            ExtractFrame = nullptr;

            // Nothing rendered, so next frame's HZB is not this frame's depth.
            bDepthPyramidValid.store(false, std::memory_order_release);
            return;
        }

        ++ExtractSerial;

        ResetPass_Extract();

        ExtractReflectionProbes(World->GetEntityRegistry(), Frame);

        for (int32 i = 1; i < SceneViews.Num(); ++i)
        {
            if (!SceneViews[i].bEnabled)
            {
                continue;
            }
            FFrameData::FCaptureViewData& Capture = Frame.Views.CaptureViews.EmplaceGetRef();
            Capture.ViewVolume     = SceneViews[i].PendingViewVolume;
            Capture.SceneViewIndex = i;
        }

        // CPU half is the parallel ECS gather and cull setup.
        CompileDrawCommands_Extract();

        for (FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            AimGlobalsAtView(Capture.SceneGlobalData, Frame.SceneGlobalData, Capture.ViewVolume, SceneViews[Capture.SceneViewIndex].Size);
        }

        if (Frame.ReflectionProbes.BakingProbe >= 0)
        {
            const uint32 FaceSize = Frame.ReflectionProbes.BakeFaceSize;
            for (int32 Face = 0; Face < 6; ++Face)
            {
                AimGlobalsAtView(Frame.ReflectionProbes.FaceGlobals[Face], Frame.SceneGlobalData,
                                 Frame.ReflectionProbes.FaceVolumes[Face], FUIntVector2(FaceSize, FaceSize));
            }
        }

        Frame.Lighting.AtlasTiles = ShadowAtlas.GetAllocatedTiles();

        for (uint32 Channel = 0; Channel < FImmediateLineRenderer::NumChannels; ++Channel)
        {
            Frame.Primitives.ImmediateLines[Channel] = ImmediateLines.Snapshot((FImmediateLineRenderer::EChannel)Channel);
        }

        // After every task, since it also expires escaped pointers; sound because no world simulates while extract runs, so no write lands between the epoch reads and here.
        World->GetEntityRegistry().AdvanceChangeTick();

        Frame.bExtractedThisFrame = true;
        SceneViews[0].TemporalFrameIndex = SceneViews[0].PendingTemporalFrameIndex;
        SceneViews[0].PrevViewProjection = SceneViews[0].PendingViewProjection;

        ExtractFrame = nullptr;
    }

    void FDefaultSceneRenderer::PreparePostProcess(FFrameData& Frame, const SPostProcessSettings* PostProcess)
    {
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

        Frame.PostProcess.ActivePostProcessMaterials.Reset();
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
            FFrameData::FPostProcessMaterial& Out = Frame.PostProcess.ActivePostProcessMaterials.EmplaceGetRef();
            Out.Shaders.VertexShader = VS;
            Out.Shaders.PixelShader  = PS;
            Out.MaterialIndex        = (uint32)PPInterface->GetMaterialIndex();
        }
    }

    void FDefaultSceneRenderer::PreparePrimaryGlobals(FFrameData& Frame, const FViewVolume& ViewVolume, const FViewLayerSettings& Layers)
    {
        using namespace SceneRendererExtractInternal;

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
        const CRendererSettings* RenderSettings         = GetDefault<CRendererSettings>();
        const bool bSpecularAA                          = RenderSettings != nullptr && RenderSettings->bSpecularAntiAliasing;
        SceneGlobalData.SpecularAAVariance              = bSpecularAA ? Math::Clamp(RenderSettings->SpecularAntiAliasingStrength, 0.0f, 1.0f) : 0.0f;
        SceneGlobalData.SpecularAAThreshold             = bSpecularAA ? Math::Clamp(RenderSettings->SpecularAntiAliasingLimit, 0.0f, 1.0f) : 0.0f;
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
        SceneGlobalData.CullData.CameraLayerMask        = Layers.CullMask;

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
    }

    // A frame or two at startup is normal; a default material that never compiled never recovers.
    bool FDefaultSceneRenderer::IsDefaultMaterialReady()
    {
        CMaterial* FallbackMaterial = CMaterial::GetDefaultMaterial();
        if (!IsValid(FallbackMaterial) || !FallbackMaterial->IsReadyForRender())
        {
            if (!bWarnedFallbackMaterial)
            {
                bWarnedFallbackMaterial = true;
                LOG_WARN("Extract skipped: the default material is {}. Every frame is skipped until it is "
                         "ready, and the viewport shows its clear color.",
                         IsValid(FallbackMaterial) ? "still compiling" : "missing");
            }

            return false;
        }

        if (bWarnedFallbackMaterial)
        {
            bWarnedFallbackMaterial = false;
            LOG_DISPLAY("Default material is ready; extract resumed.");
        }
        return true;
    }

    FDefaultSceneRenderer::FThreadLocalDrawData& FDefaultSceneRenderer::AcquireThreadLocalDrawData(uint32 Slot)
    {
        // PrepareGatherScratch already reset every entry, so this only marks it for the merge.
        FThreadLocalDrawData& Local = ThreadLocalStorage[Slot];
        Local.bTouched = true;
        return Local;
    }

    void FDefaultSceneRenderer::CompileDrawCommands_Extract()
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Render Scene");

        FFrameData&     Frame    = *ExtractFrame;
        ECS::FRegistry& Registry = World->GetEntityRegistry();

        PrepareRetainedScene(Registry);
        LaunchExtracts(Registry, Frame);

        ExtractDebugText(Frame);

        {
            LUMINA_PROFILE_SECTION_COLORED("Wait Extracts", tracy::Color::Crimson);
            ExtractLatch.Wait();
        }

        FinalizeLighting(Registry, Frame);
    }

    void FDefaultSceneRenderer::PrepareRetainedScene(ECS::FRegistry& Registry)
    {
        ECS::Utils::ResolveAllDirtyTransforms(Registry);
        ResolveDynamicMeshMaterials(Registry, FRenderDirtyTracker::Ensure(Registry));
        ResolveDirtyMeshComponents();
        SyncScenePrimitives();
    }

    // Every view and storage the tasks read is built here on the main thread, since building one can create a pool.
    void FDefaultSceneRenderer::LaunchExtracts(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        ExtractLatch.Reset(1);
        LightsReady.Reset();

        ResetGeometry_Extract();
        PrepareGatherScratch(Frame);

        // Resolves the grass meshes, so it runs before any worker could be reading the resolve cache.
        ScheduleTerrainExtract(Registry, Frame);

        // Lights spawn first, since the skinned cull keeps casters near the shadow lights they find.
        ScheduleLightExtract(Registry);
        ScheduleSkinnedGather(Registry);
        ScheduleBillboardExtract(Registry, Frame);

        ScheduleLineBatching(Registry);

        #if USING(WITH_EDITOR)
        const FEditorGridRequest* Grid = Registry.Ctx().Find<FEditorGridRequest>();
        Frame.Extracts.Grid = Grid != nullptr ? *Grid : FEditorGridRequest{};
        #endif

        // Emitters, so none may outrank the mesh critical path.
        SchedulePrimitiveExtracts(Registry, Frame);
        ScheduleParticleExtract(Registry, Frame);
        ScheduleSplineExtract(Registry, Frame);
        ScheduleFogExtract(Registry, Frame);
        Jobs::Spawn(Jobs::EJobPriority::Normal, ExtractLatch, [this, &Frame]
        {
            LUMINA_PROFILE_SECTION("Extract Paint Ops");
            Frame.Extracts.PaintOps.Reset();
            World->DrainRenderTargetPaints(Frame.Extracts.PaintOps);
        });

        ExtractLatch.CountDown();
    }

    void FDefaultSceneRenderer::FinalizeLighting(ECS::FRegistry& Registry, FFrameData& Frame)
    {
        // After the waits, so the streamer walks CMaterial state with no gather in flight.
        PublishStreamingFeedback();

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

        // Takes a shadow slot for the rain occlusion view, so it runs before the counter is read back.
        ExtractWeather(Registry, Frame);
        ExtractWeatherInteraction(Registry, Frame);
        ApplyWeatherToClouds(Frame);

        // This is the last writer of the shadow counter, which can overshoot like the light count.
        NumLiveShadows = Math::Min(Frame.Lighting.ShadowDataCount.load(std::memory_order_acquire),
                                   (uint32)MAX_SHADOWS);
        LUMINA_PROFILE_VALUE("Shadows/LocalShadowed", NumLiveShadows);
        LUMINA_PROFILE_VALUE("Lights/Live", NumLiveLights);

        // The skylight reads the sun direction ProcessDirectionalLight wrote.
        const SEnvironmentComponent* ActiveEnv = ExtractEnvironment(Registry, Frame);
        ExtractSkyLight(Registry, Frame, ActiveEnv);

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

        BuildCullViews(Frame.ViewVolume);

        // Last extract write, and on this thread, because Extract snapshots ImmediateLines a few lines later.
        ApplyCullFreeze(Frame);
    }

    // Exact only for a change-tracked pool read through const access, and only because Extract advances the change tick after every read.
    bool FDefaultSceneRenderer::PoolMayHaveChanged(const ECS::FRegistry& Registry, ECS::FComponentTypeID TypeID, FPoolWatch& Watch)
    {
        const uint64 Epoch = Registry.GetWriteEpoch(TypeID);
        const bool bChanged = Epoch != Watch.Epoch;
        Watch.Epoch = Epoch;
        return bChanged;
    }

    // The live list only changes when the pool's dense entities do, so an unchanged pool costs one compare.
    void FDefaultSceneRenderer::RefreshLiveEntities(TSpan<const ECS::FEntity> DenseSlots, TVector<ECS::FEntity>& Live, TVector<ECS::FEntity>& DenseSnapshot)
    {
        const int32         DenseSize = DenseSlots.Num();
        const ECS::FEntity* Dense     = DenseSlots.GetData();
        if (DenseSnapshot.Num() == DenseSize &&
            (DenseSize == 0 || Memory::Memcmp(DenseSnapshot.GetData(), Dense, (size_t)DenseSize * sizeof(ECS::FEntity)) == 0))
        {
            return;
        }

        DenseSnapshot.Reset();
        DenseSnapshot.Append(Dense, DenseSize);

        Live.Reset();
        for (int32 Index = DenseSize; Index-- > 0;)
        {
            if (!Dense[Index].IsTombstone())
            {
                Live.Add(Dense[Index]);
            }
        }
    }

    void FDefaultSceneRenderer::PrepareGatherScratch(FFrameData& Frame)
    {
        auto& DrawCommands = Frame.Geometry.DrawCommands;

        // One command per pipeline batch, not per primitive; the merge emits exactly this many.
        DrawCommands.Reserve(ScenePrimitives.GetBatches().Num());

        // Every bound skinned index fits before the gather, so the workers write in place instead of deferring.
        TVector<FSkinnedFrameData>& SkinnedData = Frame.Geometry.SkinnedFrameData;
        if ((uint32)SkinnedData.Num() < ScenePrimitives.GetSkinnedIndexExtent())
        {
            SkinnedData.SetNum(ScenePrimitives.GetSkinnedIndexExtent());
        }

        const uint32 NumThreads = Jobs::GetNumThreadSlots();

        TVector<FThreadLocalDrawData>& ThreadLocal = ThreadLocalStorage;
        if (static_cast<uint32>(ThreadLocal.Num()) < NumThreads)
        {
            ThreadLocal.Reserve(NumThreads);
            while (static_cast<uint32>(ThreadLocal.Num()) < NumThreads)
            {
                ThreadLocal.Emplace();
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

    void FDefaultSceneRenderer::ScheduleLineBatching(ECS::FRegistry& Registry)
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
            Jobs::Spawn(Jobs::EJobPriority::Normal, ExtractLatch, [this, LineBatcher, LineChunkCount]() -> Jobs::TTask<void>
            {
                co_await Jobs::Call(Jobs::ForEach(LineChunkCount, [this](const Jobs::FParallelRange& Range)
                {
                    BatchLineChunks(Range);
                }, 1));
                FinalizeBatchedLines(*LineBatcher);
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
            FrozenCull.CustomDepthCullView = Frame.Views.CustomDepthCullView;
            FrozenCull.bValid            = true;
            return;
        }

        // The view COUNT freezes too, since bucket indices and the dispatch grid all derive from it.
        Frame.Views.CullViews       = FrozenCull.Views;
        Frame.Views.CascadeViewBase = FrozenCull.CascadeViewBase;
        Frame.Views.NumCascadeViews = FrozenCull.NumCascadeViews;
        Frame.Views.NumNearCascadeViews = FrozenCull.NumNearCascadeViews;
        Frame.Views.CustomDepthCullView = FrozenCull.CustomDepthCullView;

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
        if (Slot == Constants::kIndexNone || Slot >= MAX_SHADOWS)
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
        if (!FrozenCull.bHasCascadeShadow || Slot == Constants::kIndexNone || Slot >= MAX_SHADOWS)
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
            {0,1},{2,3},{0,2},{1,3},
            {4,5},{6,7},{4,6},{5,7},
            {0,4},{1,5},{2,6},{3,7},
        };

        const FVector4 Color(1.0f, 0.55f, 0.1f, 1.0f);
        for (const auto& E : kEdges)
        {
            ImmediateLines.Line(Corner[E[0]], Corner[E[1]], Color, FImmediateLineRenderer::XRay);
        }
    }

    void FDefaultSceneRenderer::BuildCullViews(const FViewVolume& ViewVolume)
    {
        using namespace SceneRendererExtractInternal;

        FFrameData& Frame = *ExtractFrame;
        auto& CullViews                = Frame.Views.CullViews;
        auto& LightData                = Frame.Lighting.LightData;
        auto& PackedShadows            = Frame.Lighting.PackedShadows;
        auto& PointShadowCullViewBases = Frame.Views.PointShadowCullViewBases;
        auto& SpotShadowCullViewBases  = Frame.Views.SpotShadowCullViewBases;
        uint32& CascadeViewBase        = Frame.Views.CascadeViewBase;
        Frame.Views.NumCascadeViews    = 0;
        Frame.Views.NumNearCascadeViews = 0;
        Frame.Views.CustomDepthCullView = Constants::kIndexNoneU32;
        const bool bCustomDepthView    = HasCustomDepthPrimitives();

        const float CameraTolerance   = CameraLODPixels();
        const float ShadowTolerance   = ShadowLODTexels();
        const float ShadowAtlasTexels = (float)ShadowAtlas.GetConfig().AtlasResolution;

        // The camera and its shadows draw what the camera draws, and the other views skip a remapped Foreground layer, which only lines up from the camera.
        const FViewLayerSettings& Layers = Frame.ViewLayers;
        const uint32 OtherViewLayers = Layers.IsForegroundActive() ? ~RenderLayers::kForegroundMask : RenderLayers::kAllMask;
        uint32 PushLayers = Layers.CullMask;

        auto PushView = [&](const FMatrix4& ViewProjection, const FVector3& Origin, uint32 Flags, float LODErrorScale,
                            uint32 CascadeIndex = Constants::kIndexNoneU32, float MinBoundsDiameter = 0.0f)
        {
            const uint32 ViewIndex = (uint32)CullViews.Num();
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
            View.LayerMask          = PushLayers;
            CullViews.Add(View);

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
            1u +
            (LightData.bHasSun ? (uint32)ActiveCascadeCount : 0u) +
            NumPointFaceViews +
            (uint32)PackedShadows[(uint32)ELightType::Spot].Num() +
            (uint32)Frame.Views.CaptureViews.Num() +
            (Frame.ReflectionProbes.BakingProbe >= 0 ? 6u : 0u) +
            (Frame.Weather.OcclusionShadowDataIndex != Constants::kIndexNone ? 1u : 0u) +
            (bCustomDepthView ? 1u : 0u);

        ASSERT(NumViews <= (uint32)GMaxCullViews);
        LUMINA_PROFILE_VALUE("Shadows/PointFaceViews", NumPointFaceViews);
        LUMINA_PROFILE_VALUE("Cull/Views", NumViews);

        CullViews.Reserve(NumViews);

        CascadeViewBase = Constants::kIndexNoneU32;
        PointShadowCullViewBases.Reset();
        PointShadowCullViewBases.Reserve(PackedShadows[(uint32)ELightType::Point].Num());
        SpotShadowCullViewBases.Reset();
        SpotShadowCullViewBases.Reserve(PackedShadows[(uint32)ELightType::Spot].Num());

        const uint32 ConeFlag = FrameSettings.bConeCull ? (uint32)ECullViewFlags::Cone : 0u;
        const FMatrix4 CameraVP = ViewVolume.GetProjectionMatrix() * ViewVolume.GetViewMatrix();
        uint32 CameraFlags = ConeFlag;

        {
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

                CascadeViewBase = (uint32)CullViews.Num();
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
                PointShadowCullViewBases.Add(Constants::kIndexNoneU32);
                continue;
            }

            const FLightShadowData& ShadowData = Frame.Lighting.Shadows[PointShadow.ShadowDataIndex];
            const FLight& Light = Frame.Lighting.Lights[PointShadow.LightIndex];
            const uint32 FaceFlags =
                (FrameSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
                ConeFlag |
                ECullViewFlags::CastShadowOnly;

            // Only faces holding a tile get a view, in face order, which is how the shadow pass walks them.
            PointShadowCullViewBases.Add((uint32)CullViews.Num());
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

        for (const FLightShadow& SpotShadow : PackedShadows[(uint32)ELightType::Spot])
        {
            if (SpotShadow.ShadowDataIndex < 0)
            {
                SpotShadowCullViewBases.Add(Constants::kIndexNoneU32);
                continue;
            }

            const FLightShadowData& ShadowData = Frame.Lighting.Shadows[SpotShadow.ShadowDataIndex];
            const FLight& Light = Frame.Lighting.Lights[SpotShadow.LightIndex];
            const uint32 SpotFlags =
                (FrameSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
                ConeFlag |
                ECullViewFlags::CastShadowOnly;

            SpotShadowCullViewBases.Add((uint32)CullViews.Num());
            const float SpotTexels = ShadowData.Shadow[0].AtlasUVScale.y * ShadowAtlasTexels;
            PushView(ShadowData.ViewProjection[0], Light.Position, SpotFlags,
                     MakeLODErrorScale(ShadowData.ViewProjection[0], SpotTexels, ShadowTolerance));
        }

        PushLayers = OtherViewLayers;
        for (FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            const FMatrix4 CaptureVP = Capture.ViewVolume.GetProjectionMatrix() * Capture.ViewVolume.GetViewMatrix();
            const uint32 CaptureFlags = ECullViewFlags::Frustum | ConeFlag
                                      | (Capture.ViewVolume.IsOrthographic() ? (uint32)ECullViewFlags::OrthographicLOD : 0u);
            // The capture's globals are aimed after this runs, so its size comes from the view itself.
            const float  CaptureHeight = (float)SceneViews[Capture.SceneViewIndex].Size.y;
            Capture.CameraViewIndex = PushView(CaptureVP, Capture.ViewVolume.GetViewPosition(), CaptureFlags,
                                               MakeLODErrorScale(CaptureVP, CaptureHeight, CameraTolerance));
        }

        if (Frame.Weather.OcclusionShadowDataIndex != Constants::kIndexNone)
        {
            // No cone test, since its sphere form assumes a perspective view origin.
            const uint32 RainFlags = ECullViewFlags::Frustum | ECullViewFlags::CastShadowOnly | ECullViewFlags::OrthographicLOD | ECullViewFlags::NoFoliage;
            const FMatrix4& RainVP = Frame.Weather.OcclusionViewProjection;
            Frame.Weather.OcclusionCullView = PushView(RainVP, ViewVolume.GetViewPosition(), RainFlags,
                MakeLODErrorScale(RainVP, (float)Frame.Weather.OcclusionResolution, ShadowTolerance));
        }

        // The camera's own rejects minus the Hi-Z, since custom depth has to hold what the scene hides.
        if (bCustomDepthView)
        {
            PushLayers = Layers.CullMask;
            const uint32 CustomDepthFlags = (CameraFlags & ~(uint32)ECullViewFlags::MeshletHiZ) | ECullViewFlags::CustomDepthOnly;
            Frame.Views.CustomDepthCullView = PushView(CameraVP, ViewVolume.GetViewPosition(), CustomDepthFlags,
                                                       Frame.Views.CameraLODView.LODErrorScale);
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

    uint32 FDefaultSceneRenderer::PrepareBatchedLines(FLineBatcherComponent& Batcher)
    {
        using FLineInstance = FLineBatcherComponent::FLineInstance;
        constexpr uint32 kMaxBuckets = FLineBatchScratch::kMaxBuckets;
        constexpr uint32 kChunkLines = 4096;

        LineChunkScratch.Reset();
        uint32 LineCount = 0;
        auto AddSource = [&](const FLineInstance* Data, uint32 Num)
        {
            for (uint32 Off = 0; Off < Num; Off += kChunkLines)
            {
                LineChunkScratch.Emplace(Data + Off, Math::Min(kChunkLines, Num - Off));
            }
            LineCount += Num;
        };

        if (!Batcher.Lines.IsEmpty())
        {
            AddSource(Batcher.Lines.GetData(), (uint32)Batcher.Lines.Num());
        }
        for (TVector<FLineInstance>& Buffer : Batcher.ThreadBuffers)
        {
            if (!Buffer.IsEmpty())
            {
                AddSource(Buffer.GetData(), (uint32)Buffer.Num());
            }
        }

        if (LineCount == 0)
        {
            return 0;
        }

        const uint32 NumThreads = Jobs::GetNumThreadSlots();
        if (static_cast<uint32>(LineBatchScratch.Num()) < NumThreads)
        {
            LineBatchScratch.SetNum(NumThreads);
        }
        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FLineBatchScratch& S = LineBatchScratch[t];
            S.NumBuckets = 0;
            S.Survivors.Reset();
            for (uint32 b = 0; b < kMaxBuckets; ++b)
            {
                S.BucketVerts[b].Reset();
            }
        }

        return (uint32)LineChunkScratch.Num();
    }

    void FDefaultSceneRenderer::BatchLineChunks(const Jobs::FParallelRange& Range)
    {
        LUMINA_PROFILE_SECTION("Batch Lines");

        using FLineInstance = FLineBatcherComponent::FLineInstance;
        constexpr uint32 kMaxBuckets = FLineBatchScratch::kMaxBuckets;

        const float     Dt      = ExtractFrame->SceneGlobalData.DeltaTime;
        const FFrustum& Frustum = ExtractFrame->CameraFrustum;
        FLineBatchScratch&     S      = LineBatchScratch[Range.Thread];
        const FLineChunk* const Chunks = LineChunkScratch.GetData();

        for (uint32 c = Range.Start; c < Range.End; ++c)
        {
            const FLineChunk& Chunk = Chunks[c];
            for (uint32 i = 0; i < Chunk.Count; ++i)
            {
                FLineInstance Line = Chunk.Data[i];

                const SAABB LineBounds(Math::Min(Line.Start, Line.End), Math::Max(Line.Start, Line.End));
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
                    V.Emplace(Line.Start, Line.ColorPacked);
                    V.Emplace(Line.End,   Line.ColorPacked);
                }

                if (Line.bSingleFrame)
                {
                    continue;
                }

                Line.RemainingLifetime -= Dt;
                if (Line.RemainingLifetime > 0.0f)
                {
                    S.Survivors.Add(Line);
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

        const uint32 NumThreads = Jobs::GetNumThreadSlots();

        struct FGlobalBucket
        {
            float   Thickness;
            uint8   bDepthTest;
            uint32  VertexCount;
            uint32  StartVertex;
        };
        TInlineVector<FGlobalBucket, kMaxBuckets> Global;

        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FLineBatchScratch& S = LineBatchScratch[t];
            for (uint32 b = 0; b < S.NumBuckets; ++b)
            {
                const uint32 VC = (uint32)S.BucketVerts[b].Num();
                if (VC == 0)
                {
                    S.GlobalBucket[b] = Constants::kIndexNoneU32;
                    continue;
                }

                uint32 G = Constants::kIndexNoneU32;
                for (uint32 k = 0, n = (uint32)Global.Num(); k < n; ++k)
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
                    G = (static_cast<uint32>(Global.Num()) < kMaxBuckets) ? (uint32)Global.Num() : (kMaxBuckets - 1);
                    if (G == (uint32)Global.Num())
                    {
                        Global.Emplace(FGlobalBucket{ S.BucketThickness[b], S.BucketDepthTest[b], 0u, 0u });
                    }
                }
                Global[G].VertexCount += VC;
                S.GlobalBucket[b] = G;
            }
        }

        // Prefix sum to give each global bucket a contiguous range in SimpleVertices.
        const uint32 BaseVertex = (uint32)SimpleVertices.Num();
        uint32 Cursor = BaseVertex;
        for (FGlobalBucket& B : Global)
        {
            B.StartVertex = Cursor;
            Cursor += B.VertexCount;
        }
        SimpleVertices.SetNum(Cursor);

        const bool bParallel = (Cursor - BaseVertex) > 4096;

        // Hand each (worker, bucket) a disjoint sub-range within its global bucket so the copy is race-free.
        TInlineVector<uint32, kMaxBuckets> GlobalWrite;
        GlobalWrite.SetNum(Global.Num());
        for (uint32 k = 0, n = (uint32)Global.Num(); k < n; ++k)
        {
            GlobalWrite[k] = Global[k].StartVertex;
        }
        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FLineBatchScratch& S = LineBatchScratch[t];
            for (uint32 b = 0; b < S.NumBuckets; ++b)
            {
                const uint32 VC = (uint32)S.BucketVerts[b].Num();
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
        FSimpleElementVertex* const Dst = SimpleVertices.GetData();
        auto CopyBody = [&](const Jobs::FParallelRange& Range)
        {
            LUMINA_PROFILE_SECTION("Copy Batched Lines");
            for (uint32 t = Range.Start; t < Range.End; ++t)
            {
                FLineBatchScratch& S = LineBatchScratch[t];
                for (uint32 b = 0; b < S.NumBuckets; ++b)
                {
                    const TVector<FSimpleElementVertex>& V = S.BucketVerts[b];
                    if (V.IsEmpty())
                    {
                        continue;
                    }
                    std::memcpy(Dst + S.WriteCursor[b], V.GetData(), V.Num() * sizeof(FSimpleElementVertex));
                }
            }
        };
        if (bParallel) { Jobs::ParallelFor(NumThreads, CopyBody, 1); }
        else           { CopyBody(Jobs::FParallelRange{ 0u, NumThreads, 0u }); }

        LineBatches.Reserve(LineBatches.Num() + Global.Num());
        for (const FGlobalBucket& B : Global)
        {
            LineBatches.Emplace(B.StartVertex, B.VertexCount, B.Thickness, (bool)B.bDepthTest);
        }

        // Rebuild the persistent line list from surviving (non-single-frame) lines; order is irrelevant.
        uint32 SurvivorTotal = 0;
        for (uint32 t = 0; t < NumThreads; ++t)
        {
            SurvivorTotal += (uint32)LineBatchScratch[t].Survivors.Num();
        }
        if (SurvivorTotal == 0)
        {
            Lines.Reset();
        }
        else
        {
            LineCompactScratch.Reset();
            LineCompactScratch.Reserve(SurvivorTotal);
            for (uint32 t = 0; t < NumThreads; ++t)
            {
                const TVector<FLineInstance>& Sv = LineBatchScratch[t].Survivors;
                LineCompactScratch.Append(Sv);
            }
            swap(Lines, LineCompactScratch);
        }

        // Reset the per-worker produce buffers for next frame (capacity retained, so no per-frame realloc).
        for (TVector<FLineInstance>& Buffer : ThreadBuffers)
        {
            Buffer.Reset();
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
        if (Batches.IsEmpty())
        {
            return;
        }

        const float Dt = SceneGlobalData.DeltaTime;
        int32 WriteIdx = 0;
        for (int32 i = 0, N = Batches.Num(); i < N; ++i)
        {
            FTriangleBatcherComponent::FBatchInstance& Batch = Batches[i];
            if (!Batch.Vertices.IsEmpty())
            {
                const uint32 Start = (uint32)SolidVertices.Num();
                SolidVertices.Append(Batch.Vertices);
                SolidBatches.Emplace(Start, (uint32)Batch.Vertices.Num(), Batch.Mode);
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
        Batches.SetNum(WriteIdx);
    }

    void FDefaultSceneRenderer::DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale)
    {
        if (ResourceID < 0 || ExtractFrame == nullptr)
        {
            return;
        }

        FBillboardInstance& Billboard   = ExtractFrame->Primitives.BillboardInstances.EmplaceGetRef();
        Billboard.TextureIndex          = (uint32)ResourceID;
        Billboard.Position              = Location;
        Billboard.Size                  = Scale;
        Billboard.EntityID              = ECS::NullEntity.Value;
    }

    void FDefaultSceneRenderer::ResetPass_Extract()
    {
        FFrameData& Frame = *ExtractFrame;

        Frame.Primitives.SimpleVertices.Reset();
        Frame.Primitives.LineBatches.Reset();
        for (FImmediateLineRenderer::FDrawRange& Range : Frame.Primitives.ImmediateLines)
        {
            Range = {};
        }
        Frame.Primitives.SolidVertices.Reset();
        Frame.Primitives.SolidBatches.Reset();
        Frame.Views.CullViews.Reset();
        Frame.Views.CaptureViews.Reset();
        Memory::Memzero(&Frame.Lighting.LightData, sizeof(Frame.Lighting.LightData));
        Frame.Lighting.ShadowDataCount.store(0, std::memory_order_release);
        ShadowAtlas.FreeTiles();
        Frame.Lighting.ShadowRequests.Reset();
        Frame.Lighting.AtlasTiles.Reset();
        Frame.Lighting.LightFunctionRequests.Reset();
        Frame.Lighting.LightFunctionDraws.Reset();
        Frame.Primitives.BillboardInstances.Reset();
        Frame.Primitives.WidgetInstances.Reset();
        Frame.Primitives.GlyphInstances.Reset();
        Frame.Primitives.SpriteInstances.Reset();
        Frame.Primitives.SpriteBatches.Reset();
        Frame.Primitives.TextBatches.Reset();
        Frame.FrameStats = {};

        for (int i = 0; i < (int)ELightType::Num; ++i)
        {
            Frame.Lighting.PackedShadows[i].Reset();
        }
    }

    // Split out of ResetPass so it can be ordered against SyncScenePrimitives.
    void FDefaultSceneRenderer::ResetGeometry_Extract()
    {
        LUMINA_PROFILE_SCOPE();

        FFrameData& Frame = *ExtractFrame;

        Frame.Geometry.DrawCommands.Reset();
        Frame.Geometry.OpaqueDrawList.Reset();
        Frame.Geometry.TranslucentDrawList.Reset();
        // BonesData keeps its capacity; the layout pass resizes it to the live arena extent.
        Frame.Geometry.BoneCount = 0;
        Frame.Geometry.BoneUploadRanges.Reset();
        Frame.Views.NumDrawsPerView   = 0;
    }
}
