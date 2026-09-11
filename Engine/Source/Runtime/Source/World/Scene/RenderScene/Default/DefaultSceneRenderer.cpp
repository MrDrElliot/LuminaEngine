#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    #if !defined(LE_SHIPPING)
    // Development only. Off is for measuring; on is what proves no pass reads memory nothing wrote.
    static TConsoleVar CVarPoisonUninitializedBuffers("r.Buffers.PoisonUninitialized", true,
        "Fill a grown scene buffer with a poison pattern so a read of unwritten memory shows as garbage.");
    static constexpr uint32 kUninitializedBufferPoison = 0xDEADBEEFu;
    #endif

    FDefaultSceneRenderer::FDefaultSceneRenderer(CWorld* InWorld)
        : IRenderScene(InWorld)
        , ShadowAtlas(FShadowAtlasConfig())
    {
    }

        void FDefaultSceneRenderer::Init()
    {
        LUMINA_MEMORY_SCOPE("Render Scene");

        RHI::WaitDeviceIdle();

        // Shared (view-independent) buffers + images first.
        InitBuffers();

        InitSharedResources();

        AppliedIBLResolution = FIBLBakeResolution{};
        InitSkyCube(AppliedIBLResolution.SkyCube);
        InitIBLConvolutionTargets(AppliedIBLResolution);

        ShadowAtlas.InitImage();

        {
            RHI::FTextureDesc Desc;
            Desc.Type      = RHI::ETextureType::Tex2D;
            Desc.Dimension = FUIntVector3(GCSMAtlasWidth, GCSMAtlasHeight, 1);
            Desc.Format    = EFormat::D32;
            Desc.Usage     = RHI::EImageUsageFlags::DepthAttachment | RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;
            NamedImages[(int)ENamedImage::Cascade] = CreateSceneImage(Desc);
        }

        {
            const uint32 PyramidW = GCSMAtlasWidth  / 2u;
            const uint32 PyramidH = GCSMAtlasHeight / 2u;

            RHI::FTextureDesc Desc;
            Desc.Type      = RHI::ETextureType::Tex2D;
            Desc.Dimension = FUIntVector3(PyramidW, PyramidH, 1);
            Desc.Format    = EFormat::R32_FLOAT;
            Desc.MipCount  = RenderUtils::CalculateMipCount(PyramidW, PyramidH);
            Desc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            NamedImages[(int)ENamedImage::CascadePyramid] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);
        }

        SceneViews.reserve(MaxSceneViews);

        // Primary view (index 0) tracks the swapchain size.
        AddSceneView(Windowing::GetPrimaryWindowHandle()->GetExtent(), /*bPrimary*/ true);

        SwapchainResizedHandle = FRenderManager::OnSwapchainResized.AddMember(this, &FDefaultSceneRenderer::SwapchainResized);

        if (World != nullptr)
        {
            ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);

            ECS::Utils::SetPublishMovedTransforms(Registry, true);

            ScenePrimitives.Reset(&Registry);
        }
    }

    FDefaultSceneRenderer::FSceneView& FDefaultSceneRenderer::AddSceneView(const FUIntVector2& Size, bool bPrimary)
    {
        SceneViews.emplace_back();
        FSceneView& View = SceneViews.back();
        View.bIsPrimary = bPrimary;
        View.Size       = Math::Max(Size, FUIntVector2(1));

        // Per-view clustered-lighting grid (built from this view's projection).
        View.ClusterBuffer = CreateSceneBuffer(sizeof(FCluster) * NumClusters, "View.ClusterGrid");
        View.bClusterGridDirty = true;   // fresh buffer has undefined contents.

        InitViewImages(View);

        return View;
    }

    int32 FDefaultSceneRenderer::RegisterCaptureView(const FUIntVector2& Size)
    {
        const FUIntVector2 ClampedSize = Math::Max(Size, FUIntVector2(1));
        for (int32 i = 1; i < (int32)SceneViews.size(); ++i)
        {
            if (!SceneViews[i].bEnabled && !SceneViews[i].bReservedForProbeBake && SceneViews[i].Size == ClampedSize)
            {
                return i;
            }
        }

        if (SceneViews.size() >= MaxSceneViews)
        {
            return -1;
        }

        const int32 Handle = (int32)SceneViews.size();
        AddSceneView(ClampedSize, /*bPrimary*/ false);
        return Handle;
    }

    bool FDefaultSceneRenderer::SetCaptureView(int32 Handle, const FViewVolume& View, bool bEnabled)
    {
        if (Handle <= 0 || Handle >= (int32)SceneViews.size())
        {
            return false;
        }
        SceneViews[Handle].PendingViewVolume = View;
        SceneViews[Handle].bEnabled          = bEnabled;
        return true;
    }

    int32 FDefaultSceneRenderer::GetCaptureDisplayResourceID(int32 Handle) const
    {
        if (Handle <= 0 || Handle >= (int32)SceneViews.size())
        {
            return -1;
        }
        return SceneViews[Handle].Output.GetResourceID();
    }

    void FDefaultSceneRenderer::InitSharedResources()
    {
        FSharedRenderResources& Shared = Render().GetSharedRenderResources();

        if (!Shared.bInitialized)
        {
            BakeBRDFLUT();

            Shared.SMAAArea = RHI::Textures::Create(RHI::FTexture2DDesc{ .Width = AREATEX_WIDTH, .Height = AREATEX_HEIGHT, .Format = EFormat::RG8_UNORM,
                                                                        .DebugName = "Shared.SMAAArea" });
            RHI::Textures::Upload(Shared.SMAAArea, 0, areaTexBytes, AREATEX_SIZE, AREATEX_WIDTH);

            Shared.SMAASearch = RHI::Textures::Create(RHI::FTexture2DDesc{ .Width = SEARCHTEX_WIDTH, .Height = SEARCHTEX_HEIGHT, .Format = EFormat::R8_UNORM,
                                                                          .DebugName = "Shared.SMAASearch" });
            RHI::Textures::Upload(Shared.SMAASearch, 0, searchTexBytes, SEARCHTEX_SIZE, SEARCHTEX_WIDTH);

            #if USING(WITH_EDITOR)
            const FString Dir = Paths::GetEngineResourceDirectory();
            const char* IconFiles[FSharedRenderResources::kEditorIconCount] =
            {
                "/Textures/PointLight.png", "/Textures/DirectionalLight.png", "/Textures/SkyLight.png",
                "/Textures/SpotLight.png", "/Textures/CameraIcon.png", "/Textures/PersonIcon.png",
                "/Textures/Molecule.png", "/Textures/AudioSource.png", "/Textures/AudioListener.png"
            };
            for (int i = 0; i < FSharedRenderResources::kEditorIconCount; ++i)
            {
                if (auto Imported = Import::Textures::ImportTexture(Dir + IconFiles[i], false))
                {
                    const FString IconName = FString("Shared.EditorIcon") + IconFiles[i];
                    Shared.EditorIcons[i] = RHI::Textures::Create(RHI::FTexture2DDesc
                    {
                        .Width  = Imported->Dimensions.x,
                        .Height = Imported->Dimensions.y,
                        .Format = Imported->Format,
                        .DebugName = IconName.c_str()
                    });
                    RHI::Textures::Upload(Shared.EditorIcons[i], 0, Imported->Pixels.data(), Imported->Pixels.size(), Imported->Dimensions.x);
                }
                else
                {
                    LOG_WARN("Editor icon '{}' failed to import; its billboard draws blank.", IconFiles[i]);
                }
            }
            #endif

            Shared.bInitialized = true;
        }

        auto Alias = [](const RHI::FManagedTexture& Managed, EFormat Format, const FUIntVector2& Extent)
        {
            FSceneImage Image;
            Image.Texture        = Managed.Texture;
            Image.SampledSlot    = Managed.SampledSlot;
            Image.Desc.Type      = RHI::ETextureType::Tex2D;
            Image.Desc.Dimension = FUIntVector3(Extent.x, Extent.y, 1);
            Image.Desc.Format    = Format;
            return Image;
        };

        FSharedRenderResources& SharedNow = Render().GetSharedRenderResources();
        NamedImages[(int)ENamedImage::BRDFLut]    = Alias(SharedNow.BRDFLut, EFormat::RG16_FLOAT, FUIntVector2(256, 256));
        NamedImages[(int)ENamedImage::BRDFLut].MipUAVSlots.push_back(SharedNow.BRDFLutUAV);
        NamedImages[(int)ENamedImage::SMAAArea]   = Alias(SharedNow.SMAAArea, EFormat::RG8_UNORM, FUIntVector2(AREATEX_WIDTH, AREATEX_HEIGHT));
        NamedImages[(int)ENamedImage::SMAASearch] = Alias(SharedNow.SMAASearch, EFormat::R8_UNORM, FUIntVector2(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT));

        #if USING(WITH_EDITOR)
        const ENamedImage IconSlots[FSharedRenderResources::kEditorIconCount] =
        {
            ENamedImage::PointLightIcon, ENamedImage::DirectionalLightIcon, ENamedImage::SkyLightIcon,
            ENamedImage::SpotLightIcon, ENamedImage::CameraIcon, ENamedImage::CharacterIcon,
            ENamedImage::ParticleSystemIcon, ENamedImage::AudioSourceIcon, ENamedImage::AudioListenerIcon
        };
        for (int i = 0; i < FSharedRenderResources::kEditorIconCount; ++i)
        {
            NamedImages[(int)IconSlots[i]] = Alias(SharedNow.EditorIcons[i], EFormat::RGBA8_UNORM, FUIntVector2(1, 1));
        }
        #endif

        NameOwnedImages(NamedImages);
    }

    FDefaultSceneRenderer::~FDefaultSceneRenderer()
    {
        RHI::WaitDeviceIdle();

        FRenderManager::OnSwapchainResized.Remove(SwapchainResizedHandle);

        if (World != nullptr)
        {
            ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
            ECS::Utils::SetPublishMovedTransforms(Registry, false);
            MovedTransformScratch.clear();
            ECS::Utils::DrainMovedTransforms(Registry, MovedTransformScratch);
            MovedTransformScratch.clear();

            ScenePrimitives.Reset(&Registry);
        }

        // Per-view images + cluster buffers.
        for (FSceneView& View : SceneViews)
        {
            ReleaseViewImages(View);
            if (View.ClusterBuffer)
            {
                RHI::Retire(View.ClusterBuffer);
                View.ClusterBuffer = {};
            }
        }
        SceneViews.clear();
        CurrentView = nullptr;

        for (FSceneImage& Image : NamedImages)
        {
            if (Image.bOwned)
            {
                RetireSceneImage(Image);
            }
        }

        // Persistent GPU buffers + rings.
        auto FreeBuffer = [](RHI::FGPUAllocation& Buffer)
        {
            if (Buffer)
            {
                RHI::Retire(Buffer);
                Buffer = {};
            }
        };
        FreeBuffer(PreSkinnedVerticesBuffer);

        FreeBuffer(RetainedCullEntryBuffer);
        FreeBuffer(RetainedTransformBuffer);
        FreeBuffer(RetainedStaticBuffer);
        FreeBuffer(SkinnedMeshletBoundsBuffer);
        FreeBuffer(SkinnedMeshletConeBuffer);
        FreeBuffer(SurfaceDescBuffer);
        FreeBuffer(BoneArenaBuffer);
        FreeBuffer(SkinnedFrameDataBuffer);
        FreeBuffer(SkinnedSlotListBuffer);
        FreeBuffer(InstanceVisibilityBuffers[0]);
        FreeBuffer(InstanceVisibilityBuffers[1]);

        for (auto& [Entity, States] : GrassGPUStates)
        {
            for (FGrassGPUState& State : States)
            {
                FreeBuffer(State.PrevCursorBuffer);
            }
        }

        for (uint32 Slot = 0; Slot < RHI::kFramesInFlight; ++Slot)
        {
            FreeBuffer(RenderBucketRing[Slot]);
            FreeBuffer(MeshletDrawListRing[Slot]);
            FreeBuffer(MeshDrawArgsRing[Slot]);
            FreeBuffer(FrameScratchRing[Slot]);
            FreeBuffer(MeshletBlockRing[Slot]);
            FreeBuffer(BlockDispatchArgsRing[Slot]);
            FreeBuffer(MeshletCullDispatchArgsRing[Slot]);
            FreeBuffer(SkinDispatchArgsRing[Slot]);
            FreeBuffer(SkinWorkBaseRing[Slot]);
            FreeBuffer(InstanceViewRangeRing[Slot]);
            FreeBuffer(MaterialClassifyRing[Slot]);
            FreeBuffer(MaterialPixelListRing[Slot]);

            // GPU-driven scene per-frame outputs.
            FreeBuffer(VisibleInstanceRing[Slot]);
            FreeBuffer(TotalsRing[Slot]);

            // Raw GPUPtr rather than an RHI::FGPUAllocation (CPURead allocation, persistently mapped).
            if (MeshletBoundReadback[Slot].Gpu != 0)
            {
                RHI::Retire(MeshletBoundReadback[Slot]);
                MeshletBoundReadback[Slot] = {};
            }
        }

        // Render-phase-owned terrain / particle GPU state.
        for (auto& [Entity, State] : TerrainGPUStates)
        {
            RetireSceneImage(State.HeightmapTexture);
            RetireSceneImage(State.NormalTexture);
            RetireSceneImage(State.LayerWeightTexture);
            FreeBuffer(State.ChunkInfoBuffer);
            FreeBuffer(State.MeshletInfoBuffer);
            FreeBuffer(State.VisibleMeshletBuffer);
            FreeBuffer(State.IndirectDrawBuffer);
        }
        TerrainGPUStates.clear();

        for (auto& [Entity, States] : ParticleGPUStates)
        {
            for (FParticleGPUState& State : States)
            {
                if (State.ParticleBuffer)     { RHI::Retire(State.ParticleBuffer); }
                if (State.SpawnCounterBuffer) { RHI::Retire(State.SpawnCounterBuffer); }
                if (State.AttributeBuffer)    { RHI::Retire(State.AttributeBuffer); }
                if (State.SortIndexBuffer)    { RHI::Retire(State.SortIndexBuffer); }
                if (State.SortDrawArgsBuffer) { RHI::Retire(State.SortDrawArgsBuffer); }
            }
        }
        ParticleGPUStates.clear();

        #if USING(WITH_EDITOR)
        for (FPickerReadbackSlot& Slot : PickerReadbackRing)
        {
            if (Slot.Readback.Gpu != 0)
            {
                RHI::Retire(Slot.Readback);
                Slot.Readback = {};
            }
        }
        #endif

        // Pipeline + depth-state caches.
        for (auto& [Hash, Pipeline] : PipelineCache)
        {
            RHI::Retire(Pipeline);
        }
        PipelineCache.clear();
    }

    void FDefaultSceneRenderer::PrepareRender(uint8 /*FrameIndex*/)
    {
        LUMINA_PROFILE_SCOPE();

        FFrameData& Frame = FrameData;
        if (!Frame.bExtractedThisFrame)
        {
            return;
        }

        SyncIBLResolution(Frame.Volumetrics.IBLResolution);

        // Same reason, since the scratch cube may need resizing for this frame's bake.
        if (Frame.ReflectionProbes.BakingProbe >= 0)
        {
            SyncProbeCaptureCube(Frame.ReflectionProbes.BakeFaceSize);
        }
    }

    void FDefaultSceneRenderer::RenderView(uint8 FrameIndex)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Render Scene");

        const uint8 Slot = (uint8)(FrameIndex % RHI::kFramesInFlight);
        RenderFrame = &FrameData;
        FFrameData& Frame = FrameData;

        if (!Frame.bExtractedThisFrame)
        {
            // Nothing has composited yet, so the viewport is showing undefined Output contents.
            if (FramesComposited == 0 && !bWarnedNoComposite)
            {
                bWarnedNoComposite = true;
                LOG_WARN("RenderView skipped before any frame composited (no extract this frame); ticking={}, "
                         "suspended={}. The viewport reads its cleared Output until a frame lands.",
                         World != nullptr && World->IsTickingThisFrame(),
                         World != nullptr && World->IsSuspended());
            }
            RenderFrame = nullptr;
            return;
        }

        CurrentFrameSlot = Slot;

        // Advanced before the first PointAtView so every view this frame stamps the same tick.
        ++OptionalImageTick;

        // Allocated once; BuildViewSceneRoot publishes its address to the primary view's shaders.
        EnsureStreamingFeedbackBuffer();

        // IBL cube reconciliation already ran serially in PrepareRender (it issues WaitDeviceIdle).

        PointAtView(SceneViews[0]);
        CurrentCameraEarlyView = 0u;                                // primary's early/frustum cull view

        Frame.SceneGlobalData.CullData.PyramidWidth      = (float)GetNamedImage(ENamedImage::DepthPyramid).GetSizeX();
        Frame.SceneGlobalData.CullData.PyramidHeight     = (float)GetNamedImage(ENamedImage::DepthPyramid).GetSizeY();
        Frame.SceneGlobalData.CullData.DepthPyramidIndex = (uint32)GetNamedImage(ENamedImage::DepthPyramid).GetResourceID();

        {
            const FSceneImage& CascadePyramid = GetNamedImage(ENamedImage::CascadePyramid);
            FCullData& CullData = Frame.SceneGlobalData.CullData;

            CullData.CascadePyramidIndex    = (uint32)CascadePyramid.GetResourceID();
            CullData.CascadePyramidWidth    = (float)CascadePyramid.GetSizeX();
            CullData.CascadePyramidHeight   = (float)CascadePyramid.GetSizeY();
            CullData.CascadePyramidMipCount = CascadePyramid.GetNumMips();
            if (!bCascadePyramidValid.load(std::memory_order_acquire))
            {
                CullData.bCascadeHZBValid = 0u;
            }

            for (int32 i = 0; i < NumCascades; ++i)
            {
                CullData.CascadeHZBTile[i] = FVector4(
                    (float)(GCSMCascadeOriginX[i] / 2) / CullData.CascadePyramidWidth,
                    (float)(GCSMCascadeOriginY[i] / 2) / CullData.CascadePyramidHeight,
                    (float)(GCSMCascadeSizes[i]   / 2) / CullData.CascadePyramidWidth,
                    (float)(GCSMCascadeSizes[i]   / 2) / CullData.CascadePyramidHeight);
            }
        }

        DropStaleFrozenOcclusion(Frame);

        // Publish this frame's stats for the editor-side GetRenderStats() reader.
        RenderStats = Frame.FrameStats;

        // Published here, because a frozen cull skips both DepthPyramidPass calls and rebuilds nothing.
        if (!FrameSettings.bFreezeCulling)
        {
            bDepthPyramidValid.store(true, std::memory_order_release);
        }

        RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::GetGlobalHeap());
        // Projection bakes the Vulkan Y-flip, so CCW-wound geometry lands clockwise in framebuffer space.
        RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CW);

        BeginFrameScratch(CL, Frame);
        GrassCursorCursor = 0;
        
        {
            RHI::CmdBeginMarker(CL, "RenderView Geometry");

            {
                // World-space widget RTs rasterize before the scene's widget pass samples them.
                SCENE_GPU_SCOPE(CL, "RmlUi Widgets");
                RmlUi::RenderWorldWidgets(World, CL);
            }

            {
                SCENE_GPU_SCOPE(CL, "Reset Pass");
                ResetPass_Render(CL);
            }

            {
                SCENE_GPU_SCOPE(CL, "Compile Draw Commands");
                CompileDrawCommands_Render(CL);
            }

            {
                SCENE_GPU_SCOPE(CL, "Sky Cube Capture");
                SkyCubeCapturePass(CL);
            }

            {
                SCENE_GPU_SCOPE(CL, "Texture Paint");
                TexturePaintPass(CL);
            }

            {
                SCENE_GPU_SCOPE(CL, "Terrain Update");
                TerrainUpdatePass(CL);
            }

            {
                SCENE_GPU_SCOPE(CL, "Terrain Cull");
                TerrainCullPass(CL);
            }

            {
                {
                    SCENE_GPU_SCOPE(CL, "Skinning");
                    SkinningPass(CL);
                }
                
                {
                    SCENE_GPU_SCOPE(CL, "VisBuffer Early");
                    VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ true, ECullPhase::Early);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Terrain Depth");
                    TerrainDepthPrePass(CL);
                }
                
                if (!FrameSettings.bFreezeCulling)
                {
                    SCENE_GPU_SCOPE(CL, "Depth Pyramid (Mid)");
                    DepthPyramidPass(CL);
                }
                
                MeshletCullPass(CL, EMeshletSlice::Late);

                {
                    SCENE_GPU_SCOPE(CL, "VisBuffer Late");
                    VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ false, ECullPhase::Late);
                }

                // No rebuild here. Nothing between this and the build after Terrain Render writes depth or reads it.

                {
                    SCENE_GPU_SCOPE(CL, "Cluster Build");
                    ClusterBuildPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Light Cull");
                    LightCullPass(CL);
                }
                
                {
                    SCENE_GPU_SCOPE(CL, "Point Shadows");
                    PointShadowPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Spot Shadows");
                    SpotShadowPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Cascaded Shadows");
                    CascadedShowPass(CL, Frame.Views.CascadeViewBase);
                }

                RHI::CmdEndMarker(CL);
                RHI::CmdBeginMarker(CL, "RenderView Shading");
                
                if (!FrameSettings.bFreezeCulling)
                {
                    SCENE_GPU_SCOPE(CL, "Cascade Pyramid");
                    CascadePyramidPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Sky Irradiance");
                    IrradianceConvolutionPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Sky Prefilter");
                    PrefilterEnvMapPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Decals");
                    DecalPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "GTAO");
                    GTAOPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Shadow Mask");
                    ShadowMaskPass(CL);
                }
                
                {
                    SCENE_GPU_SCOPE(CL, "Environment");
                    EnvironmentPass(CL);
                }

                // Camera-only base for terrain and sky; the material pass then overwrites its own pixels.
                if (IsVelocityWanted())
                {
                    SCENE_GPU_SCOPE(CL, "Velocity");
                    VelocityPass(CL);
                }

                {
                    VisBufferClassifyPass(CL);
                    MaterialGBufferPass(CL);
                    DeferredLightingPass(CL);

                    #if USING(WITH_EDITOR)
                    PickerResolvePass(CL);
                    #endif
                    #if !defined(LE_SHIPPING)
                    SceneDebugViewPass(CL);
                    #endif
                }

                {
                    SCENE_GPU_SCOPE(CL, "Terrain Render");
                    TerrainRenderPass(CL);
                }

                #if !defined(LE_SHIPPING)
                VelocityDebugPass(CL);
                #endif
                
                if (!FrameSettings.bFreezeCulling)
                {
                    SCENE_GPU_SCOPE(CL, "Depth Pyramid (End)");
                    DepthPyramidPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Screen Space Reflections");
                    ScreenSpaceReflectionsPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Water");
                    WaterPass(CL);
                }
                
                {
                    SCENE_GPU_SCOPE(CL, "Aerial Perspective");
                    AerialPerspectivePass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Volumetric Clouds");
                    VolumetricCloudPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Cloud Shadow Map");
                    CloudShadowMapPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Froxel Fog Inject");
                    FroxelInjectPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Froxel Fog Integrate");
                    FroxelIntegratePass(CL);
                }

                // Before translucency, which fogs itself in BasePixelPass; this sees only opaque depth.
                {
                    SCENE_GPU_SCOPE(CL, "Atmosphere Composite");
                    AtmosphereCompositePass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Moment Generation");
                    MomentGenerationPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Transparent");
                    TransparentPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "OIT Resolve");
                    OITResolvePass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Unordered Translucent");
                    UnorderedTranslucentPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Batched Solid Tris");
                    BatchedTriangleDraw(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Batched Lines");
                    BatchedLineDraw(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Particles Simulate");
                    ParticleSimulatePass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Particles Sort");
                    ParticleSortPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Particles Render");
                    ParticleRenderPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Billboards");
                    BillboardPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Sprites");
                    SpritePass(CL);
                }

                // World-space text, pre-tone-map into HDR + Picker (one MRT pass), like billboards.
                {
                    SCENE_GPU_SCOPE(CL, "Text");
                    TextPass(CL);
                }

                #if USING(WITH_EDITOR)
                {
                    SCENE_GPU_SCOPE(CL, "Widget Picker");
                    WidgetPickerPass(CL);
                }
                {
                    // After the last picker RT write; readback happens lazily in GetEntityAtPixel.
                    SCENE_GPU_SCOPE(CL, "Picker Readback");
                    IssuePickerReadback(CL);
                }
                #endif

                {
                    SCENE_GPU_SCOPE(CL, "Underwater");
                    UnderwaterPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Bloom");
                    BloomPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Auto Exposure");
                    AutoExposurePass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Tone Mapping");
                    ToneMappingPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Post Process Materials");
                    PostProcessMaterialPass(CL);
                }

                if (GetSMAAMode() != ESMAAMode::Off)
                {
                    SCENE_GPU_SCOPE(CL, "SMAA");
                    SMAAEdgeDetectionPass(CL);
                    SMAABlendWeightPass(CL);
                    SMAANeighborhoodBlendPass(CL);
                }

                // Before the outline and the widgets, which own the resolved output and must not be blended.
                if (IsTemporalAAEnabled())
                {
                    SCENE_GPU_SCOPE(CL, "Temporal Resolve");
                    TemporalResolvePass(CL);
                }

                #if USING(WITH_EDITOR)
                // After SMAA so the neighborhood blend cannot smear the outline, before Widgets so UI wins.
                {
                    SCENE_GPU_SCOPE(CL, "Selection Outline");
                    SelectionOutlinePass(CL);
                }
                #endif

                {
                    SCENE_GPU_SCOPE(CL, "Widgets");
                    WidgetPass(CL);
                }

                #if !defined(LE_SHIPPING)
                {
                    SCENE_GPU_SCOPE(CL, "Debug Text");
                    DebugTextPass(CL);
                }
                #endif

                for (const FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
                {
                    if (Capture.SceneViewIndex <= 0 || Capture.SceneViewIndex >= (int32)SceneViews.size())
                    {
                        continue;
                    }

                    SCENE_GPU_SCOPE(CL, "Capture View");

                    FSceneView& View = SceneViews[Capture.SceneViewIndex];
                    PointAtView(View);

                    CurrentCameraEarlyView = Capture.CameraViewIndex;

                    SetSceneRoot(CL, View,
                        RHI::CopyTransient(MakeSecondaryViewGlobals(Capture.SceneGlobalData)));

                    RenderCaptureView(CL);
                }

                ReflectionProbeBakePass(CL);
            }

            {
                // Screen-space world UI composites onto the primary display-referred output.
                SCENE_GPU_SCOPE(CL, "RmlUi World UI");
                RmlUi::RenderWorldUI(World, CL);
            }

            Barriers::RasterToRead(CL);

            RHI::CmdEndMarker(CL);
        }

        // After every material lane has run, so the accumulated mask is this frame's complete demand.
        CollectStreamingFeedback(CL);

        // Last, so the copy captures the fully uploaded state and next frame reads it as the past.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute | RHI::EStageFlags::Transfer, RHI::EStageFlags::Transfer);
        SnapshotMotionState(CL);

        RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});

        if (FramesComposited == 0)
        {
            LOG_TRACE("First frame composited into Output ({}x{}, slot {}).",
                      SceneViews[0].Size.x, SceneViews[0].Size.y, SceneViews[0].Output.GetResourceID());
        }
        ++FramesComposited;

        RenderFrame = nullptr;
    }

    FSceneGlobalData FDefaultSceneRenderer::MakeSecondaryViewGlobals(const FSceneGlobalData& ViewGlobals)
    {
        FSceneGlobalData Globals = ViewGlobals;

        const FCullData& Primary = RenderFrame->SceneGlobalData.CullData;
        Globals.CullData.MeshletDrawTag            = Primary.MeshletDrawTag;
        Globals.CullData.MeshletDrawListCapacity   = Primary.MeshletDrawListCapacity;
        Globals.CullData.InstanceNum               = Primary.InstanceNum;
        Globals.CullData.BoneNum                   = Primary.BoneNum;

        // This view's OWN camera, since a capture culls from where it is looking.
        Globals.CullData.CullCameraPosition        = Globals.CameraData.Location;
        Globals.CullData.CullCameraView            = Globals.CameraData.View;
        Globals.CullData.CullCameraProjection      = Globals.CameraData.Projection;
        Globals.CullData.CullNearPlane             = Globals.NearPlane;
        Globals.CullData.CullFarPlane              = Globals.FarPlane;

        // This view's OWN sun-shadow mask, or a skipped mask pass shades every pixel fully lit.
        const FSceneLightData& Lights   = RenderFrame->Lighting.LightData;
        FrameFlags.bShadowMaskValid = (Lights.bHasSun != 0)
                                       && (RenderFrame->Lighting.Lights[0].ShadowDataIndex != INDEX_NONE);
        Globals.ShadowMaskIndex         = FrameFlags.bShadowMaskValid
            ? (uint32)CurrentView->Images[(int)ENamedImage::ShadowMask].GetResourceID()
            : ~0u;

        // This view's OWN moment targets, or captures rebuild transmittance from the main camera.
        Globals.MomentZerothIndex = (uint32)CurrentView->Images[(int)ENamedImage::MomentZeroth].GetResourceID();
        Globals.MomentsIndex      = (uint32)CurrentView->Images[(int)ENamedImage::Moments].GetResourceID();

        PublishFogGlobals(Globals);

        return Globals;
    }

    void FDefaultSceneRenderer::RenderCaptureView(RHI::FCmdListH CL)
    {
        VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ true);
        TerrainCullPass(CL);
        TerrainDepthPrePass(CL);
        ClusterBuildPass(CL);
        LightCullPass(CL);
        EnvironmentPass(CL);
        DecalPass(CL);
        // Resolves this view's sun shadows; the lighting dispatch reads nothing else for the sun.
        ShadowMaskPass(CL);
        VisBufferClassifyPass(CL);
        MaterialGBufferPass(CL);
        DeferredLightingPass(CL);
        TerrainRenderPass(CL);
        ScreenSpaceReflectionsPass(CL);
        WaterPass(CL);
        AerialPerspectivePass(CL);
        VolumetricCloudPass(CL);
        CloudShadowMapPass(CL);
        FroxelInjectPass(CL);
        FroxelIntegratePass(CL);
        AtmosphereCompositePass(CL);
        MomentGenerationPass(CL);
        TransparentPass(CL);
        OITResolvePass(CL);
        UnorderedTranslucentPass(CL);
        BloomPass(CL);
        AutoExposurePass(CL);
        ToneMappingPass(CL);
        PostProcessMaterialPass(CL);

        // No T2x on a capture; it has no jitter sequence of its own and would resolve against another view.
        if (GetSMAAMode() != ESMAAMode::Off)
        {
            SMAAEdgeDetectionPass(CL);
            SMAABlendWeightPass(CL);
            SMAANeighborhoodBlendPass(CL);
        }
    }

    void FDefaultSceneRenderer::SwapchainResized(FVector2 NewSize)
    {
        // A scene whose view is driven by an editor panel does not care how big the window got.
        if (!bPrimaryTracksSwapchain)
        {
            return;
        }

        PendingPrimarySize     = FUIntVector2((uint32)Math::Max(NewSize.x, 1.0f), (uint32)Math::Max(NewSize.y, 1.0f));
        bHasPendingPrimarySize = true;
    }

    // Quantized with hysteresis, since each apply is a full image realloc.
    void FDefaultSceneRenderer::SetPrimaryViewSize(const FUIntVector2& SizePixels)
    {
        bPrimaryTracksSwapchain = false;

        if (SceneViews.empty())
        {
            return;
        }

        constexpr uint32 kViewSizeGranularity = 128;
        constexpr uint32 kShrinkDeadBand      = 2 * kViewSizeGranularity;

        auto RoundUp = [](uint32 V)
        {
            const uint32 Clamped = Math::Max(V, 1u);
            return ((Clamped + kViewSizeGranularity - 1u) / kViewSizeGranularity) * kViewSizeGranularity;
        };

        const FUIntVector2 Wanted(RoundUp(SizePixels.x), RoundUp(SizePixels.y));
        const FUIntVector2 Current = SceneViews[0].Size;

        auto Pick = [](uint32 WantedAxis, uint32 CurrentAxis)
        {
            const bool bGrow   = WantedAxis > CurrentAxis;
            const bool bShrink = WantedAxis + kShrinkDeadBand <= CurrentAxis;
            return (bGrow || bShrink) ? WantedAxis : CurrentAxis;
        };

        const FUIntVector2 Target(Pick(Wanted.x, Current.x), Pick(Wanted.y, Current.y));
        if (Target == Current)
        {
            bHasPendingPrimarySize = false;
            return;
        }

        // Resizing here would drop the last good frame of a world that cannot repaint; Extract applies it.
        PendingPrimarySize     = Target;
        bHasPendingPrimarySize = true;
    }

    void FDefaultSceneRenderer::ApplyPendingPrimarySize()
    {
        if (!bHasPendingPrimarySize)
        {
            return;
        }

        bHasPendingPrimarySize = false;
        if (PendingPrimarySize != SceneViews[0].Size)
        {
            ResizePrimaryView(PendingPrimarySize);
        }
    }

    void FDefaultSceneRenderer::ResizePrimaryView(const FUIntVector2& NewSize)
    {
        LOG_TRACE("Primary view resized {}x{} -> {}x{}; Output texture replaced.",
                  SceneViews[0].Size.x, SceneViews[0].Size.y, NewSize.x, NewSize.y);

        // Only the primary view is resized here; capture views keep their own size.
        FSceneView& Primary = SceneViews[0];
        Primary.Size = FUIntVector2(Math::Max(NewSize.x, 1u), Math::Max(NewSize.y, 1u));

        InitFrameResources();

        // New depth targets, so whatever the pyramid holds is from the old extent.
        bDepthPyramidValid.store(false, std::memory_order_release);

        #if USING(WITH_EDITOR)
        // Drop old readback slots; sized to previous extent so pixel grid no longer matches clicks.
        for (FPickerReadbackSlot& Slot : PickerReadbackRing)
        {
            if (Slot.Readback.Gpu != 0)
            {
                RHI::Retire(Slot.Readback);
                Slot.Readback = {};
            }
            Slot.Width = 0;
            Slot.Height = 0;
            Slot.bPending = false;
        }
        #endif
    }

    void FDefaultSceneRenderer::ResetPass_Render(RHI::FCmdListH CL)
    {
        // Every target is cleared by the render pass that first writes it; nothing is transfer-cleared here.
        bLocalAtlasClearedThisFrame = false;
        bCascadeClearedThisFrame    = false;
    }

    RHI::ELoadOp FDefaultSceneRenderer::TakeLocalAtlasLoadOp()
    {
        const RHI::ELoadOp Op = bLocalAtlasClearedThisFrame ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        bLocalAtlasClearedThisFrame = true;
        return Op;
    }

    RHI::ELoadOp FDefaultSceneRenderer::TakeCascadeLoadOp()
    {
        const RHI::ELoadOp Op = bCascadeClearedThisFrame ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        bCascadeClearedThisFrame = true;
        return Op;
    }

    void FDefaultSceneRenderer::BeginFrameScratch(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        uint32 GrassCursors = 0;
        for (const FFrameData::FTerrainExtract& Terrain : Frame.Extracts.TerrainExtracts)
        {
            GrassCursors += (uint32)Terrain.Grass.size();
        }

        const uint64 UsedBytes = kScratchGrassCursorsOffset + (uint64)GrassCursors * sizeof(uint32);
        ResizeBufferIfNeeded(CL, FrameScratchRing[CurrentFrameSlot], UsedBytes, 1.5f, FrameScratchLowUsage[CurrentFrameSlot],
                             true, EBufferInit::Undefined, "Frame.Scratch");

        RHI::CmdMemzero(CL, { FrameScratchRing[CurrentFrameSlot].Gpu, UsedBytes });
        Barriers::TransferToCompute(CL);
    }

    FDefaultSceneRenderer::ENamedImage FDefaultSceneRenderer::GetTemporalCurrentImage() const
    {
        const uint32 Parity = CurrentView != nullptr ? (CurrentView->TemporalFrameIndex & 1u) : 0u;
        return Parity == 0u ? ENamedImage::TemporalHistoryA : ENamedImage::TemporalHistoryB;
    }

    FDefaultSceneRenderer::ENamedImage FDefaultSceneRenderer::GetTemporalHistoryImage() const
    {
        const uint32 Parity = CurrentView != nullptr ? (CurrentView->TemporalFrameIndex & 1u) : 0u;
        return Parity == 0u ? ENamedImage::TemporalHistoryB : ENamedImage::TemporalHistoryA;
    }

    FVector2 FDefaultSceneRenderer::GetTemporalJitterNDC(const FSceneView& View, uint32 FrameIndex)
    {
        if (View.Size.x == 0 || View.Size.y == 0)
        {
            return FVector2(0.0f);
        }

        // Main diagonal in screen space, which is the pair the AreaTex T2x slices were generated against.
        const FVector2 kSubsamples[2] = { FVector2(-0.25f, -0.25f), FVector2(0.25f, 0.25f) };

        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        const float Scale = Settings != nullptr ? Math::Clamp(Settings->TemporalJitterScale, 0.0f, 1.0f) : 1.0f;

        const FVector2 Offset = kSubsamples[FrameIndex & 1u] * Scale;
        return FVector2((Offset.x * 2.0f) / (float)View.Size.x,
                        (Offset.y * 2.0f) / (float)View.Size.y);
    }

    void FDefaultSceneRenderer::InitBuffers()
    {

        // GPU pre-skinning output, written by Skinning.slang and read by every draw VS via BDA.
        PreSkinnedVerticesBuffer = CreateSceneBuffer(sizeof(FPreSkinnedVertex) * 64 * 1024, "Cull.PreSkinnedVertices");

        for (uint32 Slot = 0; Slot < RHI::kFramesInFlight; ++Slot)
        {
            MeshletDrawListRing[Slot] = CreateSceneBuffer(sizeof(uint32) * 2, "Cull.MeshletDrawList");

            // Per-(view, draw) cull layout. Sized for real in CompileDrawCommands_Render.
            RenderBucketRing[Slot] = CreateSceneBuffer(sizeof(FRenderBucketGPU), "Cull.RenderBuckets");

            MeshletBlockRing[Slot] = CreateSceneBuffer(sizeof(uint32) * 2, "Cull.MeshletBlocks");
            // Fixed size, one grid, rewritten by BuildDrawPrefix every frame.
            BlockDispatchArgsRing[Slot] = CreateSceneBuffer(sizeof(RHI::FDispatchIndirectArguments), "Cull.BlockDispatchArgs");
            SkinDispatchArgsRing[Slot]  = CreateSceneBuffer(sizeof(RHI::FDispatchIndirectArguments), "Cull.SkinDispatchArgs");
            SkinWorkBaseRing[Slot]      = CreateSceneBuffer(sizeof(uint32) * 2, "Cull.SkinWorkBase");
            MeshletCullDispatchArgsRing[Slot] = CreateSceneBuffer(sizeof(RHI::FDispatchIndirectArguments), "Cull.MeshletCullDispatchArgs");

            TotalsRing[Slot] = CreateSceneBuffer(sizeof(uint32) * kTotalsSlots, "Cull.Totals");
            TotalsZeroed[Slot] = false;

            // GPU-driven scene per-frame outputs. Sized for real in CompileDrawCommands_Render.
            VisibleInstanceRing[Slot]       = CreateSceneBuffer(sizeof(FGPUInstance), "Cull.VisibleInstances");

            if (MeshletBoundReadback[Slot].Gpu == 0)
            {
                MeshletBoundReadback[Slot] = RHI::Malloc(sizeof(uint32) * kTotalsSlots,
                                                         RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
                RHI::SetDebugName(MeshletBoundReadback[Slot].Gpu, "Readback.MeshletBounds");

                if (void* Host = MeshletBoundReadback[Slot].Cpu)
                {
                    Memory::Memzero(Host, sizeof(uint32) * kTotalsSlots);
                }
            }
        }
    }

    static uint32 PreviousPow2(uint32 v)
    {
        uint32 r = 1;
        while (r * 2 < v)
        {
            r *= 2;
        }
        return r;
    }

            void FDefaultSceneRenderer::ReleaseViewImages(FSceneView& View)
    {
        auto Release = [](FSceneImage& Image)
        {
            RetireSceneImage(Image);
        };

        for (FSceneImage& Image : View.Images)
        {
            if (Image.bOwned)
            {
                Release(Image);
            }
        }
        Release(View.Output);
        Release(View.BloomChainImage);

        // Shared aliases, so just drop the copies and let the owners release them.
        View.Images.fill(FSceneImage{});
        View.ImageLastUsedTick.fill(0);
    }

    static const char* ENamedImageToString(FDefaultSceneRenderer::ENamedImage Image)
    {
        using ENamedImage = FDefaultSceneRenderer::ENamedImage;
        switch (Image)
        {
        case ENamedImage::HDR:                return "Scene.HDR";
        case ENamedImage::LDR:                return "Scene.LDR";
        case ENamedImage::PostProcessScratch: return "Scene.PostProcessScratch";
        case ENamedImage::SMAAEdges:          return "Scene.SMAAEdges";
        case ENamedImage::SMAABlend:          return "Scene.SMAABlend";
        case ENamedImage::SMAAArea:           return "Scene.SMAAArea";
        case ENamedImage::SMAASearch:         return "Scene.SMAASearch";
        case ENamedImage::GTAOWorkingDepth:   return "Scene.GTAOWorkingDepth";
        case ENamedImage::GTAOEdges:          return "Scene.GTAOEdges";
        case ENamedImage::GTAO:               return "Scene.GTAO";
        case ENamedImage::GTAODenoise:        return "Scene.GTAODenoise";
        case ENamedImage::GTAOBlur:           return "Scene.GTAOBlur";
        case ENamedImage::ShadowMask:         return "Scene.ShadowMask";
        case ENamedImage::Cascade:            return "Scene.Cascade";
        case ENamedImage::CascadePyramid:     return "Scene.CascadePyramid";
        case ENamedImage::DepthAttachment:    return "Scene.DepthAttachment";
        case ENamedImage::DepthPyramid:       return "Scene.DepthPyramid";
        case ENamedImage::Picker:             return "Scene.Picker";
        case ENamedImage::VisBuffer:          return "Scene.VisBuffer";
        case ENamedImage::GBufferA:           return "Scene.GBufferA";
        case ENamedImage::GBufferB:           return "Scene.GBufferB";
        case ENamedImage::GBufferC:           return "Scene.GBufferC";
        case ENamedImage::GBufferD:           return "Scene.GBufferD";
        case ENamedImage::Accum:              return "Scene.Accum";
        case ENamedImage::MomentZeroth:       return "Scene.MomentZeroth";
        case ENamedImage::Moments:            return "Scene.Moments";
        case ENamedImage::WaterRefraction:    return "Scene.WaterRefraction";
        case ENamedImage::DBufferA:           return "Scene.DBufferA";
        case ENamedImage::DBufferB:           return "Scene.DBufferB";
        case ENamedImage::DBufferC:           return "Scene.DBufferC";
        case ENamedImage::AdaptedLuminance:   return "Scene.AdaptedLuminance";
        case ENamedImage::FroxelScatter:      return "Scene.FroxelScatter";
        case ENamedImage::FroxelIntegrated:   return "Scene.FroxelIntegrated";
        case ENamedImage::AerialInScatter:    return "Scene.AerialInScatter";
        case ENamedImage::AerialTransmittance: return "Scene.AerialTransmittance";
        case ENamedImage::CloudNoise:         return "Scene.CloudNoise";
        case ENamedImage::CloudScatter:       return "Scene.CloudScatter";
        case ENamedImage::CloudShadow:        return "Scene.CloudShadow";
        case ENamedImage::BRDFLut:            return "Scene.BRDFLut";
        case ENamedImage::SkyCube:            return "Scene.SkyCube";
        case ENamedImage::SkyIrradiance:      return "Scene.SkyIrradiance";
        case ENamedImage::SkyPrefilter:       return "Scene.SkyPrefilter";
        case ENamedImage::ProbeCaptureCube:   return "Scene.ProbeCaptureCube";
        case ENamedImage::ProbePrefiltered:   return "Scene.ProbePrefiltered";
        case ENamedImage::Velocity:           return "Scene.Velocity";
        case ENamedImage::TemporalHistoryA:   return "Scene.TemporalHistoryA";
        case ENamedImage::TemporalHistoryB:   return "Scene.TemporalHistoryB";

        #if USING(WITH_EDITOR)
        case ENamedImage::PointLightIcon:       return "Scene.PointLightIcon";
        case ENamedImage::DirectionalLightIcon: return "Scene.DirectionalLightIcon";
        case ENamedImage::SkyLightIcon:         return "Scene.SkyLightIcon";
        case ENamedImage::SpotLightIcon:        return "Scene.SpotLightIcon";
        case ENamedImage::CameraIcon:           return "Scene.CameraIcon";
        case ENamedImage::CharacterIcon:        return "Scene.CharacterIcon";
        case ENamedImage::ParticleSystemIcon:   return "Scene.ParticleSystemIcon";
        case ENamedImage::AudioSourceIcon:      return "Scene.AudioSourceIcon";
        case ENamedImage::AudioListenerIcon:    return "Scene.AudioListenerIcon";
        #endif

        case ENamedImage::Num:                break;
        }
        return "Scene.Unknown";
    }

    void FDefaultSceneRenderer::NameOwnedImages(TArray<FSceneImage, (int)ENamedImage::Num>& Images)
    {
        for (int i = 0; i < (int)ENamedImage::Num; ++i)
        {
            if (Images[i].bOwned && Images[i].IsValid())
            {
                RHI::SetDebugName(Images[i].Texture, ENamedImageToString((ENamedImage)i));
            }
        }
    }

    bool FDefaultSceneRenderer::IsOptionalNamedImage(ENamedImage Image)
    {
        switch (Image)
        {
        case ENamedImage::Accum:
        case ENamedImage::MomentZeroth:
        case ENamedImage::Moments:
        case ENamedImage::WaterRefraction:
        case ENamedImage::DBufferA:
        case ENamedImage::DBufferB:
        case ENamedImage::DBufferC:
        case ENamedImage::Velocity:
        case ENamedImage::TemporalHistoryA:
        case ENamedImage::TemporalHistoryB:
            return true;
        default:
            return false;
        }
    }

    bool FDefaultSceneRenderer::MakeOptionalImageDesc(ENamedImage Image, const FUIntVector2& Extent, RHI::FTextureDesc& OutDesc)
    {
        OutDesc           = RHI::FTextureDesc{};
        OutDesc.Type      = RHI::ETextureType::Tex2D;
        OutDesc.Dimension = FUIntVector3(Extent.x, Extent.y, 1);
        OutDesc.Usage     = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled;

        switch (Image)
        {
        case ENamedImage::Accum:
            OutDesc.Format = EFormat::RGBA16_FLOAT;
            return true;

        // Additively blended and fp32, since the Hankel reconstruction is ill-conditioned in fp16.
        case ENamedImage::MomentZeroth:
            OutDesc.Format = EFormat::R32_FLOAT;
            return true;

        case ENamedImage::Moments:
            OutDesc.Format = EFormat::RGBA32_FLOAT;
            return true;

        // Never rendered into, since WaterPass copies HDR here and samples it.
        case ENamedImage::WaterRefraction:
            OutDesc.Format = EFormat::RGBA16_FLOAT;
            OutDesc.Usage  = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;
            return true;

        case ENamedImage::DBufferA:
        case ENamedImage::DBufferB:
        case ENamedImage::DBufferC:
            OutDesc.Format = EFormat::RGBA8_UNORM;
            return true;

        // Signed screen-space offset in UV units, so it needs float rather than the usual unorm.
        case ENamedImage::Velocity:
            OutDesc.Format = EFormat::RG16_FLOAT;
            // Storage as well, because the material pass overwrites its own pixels from compute.
            OutDesc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled |
                             RHI::EImageUsageFlags::Storage;
            return true;

        // Post-tonemap display-referred color, matching the view output the resolve writes.
        case ENamedImage::TemporalHistoryA:
        case ENamedImage::TemporalHistoryB:
            OutDesc.Format = EFormat::RGBA8_UNORM;
            return true;

        default:
            return false;
        }
    }

    void FDefaultSceneRenderer::EnsureOptionalViewImages(FSceneView& View)
    {
        if (RenderFrame == nullptr || View.Size.x == 0 || View.Size.y == 0)
        {
            return;
        }

        const FFrameData& Frame = *RenderFrame;

        // Exactly the conditions the consuming passes early-out on, so a target exists when needed.
        const bool bTranslucency = !Frame.Geometry.TranslucentDrawList.empty();
        const bool bDecals       = !Frame.Primitives.DecalExtracts.empty();
        const bool bWater        = !Frame.Water.Surfaces.empty();

        auto Want = [this, &View](ENamedImage Image, bool bNeeded)
        {
            if (!bNeeded)
            {
                return;
            }

            View.ImageLastUsedTick[(int)Image] = OptionalImageTick;

            FSceneImage& Slot = View.Images[(int)Image];
            if (Slot.IsValid())
            {
                return;
            }

            RHI::FTextureDesc Desc;
            if (!MakeOptionalImageDesc(Image, View.Size, Desc))
            {
                return;
            }

            Slot = CreateSceneImage(Desc);
            RHI::SetDebugName(Slot.Texture, ENamedImageToString(Image));
        };

        Want(ENamedImage::Accum,           bTranslucency);
        Want(ENamedImage::MomentZeroth,    bTranslucency);
        Want(ENamedImage::Moments,         bTranslucency);
        // SSR needs the same scene-color snapshot the water pass refracts through.
        const CRendererSettings* RendererSettings = GetDefault<CRendererSettings>();
        const bool bSSR = RendererSettings != nullptr && RendererSettings->bScreenSpaceReflections;
        Want(ENamedImage::WaterRefraction, bWater || bSSR);
        Want(ENamedImage::DBufferA,        bDecals);
        Want(ENamedImage::DBufferB,        bDecals);
        Want(ENamedImage::DBufferC,        bDecals);

        const bool bTemporal = IsTemporalAAEnabledFor(View);
        const bool bHadTemporalTargets = View.Images[(int)ENamedImage::TemporalHistoryA].IsValid()
                                      && View.Images[(int)ENamedImage::TemporalHistoryB].IsValid();
        Want(ENamedImage::Velocity,         bTemporal || (View.bIsPrimary && IsVelocityDebugActive()));
        Want(ENamedImage::TemporalHistoryA, bTemporal);
        Want(ENamedImage::TemporalHistoryB, bTemporal);

        // A freshly created pair holds whatever the allocator handed back, which is not a previous frame.
        if (bTemporal && !bHadTemporalTargets)
        {
            View.bTemporalHistoryValid = false;
        }

        ReleaseIdleOptionalImages(View);
    }

    void FDefaultSceneRenderer::ReleaseIdleOptionalImages(FSceneView& View)
    {
        // Long enough to survive walking past glass, short enough that empty levels stop paying.
        constexpr uint64 kIdleTicksBeforeRelease = 600;   // ~10s at 60fps

        if (OptionalImageTick < kIdleTicksBeforeRelease)
        {
            return;
        }

        for (int32 i = 0; i < (int32)ENamedImage::Num; ++i)
        {
            const ENamedImage Image = (ENamedImage)i;
            if (!IsOptionalNamedImage(Image))
            {
                continue;
            }

            FSceneImage& Slot = View.Images[i];
            if (!Slot.IsValid())
            {
                continue;
            }

            if (OptionalImageTick - View.ImageLastUsedTick[i] >= kIdleTicksBeforeRelease)
            {
                // Frame-deferred, since a frame already recorded against this image is still in flight.
                RetireSceneImage(Slot);
            }
        }
    }

    void FDefaultSceneRenderer::PointAtView(FSceneView& View)
    {
        CurrentView = &View;
        EnsureOptionalViewImages(View);
    }

    void FDefaultSceneRenderer::InitViewImages(FSceneView& View, uint32 ReuseOutputSlot)
    {
        const FUIntVector2 Extent = View.Size;

        View.Images = NamedImages;

        for (FSceneImage& Seeded : View.Images)
        {
            Seeded.bOwned = false;
        }

        RHI::FTextureDesc Desc;
        Desc.Type      = RHI::ETextureType::Tex2D;
        Desc.Dimension = FUIntVector3(Extent.x, Extent.y, 1);

        Desc.Format = EFormat::RGBA8_UNORM;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled |
                      RHI::EImageUsageFlags::TransferDst | RHI::EImageUsageFlags::TransferSrc;
        View.Output = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ false, ReuseOutputSlot);

        // Storage as well, because the deferred lighting pass writes HDR from compute.
        Desc.Format = EFormat::RGBA16_FLOAT;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled |
                      RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::TransferSrc;
        View.Images[(int)ENamedImage::HDR] = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ true);

        // LDR + post-process ping-pong scratch; both copy source/dest in the PP chain hand-off.
        Desc.Format = EFormat::RGBA8_UNORM;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled |
                      RHI::EImageUsageFlags::TransferSrc | RHI::EImageUsageFlags::TransferDst;
        View.Images[(int)ENamedImage::LDR]                = CreateSceneImage(Desc);
        View.Images[(int)ENamedImage::PostProcessScratch] = CreateSceneImage(Desc);

        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled;

        Desc.Format = EFormat::RG8_UNORM;
        View.Images[(int)ENamedImage::SMAAEdges] = CreateSceneImage(Desc);

        Desc.Format = EFormat::RGBA8_UNORM;
        View.Images[(int)ENamedImage::SMAABlend] = CreateSceneImage(Desc);

        // XeGTAO runs entirely in compute at full resolution, so every stage target needs a storage view.
        Desc.Format    = EFormat::R8_UNORM;
        Desc.Dimension = FUIntVector3(Extent.x, Extent.y, 1);
        Desc.Usage     = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled |
                         RHI::EImageUsageFlags::Storage;
        View.Images[(int)ENamedImage::GTAOEdges]   = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ true);
        View.Images[(int)ENamedImage::GTAO]        = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ true);
        View.Images[(int)ENamedImage::GTAODenoise] = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ true);
        View.Images[(int)ENamedImage::GTAOBlur]    = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ true);

        {
            RHI::FTextureDesc WorkingDepthDesc;
            WorkingDepthDesc.Type      = RHI::ETextureType::Tex2D;
            WorkingDepthDesc.Dimension = FUIntVector3(Extent.x, Extent.y, 1);
            WorkingDepthDesc.Format    = EFormat::R32_FLOAT;
            WorkingDepthDesc.MipCount  = GTAODepthMipLevels;
            WorkingDepthDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.Images[(int)ENamedImage::GTAOWorkingDepth] = CreateSceneImage(WorkingDepthDesc, true, /*bMipUAVs*/ true);
        }

        Desc.Usage = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled;
        View.Images[(int)ENamedImage::ShadowMask]  = CreateSceneImage(Desc);

        // Scene depth; transfer-dst for the no-occluder clear.
        Desc.Format = EFormat::D32;
        Desc.Usage  = RHI::EImageUsageFlags::DepthAttachment | RHI::EImageUsageFlags::Sampled |
                      RHI::EImageUsageFlags::TransferDst;
        View.Images[(int)ENamedImage::DepthAttachment] = CreateSceneImage(Desc);

        {
            const uint32 Width  = PreviousPow2(Extent.x);
            const uint32 Height = PreviousPow2(Extent.y);

            // R16_FLOAT HZB, reverse-Z [0,1], min-reduced, with conservative quantization error.
            RHI::FTextureDesc PyramidDesc;
            PyramidDesc.Type      = RHI::ETextureType::Tex2D;
            PyramidDesc.Dimension = FUIntVector3(Width, Height, 1);
            PyramidDesc.Format    = EFormat::R16_FLOAT;
            PyramidDesc.MipCount  = RenderUtils::CalculateMipCount(Width, Height);
            PyramidDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.Images[(int)ENamedImage::DepthPyramid] = CreateSceneImage(PyramidDesc, true, /*bMipUAVs*/ true);
        }

        // Entity picker and copy source for the click readback; a packaged game never picks.
        #if USING(WITH_EDITOR)
        Desc.Format = EFormat::R32_UINT;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferSrc;
        View.Images[(int)ENamedImage::Picker] = CreateSceneImage(Desc);
        #endif

        Desc.Format = EFormat::R32_UINT;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled;
        View.Images[(int)ENamedImage::VisBuffer] = CreateSceneImage(Desc);

        // Storage and Sampled only; never cleared, since lighting walks the same compacted list.
        Desc.Usage  = RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::Sampled;
        Desc.Format = EFormat::RGBA8_UNORM;
        View.Images[(int)ENamedImage::GBufferA] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);
        View.Images[(int)ENamedImage::GBufferC] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);
        Desc.Format = EFormat::RGBA16_FLOAT;
        View.Images[(int)ENamedImage::GBufferB] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);
        Desc.Format = EFormat::R11G11B10_FLOAT;
        View.Images[(int)ENamedImage::GBufferD] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);

        // The optional targets now arrive through EnsureOptionalViewImages on first use.

        {
            float FroxelScale = 1.0f;
            if (const CRendererSettings* RS = GetDefault<CRendererSettings>())
            {
                FroxelScale = Math::Clamp(RS->FroxelResolutionScale, 0.25f, 2.0f);
            }
            FroxelGridSize = FUIntVector3(
                Math::Max(1u, (uint32)(GFroxelGridX * FroxelScale + 0.5f)),
                Math::Max(1u, (uint32)(GFroxelGridY * FroxelScale + 0.5f)),
                Math::Max(1u, (uint32)(GFroxelGridZ * FroxelScale + 0.5f)));

            RHI::FTextureDesc FroxelDesc;
            FroxelDesc.Type      = RHI::ETextureType::Tex3D;
            FroxelDesc.Dimension = FroxelGridSize;
            FroxelDesc.Format    = EFormat::RGBA16_FLOAT;
            FroxelDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.Images[(int)ENamedImage::FroxelScatter]    = CreateSceneImage(FroxelDesc, true, true);
            View.Images[(int)ENamedImage::FroxelIntegrated] = CreateSceneImage(FroxelDesc, true, true);
        }

        {
            RHI::FTextureDesc AerialDesc;
            AerialDesc.Type      = RHI::ETextureType::Tex3D;
            AerialDesc.Dimension = FUIntVector3(kAerialLUTSize, kAerialLUTSize, kAerialLUTSlices);
            AerialDesc.Format    = EFormat::RGBA16_FLOAT;
            AerialDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.Images[(int)ENamedImage::AerialInScatter]     = CreateSceneImage(AerialDesc, true, true);
            View.Images[(int)ENamedImage::AerialTransmittance] = CreateSceneImage(AerialDesc, true, true);
        }

        {
            RHI::FTextureDesc CloudDesc;
            CloudDesc.Type      = RHI::ETextureType::Tex3D;
            CloudDesc.Dimension = FUIntVector3(kCloudNoiseSize, kCloudNoiseSize, kCloudNoiseSize);
            CloudDesc.Format    = EFormat::RGBA8_UNORM;
            CloudDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.Images[(int)ENamedImage::CloudNoise] = CreateSceneImage(CloudDesc, true, true);
            View.bCloudNoiseBaked = false;

            RHI::FTextureDesc ScatterDesc;
            ScatterDesc.Type      = RHI::ETextureType::Tex2D;
            ScatterDesc.Dimension = FUIntVector3(std::max<uint32>(Extent.x / 2u, 1u),
                                                 std::max<uint32>(Extent.y / 2u, 1u), 1);
            ScatterDesc.Format    = EFormat::RGBA16_FLOAT;
            ScatterDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.Images[(int)ENamedImage::CloudScatter] = CreateSceneImage(ScatterDesc, true, true);

            int32 CloudShadowRes = 512;
            if (const CRendererSettings* RS = GetDefault<CRendererSettings>())
            {
                CloudShadowRes = Math::Clamp(RS->CloudShadowResolution, 128, 2048);
            }
            RHI::FTextureDesc CloudShadowDesc;
            CloudShadowDesc.Type      = RHI::ETextureType::Tex2D;
            CloudShadowDesc.Dimension = FUIntVector3((uint32)CloudShadowRes, (uint32)CloudShadowRes, 1);
            CloudShadowDesc.Format    = EFormat::R16_FLOAT;
            CloudShadowDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.Images[(int)ENamedImage::CloudShadow] = CreateSceneImage(CloudShadowDesc, true, true);
        }

        {
            const uint32 BloomW = std::max<uint32>(Extent.x / 2u, 1u);
            const uint32 BloomH = std::max<uint32>(Extent.y / 2u, 1u);

            RHI::FTextureDesc BloomDesc;
            BloomDesc.Type      = RHI::ETextureType::Tex2D;
            BloomDesc.Dimension = FUIntVector3(BloomW, BloomH, 1);
            BloomDesc.Format    = EFormat::R11G11B10_FLOAT;
            BloomDesc.MipCount  = Math::Min(BLOOM_MIP_COUNT, RenderUtils::CalculateMipCount(BloomW, BloomH));
            BloomDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.BloomChainImage = CreateSceneImage(BloomDesc, true, true);
        }

        {
            // Auto-exposure adapted luminance, a 1x1 persistent R32F carried across frames.
            RHI::FTextureDesc AdaptedDesc;
            AdaptedDesc.Type      = RHI::ETextureType::Tex2D;
            AdaptedDesc.Dimension = FUIntVector3(1, 1, 1);
            AdaptedDesc.Format    = EFormat::R32_FLOAT;
            AdaptedDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.Images[(int)ENamedImage::AdaptedLuminance] = CreateSceneImage(AdaptedDesc, true, true);
        }

        NameOwnedImages(View.Images);
        RHI::SetDebugName(View.Output.Texture, "View.Output");
        RHI::SetDebugName(View.BloomChainImage.Texture, "View.BloomChain");
    }

    void FDefaultSceneRenderer::BakeBRDFLUT()
    {
        constexpr uint32 BRDFLutSize = 256u;

        FSharedRenderResources& Shared = Render().GetSharedRenderResources();

        Shared.BRDFLut = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width    = BRDFLutSize,
            .Height   = BRDFLutSize,
            .Format   = EFormat::RG16_FLOAT,
            .bStorage = true,
            .DebugName = "Shared.BRDFLut"
        });
        Shared.BRDFLutUAV = RHI::Textures::StorageSlot(Shared.BRDFLut, 0);

        static const FShaderH ComputeShader = FShaderLibrary::Get("BRDFIntegration.slang");
        if (!ComputeShader)
        {
            return;
        }

        const FShaderEntry* ComputeEntry = FShaderLibrary::Resolve(ComputeShader);
        if (ComputeEntry == nullptr)
        {
            return;
        }

        RHI::FPipelineH Pipeline = RHI::CreateComputePipeline(ComputeEntry->Source());

        RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::GetGlobalHeap());
        RHI::CmdSetPipeline(CL, Pipeline);

        struct FBRDFArgs { uint32 OutUAV; uint32 Width; uint32 Height; uint32 _Pad0; };
        const FBRDFArgs Args{ Shared.BRDFLutUAV, BRDFLutSize, BRDFLutSize, 0 };
        const RHI::GPUPtr ArgsPtr = RHI::CopyTransient(Args);

        constexpr uint32 BRDFLutTile = 8u;
        const uint32 Groups = RenderUtils::GetGroupCount(BRDFLutSize, BRDFLutTile);
        RHI::CmdDispatch(CL, ArgsPtr, Groups, Groups, 1);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);

        const uint64 BakeValue = RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});
        RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Graphics), BakeValue);
        RHI::Retire(Pipeline);
    }

    void FDefaultSceneRenderer::InitSkyCube(uint32 FaceSize)
    {
        RHI::FTextureDesc Desc;
        Desc.Type       = RHI::ETextureType::TexCube;
        Desc.Dimension  = FUIntVector3(FaceSize, FaceSize, 1);
        Desc.LayerCount = 6;
        Desc.Format     = EFormat::R11G11B10_FLOAT;
        Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::TransferDst;

        NamedImages[(int)ENamedImage::SkyCube] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);
    }

    void FDefaultSceneRenderer::InitIBLConvolutionTargets(const FIBLBakeResolution& Resolution)
    {
        {
            RHI::FTextureDesc Desc;
            Desc.Type       = RHI::ETextureType::TexCube;
            Desc.Dimension  = FUIntVector3(Resolution.Irradiance, Resolution.Irradiance, 1);
            Desc.LayerCount = 6;
            Desc.Format     = EFormat::R11G11B10_FLOAT;
            Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::TransferDst;

            NamedImages[(int)ENamedImage::SkyIrradiance] = CreateSceneImage(Desc, true, true);
        }

        {
            RHI::FTextureDesc Desc;
            Desc.Type       = RHI::ETextureType::TexCube;
            Desc.Dimension  = FUIntVector3(Resolution.Prefilter, Resolution.Prefilter, 1);
            Desc.LayerCount = 6;
            Desc.MipCount   = Resolution.Mips;
            Desc.Format     = EFormat::R11G11B10_FLOAT;
            Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::TransferDst;

            NamedImages[(int)ENamedImage::SkyPrefilter] = CreateSceneImage(Desc, true, true);
        }

        NameOwnedImages(NamedImages);
    }

    void FDefaultSceneRenderer::InitReflectionProbeTargets()
    {
        if (NamedImages[(int)ENamedImage::ProbePrefiltered].IsValid())
        {
            return;
        }

        {
            RHI::FTextureDesc Desc;
            Desc.Type       = RHI::ETextureType::TexCubeArray;
            Desc.Dimension  = FUIntVector3(ProbePrefilterBaseSize, ProbePrefilterBaseSize, 1);
            Desc.LayerCount = MaxReflectionProbes * 6;
            Desc.MipCount   = ProbePrefilterMips;
            Desc.Format     = EFormat::R11G11B10_FLOAT;
            Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::TransferDst;

            NamedImages[(int)ENamedImage::ProbePrefiltered] = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ true);
        }

        RHI::FCmdListH CL = RHI::OpenCommandList();
        const float Black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        RHI::CmdBarrier(CL, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute, RHI::EStageFlags::Transfer);
        RHI::CmdClearTexture(CL, NamedImages[(int)ENamedImage::ProbePrefiltered].Texture, Black);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);
        const uint64 ClearValue = RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});
        RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Graphics), ClearValue);

        NameOwnedImages(NamedImages);
    }

    void FDefaultSceneRenderer::SyncProbeCaptureCube(uint32 FaceSize)
    {
        if (FaceSize == 0 || FaceSize == ProbeCaptureCubeSize)
        {
            return;
        }

        RetireSceneImage(NamedImages[(int)ENamedImage::ProbeCaptureCube]);

        RHI::FTextureDesc Desc;
        Desc.Type       = RHI::ETextureType::TexCube;
        Desc.Dimension  = FUIntVector3(FaceSize, FaceSize, 1);
        Desc.LayerCount = 6;
        // Matches the HDR scene color it is copied from, which keeps that copy a format-identical blit.
        Desc.Format     = EFormat::RGBA16_FLOAT;
        Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;

        NamedImages[(int)ENamedImage::ProbeCaptureCube] = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ false);
        ProbeCaptureCubeSize = FaceSize;

        NameOwnedImages(NamedImages);
    }

    void FDefaultSceneRenderer::SyncIBLResolution(const FIBLBakeResolution& Resolution)
    {
        if (Resolution == AppliedIBLResolution)
        {
            return;
        }

        RetireSceneImage(NamedImages[(int)ENamedImage::SkyCube]);
        RetireSceneImage(NamedImages[(int)ENamedImage::SkyIrradiance]);
        RetireSceneImage(NamedImages[(int)ENamedImage::SkyPrefilter]);

        InitSkyCube(Resolution.SkyCube);
        InitIBLConvolutionTargets(Resolution);

        for (FSceneView& View : SceneViews)
        {
            View.Images[(int)ENamedImage::SkyCube]       = BorrowSceneImage(NamedImages[(int)ENamedImage::SkyCube]);
            View.Images[(int)ENamedImage::SkyIrradiance] = BorrowSceneImage(NamedImages[(int)ENamedImage::SkyIrradiance]);
            View.Images[(int)ENamedImage::SkyPrefilter]  = BorrowSceneImage(NamedImages[(int)ENamedImage::SkyPrefilter]);
        }

        AppliedIBLResolution = Resolution;
    }

    void FDefaultSceneRenderer::InitFrameResources()
    {
        FSceneView& Primary = SceneViews[0];

        // Output's heap slot survives the resize; only the texture behind it is replaced.
        const uint32 OutputSlot = DetachSampledSlot(Primary.Output);

        ReleaseViewImages(Primary);
        InitViewImages(Primary, OutputSlot);
    }

    void FDefaultSceneRenderer::EnsureStreamingFeedbackBuffer()
    {
        // One uint per MATERIAL slot, not per texture; the streamer expands that to textures.
        if (StreamingFeedbackBuffer)
        {
            return;
        }

        // Fixed rather than tracking the material table, so the address never moves under a readback.
        StreamingFeedbackSlots = 4096;
        const uint64 Bytes     = (uint64)StreamingFeedbackSlots * sizeof(uint32);

        StreamingFeedbackBuffer = CreateSceneBuffer(Bytes, "Streaming.Feedback");
        for (uint32 i = 0; i < RHI::kFramesInFlight; ++i)
        {
            StreamingFeedbackReadback[i] = RHI::Malloc(Bytes, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            RHI::SetDebugName(StreamingFeedbackReadback[i].Gpu, "Readback.StreamingFeedback");
            StreamingFeedbackStamp[i] = 0;
        }
    }

    void FDefaultSceneRenderer::CollectStreamingFeedback(RHI::FCmdListH CL)
    {
        if (!StreamingFeedbackBuffer || StreamingFeedbackSlots == 0)
        {
            return;
        }

        const uint32 Slot = CurrentFrameSlot;
        if (!StreamingFeedbackReadback[Slot])
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Streaming Feedback");

        // Every material lane that reports into the mask has run by now, so it is complete.
        RHI::CmdBarrier(CL, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute, RHI::EStageFlags::Transfer);
        RHI::CmdMemcpy(CL, { StreamingFeedbackReadback[Slot].Gpu, StreamingFeedbackBuffer.Size }, StreamingFeedbackBuffer);

        // Zero AFTER the copy, so the next frame's mask is what it sampled, not a growing union.
        RHI::Barriers::TransferToTransfer(CL);
        RHI::CmdMemzero(CL, StreamingFeedbackBuffer);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute);

        StreamingFeedbackStamp[Slot] = ++StreamingFeedbackFrame;
    }

    void FDefaultSceneRenderer::PublishStreamingFeedback()
    {
        FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet();
        if (Streaming == nullptr || StreamingFeedbackSlots == 0)
        {
            return;
        }

        // The slot written kFramesInFlight ago, whose copy the frame ring has already waited on.
        const uint32 Slot = (CurrentFrameSlot + 1u) % RHI::kFramesInFlight;
        if (StreamingFeedbackStamp[Slot] == 0 || !StreamingFeedbackReadback[Slot])
        {
            return;
        }

        const uint32* Masks = StreamingFeedbackReadback[Slot].CpuAs<const uint32>();
        if (Masks == nullptr)
        {
            return;
        }

        Streaming->SubmitMaterialFeedback(Masks, StreamingFeedbackSlots);
    }

    uint64 FDefaultSceneRenderer::BuildViewSceneRoot(FSceneView& View)
    {
        RHI::FTransientAlloc Alloc = RHI::AllocTransient(sizeof(FSceneRoot));
        FSceneRoot* Root = static_cast<FSceneRoot*>(Alloc.Cpu);

        *Root = SceneRootShared;
        Root->Clusters           = { View.ClusterBuffer };
        Root->BRDFLutIndex       = (uint32)View.Images[(int)ENamedImage::BRDFLut].GetResourceID();
        Root->SkyIrradianceIndex = (uint32)View.Images[(int)ENamedImage::SkyIrradiance].GetResourceID();
        {
            const FSceneImage& Prefilter = View.Images[(int)ENamedImage::SkyPrefilter];
            const uint32 PrefilterID     = (uint32)Prefilter.GetResourceID();
            Root->SkyPrefilterIndex = (PrefilterID & 0x00FFFFFFu) | (Prefilter.GetNumMips() << 24);
        }
        Root->SkyCubeIndex       = (uint32)View.Images[(int)ENamedImage::SkyCube].GetResourceID();
        Root->ShadowCascadeIndex = (uint32)GetNamedImage(ENamedImage::Cascade).GetResourceID();
        Root->ShadowAtlasIndex   = (uint32)ShadowAtlas.GetImage().GetResourceID();

        Root->Splines       = SplineBufferSpan;
        Root->SplinePoints  = SplinePointBufferSpan;
        Root->SplineSamples = SplineSampleBufferSpan;

        // Re-read every frame, since the slab moves when it grows and only the root holds its address.
        Root->MeshletHeaders = RHI::TGPUSpan<FMeshletHeaderGPU>::FromAddress(MeshletHeaderSlab::GetAddress(),
                                                                            MeshletHeaderSlab::GetCapacity());

        // Only the primary view reports, or a probe bake would drive residency for a 64px cube face.
        const bool bWantsFeedback = View.bIsPrimary && !bCapturingProbe;
        Root->StreamingFeedback = bWantsFeedback
            ? RHI::TGPUSpan<uint32>{ StreamingFeedbackBuffer, StreamingFeedbackSlots }
            : RHI::TGPUSpan<uint32>{};

        const FSceneImage& ProbeArray = NamedImages[(int)ENamedImage::ProbePrefiltered];
        if (!bCapturingProbe && NumActiveProbes > 0 && ProbeArray.IsValid())
        {
            Root->ReflectionProbes    = ProbeBufferSpan;
            Root->ProbeCubeArrayIndex = ((uint32)ProbeArray.GetResourceID() & 0x00FFFFFFu) | (ProbeArray.GetNumMips() << 24);
        }
        return Alloc.Gpu;
    }

    RHI::FPipelineH FDefaultSceneRenderer::GetOrCreatePipeline(const FGraphicsPipelineKey& Key)
    {
        size_t Seed = 0;
        Hash::HashCombine(Seed, Key.VS.Handle);
        Hash::HashCombine(Seed, Key.PS.Handle);
        Hash::HashCombine(Seed, Key.MS.Handle);
        Hash::HashCombine(Seed, ((uint64)Key.Topology) | ((uint64)Key.bWireframe << 8) |
                                ((uint64)Key.bAlphaToCoverage << 9) | ((uint64)Key.SampleCount << 16) |
                                ((uint64)Key.DepthFormat << 24) |
                                ((uint64)Key.ShadingFeatures << 40) | ((uint64)Key.bVisBufferMasked << 56) |
                                ((uint64)Key.SkinnedMode << 57) | ((uint64)Key.TriCullMode << 59) |
                                ((uint64)Key.SkyMode << 61) |
                                0ull);
        for (const RHI::FColorTarget& Target : Key.ColorTargets)
        {
            const RHI::FBlendDesc& B = Target.Blend;
            uint64 Bits = (uint64)Target.Format;
            Bits = (Bits << 1)  | (uint64)B.bBlendEnable;
            Bits = (Bits << 3)  | (uint64)B.ColorOp;
            Bits = (Bits << 4)  | (uint64)B.SrcColorFactor;
            Bits = (Bits << 4)  | (uint64)B.DstColorFactor;
            Bits = (Bits << 3)  | (uint64)B.AlphaOp;
            Bits = (Bits << 4)  | (uint64)B.SrcAlphaFactor;
            Bits = (Bits << 4)  | (uint64)B.DstAlphaFactor;
            Bits = (Bits << 4)  | (uint64)B.ColorWriteMask;
            Hash::HashCombine(Seed, Bits);
        }

        {
            FReadScopeLock Lock(PipelineCacheMutex);
            auto It = PipelineCache.find(Seed);
            if (It != PipelineCache.end())
            {
                return It->second;
            }
        }

        RHI::FRasterDesc Desc;
        Desc.Topology         = Key.Topology;
        Desc.bWireframe       = Key.bWireframe;
        Desc.bAlphaToCoverage = Key.bAlphaToCoverage;
        Desc.SampleCount      = Key.SampleCount;
        Desc.DepthFormat      = Key.DepthFormat;
        Desc.ColorTargets     = TSpan<const RHI::FColorTarget>(Key.ColorTargets.data(), Key.ColorTargets.size());

        // A handle that no longer resolves was freed since the key was built, so bail.
        const FShaderEntry* VSEntry = FShaderLibrary::Resolve(Key.VS);
        const FShaderEntry* PSEntry = FShaderLibrary::Resolve(Key.PS);
        const FShaderEntry* MSEntry = FShaderLibrary::Resolve(Key.MS);

        if ((Key.MS != nullptr && MSEntry == nullptr) || (Key.MS == nullptr && VSEntry == nullptr))
        {
            LOG_WARN("Pipeline: shader entry was released before the pipeline was built; the caller is "
                     "holding a superseded handle and must re-resolve.");
            return {};
        }

        const RHI::FShaderSource PSSource = PSEntry != nullptr ? PSEntry->Source() : RHI::FShaderSource{};

        auto MakeUInt = [](uint32 Id, uint32 Value) -> RHI::FSpecializationConstant
        {
            return RHI::FSpecializationConstant{ .ConstantID = Id, .AsInt = (uint64)Value, .Type = RHI::ESpecializationConstantType::UInt32 };
        };
        const RHI::FSpecializationConstant SpecConsts[] =
        {
            MakeUInt(1, (Key.ShadingFeatures & SF_DebugViews) ? 1u : 0u),
            MakeUInt(2, (Key.ShadingFeatures & SF_Decals)     ? 1u : 0u),
            MakeUInt(3, (Key.ShadingFeatures & SF_GTAO)       ? 1u : 0u),
            MakeUInt(4, Key.bVisBufferMasked ? 1u : 0u),
            MakeUInt(5, (uint32)Key.SkinnedMode),   // SPEC_SKINNED 0=static, 1=skinned, 2=dynamic
            MakeUInt(6, (Key.ShadingFeatures & SF_ShadowMask) ? 1u : 0u),
            MakeUInt(7, (uint32)Key.TriCullMode),   // SPEC_TRI_CULL, per-triangle rejects
            MakeUInt(8, (uint32)Key.SkyMode),       // SPEC_SKY_MODE, GSkyMode_Runtime = branch at runtime
        };
        const TSpan<const RHI::FSpecializationConstant> Consts(SpecConsts, 8);

        FWriteScopeLock Lock(PipelineCacheMutex);
        if (auto Existing = PipelineCache.find(Seed); Existing != PipelineCache.end())
        {
            return Existing->second;
        }

        // Task-less by construction, since MeshletCull.slang compacted before any draw was recorded.
        RHI::FPipelineH Pipeline = MSEntry != nullptr
            ? RHI::CreateMeshShaderPipeline(RHI::FShaderSource{}, MSEntry->Source(), PSSource, Desc, Consts)
            : RHI::CreateGraphicsPipeline(VSEntry->Source(), PSSource, Desc, Consts);
        PipelineCache.emplace(Seed, Pipeline);

#if USING(WITH_EDITOR)
        if (Key.PS != nullptr && !FShaderLibrary::HasPipelineStats(Key.PS))
        {
            TVector<RHI::FPipelineStat> Stats;
            if (RHI::GetPipelineStatistics(Pipeline, Stats))
            {
                FShaderLibrary::PublishPipelineStats(Key.PS, Move(Stats));
            }
        }
#endif

        return Pipeline;
    }

    RHI::FPipelineH FDefaultSceneRenderer::GetOrCreateComputePipeline(FShaderH CS,
        TSpan<const RHI::FSpecializationConstant> Constants)
    {
        size_t Seed = 0;
        Hash::HashCombine(Seed, CS.Handle);
        Hash::HashCombine(Seed, 0xC0C0C0C0ull);   // disambiguate from graphics keys
        
        for (const RHI::FSpecializationConstant& Constant : Constants)
        {
            Hash::HashCombine(Seed, Constant.ConstantID);
            Hash::HashCombine(Seed, Constant.AsInt);
            Hash::HashCombine(Seed, (uint32)Constant.Type);
        }

        {
            FReadScopeLock Lock(PipelineCacheMutex);
            auto It = PipelineCache.find(Seed);
            if (It != PipelineCache.end())
            {
                return It->second;
            }
        }

        // See GetOrCreatePipeline for why the write lock spans creation and re-checks.
        FWriteScopeLock Lock(PipelineCacheMutex);
        if (auto Existing = PipelineCache.find(Seed); Existing != PipelineCache.end())
        {
            return Existing->second;
        }

        const FShaderEntry* CSEntry = FShaderLibrary::Resolve(CS);
        if (CSEntry == nullptr)
        {
            LOG_WARN("Pipeline: compute shader entry was released before the pipeline was built.");
            return {};
        }

        RHI::FPipelineH Pipeline = RHI::CreateComputePipeline(CSEntry->Source(), Constants);
        PipelineCache.emplace(Seed, Pipeline);

        #if USING(WITH_EDITOR)
        if (!FShaderLibrary::HasPipelineStats(CS))
        {
            TVector<RHI::FPipelineStat> Stats;
            if (RHI::GetPipelineStatistics(Pipeline, Stats))
            {
                FShaderLibrary::PublishPipelineStats(CS, Move(Stats));
            }
        }

        RHI::DumpPipelineISA(Pipeline, CSEntry->Path);
        #endif

        return Pipeline;
    }

    void FDefaultSceneRenderer::SetViewportScissor(RHI::FCmdListH CL, const FUIntVector2& Extent)
    {
        const RHI::FRect Rect{ 0, (int)Extent.x, 0, (int)Extent.y };
        RHI::CmdSetViewport(CL, Rect);
        RHI::CmdSetScissor(CL, Rect);
    }

    void FDefaultSceneRenderer::WriteBuffer(RHI::FCmdListH CL, RHI::GPUPtr Dst, const void* Data, uint64 Size)
    {
        RHI::FTransientAlloc Staging = RHI::AllocTransient(Size);
        Memory::Memcpy(Staging.Cpu, Data, Size);
        RHI::CmdMemcpy(CL, { Dst, Size }, { Staging.Gpu, Size });
    }

    void FDefaultSceneRenderer::WriteBufferRuns(RHI::FCmdListH CL, RHI::GPUPtr Dst, const void* Src, uint64 Stride,
                                               const TVector<FUIntVector2>& Runs)
    {
        uint64 Total = 0;
        for (const FUIntVector2& Run : Runs)
        {
            Total += (uint64)Run.y * Stride;
        }

        if (Total == 0)
        {
            return;
        }

        const RHI::FTransientAlloc Staging = RHI::AllocTransient(Total);
        uint8* const StagingBytes = (uint8*)Staging.Cpu;
        if (StagingBytes == nullptr)
        {
            return;
        }

        TVector<RHI::FBufferCopy>& Copies = UploadCopyScratch;
        Copies.clear();
        Copies.reserve(Runs.size());

        uint64 Cursor = 0;
        for (const FUIntVector2& Run : Runs)
        {
            const uint64 Bytes = (uint64)Run.y * Stride;
            if (Bytes == 0)
            {
                continue;
            }

            const uint64 SrcOffset = (uint64)Run.x * Stride;
            Memory::Memcpy(StagingBytes + Cursor, (const uint8*)Src + SrcOffset, Bytes);
            Copies.push_back(RHI::FBufferCopy{ { Dst + SrcOffset, Bytes }, { Staging.Gpu + Cursor, Bytes } });
            Cursor += Bytes;
        }

        RHI::CmdMemcpyBatch(CL, TSpan<const RHI::FBufferCopy>(Copies.data(), Copies.size()));
    }

    void FDefaultSceneRenderer::StageWrite(RHI::GPUPtr Dst, const void* Data, uint64 Size)
    {
        if (Size == 0)
        {
            return;
        }

        RHI::FTransientAlloc Staging = RHI::AllocTransient(Size);
        Memory::Memcpy(Staging.Cpu, Data, Size);
        StagedWrites.push_back(RHI::FBufferCopy{ { Dst, Size }, { Staging.Gpu, Size } });
    }

    void FDefaultSceneRenderer::FlushStagedWrites(RHI::FCmdListH CL)
    {
        RHI::CmdMemcpyBatch(CL, TSpan<const RHI::FBufferCopy>(StagedWrites.data(), StagedWrites.size()));
        StagedWrites.clear();
    }

    void FDefaultSceneRenderer::ResizeBufferIfNeeded(RHI::FCmdListH CL, RHI::FGPUAllocation& Buffer, uint64 NeededSize,
                                                   float SlackFactor, uint32& LowUsageCounter,
                                                   bool bAllowShrink, EBufferInit Init, const char* DebugName)
    {
        NeededSize = Math::Max<uint64>(NeededSize, 16ull);

        auto AlignUp16 = [](uint64 Size) { return (Size + 15ull) & ~15ull; };

        // Allocates before retiring the old one, so a failed grow leaves the previous usable.
        const auto Reallocate = [&]() -> bool
        {
            const RHI::FGPUAllocation Grown = CreateSceneBuffer(AlignUp16((uint64)((double)NeededSize * SlackFactor)), DebugName);
            if (!Grown)
            {
                return false;
            }

            DeferFree(Buffer);
            Buffer = Grown;
            LowUsageCounter = 0;

            if (Init == EBufferInit::Zeroed)
            {
                RHI::CmdMemzero(CL, Buffer);
                RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::IndirectArguments);
            }
            #if !defined(LE_SHIPPING)
            else if (CVarPoisonUninitializedBuffers.GetValue())
            {
                RHI::CmdMemset(CL, Buffer, kUninitializedBufferPoison);
                RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::IndirectArguments);
            }
            #endif
            return true;
        };

        if (NeededSize > Buffer.Size)
        {
            if (!Reallocate())
            {
                LOG_ERROR("RenderScene: scene buffer '{}' could not grow to {} MiB; keeping the {} MiB it "
                          "already has and running degraded.",
                          DebugName != nullptr ? DebugName : "<unnamed>",
                          NeededSize / (1024ull * 1024ull), Buffer.Size / (1024ull * 1024ull));
            }
            return;
        }

        // Shrink after sustained low usage (<25% of capacity).
        if (bAllowShrink && NeededSize * 4ull < Buffer.Size)
        {
            if (++LowUsageCounter >= 120u)
            {
                Reallocate();
            }
        }
        else
        {
            LowUsageCounter = 0;
        }
    }

    void FDefaultSceneRenderer::DeferFree(const RHI::FGPUAllocation& Allocation)
    {
        RHI::Retire(Allocation);
    }

    //~ End new-RHI helpers

    uint32 FDefaultSceneRenderer::GetDisplayResourceID() const
    {
        if (SceneViews.empty())
        {
            return ~0u;
        }
        const int32 ID = SceneViews[0].Output.GetResourceID();
        return ID < 0 ? ~0u : (uint32)ID;
    }

    FUIntVector2 FDefaultSceneRenderer::GetRenderExtent() const
    {
        return SceneViews.empty() ? FUIntVector2(0) : SceneViews[0].Size;
    }
}
