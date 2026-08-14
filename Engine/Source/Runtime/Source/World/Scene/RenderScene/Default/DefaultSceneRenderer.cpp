#include "RuntimePCH.h"
#include "DefaultSceneRenderer.h"
#include <algorithm>
#include "Animation/SkeletalMeshUtils.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Config/EngineSettings.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Windows/Window.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "Renderer/MeshletHeaderSlab.h"
#include "Renderer/RendererUtils.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/RHITexture.h"
#include "Renderer/RenderManager.h"
#include "Renderer/TextureStreamingManager.h"
#include "TaskSystem/TaskGraph.h"
#include "TaskSystem/TaskSystem.h"
#include "Tools/Import/ImportHelpers.h"
#include "UI/RmlUiBridge.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/BillboardComponent.h"
#include "World/Entity/Components/WidgetComponent.h"
#include "World/Entity/Components/TextComponent.h"
#include "Tools/FontManager/FontManager.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/ExponentialHeightFogComponent.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/Entity/Components/LineBatcherComponent.h"
#include "World/Entity/Components/TriangleBatcherComponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/Entity/Components/DecalComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/ReflectionProbeComponent.h"
#include "World/Entity/Components/SplineComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Scene/RenderScene/EnvironmentRenderTypes.h"
#include "World/Scene/RenderScene/MeshDrawCommand.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "World/Scene/RenderScene/TerrainMeshletBuilder.h"
#include "World/Scene/RenderScene/TerrainRenderTypes.h"
#include "World/Subsystems/WorldSettings.h"
#include "Renderer/SMAA/AreaTex.h"
#include "Renderer/SMAA/SearchTex.h"
#include "TaskSystem/FiberSync.h"
#include "Log/Log.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    namespace
    {
        constexpr uint32 GFroxelGridX = 160;
        constexpr uint32 GFroxelGridY = 90;
        constexpr uint32 GFroxelGridZ = 128;

        // Hard cap on non-sun volumetric lights per frame; packed into the inject push constants.
        constexpr uint32 GFroxelMaxLocalLights = 16;


        static TAtomic<uint32> GReflectionProbeRebakeRequests{0};

        static FAutoConsoleCommand GCmdRebakeReflectionProbes(
            "r.ReflectionProbes.Rebake",
            "Recapture every reflection probe. Needed after moving world geometry, which does not itself "
            "invalidate a bake (only changing a probe does).",
            []{ RequestReflectionProbeRebake(); });


        // One grain for the whole gather: all primitive types share a single dense array.
        constexpr uint32 GPrimitiveGrain = 256;


        // Uncapped, a large skinned crowd asks for tens of GB and skinning writes past the allocation.
        // 10.5 Mi at the 32 B stride is the same 336 MB the old 12 Mi cost at 28 B.
        constexpr uint32 GMaxPreSkinnedVertices = (21 * 1024 * 1024) / 2;

        // A zero grain would ask for an unbounded task split; floor it.
    }

    void RequestReflectionProbeRebake()
    {
        GReflectionProbeRebakeRequests.fetch_add(1, std::memory_order_relaxed);
    }

    struct FScopedGPUMarker
    {
        RHI::FCmdListH CL;
        FScopedGPUMarker(RHI::FCmdListH InCL, const char* Name) : CL(InCL) { RHI::CmdBeginMarker(CL, Name); }
        ~FScopedGPUMarker() { RHI::CmdEndMarker(CL); }
    };
    #define SCENE_MARKER_CONCAT_INNER(A, B) A##B
    #define SCENE_MARKER_CONCAT(A, B) SCENE_MARKER_CONCAT_INNER(A, B)
    #define SCENE_GPU_SCOPE(InCL, Name) FScopedGPUMarker SCENE_MARKER_CONCAT(GpuMarker_, __LINE__)(InCL, Name)
    
    namespace Barriers = RHI::Barriers;

    FDefaultSceneRenderer::FDefaultSceneRenderer(CWorld* InWorld)
        : IRenderScene(InWorld)
        , ShadowAtlas(FShadowAtlasConfig())
    {
    }

    static uint8 ResolveVisBufferSampleCount(EMSAASampleCount RequestedSetting)
    {
        const uint8 Requested = ::Lumina::GetMSAASampleCount(RequestedSetting);
        if (Requested > 1u)
        {
            static bool bWarned = false;
            if (!bWarned)
            {
                bWarned = true;
                LOG_WARN("MSAA ({}x) is not supported by the VisBuffer renderer; rendering at 1x with SMAA.", (uint32)Requested);
            }
            return 1u;
        }
        return Requested;
    }

    void FDefaultSceneRenderer::Init()
    {
        LUMINA_MEMORY_SCOPE("Render Scene");

        RHI::WaitDeviceIdle();

        const SDefaultWorldSettings& InitSettings = World ? World->GetDefaultWorldSettings() : SDefaultWorldSettings{};
        MSAASampleCount = ResolveVisBufferSampleCount(InitSettings.MSAASampleCount);

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
            FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

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
            const char* IconFiles[7] =
            {
                "/Textures/PointLight.png", "/Textures/DirectionalLight.png", "/Textures/SkyLight.png",
                "/Textures/SpotLight.png", "/Textures/CameraIcon.png", "/Textures/PersonIcon.png",
                "/Textures/Molecule.png"
            };
            for (int i = 0; i < 7; ++i)
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
        const ENamedImage IconSlots[7] =
        {
            ENamedImage::PointLightIcon, ENamedImage::DirectionalLightIcon, ENamedImage::SkyLightIcon,
            ENamedImage::SpotLightIcon, ENamedImage::CameraIcon, ENamedImage::CharacterIcon,
            ENamedImage::ParticleSystemIcon
        };
        for (int i = 0; i < 7; ++i)
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
            FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);
            ECS::Utils::SetPublishMovedTransforms(Registry, false);
            MovedTransformScratch.clear();
            ECS::Utils::DrainMovedTransforms(Registry, MovedTransformScratch);
            MovedTransformScratch.clear();

            ScenePrimitives.Reset(&Registry);
        }

        // Per-view images + cluster buffers.
        for (FSceneView& View : SceneViews)
        {
            ReleaseViewImages(View, /*bDeferRelease*/ false);
            if (View.ClusterBuffer)
            {
                RHI::Free(View.ClusterBuffer.Ptr);
                View.ClusterBuffer = {};
            }
        }
        SceneViews.clear();
        CurrentView = nullptr;

        for (FSceneImage& Image : NamedImages)
        {
            if (Image.bOwned)
            {
                ReleaseSceneImage(Image);
            }
        }

        // Persistent GPU buffers + rings.
        auto FreeBuffer = [](FSceneBuffer& Buffer)
        {
            if (Buffer)
            {
                RHI::Free(Buffer.Ptr);
                Buffer = {};
            }
        };
        FreeBuffer(PreSkinnedVerticesBuffer);

        FreeBuffer(RetainedCullEntryBuffer);
        FreeBuffer(RetainedTransformBuffer);
        FreeBuffer(RetainedStaticBuffer);
        FreeBuffer(SurfaceDescBuffer);
        FreeBuffer(BoneArenaBuffer);
        FreeBuffer(SkinnedFrameDataBuffer);
        FreeBuffer(SkinnedSlotListBuffer);

        for (uint32 Slot = 0; Slot < RHI::kFramesInFlight; ++Slot)
        {
            FreeBuffer(RenderBucketRing[Slot]);
            FreeBuffer(MeshletDrawListRing[Slot]);
            FreeBuffer(MeshDrawArgsRing[Slot]);
            FreeBuffer(SpdCounterRing[Slot]);
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
            FreeBuffer(CullCounterRing[Slot]);
            FreeBuffer(TotalsRing[Slot]);

            // Raw GPUPtr rather than an FSceneBuffer (CPURead allocation, persistently mapped).
            if (MeshletBoundReadback[Slot] != 0)
            {
                RHI::Free(MeshletBoundReadback[Slot]);
                MeshletBoundReadback[Slot] = 0;
            }
        }

        // Render-phase-owned terrain / particle GPU state.
        for (auto& [Entity, State] : TerrainGPUStates)
        {
            ReleaseSceneImage(State.HeightmapTexture);
            ReleaseSceneImage(State.NormalTexture);
            ReleaseSceneImage(State.LayerWeightTexture);
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
                if (State.ParticleBuffer)     { RHI::Free(State.ParticleBuffer); }
                if (State.SpawnCounterBuffer) { RHI::Free(State.SpawnCounterBuffer); }
                if (State.AttributeBuffer)    { RHI::Free(State.AttributeBuffer); }
            }
        }
        ParticleGPUStates.clear();

        #if USING(WITH_EDITOR)
        for (FPickerReadbackSlot& Slot : PickerReadbackRing)
        {
            if (Slot.Readback != 0)
            {
                RHI::Free(Slot.Readback);
                Slot.Readback = 0;
            }
        }
        #endif

        // Pipeline + depth-state caches.
        for (auto& [Hash, Pipeline] : PipelineCache)
        {
            RHI::FreeH(Pipeline);
        }
        PipelineCache.clear();
        for (auto& [Hash, State] : DepthStateCache)
        {
            RHI::FreeH(State);
        }
        DepthStateCache.clear();
    }

    namespace
    {
        // Defined in the terrain helper block below; Extract prep.
        void PrepareTerrainExtract(STerrainComponent& Terrain, const FMatrix4& WorldMatrix, FDefaultSceneRenderer::FFrameData::FTerrainExtract& Out);
    }

    void FDefaultSceneRenderer::Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Render Scene");

        ExtractFrame = &FrameData;
        FFrameData& Frame = *ExtractFrame;

        Frame.bExtractedThisFrame  = false;
        Frame.CachedWorldSettings  = World->GetDefaultWorldSettings();
        Frame.CachedWorldDeltaTime = (float)World->GetWorldDeltaTime();
        Frame.ViewVolume           = ViewVolume;

        // PostProcess is a stack temporary in CWorld::Extract -- value-copy it.
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
            if (PPInterface == nullptr || !PPInterface->IsReadyForRender())
            {
                continue;
            }
            CMaterial* PPMaterial = PPInterface->GetMaterial();
            if (PPMaterial == nullptr || PPMaterial->GetMaterialType() != EMaterialType::PostProcess)
            {
                continue;
            }
            const FShaderH VS = PPMaterial->GetVertexShader();
            const FShaderH PS = PPMaterial->GetPixelShader();
            if (VS == nullptr || PS == nullptr)
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
        SceneGlobalData.ScreenSize                      = FUIntVector4(PrimarySize.x, PrimarySize.y, 0, 0);
        SceneGlobalData.GridSize                        = FUIntVector4(ClusterGridSizeX, ClusterGridSizeY, ClusterGridSizeZ, 0);
        SceneGlobalData.Time                            = (float)World->GetTimeSinceWorldCreation();
        SceneGlobalData.DeltaTime                       = Frame.CachedWorldDeltaTime;
        SceneGlobalData.FarPlane                        = ViewVolume.GetFar();
        SceneGlobalData.NearPlane                       = ViewVolume.GetNear();
        SceneGlobalData.GTAOSettings                    = FGTAOSettings{};
        SceneGlobalData.GTAOSettings.Radius             = Frame.CachedWorldSettings.GTAORadius;
        SceneGlobalData.GTAOSettings.Intensity          = Frame.CachedWorldSettings.GTAOIntensity;
        SceneGlobalData.GTAOSettings.Power              = Frame.CachedWorldSettings.GTAOPower;
        SceneGlobalData.ParallaxSettings.SampleScale       = 1.0f;
        SceneGlobalData.ParallaxSettings.LODBias           = 0.0f;
        SceneGlobalData.ParallaxSettings.ShadowSampleScale = 1.0f;
        Frame.CameraFrustum                             = ViewVolume.GetFrustum();
        SceneGlobalData.CullData.Frustum                = AsGPU(Frame.CameraFrustum);
        SceneGlobalData.CullData.ShadowFrustum          = SceneGlobalData.CullData.Frustum; // Rebuilt after directional light is processed.
        SceneGlobalData.CullData.bHasDirectional        = 0u;
        SceneGlobalData.CullData.bFrustumCull           = RenderSettings.bFrustumCull;
        SceneGlobalData.CullData.bOcclusionCull         = RenderSettings.bOcclusionCull;

        // Copied, not aliased: freezing the cull replaces these and leaves the render camera alone.
        SceneGlobalData.CullData.CullCameraPosition     = SceneGlobalData.CameraData.Location;
        SceneGlobalData.CullData.CullCameraView         = SceneGlobalData.CameraData.View;
        SceneGlobalData.CullData.CullCameraProjection   = SceneGlobalData.CameraData.Projection;
        SceneGlobalData.CullData.ShadowMaxDistance      = 5000.0f;
        SceneGlobalData.CullData.bShadowOcclusionCull   = RenderSettings.bShadowOcclusionCull;
        SceneGlobalData.CullData.ShadowLODBias          = RenderSettings.ShadowLODBias;
        SceneGlobalData.CullData.ShadowCoarseLODDistSq  = RenderSettings.ShadowCoarseLODDistance
                                                        * RenderSettings.ShadowCoarseLODDistance;
        CascadeMinTexels                                = 1.0f;
        SceneGlobalData.CullData.DebugMode              = (uint32)RenderSettings.Flags;
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

        // CPU half: parallel ECS gather + cull setup.
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
            }
        }

        Frame.Lighting.AtlasTiles = ShadowAtlas.GetAllocatedTiles();

        for (uint32 Channel = 0; Channel < FImmediateLineRenderer::NumChannels; ++Channel)
        {
            Frame.Primitives.ImmediateLines[Channel] = ImmediateLines.Snapshot((FImmediateLineRenderer::EChannel)Channel);
        }

        Frame.bExtractedThisFrame = true;

        ExtractFrame = nullptr;
    }

    void FDefaultSceneRenderer::ExtractSplines(FEntityRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Extract Splines");

        auto& Splines = Frame.Splines.Splines;
        auto& Points  = Frame.Splines.Points;
        auto& Samples = Frame.Splines.Samples;
        Splines.clear();
        Points.clear();
        Samples.clear();

        auto SplineView = Registry.view<SSplineComponent, STransformComponent>(entt::exclude<SDisabledTag>);

        TVector<FSplineSample> ScratchSamples;

        SplineView.each([&](entt::entity Entity, const SSplineComponent& Spline, const STransformComponent& Transform)
        {
            // The opt-in is the whole point: a spline used as pure authoring data must cost no upload.
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
            Header.EntityID     = (uint32)entt::to_integral(Entity);
            Header._Pad         = 0u;

            // Control points go up in WORLD space to match the samples, so a shader never has to know
            // which of the two arrays it is reading from to decide whether a transform is needed.
            for (const SSplinePoint& Point : Spline.Points)
            {
                FGPUSplinePoint Gpu;
                Gpu.Location      = FVector3(LocalToWorld * FVector4(Point.Location, 1.0f));
                Gpu.Roll          = Point.Roll;
                // Tangents are directions: w = 0 so the translation stays out of them.
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

    void FDefaultSceneRenderer::ExtractReflectionProbes(FEntityRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Extract Reflection Probes");

        auto& Probes   = Frame.ReflectionProbes.Probes;
        auto& Captures = Frame.ReflectionProbes.Captures;
        Probes.clear();
        Captures.clear();
        Frame.ReflectionProbes.bNeedsRebake = false;

        auto ProbeView = Registry.view<SReflectionProbeComponent, STransformComponent>(entt::exclude<SDisabledTag>);

        struct FProbeSortEntry
        {
            int32                   Priority;
            FGPUReflectionProbe     Gpu;
            FReflectionProbeCapture Capture;
        };
        TVector<FProbeSortEntry> Sorted;

        ProbeView.each([&](entt::entity Entity, const SReflectionProbeComponent& Probe, const STransformComponent& Transform)
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

        eastl::stable_sort(Sorted.begin(), Sorted.end(), [](const FProbeSortEntry& A, const FProbeSortEntry& B)
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

    void FDefaultSceneRenderer::PrepareRender(uint8 /*FrameIndex*/)
    {
        LUMINA_PROFILE_SCOPE();

        FFrameData& Frame = FrameData;
        if (!Frame.bExtractedThisFrame)
        {
            return;
        }

        SyncIBLResolution(Frame.Volumetrics.IBLResolution);

        // Same reason: the scratch cube may need resizing for this frame's bake.
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

        // SyncMSAAState reads Frame.CachedWorldSettings, so RenderFrame must be set first.
        SyncMSAAState();

        if (!Frame.bExtractedThisFrame)
        {
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

        // Publish this frame's stats for the editor-side GetRenderStats() reader.
        RenderStats = Frame.FrameStats;

        // From here the frame is submitted, so the pyramid this frame builds is usable next frame.
        bDepthPyramidValid.store(true, std::memory_order_release);

        RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
        // Projection bakes the Vulkan Y-flip, so CCW-wound geometry lands clockwise in framebuffer space.
        RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CW);
        
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
                
                if (!RenderSettings.bFreezeCulling)
                {
                    SCENE_GPU_SCOPE(CL, "Depth Pyramid (Mid)");
                    DepthPyramidPass(CL);
                }
                
                if (InstanceVisibilityBuffer)
                {
                    RHI::CmdMemset(CL, InstanceVisibilityBuffer.GetAddress(), InstanceVisibilityBuffer.GetSize(), 0u);
                    Barriers::TransferToCompute(CL);
                }

                MeshletCullPass(CL, EMeshletSlice::Late);

                {
                    SCENE_GPU_SCOPE(CL, "VisBuffer Late");
                    VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ false, ECullPhase::Late);
                }
                
                if (!RenderSettings.bFreezeCulling)
                {
                    SCENE_GPU_SCOPE(CL, "Depth Pyramid (End)");
                    DepthPyramidPass(CL);
                }


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
                
                if (!RenderSettings.bFreezeCulling)
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
                    GTAOBlurPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Shadow Mask");
                    ShadowMaskPass(CL);
                }
                
                {
                    SCENE_GPU_SCOPE(CL, "Environment");
                    EnvironmentPass(CL);
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
                
                if (!RenderSettings.bFreezeCulling)
                {
                    SCENE_GPU_SCOPE(CL, "Depth Pyramid (End)");
                    DepthPyramidPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Water");
                    WaterPass(CL);
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
                    SCENE_GPU_SCOPE(CL, "Additive Translucent");
                    AdditiveTranslucentPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Froxel Fog Inject");
                    FroxelInjectPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Froxel Fog Integrate");
                    FroxelIntegratePass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Froxel Fog Apply");
                    FroxelApplyPass(CL);
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
                    SCENE_GPU_SCOPE(CL, "Particles Render");
                    ParticleRenderPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Billboards");
                    BillboardPass(CL);
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

                if (Frame.CachedWorldSettings.SMAAQuality != ESMAAQuality::Off)
                {
                    SCENE_GPU_SCOPE(CL, "SMAA");
                    SMAAEdgeDetectionPass(CL);
                    SMAABlendWeightPass(CL);
                    SMAANeighborhoodBlendPass(CL);
                }

                #if USING(WITH_EDITOR)
                // After SMAA: the outline is a fixed-width UI affordance, and letting the neighbourhood
                // blend chew on a 2px line softens it into a smear. Before Widgets so UI still draws over it.
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

                    CurrentSceneRootAddr = BuildViewSceneRoot(View,
                        RHI::Core::CopyTransient(MakeSecondaryViewGlobals(Capture.SceneGlobalData)));

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

        RHI::Core::Submit(CL);

        RenderFrame = nullptr;
    }

    static constexpr uint32 GPrefilterSampleCount = 256;

    struct FPrefilterPC
    {
        uint32 SrcCubeSRV     = 0;
        uint32 OutMipUAV      = 0;
        float  Roughness      = 0.0f;
        uint32 NumSamples     = 0;
        uint32 DstLayerOffset = 0;
        uint32 _Pad0          = 0;
        uint32 _Pad1          = 0;
        uint32 _Pad2          = 0;
    };
    static_assert(sizeof(FPrefilterPC) == 32, "FPrefilterPC must match PrefilterEnvMap.slang::FPushConstants.");

    void FDefaultSceneRenderer::ReflectionProbeBakePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& Bake = Frame.ReflectionProbes;

        if (Bake.BakingProbe < 0 || Bake.BakeViewIndex <= 0 || Bake.BakeViewIndex >= (int32)SceneViews.size())
        {
            if (Bake.BakingProbe >= 0)
            {
                LOG_WARN("[Probe] Bake SKIPPED: probe={} viewIndex={} numSceneViews={}",
                         Bake.BakingProbe, Bake.BakeViewIndex, (int32)SceneViews.size());
            }
            return;
        }

        const FSceneImage& CaptureCube = NamedImages[(int)ENamedImage::ProbeCaptureCube];
        const FSceneImage& ProbeArray  = NamedImages[(int)ENamedImage::ProbePrefiltered];
        if (!CaptureCube.IsValid() || !ProbeArray.IsValid())
        {
            LOG_WARN("[Probe] Bake SKIPPED: captureCube={} probeArray={} (targets not allocated)",
                     CaptureCube.IsValid() ? 1 : 0, ProbeArray.IsValid() ? 1 : 0);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Reflection Probe Bake", tracy::Color::SkyBlue3);
        SCENE_GPU_SCOPE(CL, "Reflection Probe Bake");

        FSceneView& View = SceneViews[Bake.BakeViewIndex];

        bCapturingProbe = true;

        const uint32 FaceSize = Bake.BakeFaceSize;

        const bool     bClearToColor = Bake.Captures[Bake.BakingProbe].bClearToColor;
        const FVector3 ClearRGB      = Bake.Captures[Bake.BakingProbe].ClearColor;

        for (int32 Face = 0; Face < 6; ++Face)
        {
            if (Bake.FaceCullViews[Face] == ~0u)
            {
                continue;
            }

            PointAtView(View);
            CurrentCameraEarlyView = Bake.FaceCullViews[Face];

            CurrentSceneRootAddr = BuildViewSceneRoot(View,
                RHI::Core::CopyTransient(MakeSecondaryViewGlobals(Bake.FaceGlobals[Face])));

            if (Frame.Geometry.DrawCommands.empty())
            {
                // VisBufferPass returns early without clearing depth when there is nothing to draw.
                Barriers::AllToTransfer(CL);
                const float DepthClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::DepthAttachment).Texture, DepthClear);
                Barriers::TransferToAll(CL);
            }

            VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ true);
            TerrainCullPass(CL);
            TerrainDepthPrePass(CL);
            ClusterBuildPass(CL);
            LightCullPass(CL);

            if (bClearToColor)
            {
                const FSceneImage& ClearRT = GetSceneColorRT();

                RHI::FRenderAttachment ClearColorAttachment;
                ClearColorAttachment.Texture        = ClearRT.Texture;
                ClearColorAttachment.ResolveTexture = GetSceneColorResolve();
                ClearColorAttachment.LoadOp         = RHI::ELoadOp::Clear;
                ClearColorAttachment.StoreOp        = RHI::EStoreOp::Store;
                ClearColorAttachment.Color[0]       = ClearRGB.x;
                ClearColorAttachment.Color[1]       = ClearRGB.y;
                ClearColorAttachment.Color[2]       = ClearRGB.z;
                ClearColorAttachment.Color[3]       = 1.0f;

                RHI::FRenderPassDesc ClearPass;
                ClearPass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&ClearColorAttachment, 1);
                ClearPass.RenderArea       = GetNamedImage(ENamedImage::HDR).GetExtent();

                RHI::CmdBeginRenderPass(CL, ClearPass);
                RHI::CmdEndRenderPass(CL);
                Barriers::RasterToRead(CL);
            }
            else
            {
                EnvironmentPass(CL);
            }

            DecalPass(CL);
            ShadowMaskPass(CL);
            VisBufferClassifyPass(CL);
            MaterialGBufferPass(CL);
            DeferredLightingPass(CL);
            TerrainRenderPass(CL);

            const FSceneImage& FaceColor = GetNamedImage(ENamedImage::HDR);

            RHI::FTextureSlice SrcSlice;
            SrcSlice.Mip        = 0;
            SrcSlice.Layer      = 0;
            SrcSlice.LayerCount = 1;
            SrcSlice.Extent     = FUIntVector3(FaceSize, FaceSize, 1);

            RHI::FTextureSlice DstSlice = SrcSlice;
            DstSlice.Layer = (uint32)Face;

            Barriers::AllToTransfer(CL);
            RHI::CmdCopyTexture(CL, FaceColor.Texture, SrcSlice, CaptureCube.Texture, DstSlice);
            Barriers::TransferToAll(CL);
        }

        {
            static const FShaderH ComputeShader = FShaderLibrary::Get("PrefilterEnvMap.slang");
            if (ComputeShader != nullptr)
            {
                SCENE_GPU_SCOPE(CL, "Probe Prefilter");
                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

                const uint32 NumMips      = ProbeArray.GetNumMips();
                const uint32 BaseFaceSize = ProbeArray.GetSizeX();
                constexpr uint32 PrefilterTile = 8u;

                for (uint32 Mip = 0; Mip < NumMips; ++Mip)
                {
                    FPrefilterPC PC = {};
                    PC.SrcCubeSRV     = (uint32)CaptureCube.GetResourceID();
                    PC.OutMipUAV      = (uint32)ProbeArray.GetMipUAVIndex(Mip);
                    PC.Roughness      = (NumMips <= 1u) ? 0.0f : (float)Mip / (float)(NumMips - 1u);
                    PC.NumSamples     = GPrefilterSampleCount;
                    PC.DstLayerOffset = (uint32)Bake.BakingProbe * 6u;

                    const uint32 MipFaceSize = eastl::max<uint32>(BaseFaceSize >> Mip, 1u);
                    const uint32 GroupsXY    = RenderUtils::GetGroupCount(MipFaceSize, PrefilterTile);
                    RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
                }

                Barriers::ComputeToAll(CL);
            }
        }

        bCapturingProbe = false;

        PointAtView(SceneViews[0]);
        CurrentCameraEarlyView = 0u;

        CompletedProbeBakes.fetch_or(1u << (uint32)Bake.BakingProbe, std::memory_order_acq_rel);
    }

    FSceneGlobalData FDefaultSceneRenderer::MakeSecondaryViewGlobals(const FSceneGlobalData& ViewGlobals)
    {
        FSceneGlobalData Globals = ViewGlobals;

        const FCullData& Primary = RenderFrame->SceneGlobalData.CullData;
        Globals.CullData.MeshletDrawTag            = Primary.MeshletDrawTag;
        Globals.CullData.MeshletDrawListCapacity   = Primary.MeshletDrawListCapacity;
        Globals.CullData.InstanceNum               = Primary.InstanceNum;
        Globals.CullData.BoneNum                   = Primary.BoneNum;

        // This view's OWN camera, not the primary's: a capture culls from where it is looking, and it is
        // never the thing a cull freeze applies to.
        Globals.CullData.CullCameraPosition        = Globals.CameraData.Location;
        Globals.CullData.CullCameraView            = Globals.CameraData.View;
        Globals.CullData.CullCameraProjection      = Globals.CameraData.Projection;

        // This view's OWN sun-shadow mask. A secondary view that skipped the mask pass would read the ~0u
        // sentinel and shade every pixel fully lit -- no sun shadows in captures or probe bakes.
        const FSceneLightData& Lights   = RenderFrame->Lighting.LightData;
        RenderSettings.bShadowMaskValid = (Lights.bHasSun != 0) && (Lights.Lights[0].ShadowDataIndex != INDEX_NONE);
        Globals.ShadowMaskIndex         = RenderSettings.bShadowMaskValid
            ? (uint32)CurrentView->Images[(int)ENamedImage::ShadowMask].GetResourceID()
            : ~0u;

        // This view's OWN moment targets. Inheriting the primary's indices would have every capture
        // reconstruct transmittance from the main camera's moments.
        Globals.MomentZerothIndex = (uint32)CurrentView->Images[(int)ENamedImage::MomentZeroth].GetResourceID();
        Globals.MomentsIndex      = (uint32)CurrentView->Images[(int)ENamedImage::Moments].GetResourceID();

        return Globals;
    }

    void FDefaultSceneRenderer::RenderCaptureView(RHI::FCmdListH CL)
    {
        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            Barriers::AllToTransfer(CL);
            const float DepthClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::DepthAttachment).Texture, DepthClear);
            Barriers::TransferToAll(CL);
        }

        VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ true);
        TerrainCullPass(CL);
        TerrainDepthPrePass(CL);
        ClusterBuildPass(CL);
        LightCullPass(CL);
        EnvironmentPass(CL);
        DecalPass(CL);
        // Resolves this view's sun shadows; the deferred lighting dispatch below reads nothing else for
        // the directional term.
        ShadowMaskPass(CL);
        VisBufferClassifyPass(CL);
        MaterialGBufferPass(CL);
        DeferredLightingPass(CL);
        TerrainRenderPass(CL);
        WaterPass(CL);
        MomentGenerationPass(CL);
        TransparentPass(CL);
        OITResolvePass(CL);
        AdditiveTranslucentPass(CL);
        FroxelInjectPass(CL);
        FroxelIntegratePass(CL);
        FroxelApplyPass(CL);
        BloomPass(CL);
        AutoExposurePass(CL);
        ToneMappingPass(CL);
        PostProcessMaterialPass(CL);

        if (RenderFrame->CachedWorldSettings.SMAAQuality != ESMAAQuality::Off)
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

        ResizePrimaryView(FUIntVector2((uint32)Math::Max(NewSize.x, 1.0f), (uint32)Math::Max(NewSize.y, 1.0f)));
    }

    /**
     * Quantized, with hysteresis, because the caller feeds this the live panel size every frame and each
     * apply costs a WaitDeviceIdle plus a full realloc of the view's 22 images.
     *
     */
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
            return;
        }

        ResizePrimaryView(Target);
    }

    void FDefaultSceneRenderer::ResizePrimaryView(const FUIntVector2& NewSize)
    {
        RHI::WaitDeviceIdle();

        // Only the primary view is resized here; capture views keep their own size.
        FSceneView& Primary = SceneViews[0];
        Primary.Size = FUIntVector2(Math::Max(NewSize.x, 1u), Math::Max(NewSize.y, 1u));

        InitFrameResources();

        // New depth targets: whatever the pyramid holds is from the old extent.
        bDepthPyramidValid.store(false, std::memory_order_release);

        #if USING(WITH_EDITOR)
        // Drop old readback slots; sized to previous extent so pixel grid no longer matches clicks.
        for (FPickerReadbackSlot& Slot : PickerReadbackRing)
        {
            if (Slot.Readback != 0)
            {
                RHI::Free(Slot.Readback);
                Slot.Readback = 0;
            }
            Slot.Width = 0;
            Slot.Height = 0;
            Slot.bPending = false;
        }
        #endif
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

        if (Local.Arena.GetArena() == nullptr)
        {
            Local.ResetForFrame(FFrameArenaAllocator(&GetThreadFrameAllocator(), "RenderGather"));
            Local.Items.reserve(CurrentReservePerThread);

            Local.PrepareCounters(ScenePrimitives.GetBatches().Num());
            Local.bTouched = true;
        }
        return Local;
    }

    // Routes this frame's transform + component changes into the persistent primitive table.
    void FDefaultSceneRenderer::SyncScenePrimitives()
    {
        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        MovedTransformScratch.clear();
        if (ECS::Utils::DrainMovedTransforms(Registry, MovedTransformScratch))
        {
            FRenderDirtyTracker& Tracker = FRenderDirtyTracker::Ensure(Registry);
            for (entt::entity Entity : MovedTransformScratch)
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
                std::sort(OutList.begin(), OutList.end());
                OutList.erase(std::unique(OutList.begin(), OutList.end()), OutList.end());
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

        // Channel consumed: the decisions above are now owned by this frame.
        ScenePrimitives.ClearDirtyInstanceSlots();
        ScenePrimitives.ClearFullInstanceUpload();
        ScenePrimitives.ClearSurfaceDescsDirty();
    }

    namespace
    {
        template <typename TStorage>
        FORCEINLINE auto& PackedPayloadAt(TStorage& Storage, uint32 Index)
        {
            constexpr size_t PageSize = entt::component_traits<typename TStorage::value_type, entt::entity>::page_size;
            return Storage.raw()[Index / PageSize][Index % PageSize];
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

            // Only stamp when final; anything still loading re-arms the scan instead.
            if (Entry.bResolved)
            {
                Component.CachedEntryState = Cache.GetEntryState(Handle);
            }
            else
            {
                Component.CachedEntryState = MESH_RESOLVE_STATE_STALE;
                FMeshResolveCache::MarkPendingWork();
            }
        }

        template <typename TStorage, typename TGetMesh>
        uint32 ResolveMeshPool(TStorage& Storage, EInstanceFlags SeedFlags,
                               TVector<CMaterialInterface*>& OverrideScratch, TGetMesh&& GetMesh,
                               FRenderDirtyTracker& Tracker, EPrimitiveSource Source)
        {
            using TComponent = typename TStorage::value_type;

            const FMeshResolveCache& Cache = FMeshResolveCache::Get();

            uint32 Refreshed = 0;

            const uint32 Count = (uint32)Storage.size();
            for (uint32 i = 0; i < Count; ++i)
            {
                const entt::entity Entity = Storage.data()[i];
                if (Entity == entt::tombstone)
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

    void FDefaultSceneRenderer::ResolveDynamicMeshMaterials(FEntityRegistry& Registry, FRenderDirtyTracker& Tracker)
    {
        auto& Storage = Registry.storage<SDynamicMeshComponent>();
        const uint32 Count = (uint32)Storage.size();

        for (uint32 i = 0; i < Count; ++i)
        {
            const entt::entity Entity = Storage.data()[i];
            if (Entity == entt::tombstone)
            {
                continue;
            }

            SDynamicMeshComponent& Component = PackedPayloadAt(Storage, i);

            const TSharedPtr<FDynamicMeshRenderData> Data = Component.LoadRenderData();
            if (!Data)
            {
                continue;
            }

            // Dynamic meshes are NOT interned in FMeshResolveCache -- their geometry is per-component, so
            // there is nothing to key an entry on. That means they own no cache entry, so neither
            // InvalidateDependency nor the editor epoch can mark them stale, and this gate is the only
            // thing between a recompile and a redraw. It needs all three checks:
            //
            //   Hash   -- the override ASSIGNMENT changed (a different material in a slot).
            //   Epoch  -- the editor edited some mesh/material asset in place; blunt, editor-driven, and
            //             the same signal every cached entry gets.
            //   Stale  -- a source material recompiled and its FShaderEntry*s were superseded. This is the
            //             one the pointer hash could never see: recompiling a master leaves the same
            //             CMaterialInstance*s in the override list, so the hash never moved while the
            //             cached entries in FResolvedSurface kept pointing at dead bytecode.
            const TVector<FGeometrySurface>& Geometry = Data->Resource.GeometrySurfaces;
            uint32 Hash = 2166136261u;
            for (SIZE_T s = 0; s < Data->Surfaces.size() && s < Geometry.size(); ++s)
            {
                const void* Material = Component.GetMaterialForSlot((size_t)Geometry[s].MaterialIndex);
                const uint64 Bits    = (uint64)(uintptr_t)Material;
                for (uint32 b = 0; b < 8u; ++b)
                {
                    Hash ^= (uint32)((Bits >> (b * 8u)) & 0xFFull);
                    Hash *= 16777619u;
                }
            }
            // 0 is the never-resolved sentinel, so a real hash must not collide with it.
            Hash |= 1u;

            const uint32 Epoch = FMeshResolveCache::GetEpoch();

            bool bShaderStale = false;
            for (const FResolvedSurface& Surface : Data->Surfaces)
            {
                if (MeshResolve::IsSurfaceStale(Surface))
                {
                    bShaderStale = true;
                    break;
                }
            }

            if (Hash == Component.CachedMaterialHash
                && Epoch == Component.CachedResolveEpoch
                && !bShaderStale
                && Data->bAllMaterialsReady)
            {
                continue;
            }

            Component.RefreshResolvedMaterials();
            Component.CachedMaterialHash  = Hash;
            Component.CachedResolveEpoch  = Epoch;

            Tracker.Mark(Entity, EPrimitiveSource::DynamicMesh, EPrimitiveDirty::Data);

            if (!Data->bAllMaterialsReady)
            {
                FMeshResolveCache::MarkPendingWork();
            }
        }

        ValidateNoStaleResolves(Registry);
    }

    // Editor-only. A resolved surface holds FShaderEntry*s into a content-keyed library, so a recompile
    // silently supersedes them; the gates above are what re-resolve, and a gate missing an input fails
    // ~invisibly -- geometry keeps drawing, just with last build's shader. This is the tripwire that turns
    // that into a log line. It caught nothing when written; it exists so the next missing input is loud.
    void FDefaultSceneRenderer::ValidateNoStaleResolves(FEntityRegistry& Registry)
    {
#if USING(WITH_EDITOR)
        // The pass above just re-resolved everything it could see, so anything still stale is a gate that
        // did not fire. Rate-limited by generation: this walks every dynamic surface in the world.
        const uint32 Generation = FMeshResolveCache::GetPendingGeneration();
        if (Generation == LastStaleValidationGeneration)
        {
            return;
        }
        LastStaleValidationGeneration = Generation;

        uint32 StaleSurfaces = 0;
        uint32 StaleEntities = 0;

        auto& Storage = Registry.storage<SDynamicMeshComponent>();
        for (uint32 i = 0, Count = (uint32)Storage.size(); i < Count; ++i)
        {
            if (Storage.data()[i] == entt::tombstone)
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

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        TVector<CMaterialInterface*>& Scratch = ResolveOverrideScratch;
        FRenderDirtyTracker& Tracker = FRenderDirtyTracker::Ensure(Registry);

        uint32 Refreshed = ResolveMeshPool(Registry.storage<SStaticMeshComponent>(), EInstanceFlags::None, Scratch,
            [](const SStaticMeshComponent& C) -> CMesh* { return C.StaticMesh; },
            Tracker, EPrimitiveSource::StaticMesh);

        // Skeletal assets always carry FMeshletSkinnedVertex, so Skinned is unconditional here.
        Refreshed += ResolveMeshPool(Registry.storage<SSkeletalMeshComponent>(), EInstanceFlags::Skinned, Scratch,
            [](const SSkeletalMeshComponent& C) -> CMesh* { return C.SkeletalMesh; },
            Tracker, EPrimitiveSource::SkeletalMesh);

        // Foliage types carry no material overrides.
        for (auto&& [Entity, Foliage] : Registry.view<SFoliageComponent>().each())
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

                    // Same distinction as ResolveMeshComponent: only a null mesh is settled.
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

                if (Entry.bResolved)
                {
                    Type.CachedEntryState = Cache.GetEntryState(Handle);
                }
                else
                {
                    Type.CachedEntryState = MESH_RESOLVE_STATE_STALE;
                    FMeshResolveCache::MarkPendingWork();
                }
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

        {
            LUMINA_PROFILE_SECTION("Compile Draw Commands");
            FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);
            TAtomic<uint32> LightCount{0};
            
            auto DirectionalView     = Registry.view<SDirectionalLightComponent>(entt::exclude<SDisabledTag>);
            auto SpotLightView       = Registry.view<SSpotLightComponent>(entt::exclude<SDisabledTag>);
            auto PointLightView      = Registry.view<SPointLightComponent>(entt::exclude<SDisabledTag>);
            auto CharacterView       = Registry.view<SCharacterControllerComponent>(entt::exclude<SDisabledTag>);
            auto CameraView          = Registry.view<SCameraComponent>(entt::exclude<SDisabledTag>);
            auto BillboardView       = Registry.view<SBillboardComponent>(entt::exclude<SDisabledTag>);
            auto WidgetView          = Registry.view<SWidgetComponent>(entt::exclude<SDisabledTag>);
            auto TextView            = Registry.view<STextComponent>(entt::exclude<SDisabledTag>);
            auto LineBatcherView     = Registry.view<FLineBatcherComponent>();
            auto TriangleBatcherView = Registry.view<FTriangleBatcherComponent>();
            auto EnvironmentView     = Registry.view<SEnvironmentComponent>(entt::exclude<SDisabledTag>);
            auto SkyLightView        = Registry.view<SSkyLightComponent>(entt::exclude<SDisabledTag>);
            auto FogView             = Registry.view<SExponentialHeightFogComponent>(entt::exclude<SDisabledTag>);
            auto TerrainAllView      = Registry.view<STerrainComponent>();
            auto TerrainView         = Registry.view<STerrainComponent>(entt::exclude<SDisabledTag>);
            auto ParticleAllView     = Registry.view<SParticleSystemComponent>();
            auto ParticleView        = Registry.view<SParticleSystemComponent>(entt::exclude<SDisabledTag>);
            auto DecalView           = Registry.view<SDecalComponent>(entt::exclude<SDisabledTag>);
            auto WaterView           = Registry.view<SWaterComponent>(entt::exclude<SDisabledTag>);
            auto& TransformStorage  = Registry.storage<STransformComponent>();

            ECS::Utils::ResolveAllDirtyTransforms(Registry);

        {
            FEntityRegistry& DynRegistry = ECS::GetWorldRegistry(*World);
            ResolveDynamicMeshMaterials(DynRegistry, FRenderDirtyTracker::Ensure(DynRegistry));
        }

        ResolveDirtyMeshComponents();

            SyncScenePrimitives();

            // Per-frame CPU reject volumes built before parallel gather so workers query lock-free.
            BuildSceneCullContext();

            ResetGeometry_Extract();

            const uint32 NumPrimitives     = ScenePrimitives.Num();
            const size_t EstimatedProxies  = (size_t)NumPrimitives * 2;

            DrawCommands.reserve(EstimatedProxies);

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
            CurrentReservePerThread = (uint32)((EstimatedProxies + NumThreads - 1) / std::max(1u, NumThreads));
            
            for (uint32 t = 0; t < NumThreads; ++t)
            {
                ThreadLocal[t].ResetForFrame(FFrameArenaAllocator());
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
                    // Sized ONCE, here, before any worker can touch it: the gather writes poses straight
                    // into disjoint arena slices, which is only safe while nothing reallocates the storage.
                    // Untouched entries keep whatever was there -- they are never uploaded.
                    ExtractFrame->Geometry.BonesData.resize(ScenePrimitives.GetBoneArenaSize());

                    FTaskGraph::FNodeHandle CullNode = Graph.AddParallelFor(NumPrimitives, GPrimitiveGrain, [&](const Task::FParallelRange& Range)
                    {
                        LUMINA_PROFILE_SECTION("Cull And Emit Primitives");
                        FThreadLocalDrawData& Local = AcquireThreadLocalDrawData(Range.Thread);
                        CullAndEmitPrimitives(Range, Local);
                    }, ETaskPriority::High); // critical path: MergeNode waits on this, so it runs ahead of emitters

                    Graph.AddDependency(MergeNode, CullNode);
                }
            }

            Graph.Dispatch();

            // Dispatch blocks until the graph drains, so the per-thread rows are complete and we are back on
            // the game thread -- which is where the streamer may walk CMaterial::ResolvedTextures.
            // Dispatch above already drained the graph, so we are back on the game thread -- which is
            // where the streamer may touch CObjects.
            PublishStreamingFeedback();

            EmitTaskGraph.Reset();
            FTaskGraph& EmitGraph = EmitTaskGraph;

            FLineBatcherComponent* LineBatcher = nullptr;
            LineBatcherView.each([&](FLineBatcherComponent& C) { if (LineBatcher == nullptr)
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

                TriangleBatcherView.each([&](FTriangleBatcherComponent& TriangleBatcherComponent)
                {
                    ProcessBatchedTriangles(TriangleBatcherComponent);
                });
            }, ETaskPriority::Medium);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Process Widget Primitives");

                const FFrustum& WidgetFrustum = Frame.CameraFrustum;
                const bool      bCullWidgets   = SceneGlobalData.CullData.bFrustumCull != 0u;

                WidgetView.each([&](entt::entity Entity, SWidgetComponent& WidgetComponent)
                {
                    FWidgetRuntime& Runtime = WidgetComponent.Runtime;

                    const FMatrix4 WorldMatrix = TransformStorage.get(Entity).GetWorldMatrix();
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
                    Inst.EntityID     = entt::to_integral(Entity);
                    Inst.Pad0         = 0u;
                    Inst.Pad1         = 0u;
                });
            }, ETaskPriority::High);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Process Text Primitives");

                const FFrustum& TextFrustum = Frame.CameraFrustum;
                const bool      bCullText   = SceneGlobalData.CullData.bFrustumCull != 0u;
                const FVector3  CamRight    = FVector3(SceneGlobalData.CameraData.Right);
                const FVector3  CamUp       = FVector3(SceneGlobalData.CameraData.Up);

                TextView.each([&](entt::entity Entity, STextComponent& TextComponent)
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

                    const FMatrix4 WorldMatrix = TransformStorage.get(Entity).GetWorldMatrix();
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
                        G.EntityID  = entt::to_integral(Entity);
                    }

                    FFrameData::FTextBatch& Batch = TextBatches.emplace_back();
                    Batch.AtlasIndex    = (uint32)AtlasID;
                    Batch.AtlasWidth    = Font->GetAtlasWidth();
                    Batch.AtlasHeight   = Font->GetAtlasHeight();
                    Batch.DistanceRange = Font->GetDistanceRange();
                    Batch.FirstInstance = First;
                    Batch.Count         = (uint32)GlyphInstances.size() - First;
                    Batch.bDepthTest    = TextComponent.bDepthTest;
                });
            }, ETaskPriority::Medium); // emitter, not on the mesh critical path: don't outrank Static/Skeletal/Foliage

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Process Billboard Primitives");

                BillboardView.each([this, &BillboardInstances, &TransformStorage](entt::entity Entity, const SBillboardComponent& BillboardComponent)
                {
                    if (!BillboardComponent.Texture.IsValid() || BillboardComponent.Texture->GetResourceID() < 0)
                    {
                        return;
                    }

                    FBillboardInstance& Billboard   = BillboardInstances.emplace_back();
                    Billboard.TextureIndex          = BillboardComponent.Texture->GetResourceID();
                    Billboard.Position              = TransformStorage.get(Entity).GetWorldLocationCached();
                    Billboard.Size                  = BillboardComponent.Scale;
                    Billboard.EntityID              = entt::to_integral(Entity);
                });
                
                #if USING(WITH_EDITOR)
                // Editor visualizers: billboards for lights/cameras/sky/particles. Skipped in game/thumbnail worlds.
                if (!World->IsGameWorld())
                {
                    auto EmplaceVisualizer = [this, &BillboardInstances](entt::entity Entity, const FVector3& Position, ENamedImage Icon, const FVector4& Color, float Size = 0.20f)
                    {
                        FBillboardInstance& Billboard = BillboardInstances.emplace_back();
                        Billboard.TextureIndex        = (uint32)GetNamedImage(Icon).GetResourceID();
                        Billboard.ColorPack           = PackColor(Color);
                        Billboard.Position            = Position;
                        Billboard.Size                = Size;
                        Billboard.EntityID            = entt::to_integral(Entity);
                    };

                    // Skip editor viewport camera so the billboard doesn't sit on the user's view.
                    CameraView.each([&](entt::entity Entity, SCameraComponent&)
                    {
                        if (Registry.all_of<FEditorComponent>(Entity))
                        {
                            return;
                        }
                        EmplaceVisualizer(Entity, TransformStorage.get(Entity).GetWorldLocationCached(), ENamedImage::CameraIcon, FColor::White);
                    });

                    CharacterView.each([&](entt::entity Entity, SCharacterControllerComponent&)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.get(Entity).GetWorldLocationCached(), ENamedImage::CharacterIcon, FColor::White);
                    });

                    PointLightView.each([&](entt::entity Entity, const SPointLightComponent& Light)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.get(Entity).GetWorldLocationCached(), ENamedImage::PointLightIcon, FVector4(Light.LightColor, 1.0f));
                    });

                    SpotLightView.each([&](entt::entity Entity, const SSpotLightComponent& Light)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.get(Entity).GetWorldLocationCached(), ENamedImage::SpotLightIcon, FVector4(Light.LightColor, 1.0f));
                    });

                    DirectionalView.each([&](entt::entity Entity, const SDirectionalLightComponent& Light)
                    {
                        const auto& Transform = Registry.get<STransformComponent>(Entity);
                        EmplaceVisualizer(Entity, Transform.GetWorldLocationCached(), ENamedImage::DirectionalLightIcon, FVector4(Light.Color, 1.0f));
                    });

                    SkyLightView.each([&](entt::entity Entity, const SSkyLightComponent&)
                    {
                        const auto& Transform = Registry.get<STransformComponent>(Entity);
                        EmplaceVisualizer(Entity, Transform.GetWorldLocationCached(), ENamedImage::SkyLightIcon, FVector4(1.0f));
                    });

                    ParticleView.each([&](entt::entity Entity, const SParticleSystemComponent&)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.get(Entity).GetWorldLocationCached(), ENamedImage::ParticleSystemIcon, FVector4(1.0f));
                    });
                }
                #endif
            }, ETaskPriority::High);

            #if USING(WITH_EDITOR)
            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Selection");

                TVector<uint32>& Bits = Frame.Extracts.SelectionBits;
                Bits.clear();

                // Sized to the highest selected slot, not the registry: an entity above the top bit is
                // out of range in the shader and reads as unselected, which is the right answer anyway.
                Registry.view<FSelectedInEditorComponent>().each([&](entt::entity Entity)
                {
                    const uint32 Index = (uint32)entt::to_entity(Entity);
                    const uint32 Word  = Index >> 5u;
                    if (Word >= Bits.size())
                    {
                        Bits.resize(Word + 1u, 0u);
                    }
                    Bits[Word] |= (1u << (Index & 31u));
                });
            }, ETaskPriority::High);
            #endif

            auto DLightTask = EmitGraph.AddParallelFor(DirectionalView.handle()->size(), 32, [&](Task::FParallelRange Range)
            {
                LUMINA_PROFILE_SECTION("Process Directional Light");
                auto Handle = DirectionalView.handle();
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    entt::entity Entity = (*Handle)[i];
                    if (DirectionalView.contains(Entity))
                    {
                        auto& DirectionalLight = DirectionalView.get<SDirectionalLightComponent>(Entity);
                        ProcessDirectionalLight(DirectionalLight, LightCount);
                    }
                }
            });
            
            auto PointLightTask = EmitGraph.AddParallelFor(PointLightView.handle()->size(), 32, [&](Task::FParallelRange Range)
            {
                LUMINA_PROFILE_SECTION("Process Point Light Range");

                auto Handle = PointLightView.handle();
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    entt::entity Entity = (*Handle)[i];
                    if (PointLightView.contains(Entity))
                    {
                        auto& PointLight = PointLightView.get<SPointLightComponent>(Entity);
                        auto& Transform = TransformStorage.get(Entity);
                        ProcessPointLight(PointLight, Transform, LightCount);
                    }
                }
            });
            
            auto SpotLightTask = EmitGraph.AddParallelFor(SpotLightView.handle()->size(), 32, [&](Task::FParallelRange Range)
            {
                LUMINA_PROFILE_SECTION("Process Spot Light Range");

                auto Handle = SpotLightView.handle();
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    entt::entity Entity = (*Handle)[i];
                    if (SpotLightView.contains(Entity))
                    {
                        auto& SpotLight = SpotLightView.get<SSpotLightComponent>(Entity);
                        auto& Transform = TransformStorage.get(Entity);
                        ProcessSpotLight(SpotLight, Transform, LightCount);
                    }
                }
            });
            
            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Terrain");

                Frame.Extracts.TerrainExtracts.clear();
                Frame.Extracts.LiveTerrainEntities.clear();

                for (entt::entity Entity : TerrainAllView)
                {
                    Frame.Extracts.LiveTerrainEntities.push_back(Entity);
                }

                for (entt::entity Entity : TerrainView)
                {
                    STerrainComponent& Terrain = TerrainView.get<STerrainComponent>(Entity);

                    FFrameData::FTerrainExtract& Item = Frame.Extracts.TerrainExtracts.emplace_back();
                    Item.Entity      = Entity;
                    Item.WorldMatrix = TransformStorage.get(Entity).GetWorldMatrix();
                    PrepareTerrainExtract(Terrain, Item.WorldMatrix, Item);
                }
            }, ETaskPriority::High);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Particles");

                Frame.Extracts.ParticleExtracts.clear();
                Frame.Extracts.LiveParticleEntities.clear();

                for (entt::entity Entity : ParticleAllView)
                {
                    Frame.Extracts.LiveParticleEntities.push_back(Entity);
                }

                ParticleView.each([&](entt::entity Entity, SParticleSystemComponent& Component)
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

                    const FMatrix4 WorldMatrix = TransformStorage.get(Entity).GetWorldMatrix();
                    const int32    EmitterCount = (int32)PS->Emitters.size();

                    for (int32 EmitterIdx = 0; EmitterIdx < EmitterCount; ++EmitterIdx)
                    {
                        CParticleEmitter* Emitter = PS->Emitters[EmitterIdx].Get();
                        if (Emitter == nullptr || !Emitter->bEnabled)
                        {
                            continue;
                        }

                        FFrameData::FParticleExtract& Item = Frame.Extracts.ParticleExtracts.emplace_back();
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
                        }
                    }
                });
            }, ETaskPriority::High);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Decals");

                Frame.Primitives.DecalExtracts.clear();
                Frame.Primitives.DecalBatches.clear();
                DecalSortScratch.clear();
                DecalGroupMinSort.clear();

                DecalView.each([&](entt::entity Entity, const SDecalComponent& Decal)
                {
                    CMaterialInterface* Material = Decal.DecalMaterial.Get();
                    if (Material == nullptr || !Material->IsReadyForRender())
                    {
                        return;
                    }
                    CMaterial* ShaderOwner = Material->GetMaterial();
                    if (ShaderOwner == nullptr || ShaderOwner->GetMaterialType() != EMaterialType::Decal)
                    {
                        return;
                    }
                    const int32 MaterialIndex = Material->GetMaterialIndex();
                    if (MaterialIndex < 0)
                    {
                        return;
                    }

                    FGPUDecal Item;
                    Item.DecalToWorld  = Math::Scale(TransformStorage.get(Entity).GetWorldMatrix(), Decal.Size);
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
                eastl::stable_sort(DecalSortScratch.begin(), DecalSortScratch.end(), [&](const FDecalSortEntry& A, const FDecalSortEntry& B)
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
            }, ETaskPriority::High);

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

                WaterView.each([&](entt::entity Entity, const SWaterComponent& Water)
                {
                    const FMatrix4 WorldMatrix = TransformStorage.get(Entity).GetWorldMatrix();
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
                                                   Math::Max(Water.WaveScale, 0.05f),
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

                        const FVector3 SurfaceCenter = TransformStorage.get(Entity).GetWorldLocation();
                        Frame.Water.bUnderwaterActive = true;
                        Frame.Water.Underwater.PlaneNormalAndHeight = FVector4(0.0f, 1.0f, 0.0f, SurfaceCenter.y);
                        Frame.Water.Underwater.FogColorDensity      = FVector4(Water.UnderwaterFogColor, Math::Max(Water.UnderwaterFogDensity, 0.0f));
                        Frame.Water.Underwater.TintDistortion       = FVector4(Water.UnderwaterTint, Math::Max(Water.UnderwaterDistortion, 0.0f));
                        Frame.Water.Underwater.DeepColor            = FVector4(Water.DeepColor, 0.0f);
                    }
                });
            }, ETaskPriority::High);

            EmitGraph.Add([&]
            {
                LUMINA_PROFILE_SECTION("Extract Paint Ops");
                Frame.Extracts.PaintOps.clear();
                World->DrainRenderTargetPaints(Frame.Extracts.PaintOps);
            }, ETaskPriority::High);

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

            // LightCount can overshoot MAX_LIGHTS; clamp to match what Process*Light wrote.
            LightData.NumLights = Math::Min(LightCount.load(std::memory_order_acquire), (uint32)MAX_LIGHTS);

            // Serial fit/allocate after parallel light pass; shrinks when sum(area) exceeds atlas budget.
            AllocateShadowTiles();

            // Serial after parallel light pass; skylight below reads ActiveEnv + LightData.SunDirection set by ProcessDirectionalLight.
            const SEnvironmentComponent* ActiveEnv = nullptr;
            {
                LUMINA_PROFILE_SECTION("Environment Processing");

                bool bHasEnvironment           = false;
                RenderSettings.bGTAO           = false;
                EnvironmentParams              = FEnvironmentParams{};
                Frame.Volumetrics.EnvironmentMapID    = -1;
                Frame.Volumetrics.EnvironmentMapWidth = 0;
                // Set true below if any IBL input differs from the last bake snapshot.
                Frame.Volumetrics.bIBLDirty                = false;
                Frame.Volumetrics.bIBLConvolutionDirty     = false;

                EnvironmentView.each([&bHasEnvironment, &Frame, &EnvironmentParams, &ActiveEnv] (const SEnvironmentComponent& Env)
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
                });

                RenderSettings.bHasEnvironment = bHasEnvironment;

                Frame.Volumetrics.IBLResolution = ActiveEnv
                    ? ResolveIBLQuality(ActiveEnv->IBLQuality)
                    : LastExtractedIBLResolution;
            }

            {
                LUMINA_PROFILE_SECTION("Skylight Processing");

                LightData.AmbientLight = FVector4(0.0f);

                SkyLightView.each([&LightData, ActiveEnv] (const SSkyLightComponent& Sky)
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
                        else // Dynamic / HDRI
                        {
                            // SunDirection points FROM surface TO sun, so .y is elevation.
                            const float SunHeight = LightData.bHasSun
                                ? Math::Clamp(LightData.SunDirection.y, -1.0f, 1.0f)
                                : 0.5f;
                            const float Day = Math::Clamp(SunHeight * 2.0f + 0.2f, 0.0f, 1.0f);
                            AmbientRGB = Math::Mix(FVector3(0.05f, 0.06f, 0.10f),
                                                  FVector3(0.40f, 0.55f, 0.85f),
                                                  Day);
                        }
                    }
                    LightData.AmbientLight = FVector4(AmbientRGB, Sky.Intensity);
                });

                LightData.bHasIBL = RenderSettings.bHasEnvironment ? 1u : 0u;
            }

            // Exponential height fog. Last enabled component with density > 0 wins.
            {
                LUMINA_PROFILE_SECTION("Fog Processing");

                Frame.Volumetrics.bHasFog        = false;
                Frame.Volumetrics.bVolumetricFog = false;
                Frame.Volumetrics.FogParams      = FExponentialHeightFogParams{};

                FogView.each([&Frame, &Registry] (entt::entity Entity, const SExponentialHeightFogComponent& Fog)
                {
                    if (!Fog.bEnabled || Fog.FogDensity <= 0.0f)
                    {
                        return;
                    }

                    float BaseHeight = Fog.FogBaseHeight;
                    if (const STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity))
                    {
                        BaseHeight += Transform->GetWorldLocationCached().y;
                    }

                    FExponentialHeightFogParams& P = Frame.Volumetrics.FogParams;
                    P.InscatteringColor = FVector4(Fog.FogInscatteringColor, Fog.FogDensity);
                    P.HeightParams      = FVector4(Fog.FogHeightFalloff, BaseHeight,
                                                    Fog.FogStartDistance, Fog.FogMaxOpacity);
                    P.DirectionalColor  = FVector4(Fog.DirectionalInscatteringColor,
                                                    Fog.DirectionalInscatteringExponent);
                    P.VolumetricParams  = FVector4(Fog.VolumetricScatteringIntensity,
                                                    Fog.VolumetricAnisotropy,
                                                    Fog.VolumetricMaxDistance,
                                                    Fog.DirectionalInscatteringStartDistance);
                    // Phase attenuation reuses MultiScatterFalloff: an octave carrying less energy has also bounced
                    // more and scattered more diffusely, so separate sliders would only let artists author a conflict.
                    P.MultiScatterParams = FVector4((float)Math::Clamp(Fog.MultiScatterOctaves, 1, 4),
                                                    Fog.MultiScatterFalloff,
                                                    Fog.MultiScatterShadowLeak,
                                                    Fog.MultiScatterFalloff);

                    Frame.Volumetrics.bHasFog        = true;
                    Frame.Volumetrics.bVolumetricFog = Fog.bVolumetricFog;
                });
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
    }

    // Runs after every extract write, so it is the last word on what the cull sees. Capturing on the
    // frame the toggle goes on means that frame still culls live and the freeze starts on the next one.
    void FDefaultSceneRenderer::ApplyCullFreeze(FFrameData& Frame)
    {
        FCullData& Cull = Frame.SceneGlobalData.CullData;

        if (!RenderSettings.bFreezeCulling)
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
            FrozenCull.Frustum          = Cull.Frustum;
            FrozenCull.ShadowFrustum    = Cull.ShadowFrustum;
            for (int32 c = 0; c < NumCascades; ++c)
            {
                FrozenCull.CascadeFrustum[c] = Cull.CascadeFrustum[c];
            }
            FrozenCull.CascadeViewBase = Frame.Views.CascadeViewBase;
            FrozenCull.bValid          = true;
            return;
        }

        // The view COUNT is frozen with them: every bucket index, the block dispatch grid and the per-view
        // arg stride derive from it, so replaying a different count relayouts the regions.
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
        Cull.Frustum              = FrozenCull.Frustum;
        Cull.ShadowFrustum        = FrozenCull.ShadowFrustum;
        for (int32 c = 0; c < NumCascades; ++c)
        {
            Cull.CascadeFrustum[c] = FrozenCull.CascadeFrustum[c];
        }

        DrawFrozenCullFrustum(Frame);
    }

    // The volume the frozen cull is still evaluating against, unprojected from its own view-projection.
    // Without it a frozen cull is indistinguishable from geometry going missing for a real reason.
    void FDefaultSceneRenderer::DrawFrozenCullFrustum(const FFrameData& Frame)
    {
        const FMatrix4 InvViewProj = Math::Inverse(FrozenCull.CameraProjection * FrozenCull.CameraView);

        FVector3 Corner[8];
        for (int32 i = 0; i < 8; ++i)
        {
            // Vulkan clip volume: xy in [-1, 1], z in [0, 1]. Reverse-Z puts the near plane at w = 1.
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

    void FDefaultSceneRenderer::UploadBoneArena(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        const TVector<FBoneTransform>& Mirror = Frame.Geometry.BonesData;
        if (Mirror.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Upload Bone Arena");

        const SIZE_T ArenaBytes = Mirror.size() * sizeof(FBoneTransform);

        // Grown, never shrunk while primitives are live: a base handed out earlier has to stay addressable,
        // and the arena size only rises within a level. bAllowShrink off for exactly that reason.
        ResizeBufferIfNeeded(CL, BoneArenaBuffer, ArenaBytes, 1.25f, BoneArenaLowUsage, /*bAllowShrink*/ false,
                             EBufferInit::Zeroed, "Skinning.BoneArena");
        if (!BoneArenaBuffer)
        {
            return;
        }

        // Coalesce, so a level whose skeletons were allocated together costs one copy instead of one per
        // character. Sorted by base; adjacent and overlapping runs merge.
        TVector<FUIntVector2>& Ranges = BoneUploadScratch;
        Ranges.assign(Frame.Geometry.BoneUploadRanges.begin(), Frame.Geometry.BoneUploadRanges.end());
        if (Ranges.empty())
        {
            return;
        }

        eastl::sort(Ranges.begin(), Ranges.end(),
                    [](const FUIntVector2& A, const FUIntVector2& B) { return A.x < B.x; });

        const uint32 ArenaCount = (uint32)Mirror.size();

        uint32 RunStart = Ranges[0].x;
        uint32 RunEnd   = Ranges[0].x + Ranges[0].y;

        const auto Flush = [&](uint32 Start, uint32 End)
        {
            // The gather bounds every write against the mirror, so this only ever trims a range whose
            // primitive lost its slice between gather and upload.
            End = Math::Min(End, ArenaCount);
            if (Start >= End)
            {
                return;
            }

            WriteBuffer(CL, BoneArenaBuffer.GetAddress() + (uint64)Start * sizeof(FBoneTransform),
                        Mirror.data() + Start, (uint64)(End - Start) * sizeof(FBoneTransform));
        };

        for (SIZE_T i = 1; i < Ranges.size(); ++i)
        {
            const uint32 Start = Ranges[i].x;
            const uint32 End   = Ranges[i].x + Ranges[i].y;

            if (Start <= RunEnd)
            {
                RunEnd = Math::Max(RunEnd, End);
                continue;
            }

            Flush(RunStart, RunEnd);
            RunStart = Start;
            RunEnd   = End;
        }
        Flush(RunStart, RunEnd);
    }

    void FDefaultSceneRenderer::UploadSkinnedFrameData(RHI::FCmdListH CL, FFrameData& Frame)
    {
        TVector<FSkinnedFrameData>& Data  = Frame.Geometry.SkinnedFrameData;
        const TVector<uint32>&      Slots = Frame.Geometry.SkinnedSlots;

        // Monotonic, and minted here rather than reused from MeshletDrawTag: that one wraps at 4095, and a
        // slot left ungathered for exactly one wrap would match again and be emitted with ranges thousands
        // of frames stale. A 32-bit counter cannot alias inside any session.
        CurrentSkinnedFrameTag++;

        if (Data.empty() || Slots.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Upload Skinned Frame Data");

        ResizeBufferIfNeeded(CL, SkinnedFrameDataBuffer, Data.size() * sizeof(FSkinnedFrameData), 1.25f,
                             SkinnedFrameDataLowUsage, true, EBufferInit::Zeroed, "Skinning.FrameData");
        ResizeBufferIfNeeded(CL, SkinnedSlotListBuffer, Slots.size() * sizeof(uint32), 1.5f,
                             SkinnedSlotListLowUsage, true, EBufferInit::Zeroed, "Skinning.SlotList");
        if (!SkinnedFrameDataBuffer || !SkinnedSlotListBuffer)
        {
            return;
        }

        WriteBuffer(CL, SkinnedSlotListBuffer.GetAddress(), Slots.data(), Slots.size() * sizeof(uint32));

        // Stamped now, on the render thread, because the merge that filled these entries ran against a
        // different FFrameData and could not know the tag this frame would cull against.
        for (uint32 Slot : Slots)
        {
            if (Slot < (uint32)Data.size())
            {
                Data[Slot].FrameTag = CurrentSkinnedFrameTag;
            }
        }

        // Only the gathered slots, coalesced: the array spans every retained slot, and re-sending all of it
        // to update the handful that are skeletal would be most of the traffic for none of the information.
        // Slots ungathered this frame keep last frame's bytes; their frame tag is what rejects them.
        TVector<FUIntVector2>& Ranges = SkinnedUploadScratch;
        Ranges.clear();
        Ranges.reserve(Slots.size());
        for (uint32 Slot : Slots)
        {
            Ranges.push_back(FUIntVector2{ Slot, 1u });
        }

        eastl::sort(Ranges.begin(), Ranges.end(),
                    [](const FUIntVector2& A, const FUIntVector2& B) { return A.x < B.x; });

        const uint32 Count = (uint32)Data.size();

        const auto Flush = [&](uint32 Start, uint32 End)
        {
            End = Math::Min(End, Count);
            if (Start >= End)
            {
                return;
            }
            WriteBuffer(CL, SkinnedFrameDataBuffer.GetAddress() + (uint64)Start * sizeof(FSkinnedFrameData),
                        Data.data() + Start, (uint64)(End - Start) * sizeof(FSkinnedFrameData));
        };

        uint32 RunStart = Ranges[0].x;
        uint32 RunEnd   = Ranges[0].x + Ranges[0].y;
        for (SIZE_T i = 1; i < Ranges.size(); ++i)
        {
            const uint32 Start = Ranges[i].x;
            const uint32 End   = Ranges[i].x + Ranges[i].y;
            if (Start <= RunEnd)
            {
                RunEnd = Math::Max(RunEnd, End);
                continue;
            }
            Flush(RunStart, RunEnd);
            RunStart = Start;
            RunEnd   = End;
        }
        Flush(RunStart, RunEnd);
    }

    void FDefaultSceneRenderer::CompileDrawCommands_Render(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Render Scene");

        FFrameData& Frame = *RenderFrame;
        ApplyCullFreeze(Frame);
        auto& SceneGlobalData            = Frame.SceneGlobalData;
        const auto& BonesData            = Frame.Geometry.BonesData;
        const auto& CullViews            = Frame.Views.CullViews;
        const auto& LightData            = Frame.Lighting.LightData;
        const auto& EnvironmentParams    = Frame.Volumetrics.EnvironmentParams;
        const int32 EnvironmentMapID     = Frame.Volumetrics.EnvironmentMapID;
        const auto& BillboardInstances   = Frame.Primitives.BillboardInstances;
        const uint32 NumDrawsPerView     = Frame.Views.NumDrawsPerView;
        bool& bIBLDirty                  = Frame.Volumetrics.bIBLDirty;
        bool& bIBLConvolutionDirty       = Frame.Volumetrics.bIBLConvolutionDirty;

        const uint32 NumCullViews                  = (uint32)CullViews.size();
        const uint32 NumDraws                      = NumDrawsPerView;
        SceneGlobalData.CullData.MeshletDrawTag    = (MeshletDrawTagCounter++ % 4095u) + 1u;

        // The claim is on the GPU now, so only the GPU knows the demand. Same lagged high-water mark the
        // block list uses: an undersized frame drops blocks to inline skinning and corrects on the next.
        PreSkinHighWater = Math::Max(PreSkinHighWater, LastPreSkinRequested);
        const SIZE_T PreSkinnedSize       = Math::Max<SIZE_T>(sizeof(FPreSkinnedVertex),
                                            (SIZE_T)Math::Max<uint32>(PreSkinHighWater, 1u) * sizeof(FPreSkinnedVertex));

        UpdateMeshletBoundFeedback(CurrentFrameSlot);

        const uint32 PredictedDrawList = Math::Max(LastDrawListRequired + LastDrawListRequired / 2u, 65536u);
        const SIZE_T MeshletDrawListSize = Math::Max<SIZE_T>(
            sizeof(uint32) * 2,
            (SIZE_T)PredictedDrawList * sizeof(uint32) * 2);

        const SIZE_T NumArgSlots = (SIZE_T)NumCullViews * (SIZE_T)NumDraws;
        
        {
            const uint32 MaxGroups = Math::Max(RHI::GetMaxMeshWorkGroupCount(), 1u);
            const uint32 WantedSubDraws = (DrawListCapacity == 0u) ? 1u : (((DrawListCapacity - 1u) / MaxGroups) + 1u);
            MeshSubDrawsPerSlice   = Math::Clamp(WantedSubDraws, 1u, 8u);
            
            static uint32 LastLoggedSubDraws = 0u;
            if (MeshSubDrawsPerSlice != LastLoggedSubDraws)
            {
                LastLoggedSubDraws = MeshSubDrawsPerSlice;
                LOG_DISPLAY("Meshlet sub-draws per slice: {} (draw-list capacity {}, mesh workgroup limit {}). "
                            "A slice needing more than {} sub-draws has the remainder dropped.",
                            MeshSubDrawsPerSlice, DrawListCapacity, MaxGroups, MeshSubDrawsPerSlice);
            }
        }

        // Mesh draw args: MeshSubDrawsPerSlice per (bucket, slice), 12B stride.
        const SIZE_T MeshDrawArgsSize = Math::Max<SIZE_T>(
            sizeof(RHI::FDrawMeshTasksIndirectArguments),
            NumArgSlots * (SIZE_T)kMeshletSliceCount * (SIZE_T)MeshSubDrawsPerSlice
                * sizeof(RHI::FDrawMeshTasksIndirectArguments));

        ResizeBufferIfNeeded(CL, PreSkinnedVerticesBuffer, PreSkinnedSize, 1.2f, PreSkinnedVerticesLowUsage,
                             true, EBufferInit::Zeroed, "Cull.PreSkinnedVertices");
        PreSkinnedVertexCapacity = (uint32)Math::Min<uint64>(
            PreSkinnedVerticesBuffer.GetSize() / sizeof(FPreSkinnedVertex), 0xFFFFFFFFull);
        {
            const uint8 Slot = CurrentFrameSlot;
            ResizeBufferIfNeeded(CL, MeshDrawArgsRing[Slot], MeshDrawArgsSize, 1.2f, MeshDrawArgsRingLowUsage[Slot],
                                 true, EBufferInit::Zeroed, "Cull.MeshDrawArgs");
            ResizeBufferIfNeeded(CL, MeshletDrawListRing[Slot], MeshletDrawListSize, 1.2f, MeshletDrawListRingLowUsage[Slot],
                                 true, EBufferInit::Zeroed, "Cull.MeshletDrawList");
            DrawListCapacity = (uint32)Math::Min<uint64>(MeshletDrawListRing[Slot].GetSize() / (sizeof(uint32) * 2), 0xFFFFFFFFull);
            
            const uint32 PredictedVisible = LastVisibleInstances + LastVisibleInstances / 2u;
            uint32 VisibleCapacityWanted = Math::Max(PredictedVisible, 4096u);

            const uint32 VisibleCapacityMax = Math::Max(Frame.Geometry.RetainedUpload.SlotCount, 1u);

            if (LastVisibleInstances == 0u)
            {
                VisibleCapacityWanted = VisibleCapacityMax;
            }

            FrameVisibleInstanceCapacity = Math::Min(VisibleCapacityWanted, VisibleCapacityMax);

            ResizeBufferIfNeeded(CL, VisibleInstanceRing[Slot],
                                 (SIZE_T)FrameVisibleInstanceCapacity * sizeof(FGPUInstance), 1.25f,
                                 VisibleInstanceLowUsage[Slot], true, EBufferInit::Zeroed, "Cull.VisibleInstances");

            const SIZE_T InstanceViewRangeSize = Math::Max<SIZE_T>(
                sizeof(uint32) * 2,
                (SIZE_T)FrameVisibleInstanceCapacity * (SIZE_T)Math::Max(NumCullViews, 1u) * sizeof(uint32) * 2);
            
            ResizeBufferIfNeeded(CL, InstanceViewRangeRing[Slot], InstanceViewRangeSize, 1.25f, InstanceViewRangeRingLowUsage[Slot],
                                 true, EBufferInit::Zeroed, "Cull.InstanceViewRanges");

            // Only the GPU knows how many blocks were appended, so its counter (lagged by the frames in flight)
            // sizes this as a high-water mark that only grows.
            BlockListHighWater = Math::Max(BlockListHighWater, LastBlocksRequested);

            const SIZE_T MeshletBlockSize = Math::Max<SIZE_T>(
                sizeof(uint32) * 2,
                (SIZE_T)Math::Max<uint32>(BlockListHighWater, 1u) * sizeof(uint32) * 2);
            ResizeBufferIfNeeded(CL, MeshletBlockRing[Slot], MeshletBlockSize, 1.2f, MeshletBlockRingLowUsage[Slot],
                                 true, EBufferInit::Zeroed, "Cull.MeshletBlocks");
            BlockListCapacity = (uint32)Math::Min<uint64>(MeshletBlockRing[Slot].GetSize() / (sizeof(uint32) * 2), 0xFFFFFFFFull);

            // The requirement is what the frame kFramesInFlight ago needed; the capacity is what this frame
            // grew to. Printing them either side of a resize made an overflow read as a false alarm.
            const auto LogOverflow = [](const char* What, uint32 Needed, uint32 GrownTo)
            {
                LOG_WARN("RenderScene: {} overflowed -- {} needed, capacity now {}. Geometry was dropped "
                         "that frame; this clears once the larger allocation cycles in.",
                         What, Needed, GrownTo);
            };

            // The block list is deliberately not reported: it is sized from a GPU high-water mark that
            // lags by the frames in flight, so it overflows once on any cold start and corrects itself
            // the next frame. That is the design working, not a fault worth a warning.
            static uint32 OverflowLogCounter = 0;
            if (LastVisibleOverflowed || LastDrawListOverflowed)
            {
                if ((OverflowLogCounter++ % 60u) == 0u)
                {
                    if (LastVisibleOverflowed)  { LogOverflow("visible-instance buffer", LastVisibleInstances, FrameVisibleInstanceCapacity); }
                    if (LastDrawListOverflowed) { LogOverflow("meshlet draw list",       LastDrawListRequired, DrawListCapacity); }
                }
            }
        }

        SceneGlobalData.CullData.MeshletDrawListCapacity = DrawListCapacity;
        SceneGlobalData.CullData.InstanceNum             = FrameVisibleInstanceCapacity;

        UploadBoneArena(CL, Frame);
        UploadSkinnedFrameData(CL, Frame);
        // The arena's size, not a per-frame total: it bounds an INDEX, and a stable base can point anywhere
        // in it regardless of how many skeletons were gathered.
        SceneGlobalData.CullData.BoneNum                 = (uint32)BonesData.size();

        {
            LUMINA_PROFILE_SECTION_COLORED("Write Scene Buffers", tracy::Color::OrangeRed3);

            const bool bEnvParamsChanged = !bEnvironmentParamsUploaded || std::memcmp(&EnvironmentParams, &LastUploadedEnvironmentParams, sizeof(FEnvironmentParams)) != 0;
            if (bEnvParamsChanged)
            {
                LastUploadedEnvironmentParams = EnvironmentParams;
                bEnvironmentParamsUploaded   = true;
            }

            const bool bSunChanged = LastIBLSunDirection != LightData.SunDirection || bLastIBLHasSun != (LightData.bHasSun != 0);
            const bool bMapChanged = LastIBLEnvironmentMapID != EnvironmentMapID;

            const bool bResChanged = Frame.Volumetrics.IBLResolution != LastExtractedIBLResolution;
            LastExtractedIBLResolution = Frame.Volumetrics.IBLResolution;

            if (RenderSettings.bHasEnvironment &&
                (!bIBLValid || bEnvParamsChanged || bSunChanged || bMapChanged || bResChanged))
            {
                bIBLDirty                  = true;
                LastIBLEnvironmentParams   = EnvironmentParams;
                LastIBLEnvironmentMapID    = EnvironmentMapID;
                LastIBLSunDirection        = LightData.SunDirection;
                bLastIBLHasSun             = (LightData.bHasSun != 0);
                bIBLValid                  = true;
            }

            constexpr float SunCosThreshold = 0.99996f;
            const bool bConvHasSunChanged = bLastConvolvedHasSun != (LightData.bHasSun != 0);
            float SunCos = 1.0f;
            if (bLastConvolvedHasSun && LightData.bHasSun)
            {
                SunCos = Math::Dot(LastConvolvedSunDirection, LightData.SunDirection);
            }
            const bool bConvSunChanged = bConvHasSunChanged || (SunCos < SunCosThreshold);
            const bool bConvParamsChanged = std::memcmp(&LastConvolvedEnvironmentParams, &EnvironmentParams, sizeof(FEnvironmentParams)) != 0;
            const bool bConvMapChanged =
                LastConvolvedEnvironmentMapID != EnvironmentMapID;

            if (RenderSettings.bHasEnvironment &&
                (!bIBLConvolutionValid || bConvParamsChanged || bConvSunChanged || bConvMapChanged || bResChanged))
            {
                bIBLConvolutionDirty           = true;
                LastConvolvedEnvironmentParams = EnvironmentParams;
                LastConvolvedEnvironmentMapID  = EnvironmentMapID;
                LastConvolvedSunDirection      = LightData.SunDirection;
                bLastConvolvedHasSun           = (LightData.bHasSun != 0);
                bIBLConvolutionValid           = true;
            }
            
            if (!RenderSettings.bHasEnvironment)
            {
                bIBLValid            = false;
                bIBLConvolutionValid = false;
            }
            SceneRootShared = FSceneRoot{};
            SceneRootShared.Lights = RHI::Core::CopyTransient(LightData);
            if (VisibleInstanceRing[CurrentFrameSlot])
            {
                SceneRootShared.Instances = VisibleInstanceRing[CurrentFrameSlot].GetAddress();
            }
            // Persistent and slot-addressed, so this is just the arena's address -- no per-frame copy of
            // the whole pose set into the transient ring any more.
            if (BoneArenaBuffer)
            {
                SceneRootShared.Bones = BoneArenaBuffer.GetAddress();
            }
            if (!BillboardInstances.empty())
            {
                SceneRootShared.Billboards = RHI::Core::CopyTransientArray(BillboardInstances.data(), BillboardInstances.size());
            }
            if (!CullViews.empty())
            {
                SceneRootShared.CullViews = RHI::Core::CopyTransientArray(CullViews.data(), CullViews.size());
            }
            if (!Frame.Primitives.WidgetInstances.empty())
            {
                SceneRootShared.Widgets = RHI::Core::CopyTransientArray(Frame.Primitives.WidgetInstances.data(), Frame.Primitives.WidgetInstances.size());
            }
            // Splines are small and bounded (a handful of components, tens of points each), so the shared
            // transient ring is the right home -- see the sizing rule in feedback_transient_ring_not_for_geometry.
            NumActiveSplines       = (uint32)Frame.Splines.Splines.size();
            SplineBufferAddr       = 0;
            SplinePointBufferAddr  = 0;
            SplineSampleBufferAddr = 0;
            if (NumActiveSplines > 0)
            {
                SplineBufferAddr = RHI::Core::CopyTransientArray(Frame.Splines.Splines.data(),
                                                                 Frame.Splines.Splines.size());
                if (!Frame.Splines.Points.empty())
                {
                    SplinePointBufferAddr = RHI::Core::CopyTransientArray(Frame.Splines.Points.data(),
                                                                          Frame.Splines.Points.size());
                }
                if (!Frame.Splines.Samples.empty())
                {
                    SplineSampleBufferAddr = RHI::Core::CopyTransientArray(Frame.Splines.Samples.data(),
                                                                           Frame.Splines.Samples.size());
                }
            }

            NumActiveProbes = (uint32)Frame.ReflectionProbes.Probes.size();
            ProbeBufferAddr = 0;
            if (NumActiveProbes > 0)
            {
                InitReflectionProbeTargets();
                ProbeBufferAddr = RHI::Core::CopyTransientArray(Frame.ReflectionProbes.Probes.data(),
                                                                Frame.ReflectionProbes.Probes.size());
            }

            SceneRootShared.Materials          = Render().GetMaterialManager().GetMaterialBuffer();
            SceneRootShared.MeshletDrawList    = GetMeshletDrawList().GetAddress();
            SceneRootShared.PreSkinnedVertices = GetPreSkinnedVerticesBuffer().GetAddress();
            if (Frame.CachedWorldSettings.bEnableGTAO)
            {
                SceneGlobalData.GTAOSettings.AOTextureIndex = (uint32)CurrentView->Images[(int)ENamedImage::GTAOBlur].GetResourceID();
            }

            RenderSettings.bShadowMaskValid = (LightData.bHasSun != 0) &&
                                              (LightData.Lights[0].ShadowDataIndex != INDEX_NONE);
            if (RenderSettings.bShadowMaskValid)
            {
                SceneGlobalData.ShadowMaskIndex = (uint32)CurrentView->Images[(int)ENamedImage::ShadowMask].GetResourceID();
            }
            else
            {
                SceneGlobalData.ShadowMaskIndex = ~0u;
            }

            // Published unconditionally: the images live as long as the view and the translucent shading pass
            // is the only reader, so there is no window where a stale index could be sampled.
            SceneGlobalData.MomentZerothIndex = (uint32)CurrentView->Images[(int)ENamedImage::MomentZeroth].GetResourceID();
            SceneGlobalData.MomentsIndex      = (uint32)CurrentView->Images[(int)ENamedImage::Moments].GetResourceID();

            CurrentSceneRootAddr = BuildViewSceneRoot(*CurrentView, RHI::Core::CopyTransient(SceneGlobalData));

            DispatchGPUSceneCull(CL, Frame);
        }
    }

    static uint32 SelectLODIndex(const FResolvedSurface& Surface, float DistSq, float RadiusSq)
    {
        // A zero radius used to yield ratio 0; without this guard every threshold would pass.
        if (RadiusSq <= 0.0f)
        {
            return 0u;
        }

        uint32 Picked = 0;
        const uint32 LastLOD = Surface.NumLODs > 0 ? Surface.NumLODs - 1u : 0u;
        for (uint32 i = 1; i <= LastLOD; ++i)
        {
            if (DistSq >= Surface.LODScreenThresholdSq[i] * RadiusSq)
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
    static uint32 ResolveSurfaceLOD(const FResolvedSurface& Surface, int32 ForcedLODIndex, bool bUseLODs, float DistSq, float RadiusSq)
    {
        if (Surface.NumLODs <= 1)
        {
            return 0u;
        }
        if (ForcedLODIndex >= 0)
        {
            return (uint32)Math::Min((int32)Surface.NumLODs - 1, ForcedLODIndex);
        }
        if (bUseLODs)
        {
            return SelectLODIndex(Surface, DistSq, RadiusSq);
        }
        return 0u;
    }

    static uint32 ResolveShadowLOD(const FResolvedSurface& Surface, uint32 CameraLOD, int32 ShadowLODBias,
                                   float DistSq, float CoarseDistSq)
    {
        if (Surface.NumLODs == 0)
        {
            return 0u;
        }
        const uint32 Cap   = (CoarseDistSq > 0.0f && DistSq >= CoarseDistSq) ? MAX_COARSE_SHADOW_LOD : MAX_SHADOW_LOD;
        const int32 Biased = (int32)CameraLOD + ShadowLODBias;
        const int32 MaxLOD = (int32)Math::Min<uint32>(Surface.NumLODs - 1u, Cap);
        return (uint32)Math::Clamp(Biased, 0, MaxLOD);
    }

    static void EmitPrimitiveSurfaces(FDefaultSceneRenderer::FThreadLocalDrawData& Local,
                                      const FScenePrimitive& Prim,
                                      const FSurfaceBinding* Bindings,
                                      uint32 EntityRecordIdx,
                                      const FSceneRenderSettings& Settings,
                                      float DistSq,
                                      float RadiusSq)
    {
        const TVector<FResolvedSurface>& Surfaces = *Prim.Surfaces;
        const uint32 MeshletHeaderSlot = Prim.MeshletHeaderSlot;
        const uint32 NumSurfaces = Math::Min((uint32)Surfaces.size(), Prim.SurfaceCount);

        for (uint32 s = 0; s < NumSurfaces; ++s)
        {
            const FResolvedSurface& Surface = Surfaces[s];
            const FSurfaceBinding&  Binding = Bindings[s];

            EInstanceFlags Flags = Prim.BaseFlags | Binding.MaterialFlags;
            if (Prim.bCastShadow && Binding.bMaterialCastsShadows)
            {
                Flags |= EInstanceFlags::CastShadow;
            }

            // CPU LOD pick replaces LOD 0; smaller ranges directly cut cull-pass cost.
            const uint32 LODIndex       = ResolveSurfaceLOD(Surface, Prim.ForcedLODIndex, Settings.bUseLODs, DistSq, RadiusSq);
            const uint32 ShadowLODIndex = ResolveShadowLOD(Surface, LODIndex, Settings.ShadowLODBias,
                                                           DistSq, Settings.ShadowCoarseLODDistance * Settings.ShadowCoarseLODDistance);

            // Zero meshlet count gates the cull shader's MeshletHeader deref.
            const uint32 SurfaceMeshletCount  = MeshletHeaderSlot ? Surface.LODMeshletCount[LODIndex]       : 0u;
            const uint32 SurfaceMeshletOffset = Surface.LODMeshletOffset[LODIndex];
            const uint32 ShadowMeshletCount   = MeshletHeaderSlot ? Surface.LODMeshletCount[ShadowLODIndex] : 0u;
            const uint32 ShadowMeshletOffset  = Surface.LODMeshletOffset[ShadowLODIndex];

            const uint32 LastLOD = Surface.NumLODs > 0u ? Math::Min(Surface.NumLODs, (uint32)MAX_MESH_LODS) - 1u : 0u;
            const uint32 MeshletTotalCount = MeshletHeaderSlot
                                           ? Surface.LODMeshletOffset[LastLOD] + Surface.LODMeshletCount[LastLOD]
                                           : 0u;

            const uint32 BatchIndex = Binding.BatchIndex;

            // Counted only to mark the batch touched, so PrepareCounters knows what to reset.
            if (Local.DrawInstanceCounts[BatchIndex]++ == 0u)
            {
                Local.TouchedSlots.push_back(BatchIndex);
            }
            Local.BatchSkinFlags[Binding.BatchIndex] |=
                EnumHasAnyFlags(Flags, EInstanceFlags::Skinned) ? 1u : 2u;

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

    void FDefaultSceneRenderer::CullAndEmitPrimitives(const Task::FParallelRange& Range, FThreadLocalDrawData& Local)
    {
        const FFrameData&        Frame           = *ExtractFrame;
        const FSceneCullContext& SceneCull       = Frame.Geometry.SceneCullContext;
        const FSceneGlobalData&  SceneGlobalData = Frame.SceneGlobalData;
        const FVector3           CameraPos       = FVector3(SceneGlobalData.CameraData.Location);

        const FScenePrimitive*      Prims    = ScenePrimitives.GetPrimitives();
        const FVector4*             Spheres  = ScenePrimitives.GetBounds();
        const FPrimitiveCullData*   Culls    = ScenePrimitives.GetCullData();
        const FSurfaceBinding*      Bindings = ScenePrimitives.GetBindings();

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);
        auto& SkeletalStorage = Registry.storage<SSkeletalMeshComponent>();

        // Written concurrently by every worker, at disjoint per-primitive offsets. Sized once before the
        // parallel gather is launched, so no worker can reallocate it out from under another.
        TVector<FBoneTransform>& ArenaMirror = ExtractFrame->Geometry.BonesData;

        const double WorldTime = World->GetTimeSinceWorldCreation();

        for (uint32 i = Range.Start; i < Range.End; ++i)
        {
            if (ScenePrimitives.GetSource(i) != EPrimitiveSource::SkeletalMesh)
            {
                continue;
            }

            const FScenePrimitive& Prim = Prims[i];

            const FVector4&           Sphere = Spheres[i];
            const FPrimitiveCullData& Cull   = Culls[i];

            const FVector3 Center = FVector3(Sphere);
            const float    Radius = Sphere.w;

            if (!SceneCull.ShouldKeep(Center, Radius, Cull.bCastShadow != 0u, Cull.MaxDrawDistance, CameraPos))
            {
                ++Local.Stats.NumInstancesCulled;
                continue;
            }

            if (Prim.Surfaces == nullptr || Prim.SurfaceCount == 0u)
            {
                continue;   // parked awaiting resolve; the sync pass retries it
            }

            const FVector3 ToCamera = Center - CameraPos;
            const float    DistSq   = Math::Dot(ToCamera, ToCamera);
            const float    RadiusSq = Radius * Radius;

            const uint32 EntityRecordIdx = (uint32)Local.EntityRecords.size();
            FEntityRecord& EntityRecord = Local.EntityRecords.emplace_back();
            EntityRecord.Transform            = Prim.Transform;
            EntityRecord.SphereBounds         = Sphere;
            EntityRecord.MeshletHeaderSlot = Prim.MeshletHeaderSlot;
            EntityRecord.CustomData           = Prim.CustomData;
            EntityRecord.EntityID             = Prim.EntityID;
            EntityRecord.BoneArenaBase        = ~0u;
            EntityRecord.BoneArenaCount       = 0u;

            //~ Everything below needs the live component.

            if (!SkeletalStorage.contains(Prim.Entity))
            {
                continue;
            }
            SSkeletalMeshComponent& MeshComponent = SkeletalStorage.get(Prim.Entity);

            if (SceneCull.IsCameraVisible(Center, Radius, Cull.MaxDrawDistance, CameraPos))
            {
                MeshComponent.LastRenderedTime = WorldTime;
            }

            CMesh* Mesh = MeshComponent.SkeletalMesh;
            if (!IsValid(Mesh))
            {
                continue;
            }

            const FMeshResource&     Resource = Mesh->GetMeshResource();
            const CSkeletalMesh*     SkelMesh = MeshComponent.SkeletalMesh.Get();
            const FSkeletonResource* SkelRes  = (SkelMesh && SkelMesh->Skeleton.IsValid())
                ? SkelMesh->Skeleton->GetSkeletonResource()
                : nullptr;
            const uint32 SkeletonBoneCount = SkelRes ? (uint32)SkelRes->GetNumBones() : 0u;

            if (Resource.RequiredBoneCount > SkeletonBoneCount)
            {
                static uint32 BoneRangeLogCount = 0;
                if (BoneRangeLogCount++ < 8u)
                {
                    LOG_ERROR("Skinning: mesh '{}' references {} bones but its skeleton provides {}. Vertices "
                              "weighted to joints >= {} read PAST the bone slice (the GPU fetch is unbounded) "
                              "and will be wildly displaced. Mesh and skeleton are out of sync -- reimport both.",
                              Mesh->GetName(), Resource.RequiredBoneCount, SkeletonBoneCount, SkeletonBoneCount);
                }
            }

            // The pose goes straight into this primitive's own arena slice. Ranges are disjoint per
            // primitive and the base is stable, so parallel workers need no shared cursor and no
            // per-thread base to be reconciled later -- that is the whole point of the arena.
            uint32 BoneArenaBase  = ~0u;
            uint32 BoneArenaCount = 0u;
            if (SkeletonBoneCount > 0
                && Prim.BoneArenaBase != kNoBoneRange
                && (SIZE_T)Prim.BoneArenaBase + SkeletonBoneCount <= ArenaMirror.size())
            {
                // The sync claimed the slice from the SKELETON's bone count; a mismatch here means the
                // asset changed and the sync has not caught up, so skip rather than write out of the slice.
                if (SkeletonBoneCount <= Prim.BoneArenaCount)
                {
                    BoneArenaBase  = Prim.BoneArenaBase;
                    BoneArenaCount = SkeletonBoneCount;

                    FBoneTransform* Dst = ArenaMirror.data() + BoneArenaBase;

                    if ((uint32)MeshComponent.BoneTransforms.size() == SkeletonBoneCount)
                    {
                        if (MeshComponent.bRenderBonesDirty || (uint32)MeshComponent.RenderBones.size() != SkeletonBoneCount * 3u)
                        {
                            SkeletalUtils::PackRenderBones(MeshComponent.BoneTransforms, MeshComponent.RenderBones);
                            MeshComponent.bRenderBonesDirty = false;
                        }

                        const FBoneTransform* Packed = reinterpret_cast<const FBoneTransform*>(MeshComponent.RenderBones.data());
                        Memory::Memcpy(Dst, Packed, (SIZE_T)SkeletonBoneCount * sizeof(FBoneTransform));
                    }
                    else
                    {
                        // No active animation: BoneWorld * InvBindMatrix collapses to identity for every bone.
                        static constexpr FBoneTransform IdentityBone{ FVector4(1,0,0,0), FVector4(0,1,0,0), FVector4(0,0,1,0) };
                        for (uint32 b = 0; b < SkeletonBoneCount; ++b)
                        {
                            Dst[b] = IdentityBone;
                        }
                    }
                }
            }

            EntityRecord.BoneArenaBase  = BoneArenaBase;
            EntityRecord.BoneArenaCount = BoneArenaCount;

            MeshComponent.LastDistanceOverRadius = (Radius > 0.0f) ? (Math::Sqrt(DistSq) / Radius) : 0.0f;

            EmitPrimitiveSurfaces(Local, Prim, Bindings + Prim.BindingBase,
                                  EntityRecordIdx, RenderSettings, DistSq, RadiusSq);
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

        // Bone bases are stable now, so there is no per-thread base to reconcile. What the merge collects
        // instead is which arena slices were written, so the upload can send those and nothing else.
        TVector<FUIntVector2>& BoneUploadRanges = Frame.Geometry.BoneUploadRanges;
        BoneUploadRanges.clear();

        uint64 TotalInstancesCulled = 0;
        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FThreadLocalDrawData& Local = ThreadLocal[t];
            TotalInstancesCulled += Local.Stats.NumInstancesCulled;

            for (FEntityRecord& Rec : Local.EntityRecords)
            {
                if (Rec.BoneArenaBase == ~0u)
                {
                    continue;
                }

                // The pose has to reach the GPU whichever path skins it: the pre-skin pass reads it, and so
                // does the inline fallback in the draw.
                BoneUploadRanges.push_back(FUIntVector2{ Rec.BoneArenaBase, Rec.BoneArenaCount });
            }
        }
        FrameStats.NumInstancesCulled += TotalInstancesCulled;

        FSceneBatchRegistry& Registry = ScenePrimitives.GetBatches();
        const uint32 NumBatches = Registry.Num();

        DrawCommands.clear();

        for (uint32 b = 0; b < NumBatches; ++b)
        {
            const FSceneBatchRegistry::FBatch& Batch = Registry.Get(b);

            uint8 SkinFlags = 0u;
            for (uint32 t = 0; t < NumThreads; ++t)
            {
                if (ThreadLocal[t].bTouched)
                {
                    SkinFlags |= ThreadLocal[t].BatchSkinFlags[b];
                }
            }
            if (SkinFlags == 0u)
            {
                SkinFlags = 2u;
            }

            FMeshDrawCommand& Cmd = DrawCommands.emplace_back();
            Cmd.VertexShader                   = Batch.VertexShader;
            Cmd.MeshShaderShadow               = Batch.MeshShaderShadow;
            Cmd.MeshShaderBase                 = Batch.MeshShaderBase;
            Cmd.PixelShader                    = Batch.PixelShader;
            Cmd.VisBufferMeshShader            = Batch.VisBufferMeshShader;
            Cmd.VisBufferMeshShaderMasked      = Batch.VisBufferMeshShaderMasked;
            Cmd.MaskedVisBufferPixelShader     = Batch.MaskedVisBufferPixelShader;
            Cmd.MomentPixelShader              = Batch.MomentPixelShader;
            Cmd.IndirectDrawOffset             = b;
            Cmd.DrawCount                      = 1u;
            Cmd.bTranslucent                   = Batch.Key.bTranslucent;
            Cmd.bMasked                        = Batch.Key.bMasked;
            Cmd.bAdditive                      = Batch.Key.bAdditive;
            Cmd.bTwoSided                      = Batch.Key.bTwoSided;
            Cmd.bAnySkinned                    = (SkinFlags & 1u) ? 1u : 0u;
            Cmd.bAnyStatic                     = (SkinFlags & 2u) ? 1u : 0u;

            if (!Batch.Key.bTranslucent)
            {
                for (const FSceneBatchRegistry::FDeferredMaterialSlot& MatSlot : Batch.DeferredMaterials)
                {
                    DeferredMaterials.push_back({ (uint32)MatSlot.MaterialIndex, MatSlot.DeferredShader });
                }
            }
        }

        // The per-draw meshlet prefix is GPU-built by BuildDrawPrefix; the CPU only publishes the count.
        NumDrawsPerView   = NumBatches;

        // Skinned instances are no longer CPU-fed into the visible buffer at all: CullInstances emits them
        // like everything else. What the CPU still owns is the per-frame half of their payload -- the LOD
        // ranges it resolved and the pre-skin slices it assigned against those ranges -- published per
        // RETAINED slot so the cull can read it before compaction exists.
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
                    // FrameTag is stamped by UploadSkinnedFrameData, on the render thread: the tag this
                    // frame will cull against does not exist yet here.
                    Out.SurfaceMeshletOffset    = Item.SurfaceMeshletOffset;
                    Out.SurfaceMeshletCount     = Item.SurfaceMeshletCount;
                    Out.ShadowMeshletOffset     = Item.ShadowMeshletOffset;
                    Out.ShadowMeshletCount      = Item.ShadowMeshletCount;
                    Out.MeshletTotalCount       = Item.MeshletTotalCount;
                    // Claimed by CullInstances, which writes them back here. Seeded to the sentinel so a
                    // slot the cull REJECTS keeps "no slice" rather than last frame's base -- the skinning
                    // pass walks this list and would otherwise write into someone else's slice.
                    Out.SkinnedVertexBase       = kNoPreSkinBase;
                    Out.ShadowSkinnedVertexBase = kNoPreSkinBase;

                    SkinnedSlots.push_back(Item.InstanceSlot);
                }
            }
        }

        const uint32 NumEmittedBatches = (uint32)DrawCommands.size();
        FrameStats.NumBatches = NumEmittedBatches;
        OpaqueDrawList.reserve(NumEmittedBatches);
        TranslucentDrawList.reserve(NumEmittedBatches);
        for (uint32 i = 0; i < NumEmittedBatches; ++i)
        {
            if (DrawCommands[i].bTranslucent)
            {
                TranslucentDrawList.push_back(i);
            }
            else
            {
                OpaqueDrawList.push_back(i);
            }
        }
    }

    bool FDefaultSceneRenderer::ShouldRequestShadow(const FVector3& LightPosition, float LightRadius) const
    {
        return ExtractFrame->CameraFrustum.IntersectsSphere(LightPosition, LightRadius);
    }

    void FDefaultSceneRenderer::BuildSceneCullContext()
    {
        LUMINA_PROFILE_SCOPE();

        FFrameData& Frame = *ExtractFrame;
        auto& SceneCullContext = Frame.Geometry.SceneCullContext;

        SceneCullContext.Reset();
        SceneCullContext.bEnabled = RenderSettings.bCPUInstanceCull;
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

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        // First enabled directional light wins (matches ProcessDirectionalLight).
        auto DirectionalView = Registry.view<SDirectionalLightComponent>(entt::exclude<SDisabledTag>);
        for (entt::entity Entity : DirectionalView)
        {
            const SDirectionalLightComponent& Light = DirectionalView.get<SDirectionalLightComponent>(Entity);
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

        // Shadow-casting locals: only keep lights whose attenuation sphere intersects camera frustum.
        auto PointView = Registry.view<SPointLightComponent, STransformComponent>(entt::exclude<SDisabledTag>);
        for (entt::entity Entity : PointView)
        {
            const SPointLightComponent& Light = PointView.get<SPointLightComponent>(Entity);
            if (!Light.bCastShadows)
            {
                continue;
            }
            const STransformComponent&  Transform = PointView.get<STransformComponent>(Entity);
            const float     Radius   = Light.Attenuation;
            // Test with the location already resident in the transform's SIMD lanes (no scalar round-trip).
            if (!SceneCullContext.Frustum.IntersectsSphere(Transform.GetWorldTransformCached().Location, Radius))
            {
                continue;
            }
            SceneCullContext.ShadowLights.push_back({ Transform.GetWorldLocationCached(), Radius });
        }

        auto SpotView = Registry.view<SSpotLightComponent, STransformComponent>(entt::exclude<SDisabledTag>);
        for (entt::entity Entity : SpotView)
        {
            const SSpotLightComponent& Light    = SpotView.get<SSpotLightComponent>(Entity);
            if (!Light.bCastShadows)
            {
                continue;
            }
            const STransformComponent& Transform = SpotView.get<STransformComponent>(Entity);
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
        FSceneLightData& LightData              = Frame.Lighting.LightData;
        TVector<FShadowRequest>& ShadowRequests = Frame.Lighting.ShadowRequests;
        FMutex& ShadowRequestMutex              = Frame.Lighting.ShadowRequestMutex;

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
        Light.Position              = TransformComponent.GetWorldLocationCached();
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

        LightData.Lights[Lights] = Light;
    }

    void FDefaultSceneRenderer::ProcessSpotLight(const SSpotLightComponent& SpotLight, const STransformComponent& TransformComponent, TAtomic<uint32>& LightCount)
    {
        FFrameData& Frame = *ExtractFrame;
        auto& LightData         = Frame.Lighting.LightData;
        auto& ShadowRequests    = Frame.Lighting.ShadowRequests;
        auto& ShadowRequestMutex= Frame.Lighting.ShadowRequestMutex;

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
        Light.Position              = TransformComponent.GetWorldLocationCached();
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

        LightData.Lights[Lights] = Light;
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
                TVector<uint32> Order;
                Order.resize(ShadowRequests.size());
                for (uint32 i = 0; i < (uint32)ShadowRequests.size(); ++i)
                {
                    Order[i] = i;
                }
                eastl::stable_sort(Order.begin(), Order.end(),
                    [&](uint32 A, uint32 B)
                    {
                        return ShadowRequests[A].DistanceToCamera > ShadowRequests[B].DistanceToCamera;
                    });

                TVector<bool> Drop;
                Drop.assign(ShadowRequests.size(), false);
                for (uint32 i = 0; i < (uint32)Order.size() && UsedViews > AvailableViews; ++i)
                {
                    const uint32 Idx = Order[i];
                    Drop[Idx] = true;
                    UsedViews -= ViewCost(ShadowRequests[Idx]);
                }

                TVector<FShadowRequest> Kept;
                Kept.reserve(ShadowRequests.size());
                for (uint32 i = 0; i < (uint32)ShadowRequests.size(); ++i)
                {
                    if (!Drop[i])
                    {
                        Kept.push_back(ShadowRequests[i]);
                    }
                }
                ShadowRequests = std::move(Kept);
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
        Sizes.resize(NumRequests);
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
        SortedIndices.resize(NumRequests);
        for (uint32 i = 0; i < NumRequests; ++i)
        {
            SortedIndices[i] = i;
        }
        eastl::sort(SortedIndices.begin(), SortedIndices.end(),
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

                LightData.Lights[Req.LightIndex].ShadowDataIndex = (int32)ShadowSlot;
                FLightShadowData& ShadowData = LightData.Shadows[ShadowSlot];

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

                LightData.Lights[Req.LightIndex].ShadowDataIndex = (int32)ShadowSlot;
                FLightShadowData& ShadowData = LightData.Shadows[ShadowSlot];
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

        // Both VisBuffer phases rasterize the SAME camera view -- the phase is a push constant, not a
        // second FCullView -- so the camera contributes one entry, not two.
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
        
        const uint32 ConeFlag = RenderSettings.bConeCull ? (uint32)ECullViewFlags::Cone : 0u;
        
        {
            const FMatrix4 CameraVP = ViewVolume.GetProjectionMatrix() * ViewVolume.GetViewMatrix();
            uint32 CameraFlags = ConeFlag;
            if (RenderSettings.bFrustumCull)
            {
                CameraFlags |= ECullViewFlags::Frustum;
            }
            if (RenderSettings.bOcclusionCull && bDepthPyramidValid.load(std::memory_order_acquire))
            {
                CameraFlags |= ECullViewFlags::Occlusion;
            }
            // The one flag that makes a view two-phase. Primary camera only: any other view is emitted
            // entirely by the early dispatch, and setting this would make the late one emit it a second
            // time. Without a valid pyramid the frame collapses to a single full-cull early phase.
            if (RenderSettings.bMeshletOcclusionCull && bDepthPyramidValid.load(std::memory_order_acquire))
            {
                CameraFlags |= ECullViewFlags::MeshletHiZ;
            }
            PushView(CameraVP, ViewVolume.GetViewPosition(), CameraFlags);
        }
        
        if (LightData.bHasSun)
        {
            const int32 SunShadowIndex = LightData.Lights[0].ShadowDataIndex;
            if (SunShadowIndex != INDEX_NONE)
            {
                const FLightShadowData& SunShadow = LightData.Shadows[SunShadowIndex];
                const uint32 CascadeFlags =
                    (RenderSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
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

            const FLightShadowData& ShadowData = LightData.Shadows[PointShadow.ShadowDataIndex];
            const FLight& Light = LightData.Lights[PointShadow.LightIndex];
            const uint32 FaceFlags =
                (RenderSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
                ConeFlag |
                ECullViewFlags::CastShadowOnly;

            PointShadowCullViewBases.push_back((uint32)CullViews.size());
            for (int32 Face = 0; Face < 6; ++Face)
            {
                PushView(ShadowData.ViewProjection[Face], Light.Position, FaceFlags);
            }
        }

        // Spotlights: one view each.
        for (const FLightShadow& SpotShadow : PackedShadows[(uint32)ELightType::Spot])
        {
            if (SpotShadow.ShadowDataIndex < 0)
            {
                SpotShadowCullViewBases.push_back(~0u);
                continue;
            }

            const FLightShadowData& ShadowData = LightData.Shadows[SpotShadow.ShadowDataIndex];
            const FLight& Light = LightData.Lights[SpotShadow.LightIndex];
            const uint32 SpotFlags =
                (RenderSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
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

    void FDefaultSceneRenderer::ProcessDirectionalLight(const SDirectionalLightComponent& DirectionalLight, TAtomic<uint32>& LightCount)
    {
        FFrameData& Frame          = *ExtractFrame;
        auto& LightData            = Frame.Lighting.LightData;
        auto& ShadowDataCount      = Frame.Lighting.ShadowDataCount;
        auto& SceneGlobalData      = Frame.SceneGlobalData;

        LightData.bHasSun = true;
        const FViewVolume& ViewVolume = Frame.ViewVolume;

        const float NearClip = ViewVolume.GetNear();
        const float FarClip  = ViewVolume.GetFar();

        // Optional black-body tint: physical sun color from correlated color temperature.
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
                CascadeShadowData     = &LightData.Shadows[ShadowSlot];
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
                                            0.0f, 0.0f);

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

        LightCount.fetch_add(1, std::memory_order_acquire);
        LightData.Lights[0] = Light;
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

        // Parallel scatter: each worker copies its buckets into their reserved slices.
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
        Billboard.EntityID              = entt::null;
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
        FFrameData& Frame = *ExtractFrame;

        Frame.Geometry.DrawCommands.clear();
        Frame.Geometry.OpaqueDrawList.clear();
        Frame.Geometry.TranslucentDrawList.clear();
        // BonesData is NOT cleared: it is arena-indexed, so clearing it would only force resize() to
        // value-initialise the whole arena again every frame. Slices nobody gathered are never uploaded,
        // so their contents are don't-care rather than stale.
        Frame.Geometry.BoneUploadRanges.clear();
        Frame.Views.NumDrawsPerView   = 0;
    }

    void FDefaultSceneRenderer::ResetPass_Render(RHI::FCmdListH CL)
    {
        // Depth, the shadow atlas and the cascade were last written as attachments -- by the previous
        // frame's raster, or by the widget pass just above. Same-queue submission orders those writes
        // ahead of these clears but does not make them available to the transfer stage, so without this
        // the clears are an unsynchronized write-after-write on all three images.
        Barriers::AllToTransfer(CL);

        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            const float DepthClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::DepthAttachment).Texture, DepthClear);
        }

        const float ShadowClear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        RHI::CmdClearTexture(CL, ShadowAtlas.GetImage().Texture, ShadowClear);
        RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::Cascade).Texture, ShadowClear);

        Barriers::TransferToAll(CL);
    }

    void FDefaultSceneRenderer::SkinningPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        // Two blocks per gathered skinned SLOT: the surface LOD it rasterizes and the coarser shadow LOD,
        // each with its own compacted slice. Both may be empty -- BuildSkinWork decides, and an all-empty
        // frame costs one 1-thread dispatch and an indirect grid of zero.
        const uint32 NumSkinned = (uint32)Frame.Geometry.SkinnedSlots.size();
        if (NumSkinned == 0)
        {
            return;
        }

        // The GPU scope is opened by the caller, not here.
        LUMINA_PROFILE_SECTION_COLORED("Skinning Pass", tracy::Color::SkyBlue);

        static const FShaderH WorkShader = FShaderLibrary::Get("BuildSkinWork.slang");
        static const FShaderH SkinShader = FShaderLibrary::Get("Skinning.slang");
        if (!SkinShader || !WorkShader)
        {
            return;
        }

        const uint8  Slot     = CurrentFrameSlot;
        const uint32 NumPairs = NumSkinned * 2u;

        ResizeBufferIfNeeded(CL, SkinWorkBaseRing[Slot], (uint64)NumPairs * sizeof(uint32), 1.5f,
                             SkinWorkBaseLowUsage[Slot], true, EBufferInit::Zeroed, "Skinning.WorkBase");
        if (!SkinWorkBaseRing[Slot] || !GetSkinDispatchArgs()
            || !SkinnedSlotListBuffer || !SkinnedFrameDataBuffer || !RetainedStaticBuffer)
        {
            return;
        }

        //~ Lay out the dispatch: one workgroup per (instance, block, meshlet), enumerated on the GPU.
        {
            struct FBuildSkinWorkPC
            {
                uint32 NumSlots;
                uint32 _Pad0;
                uint64 SlotListAddr;
                uint64 SkinnedDataAddr;
                uint64 OutWorkBaseAddr;
                uint64 OutDispatchArgsAddr;
            } WPC = {};
            static_assert(sizeof(FBuildSkinWorkPC) == 40, "FBuildSkinWorkPC must match BuildSkinWork.slang.");

            WPC.NumSlots            = NumSkinned;
            WPC.SlotListAddr        = SkinnedSlotListBuffer.GetAddress();
            WPC.SkinnedDataAddr     = SkinnedFrameDataBuffer.GetAddress();
            WPC.OutWorkBaseAddr     = SkinWorkBaseRing[Slot].GetAddress();
            WPC.OutDispatchArgsAddr = GetSkinDispatchArgs().GetAddress();

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(WorkShader));
            RHI::CmdDispatch(CL, MakeArgs(WPC), 1u, 1u, 1u);

            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute,
                RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments);
        }

        const uint32 VertexCapacity = (uint32)Math::Min<uint64>(
            GetPreSkinnedVerticesBuffer().GetSize() / sizeof(FPreSkinnedVertex), 0xFFFFFFFFull);

        struct FSkinningPushConstants
        {
            uint32 NumPairs;
            uint32 VertexCapacity;
            uint64 WorkBaseAddr;
            uint64 SlotListAddr;
            uint64 SkinnedDataAddr;
            uint64 RetainedStaticAddr;
        } PC = {};
        static_assert(sizeof(FSkinningPushConstants) == 40, "FSkinningPushConstants must match Skinning.slang.");

        PC.NumPairs           = NumPairs;
        PC.VertexCapacity     = VertexCapacity;
        PC.WorkBaseAddr       = SkinWorkBaseRing[Slot].GetAddress();
        PC.SlotListAddr       = SkinnedSlotListBuffer.GetAddress();
        PC.SkinnedDataAddr    = SkinnedFrameDataBuffer.GetAddress();
        PC.RetainedStaticAddr = RetainedStaticBuffer.GetAddress();

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(SkinShader));
        RHI::CmdDispatchIndirect(CL, MakeArgs(PC), GetSkinDispatchArgs().Ptr, 0u);

        // Pre-skinned vertices feed every draw VS.
        Barriers::ComputeToAll(CL);
    }

    void FDefaultSceneRenderer::TexturePaintPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.Extracts.PaintOps.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Texture Paint Pass", tracy::Color::Red);

        static const FShaderH PaintShader = FShaderLibrary::Get("TexturePaint.slang");
        if (!PaintShader)
        {
            return;
        }

        RHI::FPipelineH Pipeline = GetOrCreateComputePipeline(PaintShader);

        struct FPaintPC
        {
            uint32      TargetIndex;
            int32       BrushIndex;
            uint32      TargetSize[2];
            uint32      RectMin[2];
            uint32      RectMax[2];
            float       CenterPx[2];
            float       RadiusPx;
            float       Strength;
            float       Hardness;
            float       ColorR;
            float       ColorG;
            float       ColorB;
            float       ColorA;
        };

        bool bBoundPipeline = false;

        for (const FTexturePaintOp& Op : Frame.Extracts.PaintOps)
        {
            if (!RHI::IsValid(Op.Target))
            {
                continue;
            }

            if (Op.Mode == FTexturePaintOp::EMode::Clear)
            {
                Barriers::AllToTransfer(CL);
                const float Clear[4] = { Op.Color.r, Op.Color.g, Op.Color.b, Op.Color.a };
                RHI::CmdClearTexture(CL, Op.Target, Clear);
                continue;
            }

            if (Op.TargetUAV == RHI::kInvalidHeapSlot)
            {
                continue;
            }

            const uint32 W = Op.TargetExtent.x;
            const uint32 H = Op.TargetExtent.y;
            const float  CenterX = Op.CenterUV.x * (float)W;
            const float  CenterY = Op.CenterUV.y * (float)H;
            // Radius is relative to the longer side so the brush stays circular in pixels.
            const float  RadiusPx = std::max(Op.RadiusUV * (float)std::max(W, H), 1.0f);

            const int32 MinX = std::clamp((int32)std::floor(CenterX - RadiusPx), 0, (int32)W);
            const int32 MinY = std::clamp((int32)std::floor(CenterY - RadiusPx), 0, (int32)H);
            const int32 MaxX = std::clamp((int32)std::ceil (CenterX + RadiusPx), 0, (int32)W);
            const int32 MaxY = std::clamp((int32)std::ceil (CenterY + RadiusPx), 0, (int32)H);
            if (MaxX <= MinX || MaxY <= MinY)
            {
                continue;
            }

            if (!bBoundPipeline)
            {
                RHI::CmdSetPipeline(CL, Pipeline);
                bBoundPipeline = true;
            }

            // Order against any prior clear/paint of the same target.
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer | RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);

            FPaintPC PC = {};
            PC.TargetIndex   = Op.TargetUAV;
            PC.BrushIndex    = Op.BrushIndex;
            PC.TargetSize[0] = W;             PC.TargetSize[1] = H;
            PC.RectMin[0]    = (uint32)MinX;  PC.RectMin[1]    = (uint32)MinY;
            PC.RectMax[0]    = (uint32)MaxX;  PC.RectMax[1]    = (uint32)MaxY;
            PC.CenterPx[0]   = CenterX;       PC.CenterPx[1]   = CenterY;
            PC.RadiusPx      = RadiusPx;
            PC.Strength      = Op.Strength;
            PC.Hardness      = Op.Hardness;
            PC.ColorR        = Op.Color.r;
            PC.ColorG        = Op.Color.g;
            PC.ColorB        = Op.Color.b;
            PC.ColorA        = Op.Color.a;

            const uint32 DispatchX = RenderUtils::GetGroupCount((uint32)(MaxX - MinX), 8u);
            const uint32 DispatchY = RenderUtils::GetGroupCount((uint32)(MaxY - MinY), 8u);
            RHI::CmdDispatch(CL, MakeArgs(PC), DispatchX, DispatchY, 1u);
        }

        // Make the painted texels visible to every later sampler.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer | RHI::EStageFlags::Compute, RHI::EStageFlags::AllCommands);
    }

    void FDefaultSceneRenderer::VisBufferPass(RHI::FCmdListH CL, uint32 ViewIndex, bool bClear,
                                            ECullPhase::Type Phase)
    {
        const FFrameData& Frame             = *RenderFrame;
        const auto& DrawCommands            = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList          = Frame.Geometry.OpaqueDrawList;

        if (DrawCommands.empty() || ViewIndex == ~0u)
        {
            return;
        }

        // A single-phase view is drawn in full by Early, and the meshlet cull emitted nothing for it into
        // the Late slice. Skipping the draw is what collapses the frame back to one phase.
        if (Phase == ECullPhase::Late)
        {
            if (ViewIndex >= Frame.Views.CullViews.size() ||
                (GetCullViewFlags(Frame.Views.CullViews[ViewIndex]) & ECullViewFlags::MeshletHiZ) == 0u)
            {
                return;
            }
        }

        {
            const RHI::GPUPtr DrawListAddr  = GetMeshletDrawList().GetAddress();
            const RHI::GPUPtr InstancesAddr = SceneRootShared.Instances;
            const RHI::GPUPtr BucketsAddr   = GetRenderBuckets().GetAddress();
            const char* MissingBuffer = DrawListAddr == 0  ? "MeshletDrawList"
                                      : InstancesAddr == 0 ? "Instances (visible-instance ring)"
                                      : BucketsAddr == 0   ? "RenderBuckets"
                                                           : nullptr;
            if (MissingBuffer != nullptr)
            {
                static FName LastMissing;
                const FName Missing(MissingBuffer);
                if (Missing != LastMissing)
                {
                    LastMissing = Missing;
                    LOG_ERROR("VisBuffer: '{}' has no device address this frame; skipping the pass. "
                              "Drawing would page-fault the GPU inside the mesh shader.", MissingBuffer);
                }
                return;
            }
        }

        LUMINA_PROFILE_SECTION_COLORED("VisBuffer Geometry Pass", tracy::Color::Red);

        static const FShaderH VisPixel = FShaderLibrary::Get("VisBufferPixel.slang");
        if (!VisPixel)
        {
            return;
        }

        Barriers::ComputeToAll(CL);

        const FSceneImage& VisRT   = GetNamedImage(ENamedImage::VisBuffer);
        const FSceneImage& DepthRT = GetSceneDepthRT();
        const FUIntVector2 Extent  = GetNamedImage(ENamedImage::HDR).GetExtent();
        
        const RHI::ELoadOp GeomLoadOp = bClear ? RHI::ELoadOp::Clear : RHI::ELoadOp::Load;

        RHI::FRenderAttachment Color;
        Color.Texture = VisRT.Texture;
        Color.LoadOp  = GeomLoadOp;            // clears to 0 = empty (geometry stores VisID + 1)
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments               = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture        = DepthRT.Texture;
        Pass.DepthAttachment.ResolveTexture = GetSceneDepthResolve();
        Pass.DepthAttachment.LoadOp         = GeomLoadOp;
        Pass.DepthAttachment.StoreOp        = RHI::EStoreOp::Store;
        Pass.DepthAttachment.Color[0]       = 0.0f;   // reverse-Z clear
        Pass.RenderArea                     = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest = RHI::EOp::Greater;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));

        FMeshletPassContext Ctx;
        Ctx.CullViewIndex     = ViewIndex;
        Ctx.ViewportW         = (float)Extent.x;
        Ctx.ViewportH         = (float)Extent.y;
        // The two phases append into the same bucket, so each rasterizes only what its own cull added.
        Ctx.Slice             = (Phase == ECullPhase::Late) ? EMeshletSlice::Late : EMeshletSlice::Early;

        ForEachMeshletBatch(CL, OpaqueDrawList, Ctx,
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand& Batch)
            {
                const bool bMaskedClip = Batch.bMasked
                                      && Batch.MaskedVisBufferPixelShader != nullptr
                                      && Batch.VisBufferMeshShaderMasked != nullptr;

                Key.MS               = bMaskedClip ? Batch.VisBufferMeshShaderMasked : Batch.VisBufferMeshShader;
                Key.PS               = bMaskedClip ? Batch.MaskedVisBufferPixelShader : VisPixel;
                Key.bVisBufferMasked = bMaskedClip;   // interpolants only when actually masked-clipping
                Key.bWireframe       = RenderSettings.bWireframe;
                Key.SampleCount      = MSAASampleCount;
                // The backface bit must agree with the CmdSetCullMode below or two-sided geometry vanishes.
                Key.TriCullMode      = (uint8)((Batch.bTwoSided ? 0u : (uint32)TriCull_Backface)
                                             | ((RenderSettings.bWireframe || Key.SampleCount > 1) ? 0u : (uint32)TriCull_SmallPrim));
                Key.DepthFormat      = EFormat::D32;
                Key.ColorTargets.push_back({ VisRT.Desc.Format, {} });
                return true;
            },
            [&](const FMeshDrawCommand& Batch)
            {
                if (RenderSettings.bWireframe)
                {
                    RHI::CmdSetLineWidth(CL, 1.5f);
                }

                // Two-sided materials must rasterize both faces into the VisBuffer.
                RHI::CmdSetCullMode(CL, Batch.bTwoSided ? RHI::ECullMode::None : RHI::ECullMode::Back);
            });

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::BuildDepthPyramid(RHI::FCmdListH CL, const FSceneImage& Source, const FSceneImage& Pyramid, bool bReduceMax)
    {
        static const FShaderH ComputeShader = FShaderLibrary::Get("DepthPyramidSPD.slang");
        if (!ComputeShader)
        {
            return;
        }

        const FSceneBuffer SpdCounter = GetSpdCounter();

        const uint32 PyramidW = Pyramid.GetSizeX();
        const uint32 PyramidH = Pyramid.GetSizeY();
        const uint32 MipCount = Pyramid.GetNumMips();

        constexpr uint32 SpdMaxMips = 12;
        const uint32 NumMips = std::min(MipCount, SpdMaxMips);

        RHI::CmdMemset(CL, SpdCounter.Ptr, SpdCounter.Size, 0u);
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer | RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::FragmentTests,
            RHI::EStageFlags::Compute);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        struct FSpdPushConstants
        {
            uint32 PyramidSize[2];
            uint32 NumMips;
            uint32 NumWorkGroups;
            float  InvPyramidSize[2];
            uint32 SrcDepthIndex;
            uint32 ReduceMax;
            uint64 AtomicCounter;
            uint32 MipUAV[SpdMaxMips];
        } PC = {};

        constexpr uint32 SpdTileSize = 32;
        const uint32 DispatchX = RenderUtils::GetGroupCount(PyramidW, SpdTileSize);
        const uint32 DispatchY = RenderUtils::GetGroupCount(PyramidH, SpdTileSize);
        const uint32 TotalGroups = DispatchX * DispatchY;

        PC.PyramidSize[0]     = PyramidW;
        PC.PyramidSize[1]     = PyramidH;
        PC.NumMips            = NumMips;
        PC.NumWorkGroups      = TotalGroups;
        PC.InvPyramidSize[0]  = 1.0f / (float)PyramidW;
        PC.InvPyramidSize[1]  = 1.0f / (float)PyramidH;
        const int32 SrcDepthSlot = Source.GetResourceID();
        if (SrcDepthSlot < 0)
        {
            LOG_ERROR("Depth pyramid: source image has no sampled heap slot; skipping. Sampling it would "
                      "index the texture heap with 0xFFFFFFFF and fault the device.");
            return;
        }

        PC.SrcDepthIndex      = (uint32)SrcDepthSlot;
        PC.ReduceMax          = bReduceMax ? 1u : 0u;
        PC.AtomicCounter      = SpdCounter.GetAddress();
        for (uint32 i = 0; i < SpdMaxMips; ++i)
        {
            const uint32 SrcMip = (i < MipCount) ? i : 0u;
            const int32  Slot   = Pyramid.GetMipUAVIndex(SrcMip);
            if (Slot < 0)
            {
                LOG_ERROR("Depth pyramid: mip {} has no storage heap slot; skipping the pass.", SrcMip);
                return;
            }
            PC.MipUAV[i] = (uint32)Slot;
        }

        RHI::CmdDispatch(CL, MakeArgs(PC), DispatchX, DispatchY, 1);

        Barriers::ComputeToAll(CL);
    }

    void FDefaultSceneRenderer::DepthPyramidPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (DrawCommands.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Depth Pyramid Pass (SPD)", tracy::Color::Orange);

        // Reverse-Z scene depth: MIN reduction keeps the farthest occluder in each footprint.
        BuildDepthPyramid(CL,
            GetNamedImage(ENamedImage::DepthAttachment),
            GetNamedImage(ENamedImage::DepthPyramid),
            /*bReduceMax*/ false);
    }

    void FDefaultSceneRenderer::CascadePyramidPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& LightData   = Frame.Lighting.LightData;

        if (!LightData.bHasSun ||
            Frame.Geometry.DrawCommands.empty() ||
            LightData.Lights[0].ShadowDataIndex == INDEX_NONE ||
            Frame.Views.CascadeViewBase == ~0u)
        {
            bCascadePyramidValid.store(false, std::memory_order_release);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cascade Pyramid Pass (SPD)", tracy::Color::Orange3);

        BuildDepthPyramid(CL,
            GetNamedImage(ENamedImage::Cascade),
            GetNamedImage(ENamedImage::CascadePyramid),
            /*bReduceMax*/ true);

        bCascadePyramidValid.store(true, std::memory_order_release);
    }

    void FDefaultSceneRenderer::ClusterBuildPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        const bool bHasTerrain = !Frame.Extracts.TerrainExtracts.empty();
        if (DrawCommands.empty() && !bHasTerrain)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cluster Build Pass", tracy::Color::Pink2);
        
        static const FShaderH ComputeShader = FShaderLibrary::Get("ClusterBuild.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        constexpr uint32 ClusterBuildGroupSize = 64;
        constexpr uint32 ClusterDispatchGroups = (NumClusters + ClusterBuildGroupSize - 1) / ClusterBuildGroupSize;
        RHI::CmdDispatch(CL, MakeArgs(), ClusterDispatchGroups, 1, 1);

        // LightCull consumes the cluster AABBs next.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);
    }

    void FDefaultSceneRenderer::LightCullPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        const bool bHasTerrain = !Frame.Extracts.TerrainExtracts.empty();
        if (DrawCommands.empty() && !bHasTerrain)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Light Cull Pass", tracy::Color::Pink2);

        static const FShaderH ComputeShader = FShaderLibrary::Get("LightCull.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        constexpr uint32 LightCullGroupSize = 128;
        constexpr uint32 LightCullGroups    = (NumClusters + LightCullGroupSize - 1) / LightCullGroupSize;
        RHI::CmdDispatch(CL, MakeArgs(), LightCullGroups, 1, 1);

        // Cluster light lists feed the lit pixel shaders.
        Barriers::ComputeToAll(CL);
    }

    bool FDefaultSceneRenderer::BindShadowBatchPipeline(RHI::FCmdListH CL, const FMeshDrawCommand& Batch,
                                                      FShaderH PixelShader)
    {
        FGraphicsPipelineKey Key;
        Key.MS          = Batch.MeshShaderShadow;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        // SPEC_SKINNED: homogeneous batch -> dead-strip the unused vertex-load path; mixed -> runtime branch (2).
        Key.SkinnedMode = (Batch.bAnySkinned && Batch.bAnyStatic) ? 2u : (Batch.bAnySkinned ? 1u : 0u);
        Key.TriCullMode = (uint8)(TriCull_Backface | TriCull_SmallPrim);
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));
        return Key.MS != nullptr;
    }

    void FDefaultSceneRenderer::DrawShadowBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, bool bUseMesh,
        uint32 CullViewIndex, int32 ShadowDataIndex, int32 ShadowViewIndex,
        const FUIntVector2& ViewportExtent)
    {
        if (!bUseMesh)
        {
            return;
        }

        // Shadows are single-phase: they cull against the cascade pyramid at INSTANCE level and never
        // set MeshletHiZ, so the phase here is Early in the sense of "the only one".
        FMeshletPassContext Ctx;
        Ctx.CullViewIndex   = CullViewIndex;
        Ctx.ShadowDataIndex = ShadowDataIndex;
        Ctx.ShadowViewIndex = ShadowViewIndex;
        Ctx.ViewportW       = (float)ViewportExtent.x;
        Ctx.ViewportH       = (float)ViewportExtent.y;

        DrawMeshletBatch(CL, Batch, Ctx);
    }

    void FDefaultSceneRenderer::PointShadowPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands             = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList           = Frame.Geometry.OpaqueDrawList;
        const auto& LightData                = Frame.Lighting.LightData;
        const auto& PackedShadows            = Frame.Lighting.PackedShadows;
        const auto& AtlasTiles               = Frame.Lighting.AtlasTiles;
        const auto& PointShadowCullViewBases = Frame.Views.PointShadowCullViewBases;

        if (DrawCommands.empty() || PackedShadows[(uint32)ELightType::Point].empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Point Light Shadow Pass", tracy::Color::DeepPink2);

        static const FShaderH PixelShader = FShaderLibrary::Get("ShadowMappingPixel.slang");
        if (!PixelShader)
        {
            return;
        }

        const TVector<FLightShadow>& PointShadows = PackedShadows[(uint32)ELightType::Point];

        RHI::FRenderPassDesc Pass;
        Pass.DepthAttachment.Texture  = ShadowAtlas.GetImage().Texture;
        Pass.DepthAttachment.LoadOp   = RHI::ELoadOp::Load;   // ResetPass cleared the whole atlas to 1.0
        Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        Pass.RenderArea               = FUIntVector2(GShadowAtlasResolution, GShadowAtlasResolution);

        RHI::CmdBeginRenderPass(CL, Pass);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode            = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest            = RHI::EOp::Less;
        DepthDesc.DepthBias            = 1.0f;
        DepthDesc.DepthBiasSlopeFactor = 1.5f;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::Back);

        for (uint32 OpaqueIdx : OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, PixelShader);

            for (uint32 LightIdx = 0; LightIdx < PointShadows.size(); ++LightIdx)
            {
                const FLightShadow& LightShadow = PointShadows[LightIdx];
                const uint32 ViewBase = PointShadowCullViewBases[LightIdx];
                if (ViewBase == ~0u)
                {
                    continue;
                }

                const FLightShadowData& ShadowData = LightData.Shadows[LightShadow.ShadowDataIndex];

                for (int32 Face = 0; Face < 6; ++Face)
                {
                    const FLightShadow& FaceShadow = ShadowData.Shadow[Face];
                    const FShadowTile& Tile = AtlasTiles[FaceShadow.ShadowMapIndex];
                    const int32 TilePixelX = (int32)(Tile.UVOffset.x * GShadowAtlasResolution);
                    const int32 TilePixelY = (int32)(Tile.UVOffset.y * GShadowAtlasResolution);
                    const int32 TileSize   = (int32)(Tile.UVScale.x * GShadowAtlasResolution);

                    const RHI::FRect TileRect{ TilePixelX, TilePixelX + TileSize, TilePixelY, TilePixelY + TileSize };
                    RHI::CmdSetViewport(CL, TileRect);
                    RHI::CmdSetScissor(CL, TileRect);

                    DrawShadowBatch(CL, Batch, bUseMesh, ViewBase + (uint32)Face,
                                    LightShadow.ShadowDataIndex, Face,
                                    FUIntVector2((uint32)TileSize, (uint32)TileSize));
                }
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::SpotShadowPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands             = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList           = Frame.Geometry.OpaqueDrawList;
        const auto& PackedShadows            = Frame.Lighting.PackedShadows;
        const auto& AtlasTiles               = Frame.Lighting.AtlasTiles;
        const auto& SpotShadowCullViewBases  = Frame.Views.SpotShadowCullViewBases;

        if (PackedShadows[(uint32)ELightType::Spot].empty() || DrawCommands.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Spot Shadow Pass", tracy::Color::DeepPink4);

        static const FShaderH PixelShader = FShaderLibrary::Get("ShadowMappingPixel.slang");
        if (!PixelShader)
        {
            return;
        }

        // Load to preserve the point-shadow tiles PointShadowPass already wrote into this same atlas.
        RHI::FRenderPassDesc Pass;
        Pass.DepthAttachment.Texture = ShadowAtlas.GetImage().Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = FUIntVector2(GShadowAtlasResolution, GShadowAtlasResolution);

        RHI::CmdBeginRenderPass(CL, Pass);

        // See PointShadowPass for why these bias values are lower than the CSM pass.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode            = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest            = RHI::EOp::Less;
        DepthDesc.DepthBias            = 1.0f;
        DepthDesc.DepthBiasSlopeFactor = 1.5f;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::Back);

        const TVector<FLightShadow>& SpotShadows = PackedShadows[(uint32)ELightType::Spot];

        // Batch-outer: one pipeline bind per batch; per-spot tile viewports are cheap dynamic state.
        for (uint32 OpaqueIdx : OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, PixelShader);

            for (uint32 SpotIdx = 0; SpotIdx < SpotShadows.size(); ++SpotIdx)
            {
                const FLightShadow& Shadow  = SpotShadows[SpotIdx];
                const uint32 ViewIndex      = SpotShadowCullViewBases[SpotIdx];
                if (ViewIndex == ~0u)
                {
                    continue;
                }

                const FShadowTile& Tile = AtlasTiles[Shadow.ShadowMapIndex];
                const int32 TilePixelX = (int32)(Tile.UVOffset.x * GShadowAtlasResolution);
                const int32 TilePixelY = (int32)(Tile.UVOffset.y * GShadowAtlasResolution);
                const int32 TileSize   = (int32)(Tile.UVScale.x * GShadowAtlasResolution);

                const RHI::FRect TileRect{ TilePixelX, TilePixelX + TileSize, TilePixelY, TilePixelY + TileSize };
                RHI::CmdSetViewport(CL, TileRect);
                RHI::CmdSetScissor(CL, TileRect);

                // Spotlights only use ViewProjection[0], so the shadow ViewIndex is 0.
                DrawShadowBatch(CL, Batch, bUseMesh, ViewIndex, Shadow.ShadowDataIndex, 0,
                                FUIntVector2((uint32)TileSize, (uint32)TileSize));
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::CascadedShowPass(RHI::FCmdListH CL, uint32 CascadeViewBase)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands     = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList   = Frame.Geometry.OpaqueDrawList;
        const auto& LightData        = Frame.Lighting.LightData;

        if (!LightData.bHasSun || DrawCommands.empty())
        {
            return;
        }
        if (LightData.Lights[0].ShadowDataIndex == INDEX_NONE)
        {
            return;
        }

        if (CascadeViewBase == ~0u)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cascaded Shadow Map Pass", tracy::Color::DeepPink2);

        RHI::FRenderPassDesc Pass;
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::Cascade).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = FUIntVector2(GCSMAtlasWidth, GCSMAtlasHeight);

        RHI::CmdBeginRenderPass(CL, Pass);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode            = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthDesc.DepthTest            = RHI::EOp::Less;
        DepthDesc.DepthBias            = 25.0f;
        DepthDesc.DepthBiasSlopeFactor = 0.75f;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::Back);

        const int32 SunShadowDataIndex = LightData.Lights[0].ShadowDataIndex;

        for (uint32 OpaqueIdx : OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, FShaderH{});

            for (uint32 c = 0; c < (uint32)NumCascades; ++c)
            {
                const int32 TileX = GCSMCascadeOriginX[c];
                const int32 TileY = GCSMCascadeOriginY[c];
                const int32 TileW = GCSMCascadeSizes[c];

                const RHI::FRect TileRect{ TileX, TileX + TileW, TileY, TileY + TileW };
                RHI::CmdSetViewport(CL, TileRect);
                RHI::CmdSetScissor(CL, TileRect);

                // Cascade is depth-only (no pixel shader); ViewIndex indexes the cascade's ViewProjection.
                DrawShadowBatch(CL, Batch, bUseMesh, CascadeViewBase + c, SunShadowDataIndex, (int32)c,
                                FUIntVector2((uint32)TileW, (uint32)TileW));
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::DecalPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const TVector<FGPUDecal>& Decals = Frame.Primitives.DecalExtracts;

        // No decals: the DBuffers do not exist this frame (EnsureOptionalViewImages allocates them on the
        // same test), and the base pass publishes the 0xFFFFFFFF sentinel instead of their indices, so
        // nothing samples them. This used to run a clear-only pass to make that sample a no-op -- three
        // full-res clears per frame whose result the shader was already skipping.
        if (Decals.empty())
        {
            return;
        }

        const FSceneImage& DBufferA = GetNamedImage(ENamedImage::DBufferA);
        const FSceneImage& DBufferB = GetNamedImage(ENamedImage::DBufferB);
        const FSceneImage& DBufferC = GetNamedImage(ENamedImage::DBufferC);

        // Cleared to transmittance = 1 (alpha) / zero color, so the base pass reads a no-op where no decal lands.
        RHI::FRenderAttachment Colors[3];
        const RHI::FTextureH Targets[3] = { DBufferA.Texture, DBufferB.Texture, DBufferC.Texture };
        for (int i = 0; i < 3; ++i)
        {
            Colors[i].Texture  = Targets[i];
            Colors[i].LoadOp   = RHI::ELoadOp::Clear;
            Colors[i].StoreOp  = RHI::EStoreOp::Store;
            Colors[i].Color[0] = Colors[i].Color[1] = Colors[i].Color[2] = 0.0f;
            Colors[i].Color[3] = 1.0f;
        }

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(Colors, 3);
        Pass.RenderArea       = DBufferA.GetExtent();

        LUMINA_PROFILE_SECTION_COLORED("Decal Pass", tracy::Color::Orange);

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, DBufferA.GetExtent());

        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::Front);

        // Transmittance compositing: RGB = SrcAlpha "over", A *= (1 - coverage) so alpha accumulates transmittance.
        RHI::FBlendDesc DecalBlend;
        DecalBlend.bBlendEnable   = true;
        DecalBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        DecalBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        DecalBlend.ColorOp        = RHI::EBlend::Add;
        DecalBlend.SrcAlphaFactor = RHI::EFactor::Zero;
        DecalBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;
        DecalBlend.AlphaOp        = RHI::EBlend::Add;

        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);

        struct FDecalPushConstants
        {
            uint64 DecalsAddr;
            uint32 DepthIndex;
            uint32 Pad;
        };
        static_assert(sizeof(FDecalPushConstants) == 16, "FDecalPushConstants must match the slang pass block.");

        FDecalPushConstants PC = {};
        PC.DecalsAddr = RHI::Core::CopyTransientArray(Decals.data(), Decals.size());
        PC.DepthIndex = (uint32)SceneDepth.GetResourceID();

        // One instanced draw per shader batch.
        for (const FFrameData::FDecalBatch& Batch : Frame.Primitives.DecalBatches)
        {
            FShaderH VS = Batch.Shaders.VertexShader;
            FShaderH PS = Batch.Shaders.PixelShader;
            if (VS == nullptr || PS == nullptr)
            {
                continue;
            }

            FGraphicsPipelineKey Key;
            Key.VS = VS;
            Key.PS = PS;
            Key.ColorTargets.push_back({ DBufferA.Desc.Format, DecalBlend });
            Key.ColorTargets.push_back({ DBufferB.Desc.Format, DecalBlend });
            Key.ColorTargets.push_back({ DBufferC.Desc.Format, DecalBlend });
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            RHI::CmdDraw(CL, MakeArgs(PC), 36, Batch.Count, 0, Batch.FirstInstance);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    static constexpr uint32 GMaterialMaxSlots = MATERIAL_MAX_SLOTS;

    // One allocation means one barrier between the three classify dispatches instead of three, and the
    // indirect args sit in the same range as the counts that produced them.
    struct FMaterialClassifyBlock
    {
        uint32                          Counts[GMaterialMaxSlots];
        uint32                          Starts[GMaterialMaxSlots];
        uint32                          Cursors[GMaterialMaxSlots];
        uint32                          Total;
        uint32                          _Pad0[3];
        RHI::FDispatchIndirectArguments MaterialArgs[GMaterialMaxSlots];
        RHI::FDispatchIndirectArguments LightArgs;
        uint32                          _Pad1;
    };

    bool FDefaultSceneRenderer::BuildDeferredMaterialBinning(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        MaterialClassifyLayout = FMaterialClassifyLayout{};

        const FUIntVector2 Extent = GetNamedImage(ENamedImage::HDR).GetExtent();
        if (Extent.x == 0u || Extent.y == 0u)
        {
            return false;
        }

        // The pixel list packs a screen position into 16 bits per axis. Past that two pixels alias onto one
        // entry and a material would shade the wrong texel, so refuse rather than corrupt.
        if (Extent.x > 0xFFFFu || Extent.y > 0xFFFFu)
        {
            static bool bWarnedExtent = false;
            if (!bWarnedExtent)
            {
                bWarnedExtent = true;
                LOG_WARN("Render extent {}x{} exceeds the 16-bit pixel-list packing; deferred shading is disabled for this view.",
                    Extent.x, Extent.y);
            }
            return false;
        }

        const auto& DeferredMaterials = Frame.Geometry.DeferredMaterials;

        BinnedDeferredSlotShaders.clear();
        uint32 MaxMaterialIndex = 0u;
        for (const auto& M : DeferredMaterials)
        {
            if (M.DeferredShader)
            {
                MaxMaterialIndex = Math::Max(MaxMaterialIndex, M.MaterialIndex);
            }
        }

        BinnedDeferredSlotByMaterial.assign((size_t)MaxMaterialIndex + 1u, 0xFFFFFFFFu);
        for (const auto& M : DeferredMaterials)
        {
            if (!M.DeferredShader)
            {
                continue;
            }

            // Dense slot per distinct DeferredShader (linear scan: distinct visible masters per frame are few).
            uint32 Slot = 0xFFFFFFFFu;
            for (uint32 s = 0; s < (uint32)BinnedDeferredSlotShaders.size(); ++s)
            {
                if (BinnedDeferredSlotShaders[s] == M.DeferredShader)
                {
                    Slot = s;
                    break;
                }
            }

            if (Slot == 0xFFFFFFFFu)
            {
                if ((uint32)BinnedDeferredSlotShaders.size() >= GMaterialMaxSlots)
                {
                    static bool bWarnedSlotCap = false;
                    if (!bWarnedSlotCap)
                    {
                        bWarnedSlotCap = true;
                        LOG_WARN("More than {} distinct deferred material shaders are visible; the excess will not shade.",
                            GMaterialMaxSlots);
                    }
                    continue;
                }

                Slot = (uint32)BinnedDeferredSlotShaders.size();
                BinnedDeferredSlotShaders.push_back(M.DeferredShader);
            }

            BinnedDeferredSlotByMaterial[M.MaterialIndex] = Slot;
        }

        const uint32 NumSlots = (uint32)BinnedDeferredSlotShaders.size();
        if (NumSlots == 0u)
        {
            return false;
        }

        const uint64 PixelListSize = (uint64)Extent.x * (uint64)Extent.y * sizeof(uint32);

        ResizeBufferIfNeeded(CL, MaterialClassifyRing[CurrentFrameSlot], sizeof(FMaterialClassifyBlock), 1.0f,
                             MaterialClassifyRingLowUsage[CurrentFrameSlot], /*bAllowShrink*/ false,
                             EBufferInit::Zeroed, "Material.ClassifyBlock");
        ResizeBufferIfNeeded(CL, MaterialPixelListRing[CurrentFrameSlot], PixelListSize, 1.2f,
                             MaterialPixelListRingLowUsage[CurrentFrameSlot], true, EBufferInit::Zeroed,
                             "Material.PixelList");

        if (!GetMaterialClassify() || !GetMaterialPixelList())
        {
            return false;
        }

        MaterialClassifyLayout.NumSlots      = NumSlots;
        MaterialClassifyLayout.ScreenW       = Extent.x;
        MaterialClassifyLayout.ScreenH       = Extent.y;
        // From the allocation, not from what was asked for: the scatter bounds its writes against this.
        MaterialClassifyLayout.PixelCapacity = (uint32)Math::Min<uint64>(
            GetMaterialPixelList().GetSize() / sizeof(uint32), 0xFFFFFFFFull);

        return true;
    }

    // Steps 2-4: count each material's pixels, prefix-sum into start offsets, scatter every classified
    // pixel into its material's run. Also emits the indirect args, so the CPU never learns the counts.
    void FDefaultSceneRenderer::VisBufferClassifyPass(RHI::FCmdListH CL)
    {
        MaterialClassifyLayout = FMaterialClassifyLayout{};

        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("VisBuffer Classify", tracy::Color::Orange3);

        static const FShaderH CountCS = FShaderLibrary::Get("VisBufferMaterialCount.slang");
        static const FShaderH PrefixCS = FShaderLibrary::Get("VisBufferMaterialPrefixSum.slang");
        static const FShaderH ScatterCS = FShaderLibrary::Get("VisBufferMaterialScatter.slang");
        if (!CountCS || !PrefixCS || !ScatterCS)
        {
            return;
        }

        if (!BuildDeferredMaterialBinning(CL))
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "VisBuffer Classify");

        const FMaterialClassifyLayout Layout = MaterialClassifyLayout;

        const FSceneImage& VisRT    = GetNamedImage(ENamedImage::VisBuffer);
        const FSceneBuffer Classify = GetMaterialClassify();
        const FSceneBuffer PixelList = GetMaterialPixelList();

        const RHI::GPUPtr Base        = Classify.GetAddress();
        const RHI::GPUPtr CountsAddr  = Base + offsetof(FMaterialClassifyBlock, Counts);
        const RHI::GPUPtr StartsAddr  = Base + offsetof(FMaterialClassifyBlock, Starts);
        const RHI::GPUPtr CursorsAddr = Base + offsetof(FMaterialClassifyBlock, Cursors);
        const RHI::GPUPtr TotalAddr   = Base + offsetof(FMaterialClassifyBlock, Total);
        const RHI::GPUPtr MatArgsAddr = Base + offsetof(FMaterialClassifyBlock, MaterialArgs);
        const RHI::GPUPtr LitArgsAddr = Base + offsetof(FMaterialClassifyBlock, LightArgs);

        // MaterialIndex -> dense slot; uploaded to the transient ring and read by device address.
        const RHI::GPUPtr SlotByMaterialAddr =
            RHI::Core::CopyTransientArray(BinnedDeferredSlotByMaterial.data(), BinnedDeferredSlotByMaterial.size());

        // Only the counters are cleared. Starts, cursors, the total and every argument triple are fully
        // rewritten by the prefix sum below, including the slots this frame does not use.
        RHI::CmdMemset(CL, CountsAddr, sizeof(uint32) * GMaterialMaxSlots, 0u);
        Barriers::TransferToCompute(CL);

        const uint32 GroupsX = RenderUtils::GetGroupCount(Layout.ScreenW, (uint32)MATERIAL_CLASSIFY_TILE);
        const uint32 GroupsY = RenderUtils::GetGroupCount(Layout.ScreenH, (uint32)MATERIAL_CLASSIFY_TILE);

        struct FMaterialCountPC
        {
            RHI::GPUPtr CountsAddr;
            RHI::GPUPtr SlotByMaterialAddr;
            uint32      VisBufferIndex;
            uint32      ScreenW;
            uint32      ScreenH;
            uint32      DrawListCount;
            uint32      SlotByMaterialCount;
            uint32      NumSlots;
        } CountPC = {};
        static_assert(sizeof(FMaterialCountPC) == 40, "FMaterialCountPC must match VisBufferMaterialCount.slang FMaterialCountArgs.");
        CountPC.CountsAddr          = CountsAddr;
        CountPC.SlotByMaterialAddr  = SlotByMaterialAddr;
        CountPC.VisBufferIndex      = (uint32)VisRT.GetResourceID();
        CountPC.ScreenW             = Layout.ScreenW;
        CountPC.ScreenH             = Layout.ScreenH;
        CountPC.DrawListCount       = DrawListCapacity;
        CountPC.SlotByMaterialCount = (uint32)BinnedDeferredSlotByMaterial.size();
        CountPC.NumSlots            = Layout.NumSlots;

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CountCS));
        RHI::CmdDispatch(CL, MakeArgs(CountPC), GroupsX, GroupsY, 1u);

        Barriers::ComputeToAll(CL);

        struct FPrefixSumPC
        {
            RHI::GPUPtr CountsAddr;
            RHI::GPUPtr StartsAddr;
            RHI::GPUPtr CursorsAddr;
            RHI::GPUPtr ArgsAddr;
            RHI::GPUPtr LightArgsAddr;
            RHI::GPUPtr TotalAddr;
            uint32      NumSlots;
            uint32      _Pad0;
        } PrefixPC = {};
        static_assert(sizeof(FPrefixSumPC) == 56, "FPrefixSumPC must match VisBufferMaterialPrefixSum.slang FPrefixSumArgs.");
        PrefixPC.CountsAddr    = CountsAddr;
        PrefixPC.StartsAddr    = StartsAddr;
        PrefixPC.CursorsAddr   = CursorsAddr;
        PrefixPC.ArgsAddr      = MatArgsAddr;
        PrefixPC.LightArgsAddr = LitArgsAddr;
        PrefixPC.TotalAddr     = TotalAddr;
        PrefixPC.NumSlots      = Layout.NumSlots;

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(PrefixCS));
        RHI::CmdDispatch(CL, MakeArgs(PrefixPC), 1u, 1u, 1u);

        Barriers::ComputeToAll(CL);

        struct FMaterialScatterPC
        {
            RHI::GPUPtr CursorsAddr;
            RHI::GPUPtr PixelListAddr;
            RHI::GPUPtr SlotByMaterialAddr;
            uint32      VisBufferIndex;
            uint32      ScreenW;
            uint32      ScreenH;
            uint32      DrawListCount;
            uint32      SlotByMaterialCount;
            uint32      NumSlots;
            uint32      PixelListCapacity;
            uint32      _Pad0;
        } ScatterPC = {};
        static_assert(sizeof(FMaterialScatterPC) == 56, "FMaterialScatterPC must match VisBufferMaterialScatter.slang FMaterialScatterArgs.");
        ScatterPC.CursorsAddr        = CursorsAddr;
        ScatterPC.PixelListAddr      = PixelList.GetAddress();
        ScatterPC.SlotByMaterialAddr = SlotByMaterialAddr;
        ScatterPC.VisBufferIndex     = CountPC.VisBufferIndex;
        ScatterPC.ScreenW            = Layout.ScreenW;
        ScatterPC.ScreenH            = Layout.ScreenH;
        ScatterPC.DrawListCount      = DrawListCapacity;
        ScatterPC.SlotByMaterialCount = CountPC.SlotByMaterialCount;
        ScatterPC.NumSlots           = Layout.NumSlots;
        ScatterPC.PixelListCapacity  = Layout.PixelCapacity;

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ScatterCS));
        RHI::CmdDispatch(CL, MakeArgs(ScatterPC), GroupsX, GroupsY, 1u);

        // The pixel list feeds the material dispatches; the argument triples feed the indirect fetch.
        Barriers::ComputeToAll(CL);
    }

    // Step 5: one indirect compute dispatch per material over exactly the pixels it owns. Compute, not
    // a rasterized quad: a pixel shader launches in 2x2 quads, running the graph 4x on a 1-pixel tri.
    void FDefaultSceneRenderer::MaterialGBufferPass(RHI::FCmdListH CL)
    {
        // Set by VisBufferClassifyPass, which runs immediately before this and zeroes it on any bail-out.
        const FMaterialClassifyLayout Layout = MaterialClassifyLayout;
        if (Layout.NumSlots == 0u)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Material GBuffer Pass", tracy::Color::Red);
        SCENE_GPU_SCOPE(CL, "Material GBuffer");

        const FFrameData& Frame  = *RenderFrame;
        const FSceneImage& VisRT = GetNamedImage(ENamedImage::VisBuffer);

        const FSceneBuffer Classify  = GetMaterialClassify();
        const FSceneBuffer PixelList = GetMaterialPixelList();
        const RHI::GPUPtr  Base      = Classify.GetAddress();

        struct FDeferredMaterialPC
        {
            uint32      VisBufferIndex;
            uint32      DBufferAIndex;
            uint32      DBufferBIndex;
            uint32      DBufferCIndex;
            uint32      DrawListCount;
            uint32      SlotIndex;
            uint32      ScreenW;
            uint32      ScreenH;
            uint32      GBufferAUAV;
            uint32      GBufferBUAV;
            uint32      GBufferCUAV;
            uint32      GBufferDUAV;
            RHI::GPUPtr PixelListAddr;
            RHI::GPUPtr StartsAddr;
            RHI::GPUPtr CountsAddr;
        } PC = {};
        static_assert(sizeof(FDeferredMaterialPC) == 72, "FDeferredMaterialPC must match DeferredMaterial.slang FDeferredMaterialArgs.");

        PC.VisBufferIndex = (uint32)VisRT.GetResourceID();
        if (Frame.Primitives.DecalExtracts.empty())
        {
            PC.DBufferAIndex = 0xFFFFFFFFu;
            PC.DBufferBIndex = 0xFFFFFFFFu;
            PC.DBufferCIndex = 0xFFFFFFFFu;
        }
        else
        {
            PC.DBufferAIndex = (uint32)GetNamedImage(ENamedImage::DBufferA).GetResourceID();
            PC.DBufferBIndex = (uint32)GetNamedImage(ENamedImage::DBufferB).GetResourceID();
            PC.DBufferCIndex = (uint32)GetNamedImage(ENamedImage::DBufferC).GetResourceID();
        }
        PC.DrawListCount = DrawListCapacity;
        PC.ScreenW       = Layout.ScreenW;
        PC.ScreenH       = Layout.ScreenH;

        const int32 UAVA = GetNamedImage(ENamedImage::GBufferA).GetMipUAVIndex(0);
        const int32 UAVB = GetNamedImage(ENamedImage::GBufferB).GetMipUAVIndex(0);
        const int32 UAVC = GetNamedImage(ENamedImage::GBufferC).GetMipUAVIndex(0);
        const int32 UAVD = GetNamedImage(ENamedImage::GBufferD).GetMipUAVIndex(0);
        if (UAVA < 0 || UAVB < 0 || UAVC < 0 || UAVD < 0)
        {
            LOG_ERROR("Deferred material pass: a GBuffer target has no storage heap slot; skipping the pass.");
            return;
        }
        PC.GBufferAUAV = (uint32)UAVA;
        PC.GBufferBUAV = (uint32)UAVB;
        PC.GBufferCUAV = (uint32)UAVC;
        PC.GBufferDUAV = (uint32)UAVD;

        PC.PixelListAddr = PixelList.GetAddress();
        PC.StartsAddr    = Base + offsetof(FMaterialClassifyBlock, Starts);
        PC.CountsAddr    = Base + offsetof(FMaterialClassifyBlock, Counts);

        for (uint32 Slot = 0; Slot < Layout.NumSlots; ++Slot)
        {
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BinnedDeferredSlotShaders[Slot]));

            PC.SlotIndex = Slot;
            RHI::CmdDispatchIndirect(CL, MakeArgs(PC), Classify.Ptr,
                (uint32)(offsetof(FMaterialClassifyBlock, MaterialArgs) + Slot * sizeof(RHI::FDispatchIndirectArguments)));
        }

        Barriers::ComputeToAll(CL);
    }

    // Step 6: light every classified pixel exactly once from the GBuffer. Background, terrain and any
    // material without a deferred shader were never classified, so they keep what the env pass wrote.
    void FDefaultSceneRenderer::DeferredLightingPass(RHI::FCmdListH CL)
    {
        const FMaterialClassifyLayout Layout = MaterialClassifyLayout;
        if (Layout.NumSlots == 0u)
        {
            return;
        }

        static const FShaderH LightingCS = FShaderLibrary::Get("DeferredLighting.slang");
        if (!LightingCS)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Deferred Lighting Pass", tracy::Color::Gold);
        SCENE_GPU_SCOPE(CL, "Deferred Lighting");

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);
        const int32 HDRUAV = HDR.GetMipUAVIndex(0);
        if (HDRUAV < 0)
        {
            LOG_ERROR("Deferred lighting: the HDR target has no storage heap slot; skipping the pass.");
            return;
        }

        const FSceneBuffer Classify = GetMaterialClassify();

        struct FDeferredLightingPC
        {
            uint32      GBufferAIndex;
            uint32      GBufferBIndex;
            uint32      GBufferCIndex;
            uint32      GBufferDIndex;
            uint32      DepthIndex;
            uint32      HDRUAV;
            uint32      ScreenW;
            uint32      ScreenH;
            RHI::GPUPtr PixelListAddr;
            RHI::GPUPtr TotalAddr;
        } PC = {};
        static_assert(sizeof(FDeferredLightingPC) == 48, "FDeferredLightingPC must match DeferredLighting.slang FDeferredLightingArgs.");
        PC.GBufferAIndex = (uint32)GetNamedImage(ENamedImage::GBufferA).GetResourceID();
        PC.GBufferBIndex = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        PC.GBufferCIndex = (uint32)GetNamedImage(ENamedImage::GBufferC).GetResourceID();
        PC.GBufferDIndex = (uint32)GetNamedImage(ENamedImage::GBufferD).GetResourceID();
        PC.DepthIndex    = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.HDRUAV        = (uint32)HDRUAV;
        PC.ScreenW       = Layout.ScreenW;
        PC.ScreenH       = Layout.ScreenH;
        PC.PixelListAddr = GetMaterialPixelList().GetAddress();
        PC.TotalAddr     = Classify.GetAddress() + offsetof(FMaterialClassifyBlock, Total);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(LightingCS));
        RHI::CmdDispatchIndirect(CL, MakeArgs(PC), Classify.Ptr,
            (uint32)offsetof(FMaterialClassifyBlock, LightArgs));

        // AllCommands, not ComputeToAll: HDR is a UAV write here and a color attachment for every pass
        // after it, and ComputeToAll's destination set has no RasterColorOut.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::AllCommands);
    }

#if USING(WITH_EDITOR)
    // Entity ids are a property of the geometry, not any material, so they never enter the GBuffer.
    // Runs before the passes that paint their own ids on top and load what this writes.
    void FDefaultSceneRenderer::PickerResolvePass(RHI::FCmdListH CL)
    {
        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            return;
        }

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("VisBufferPicker.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Picker Resolve", tracy::Color::SlateBlue);
        SCENE_GPU_SCOPE(CL, "Picker Resolve");

        const FSceneImage& PickerImg = GetPickerRT();
        const FSceneImage& VisRT     = GetNamedImage(ENamedImage::VisBuffer);
        const FUIntVector2 Extent    = GetNamedImage(ENamedImage::HDR).GetExtent();

        RHI::FRenderAttachment Color;
        Color.Texture        = PickerImg.Texture;
        Color.ResolveTexture = GetPickerResolve();
        Color.LoadOp         = RHI::ELoadOp::Undefined;   // every pixel is written, background included
        Color.StoreOp        = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ PickerImg.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FVisBufferPickerPC
        {
            uint32 VisBufferIndex;
            uint32 DrawListCount;
        } PC = {};
        static_assert(sizeof(FVisBufferPickerPC) == 8, "FVisBufferPickerPC must match VisBufferPicker.slang FVisBufferPickerArgs.");
        PC.VisBufferIndex = (uint32)VisRT.GetResourceID();
        PC.DrawListCount  = DrawListCapacity;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
#endif

#if !defined(LE_SHIPPING)
    // Editor view modes as an overlay, kept out of the lighting shader so the production path carries
    // no debug branch. A PIXEL shader because the penumbra visualizer needs ddx/ddy.
    void FDefaultSceneRenderer::SceneDebugViewPass(RHI::FCmdListH CL)
    {
        if (MaterialClassifyLayout.NumSlots == 0u ||
            RenderFrame->SceneGlobalData.CullData.DebugMode == 0u)
        {
            return;
        }

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SceneDebugViewPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Scene Debug View", tracy::Color::Magenta);
        SCENE_GPU_SCOPE(CL, "Scene Debug View");

        const FSceneImage& ColorImg = GetSceneColorRT();
        const FUIntVector2 Extent   = FUIntVector2(MaterialClassifyLayout.ScreenW, MaterialClassifyLayout.ScreenH);

        RHI::FRenderAttachment Color;
        Color.Texture        = ColorImg.Texture;
        Color.ResolveTexture = GetSceneColorResolve();
        Color.LoadOp         = RHI::ELoadOp::Load;   // pixels the deferred lane does not own are discarded
        Color.StoreOp        = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.SampleCount = MSAASampleCount;
        Key.ColorTargets.push_back({ ColorImg.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FSceneDebugViewPC
        {
            uint32 GBufferAIndex;
            uint32 GBufferBIndex;
            uint32 GBufferCIndex;
            uint32 GBufferDIndex;
            uint32 VisBufferIndex;
            uint32 DepthIndex;
            uint32 DrawListCount;
            uint32 _Pad0;
        } PC = {};
        static_assert(sizeof(FSceneDebugViewPC) == 32, "FSceneDebugViewPC must match SceneDebugViewPixel.slang FSceneDebugViewArgs.");
        PC.GBufferAIndex  = (uint32)GetNamedImage(ENamedImage::GBufferA).GetResourceID();
        PC.GBufferBIndex  = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        PC.GBufferCIndex  = (uint32)GetNamedImage(ENamedImage::GBufferC).GetResourceID();
        PC.GBufferDIndex  = (uint32)GetNamedImage(ENamedImage::GBufferD).GetResourceID();
        PC.VisBufferIndex = (uint32)GetNamedImage(ENamedImage::VisBuffer).GetResourceID();
        PC.DepthIndex     = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.DrawListCount  = DrawListCapacity;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
#endif

#if USING(WITH_EDITOR)
    void FDefaultSceneRenderer::SelectionOutlinePass(RHI::FCmdListH CL)
    {
        const TVector<uint32>& SelectionBits = RenderFrame->Extracts.SelectionBits;
        if (SelectionBits.empty() || !CurrentView->Output.IsValid())
        {
            return;
        }
        
        const FSceneImage& PickerImg = GetNamedImage(ENamedImage::Picker);
        const int32 PickerSlot = PickerImg.IsValid() ? PickerImg.GetResourceID() : -1;
        if (PickerSlot < 0)
        {
            return;
        }

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader  = FShaderLibrary::Get("SelectionOutline.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Selection Outline", tracy::Color::Orange);
        SCENE_GPU_SCOPE(CL, "Selection Outline");

        const FSceneImage& Output = CurrentView->Output;

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, AlphaBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        // Mirrors FSelectionOutlineArgs in SelectionOutline.slang -- field for field, IN ORDER. OutlineColor
        // leads because a vector may not straddle a 16-byte boundary in a PhysicalStorageBuffer Block; see
        // the comment on the shader-side struct for what the trailing version broke.
        struct FSelectionOutlinePC
        {
            FVector4    OutlineColor;
            RHI::GPUPtr SelectionBits;
            uint32      PickerIndex;
            uint32      SelectionBitWords;
            uint32      EntityIndexMask;
            float       Thickness;
        } PC = {};

        PC.SelectionBits     = RHI::Core::CopyTransientArray(SelectionBits.data(), SelectionBits.size());
        PC.PickerIndex       = (uint32)PickerSlot;
        PC.SelectionBitWords = (uint32)SelectionBits.size();
        // From entt rather than a literal, so a change to entity_traits cannot silently start masking
        // off real index bits and outlining the wrong entity.
        PC.EntityIndexMask   = (uint32)entt::entt_traits<entt::entity>::entity_mask;
        PC.Thickness         = 2.0f;
        PC.OutlineColor      = FVector4(1.0f, 0.42f, 0.05f, 1.0f);

        if (PC.SelectionBits != 0)
        {
            RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
#endif

    void FDefaultSceneRenderer::ParticleSimulatePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Particle Simulate", tracy::Color::Orange);

        const FFrameData& Frame = *RenderFrame;
        const float DeltaTime   = Frame.CachedWorldDeltaTime;
        
        if (!ParticleGPUStates.empty())
        {
            const TVector<entt::entity>& Live = Frame.Extracts.LiveParticleEntities;
            auto IsLive = [&](entt::entity E)
            {
                return std::find(Live.begin(), Live.end(), E) != Live.end();
            };

            for (auto It = ParticleGPUStates.begin(); It != ParticleGPUStates.end();)
            {
                if (!IsLive(It->first))
                {
                    for (FParticleGPUState& Dead : It->second)
                    {
                        if (Dead.ParticleBuffer)     { DeferFree(Dead.ParticleBuffer); }
                        if (Dead.SpawnCounterBuffer) { DeferFree(Dead.SpawnCounterBuffer); }
                        if (Dead.AttributeBuffer)    { DeferFree(Dead.AttributeBuffer); }
                    }
                    It = ParticleGPUStates.erase(It);
                }
                else
                {
                    ++It;
                }
            }
        }

        bool bAnySimulated = false;

        for (const FFrameData::FParticleExtract& Item : Frame.Extracts.ParticleExtracts)
        {
            if (!Item.bReady)
            {
                continue;
            }

            const FResolvedParticleParams& Resolved = Item.Resolved;

            const uint32 MaxParticles = (uint32)Resolved.MaxParticles;
            if (MaxParticles == 0)
            {
                continue;
            }
            
            TVector<FParticleGPUState>& EntityStates = ParticleGPUStates[Item.Entity];
            if ((int32)EntityStates.size() != Item.EmitterCount)
            {
                for (int32 Stale = Item.EmitterCount; Stale < (int32)EntityStates.size(); ++Stale)
                {
                    FParticleGPUState& Dropped = EntityStates[(size_t)Stale];
                    if (Dropped.ParticleBuffer)     { DeferFree(Dropped.ParticleBuffer); }
                    if (Dropped.SpawnCounterBuffer) { DeferFree(Dropped.SpawnCounterBuffer); }
                    if (Dropped.AttributeBuffer)    { DeferFree(Dropped.AttributeBuffer); }
                }
                EntityStates.resize((size_t)Math::Max(Item.EmitterCount, 1));
            }
            FParticleGPUState& State = EntityStates[(size_t)Item.EmitterIndex];

            const bool bNeedsAlloc = (State.ParticleBuffer == 0)
                                  || (State.AllocatedMax != MaxParticles)
                                  || (State.AllocatedAttributeFloats != Item.AttributeFloatCount);
            if (bNeedsAlloc)
            {
                if (State.ParticleBuffer)     { DeferFree(State.ParticleBuffer); }
                if (State.SpawnCounterBuffer) { DeferFree(State.SpawnCounterBuffer); }
                if (State.AttributeBuffer)    { DeferFree(State.AttributeBuffer); }

                State.ParticleBufferSize = (uint64)MaxParticles * 64ull;
                State.ParticleBuffer     = RHI::Malloc(State.ParticleBufferSize, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
                State.SpawnCounterBuffer = RHI::Malloc(sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
                RHI::SetDebugName(State.ParticleBuffer,     "Particles.Particles");
                RHI::SetDebugName(State.SpawnCounterBuffer, "Particles.SpawnCounter");

                State.AttributeBufferSize = (uint64)MaxParticles * (uint64)Item.AttributeFloatCount * sizeof(float);
                State.AttributeBuffer     = RHI::Malloc(State.AttributeBufferSize, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
                RHI::SetDebugName(State.AttributeBuffer,    "Particles.Attributes");

                // Zero-fill the particle buffer so all entries start dead.
                RHI::CmdMemset(CL, State.ParticleBuffer, State.ParticleBufferSize, 0u);
                RHI::CmdMemset(CL, State.AttributeBuffer, State.AttributeBufferSize, 0u);

                State.AllocatedMax             = MaxParticles;
                State.AllocatedAttributeFloats = Item.AttributeFloatCount;
                State.SpawnAccumulator  = 0.0f;
                State.SystemAge         = 0.0f;
                State.bBurstPending     = true;
            }

            // Apply the extract-phase Activate()/Deactivate() intents to the render-owned sim state.
            if (Item.bForceReset)
            {
                RHI::CmdMemset(CL, State.ParticleBuffer, State.ParticleBufferSize, 0u);
                State.AliveTimeRemaining = 0.0f;
                State.SpawnAccumulator   = 0.0f;
                State.SystemAge          = 0.0f;
            }
            if (Item.bForceBurst)
            {
                State.bBurstPending = true;
            }

            const float ScaledDelta = DeltaTime * Item.TimeScale;
            State.TotalTime += DeltaTime;
            State.SystemAge += ScaledDelta;

            const bool bDurationExpired = (Resolved.Duration > 0.0f) && (State.SystemAge >= Resolved.Duration);
            if (bDurationExpired)
            {
                if (Resolved.bLooping)
                {
                    State.SystemAge = fmodf(State.SystemAge, Resolved.Duration);
                    State.bBurstPending = true;
                }
            }

            const bool bEmitActive = Item.bEmit && !(bDurationExpired && !Resolved.bLooping);

            uint32 SpawnCount = 0;
            if (bEmitActive && Resolved.SpawnRate > 0.0f && Item.SpawnRateMultiplier > 0.0f)
            {
                State.SpawnAccumulator += DeltaTime * Resolved.SpawnRate * Item.SpawnRateMultiplier;
                SpawnCount = (uint32)State.SpawnAccumulator;
                State.SpawnAccumulator -= (float)SpawnCount;
            }
            else
            {
                State.SpawnAccumulator = 0.0f;
            }

            const bool bDoBurst = bEmitActive && Item.bBurstOnSpawn && State.bBurstPending && Resolved.BurstCount > 0;
            if (bDoBurst)
            {
                SpawnCount += (uint32)Resolved.BurstCount;
                State.bBurstPending = false;
            }
            else if (!Item.bBurstOnSpawn)
            {
                State.bBurstPending = false;
            }

            SpawnCount = eastl::min(SpawnCount, MaxParticles);

            const float MaxLifetime = eastl::max(Resolved.LifetimeRange.y, 0.0f);
            if (SpawnCount > 0)
            {
                State.AliveTimeRemaining = eastl::max(State.AliveTimeRemaining, MaxLifetime);
            }
            State.AliveTimeRemaining = eastl::max(State.AliveTimeRemaining - ScaledDelta, 0.0f);

            if (SpawnCount == 0 && State.AliveTimeRemaining <= 0.0f)
            {
                continue;
            }

            RHI::CmdMemset(CL, State.SpawnCounterBuffer, sizeof(uint32), 0u);

            const FMatrix4 WorldMat = Item.WorldMatrix;
            const FVector3 EmitterWorld = FVector3(WorldMat * FVector4(Item.EmitterOffset, 1.0f));
            const FVector3 EmitterRight   = Math::Normalize(FVector3(WorldMat[0]));
            const FVector3 EmitterUp      = Math::Normalize(FVector3(WorldMat[1]));
            const FVector3 EmitterForward = Math::Normalize(FVector3(WorldMat[2]));
            
            FVector3 EmitterVelocity(0.0f);
            if (State.bHasPrevPosition && DeltaTime > 0.0f)
            {
                EmitterVelocity = (EmitterWorld - State.PrevEmitterPosition) / DeltaTime;
            }
            State.PrevEmitterPosition = EmitterWorld;
            State.bHasPrevPosition    = true;

            const float InheritFactor = Math::Clamp(Resolved.InheritEmitterVelocity, 0.0f, 1.0f);

            State.FrameSeed = (State.FrameSeed + 2654435761u) ^ (uint32)Item.Entity;

            uint32 SimFlags = 0u;
            if (Resolved.bLooping)
            {
                SimFlags |= PARTICLE_SIM_FLAG_LOOP;
            }
            if (State.bBurstPending)
            {
                SimFlags |= PARTICLE_SIM_FLAG_BURST_PENDING;
            }

            FParticleSimParamsGPU SimParams{};
            SimParams.EmitterPosition   = FVector4(EmitterWorld, 1.0f);
            SimParams.EmitterForward    = FVector4(EmitterForward, EmitterVelocity.x);
            SimParams.EmitterRight      = FVector4(EmitterRight,   EmitterVelocity.y);
            SimParams.EmitterUp         = FVector4(EmitterUp,      EmitterVelocity.z);
            SimParams.Counts            = FUIntVector4(MaxParticles, SpawnCount, State.FrameSeed, SimFlags);
            SimParams.Modes             = FUIntVector4((uint32)Resolved.Shape, (uint32)Resolved.VelocityMode, 0u, 0u);
            SimParams.ShapeSize         = FVector4(Resolved.ShapeSize, Math::Radians(Resolved.ShapeAngle));
            SimParams.VelocityMin       = FVector4(Resolved.VelocityMin, 0.0f);
            SimParams.VelocityMax       = FVector4(Resolved.VelocityMax, 0.0f);
            SimParams.SpeedAndLifetime  = FVector4(Resolved.SpeedRange.x, Resolved.SpeedRange.y, Resolved.LifetimeRange.x, Resolved.LifetimeRange.y);
            SimParams.Gravity           = FVector4(Resolved.Gravity, Resolved.Drag);
            SimParams.StartColor        = Resolved.StartColor;
            SimParams.EndColor          = Resolved.EndColor;
            SimParams.SizeRange         = FVector4(Resolved.StartSizeRange.x, Resolved.StartSizeRange.y, Resolved.EndSizeRange.x, Resolved.EndSizeRange.y);
            SimParams.RotationRange     = FVector4(Resolved.RotationRange.x, Resolved.RotationRange.y, Resolved.RotationSpeedRange.x, Resolved.RotationSpeedRange.y);
            SimParams.NoiseStrength     = FVector4(Resolved.NoiseStrength, Resolved.NoiseScale);
            SimParams.NoiseParams       = FVector4(Resolved.NoiseSpeed, InheritFactor, 0.0f, 0.0f);
            SimParams.Timing            = FVector4(ScaledDelta, State.TotalTime, State.SystemAge, 0.0f);

            static const FShaderH DefaultSimShader = FShaderLibrary::Get("ParticleSimulate.slang");
            FShaderH ComputeShader = Item.bUsesCustomShader ? Item.CustomComputeShader : DefaultSimShader;

            const FShaderEntry* SimEntry = FShaderLibrary::Resolve(ComputeShader);
            if (SimEntry == nullptr || !SimEntry->IsValid())
            {
                continue;
            }

            // Buffer fills (zero/reset/counter) must land before the sim reads them.
            Barriers::TransferToAll(CL);

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

            // Mirrors FParticleSimArgs in ParticleSimulate(.Template).slang: everything by device address.
            struct FParticleSimArgs
            {
                uint64 ParamsAddr;
                uint64 ParticlesAddr;
                uint64 SpawnCounterAddr;
                uint64 ModuleParamsAddr;
                uint64 AttributesAddr;
            };

            FParticleSimArgs SimArgs;
            SimArgs.ParamsAddr       = RHI::Core::CopyTransient(SimParams);
            SimArgs.ParticlesAddr    = State.ParticleBuffer;
            SimArgs.SpawnCounterAddr = State.SpawnCounterBuffer;
            SimArgs.ModuleParamsAddr = Item.ModuleParamValues.empty()
                ? 0ull
                : RHI::Core::CopyTransientArray(Item.ModuleParamValues.data(), Item.ModuleParamValues.size());
            SimArgs.AttributesAddr   = State.AttributeBuffer;

            RHI::CmdDispatch(CL, MakeArgs(SimArgs), RenderUtils::GetGroupCount(MaxParticles, 64u), 1u, 1u);
            bAnySimulated = true;
        }

        if (bAnySimulated)
        {
            // Simulated particles feed the render pass VS.
            Barriers::ComputeToAll(CL);
        }
    }

    static RHI::FBlendDesc MakeParticleBlend(EParticleBlendMode Mode)
    {
        RHI::FBlendDesc Blend;
        Blend.bBlendEnable = true;
        Blend.ColorOp      = RHI::EBlend::Add;
        Blend.AlphaOp      = RHI::EBlend::Add;

        switch (Mode)
        {
        case EParticleBlendMode::Additive:
            Blend.SrcColorFactor = RHI::EFactor::SrcAlpha;
            Blend.DstColorFactor = RHI::EFactor::One;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::One;
            break;

        case EParticleBlendMode::PreMultiplied:
            Blend.SrcColorFactor = RHI::EFactor::One;
            Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;
            break;

        case EParticleBlendMode::Multiply:
            Blend.SrcColorFactor = RHI::EFactor::DstColor;
            Blend.DstColorFactor = RHI::EFactor::Zero;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::Zero;
            break;

        case EParticleBlendMode::Alpha:
        default:
            Blend.SrcColorFactor = RHI::EFactor::SrcAlpha;
            Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;
            break;
        }
        return Blend;
    }

    void FDefaultSceneRenderer::ParticleRenderPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Particle Render", tracy::Color::OrangeRed);

        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (Frame.Extracts.ParticleExtracts.empty())
        {
            return;
        }

        static const FShaderH VertexShader = FShaderLibrary::Get("ParticleVertex.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("ParticlePixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR   = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Depth = GetNamedImage(ENamedImage::DepthAttachment);

        const bool bHDRWasWritten = !DrawCommands.empty() || RenderSettings.bHasEnvironment
            || !Frame.Extracts.TerrainExtracts.empty() || !Frame.Primitives.SolidBatches.empty()
            || !Frame.Primitives.LineBatches.empty();

        RHI::FRenderAttachment Color;
        Color.Texture  = HDR.Texture;
        Color.LoadOp   = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Color.StoreOp  = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments         = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture  = Depth.Texture;
        Pass.DepthAttachment.LoadOp   = DrawCommands.empty() ? RHI::ELoadOp::Clear : RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        Pass.DepthAttachment.Color[0] = 0.0f;
        Pass.RenderArea               = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        struct FParticlePushConstants
        {
            uint64   ParticlesAddr;
            uint32   TextureIndex;
            uint32   FacingMode;
            FVector4 Tint;
            float    VelocityStretch;
            uint32   SubUVColumns;
            uint32   SubUVRows;
            uint32   AttrFloats;        // floats per particle in the attribute buffer
            uint64   AttributesAddr;    // declared-attribute buffer, 0 when the emitter has none
            int32    AttrSlotSizeScaleX; // -1 when the stack did not declare it
            int32    AttrSlotSizeScaleY;
            int32    AttrSlotPrevPosX;
            int32    AttrSlotPrevPosY;
            int32    AttrSlotPrevPosZ;
            uint32   Pad1;
        };
        static_assert(sizeof(FParticlePushConstants) == 80, "FParticlePushConstants must match the slang pass block.");

        for (const FFrameData::FParticleExtract& Item : Frame.Extracts.ParticleExtracts)
        {
            if (!Item.bReady)
            {
                continue;
            }

            // GPUState is render-owned; the snapshot supplies everything else.
            auto ParticleStateIt = ParticleGPUStates.find(Item.Entity);
            if (ParticleStateIt == ParticleGPUStates.end()
                || Item.EmitterIndex >= (int32)ParticleStateIt->second.size())
            {
                continue;
            }
            FParticleGPUState& State = ParticleStateIt->second[(size_t)Item.EmitterIndex];
            if (!State.ParticleBuffer)
            {
                continue;
            }

            const FResolvedParticleParams& Resolved = Item.Resolved;

            RHI::FDepthStencilDesc DepthDesc;
            DepthDesc.DepthMode = Resolved.bWriteDepth
                ? (RHI::EDepthFlags::Read | RHI::EDepthFlags::Write)
                : RHI::EDepthFlags::Read;
            DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
            RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));

            FGraphicsPipelineKey Key;
            Key.VS          = VertexShader;
            Key.PS          = PixelShader;
            Key.DepthFormat = EFormat::D32;
            Key.ColorTargets.push_back({ HDR.Desc.Format, MakeParticleBlend(Resolved.BlendMode) });
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FParticlePushConstants PC = {};
            PC.ParticlesAddr     = State.ParticleBuffer;
            PC.TextureIndex      = Item.TextureIndex;
            PC.FacingMode        = (uint32)Resolved.FacingMode;
            PC.Tint              = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
            PC.VelocityStretch   = Resolved.VelocityStretch;
            PC.SubUVColumns      = (uint32)Math::Max(Resolved.SubUVColumns, 1);
            PC.SubUVRows         = (uint32)Math::Max(Resolved.SubUVRows, 1);
            PC.AttrFloats        = Math::Max(Item.AttributeFloatCount, 1u);
            PC.AttributesAddr    = State.AttributeBuffer;
            PC.AttrSlotSizeScaleX = Item.RenderAttrSlots[ParticleRenderAttribute::SizeScaleX];
            PC.AttrSlotSizeScaleY = Item.RenderAttrSlots[ParticleRenderAttribute::SizeScaleY];
            PC.AttrSlotPrevPosX   = Item.RenderAttrSlots[ParticleRenderAttribute::PrevPosX];
            PC.AttrSlotPrevPosY   = Item.RenderAttrSlots[ParticleRenderAttribute::PrevPosY];
            PC.AttrSlotPrevPosZ   = Item.RenderAttrSlots[ParticleRenderAttribute::PrevPosZ];
            PC.Pad1               = 0u;

            RHI::CmdDraw(CL, MakeArgs(PC), 6u * State.AllocatedMax, 1u, 0u, 0u);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
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
                const int32 Y1 = std::min(Y0 + 1, OldRes - 1);
                const float Ty = Fy - float(Y0);
                for (int32 X = 0; X < NewRes; ++X)
                {
                    const float Fx = (NewRes > 1) ? float(X) / float(NewRes - 1) * float(OldRes - 1) : 0.0f;
                    const int32 X0 = int(Fx);
                    const int32 X1 = std::min(X0 + 1, OldRes - 1);
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

            CMaterialInterface* TerrainMaterial = Terrain.Material.Get();
            if (!TerrainMaterial || TerrainMaterial->GetMaterialType() == EMaterialType::Terrain)
            {
                if (!TerrainMaterial || !TerrainMaterial->IsReadyForRender())
                {
                    TerrainMaterial = CMaterial::GetDefaultTerrainMaterial();
                }
                if (TerrainMaterial && TerrainMaterial->IsReadyForRender())
                {
                    Out.Shaders.VertexShader = TerrainMaterial->GetVertexShader();
                    Out.Shaders.PixelShader  = TerrainMaterial->GetPixelShader();
                    Out.MaterialIndex        = (uint32)std::max(TerrainMaterial->GetMaterialIndex(), 0);
                }
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

            // Weight upload: whole slices, matching the GPU upload granularity.
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

        static FSceneImage CreateTerrainImage(uint32 Size, uint16 ArraySize, EFormat Format, bool bUav, bool bArrayView = false,
            const char* DebugName = "Terrain.Image")
        {
            RHI::FTextureDesc Desc;
            // Shader samples weight maps as Sampler2DArray, so array textures get an array view.
            Desc.Type       = (bArrayView || ArraySize > 1) ? RHI::ETextureType::Tex2DArray : RHI::ETextureType::Tex2D;
            Desc.Dimension  = FUIntVector3(Size, Size, 1);
            Desc.LayerCount = ArraySize;
            Desc.Format     = Format;
            Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;
            if (bUav)
            {
                Desc.Usage = Desc.Usage | RHI::EImageUsageFlags::Storage;
            }
            FSceneImage Image = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ bUav);
            RHI::SetDebugName(Image.Texture, DebugName);
            return Image;
        }
    }

    void FDefaultSceneRenderer::TerrainUpdatePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Terrain Update", tracy::Color::SeaGreen);

        static const FShaderH NormalShader = FShaderLibrary::Get("TerrainNormalCompute.slang");

        const FFrameData& Frame = *RenderFrame;

        if (!TerrainGPUStates.empty())
        {
            const TVector<entt::entity>& Live = Frame.Extracts.LiveTerrainEntities;
            auto IsLive = [&](entt::entity E)
            {
                return std::find(Live.begin(), Live.end(), E) != Live.end();
            };

            for (auto It = TerrainGPUStates.begin(); It != TerrainGPUStates.end();)
            {
                if (!IsLive(It->first))
                {
                    FTerrainGPUState& Dead = It->second;
                    DeferRelease(Dead.HeightmapTexture);
                    DeferRelease(Dead.NormalTexture);
                    DeferRelease(Dead.LayerWeightTexture);
                    if (Dead.ChunkInfoBuffer)      { DeferFree(Dead.ChunkInfoBuffer.Ptr); }
                    if (Dead.MeshletInfoBuffer)    { DeferFree(Dead.MeshletInfoBuffer.Ptr); }
                    if (Dead.VisibleMeshletBuffer) { DeferFree(Dead.VisibleMeshletBuffer.Ptr); }
                    if (Dead.IndirectDrawBuffer)   { DeferFree(Dead.IndirectDrawBuffer.Ptr); }
                    It = TerrainGPUStates.erase(It);
                }
                else
                {
                    ++It;
                }
            }
        }

        bool bAnyUpload = false;

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            if (TerrainItem.Resolution < 2 || TerrainItem.ChunkResolution < 2)
            {
                continue;
            }

            FTerrainGPUState& State = TerrainGPUStates[TerrainItem.Entity];
            const uint32 Res        = (uint32)TerrainItem.Resolution;
            const uint32 LayerCount = (uint32)std::max(TerrainItem.LayerCount, 1);
            const size_t SlicePixels = size_t(Res) * size_t(Res);

            const bool bRealloc = State.AllocatedResolution != Res || State.AllocatedLayerCount != LayerCount;
            if (bRealloc)
            {
                DeferRelease(State.HeightmapTexture);
                DeferRelease(State.NormalTexture);
                DeferRelease(State.LayerWeightTexture);

                State.HeightmapTexture   = CreateTerrainImage(Res, 1u,          EFormat::R32_FLOAT, false, false, "Terrain.Heightmap");
                State.NormalTexture      = CreateTerrainImage(Res, 1u,          EFormat::RGBA8_UNORM, true, false, "Terrain.Normal");
                State.LayerWeightTexture = CreateTerrainImage(Res, (uint16)std::max(LayerCount, 1u), EFormat::R8_UNORM, false, true, "Terrain.LayerWeight");
                State.AllocatedResolution = Res;
                State.AllocatedLayerCount = LayerCount;

                if (TerrainItem.WeightUpload == 0)
                {
                    const float Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                    RHI::CmdClearTexture(CL, State.LayerWeightTexture.Texture, Zero);
                    bAnyUpload = true;
                }
            }

            // Height upload (from the snapshot)
            const int32 ResI = (int32)Res;
            const bool  bHeightDirty = TerrainItem.HeightUpload != 0;
            FIntVector2 RectMin = FIntVector2(0);
            FIntVector2 RectMax = FIntVector2(ResI - 1);

            if (TerrainItem.HeightUpload == 1 && TerrainItem.HeightBytes.size() == SlicePixels)
            {
                const RHI::GPUPtr Src = RHI::Core::CopyTransientArray(TerrainItem.HeightBytes.data(), TerrainItem.HeightBytes.size());
                RHI::CmdCopyMemoryToTexture(CL, Src, Res, State.HeightmapTexture.Texture);
                bAnyUpload = true;
            }
            else if (TerrainItem.HeightUpload == 2)
            {
                RectMin = TerrainItem.HeightRectMin;
                RectMax = TerrainItem.HeightRectMax;
                const uint32 RegionW = uint32(RectMax.x - RectMin.x + 1);
                const uint32 RegionH = uint32(RectMax.y - RectMin.y + 1);
                // Snapshot rect is tightly packed, so the source row pitch is RegionW, not Res.
                const RHI::GPUPtr Src = RHI::Core::CopyTransientArray(TerrainItem.HeightBytes.data(), TerrainItem.HeightBytes.size());

                RHI::FTextureSlice Slice;
                Slice.Offset = FUIntVector3((uint32)RectMin.x, (uint32)RectMin.y, 0);
                Slice.Extent = FUIntVector3(RegionW, RegionH, 1);
                RHI::CmdCopyMemoryToTexture(CL, Src, RegionW, State.HeightmapTexture.Texture, Slice);
                bAnyUpload = true;
            }

            // Weight upload: whole slices, packed ascending in the snapshot.
            if (TerrainItem.WeightUpload != 0 && !TerrainItem.WeightBytes.empty())
            {
                const uint8* Cursor = TerrainItem.WeightBytes.data();
                const uint8* End    = Cursor + TerrainItem.WeightBytes.size();
                for (uint32 L = 0; L < LayerCount; ++L)
                {
                    if ((TerrainItem.WeightSliceMask & (1u << L)) == 0u)
                    {
                        continue;
                    }
                    if (Cursor + SlicePixels > End)
                    {
                        break;
                    }
                    const RHI::GPUPtr Src = RHI::Core::CopyTransientArray(Cursor, SlicePixels);

                    RHI::FTextureSlice Slice;
                    Slice.Layer = L;
                    RHI::CmdCopyMemoryToTexture(CL, Src, Res, State.LayerWeightTexture.Texture, Slice);
                    Cursor += SlicePixels;
                    bAnyUpload = true;
                }
            }

            if (bHeightDirty && NormalShader)
            {
                // Heightmap upload must land before the normal recompute samples it.
                Barriers::TransferToAll(CL);

                const int32 NMinX = std::max(RectMin.x - 1, 0);
                const int32 NMinY = std::max(RectMin.y - 1, 0);
                const int32 NMaxX = std::min(RectMax.x + 1, ResI - 1);
                const int32 NMaxY = std::min(RectMax.y + 1, ResI - 1);
                const int32 NW    = NMaxX - NMinX + 1;
                const int32 NH    = NMaxY - NMinY + 1;

                struct FTerrainNormalArgs
                {
                    FTerrainNormalParams Params;
                    uint32 HeightmapIndex;
                    uint32 NormalUAV;
                    uint32 _Pad0;
                    uint32 _Pad1;
                };

                FTerrainNormalArgs NormalArgs{};
                NormalArgs.Params.Resolution    = ResI;
                NormalArgs.Params.RegionMinX    = NMinX;
                NormalArgs.Params.RegionMinY    = NMinY;
                NormalArgs.Params.RegionSizeX   = NW;
                NormalArgs.Params.RegionSizeY   = NH;
                NormalArgs.Params.TileWorldSize = TerrainItem.TileWorldSize;
                NormalArgs.Params.MaxHeight     = TerrainItem.MaxHeight;
                NormalArgs.HeightmapIndex       = (uint32)State.HeightmapTexture.GetResourceID();
                NormalArgs.NormalUAV            = (uint32)State.NormalTexture.GetMipUAVIndex(0);

                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(NormalShader));
                RHI::CmdDispatch(CL, MakeArgs(NormalArgs), RenderUtils::GetGroupCount((uint32)NW, 8u),
                                                   RenderUtils::GetGroupCount((uint32)NH, 8u), 1u);

                // Normals are sampled by the terrain VS/PS.
                Barriers::ComputeToAll(CL);
            }

            if (TerrainItem.bGeometryRebuilt)
            {
                const uint32 ChunkCount   = (uint32)TerrainItem.Chunks.size();
                const uint32 MeshletCount = (uint32)TerrainItem.Meshlets.size();

                if (ChunkCount > 0 && MeshletCount > 0)
                {
                    auto AllocSSBO = [&](FSceneBuffer& Buffer, uint64 SizeBytes, const char* DebugName)
                    {
                        if (!Buffer || Buffer.Size < SizeBytes)
                        {
                            if (Buffer)
                            {
                                DeferFree(Buffer.Ptr);
                            }
                            Buffer = CreateSceneBuffer(std::max<uint64>(SizeBytes, 16ull), DebugName);
                        }
                    };

                    AllocSSBO(State.ChunkInfoBuffer,      uint64(ChunkCount)   * sizeof(FTerrainChunkInfo),      "Terrain.ChunkInfo");
                    AllocSSBO(State.MeshletInfoBuffer,    uint64(MeshletCount) * sizeof(FTerrainMeshletInfo),   "Terrain.MeshletInfo");
                    AllocSSBO(State.VisibleMeshletBuffer, uint64(MeshletCount) * sizeof(FTerrainVisibleMeshlet), "Terrain.VisibleMeshlets");
                    AllocSSBO(State.IndirectDrawBuffer,   sizeof(RHI::FDrawIndirectArguments),                   "Terrain.IndirectDraw");

                    WriteBuffer(CL, State.ChunkInfoBuffer.Ptr,   TerrainItem.Chunks.data(),   ChunkCount * sizeof(FTerrainChunkInfo));
                    WriteBuffer(CL, State.MeshletInfoBuffer.Ptr, TerrainItem.Meshlets.data(), MeshletCount * sizeof(FTerrainMeshletInfo));
                    bAnyUpload = true;

                    State.AllocatedChunkCount   = ChunkCount;
                    State.AllocatedMeshletCount = MeshletCount;
                }
            }
        }

        if (bAnyUpload)
        {
            Barriers::TransferToAll(CL);
        }
    }

    void FDefaultSceneRenderer::TerrainCullPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.Extracts.TerrainExtracts.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Terrain Cull", tracy::Color::SeaGreen);

        static const FShaderH CullShader = FShaderLibrary::Get("TerrainCull.slang");
        if (!CullShader)
        {
            return;
        }

        bool bAnyDispatched = false;

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            auto TerrainStateIt = TerrainGPUStates.find(TerrainItem.Entity);
            if (TerrainStateIt == TerrainGPUStates.end())
            {
                continue;
            }
            FTerrainGPUState& State = TerrainStateIt->second;
            if (!State.ChunkInfoBuffer || !State.MeshletInfoBuffer || !State.VisibleMeshletBuffer || !State.IndirectDrawBuffer)
            {
                continue;
            }
            if (State.AllocatedChunkCount == 0u || State.AllocatedMeshletCount == 0u)
            {
                continue;
            }

            RHI::FDrawIndirectArguments InitialArgs{};
            InitialArgs.VertexCount   = (uint32)(GTerrainMeshletMaxQuads * 6);
            InitialArgs.InstanceCount = 0u;
            InitialArgs.FirstVertex   = 0u;
            InitialArgs.FirstInstance = 0u;
            WriteBuffer(CL, State.IndirectDrawBuffer.Ptr, &InitialArgs, sizeof(InitialArgs));
            Barriers::TransferToAll(CL);

            if (!bAnyDispatched)
            {
                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullShader));
            }

            FTerrainCullPushConstants Push{};
            Push.ChunksAddr          = State.ChunkInfoBuffer.GetAddress();
            Push.MeshletsAddr        = State.MeshletInfoBuffer.GetAddress();
            Push.VisibleMeshletsAddr = State.VisibleMeshletBuffer.GetAddress();
            Push.TerrainIndirectAddr = State.IndirectDrawBuffer.GetAddress();
            Push.ChunkCount   = State.AllocatedChunkCount;
            Push.MeshletCount = State.AllocatedMeshletCount;

            RHI::CmdDispatch(CL, MakeArgs(Push), State.AllocatedChunkCount, 1u, 1u);
            bAnyDispatched = true;
        }

        if (bAnyDispatched)
        {
            Barriers::ComputeToAll(CL);
        }
    }

    void FDefaultSceneRenderer::TerrainDepthPrePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (Frame.Extracts.TerrainExtracts.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Terrain Depth", tracy::Color::SeaGreen);

        static const FShaderH StampPS = FShaderLibrary::Get("TerrainDepthPixel.slang");
        const FSceneImage& VisRT = GetNamedImage(ENamedImage::VisBuffer);

        RHI::ELoadOp VisLoadOp = DrawCommands.empty() ? RHI::ELoadOp::Clear : RHI::ELoadOp::Load;

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            if (TerrainItem.Resolution < 2 || TerrainItem.ChunkResolution < 2)
            {
                continue;
            }

            auto TerrainStateIt = TerrainGPUStates.find(TerrainItem.Entity);
            if (TerrainStateIt == TerrainGPUStates.end())
            {
                continue;
            }
            FTerrainGPUState& State = TerrainStateIt->second;
            if (!State.HeightmapTexture || !State.NormalTexture || !State.LayerWeightTexture)
            {
                continue;
            }
            if (!State.ChunkInfoBuffer || !State.MeshletInfoBuffer || !State.VisibleMeshletBuffer || !State.IndirectDrawBuffer)
            {
                continue;
            }
            if (State.AllocatedMeshletCount == 0u)
            {
                continue;
            }

            // Shaders were resolved + ref-held at extract (Extract Terrain); null VS => skip.
            FShaderH VertexShader = TerrainItem.Shaders.VertexShader;
            if (!VertexShader)
            {
                continue;
            }

            const entt::entity Entity = TerrainItem.Entity;
            const uint32 Res = (uint32)TerrainItem.Resolution;

            const FMatrix4 WorldMat    = TerrainItem.WorldMatrix;
            const FVector3 WorldOrigin = FVector3(WorldMat[3]);
            const float HalfSize        = TerrainItem.TileWorldSize * 0.5f;

            const int32 QuadsPerChunk        = std::max(1, TerrainItem.ChunkResolution - 1);
            const int32 ChunksPerSide        = std::max(1, ((int32)Res - 1) / QuadsPerChunk);
            const int32 MeshletsPerChunkSide = (QuadsPerChunk + GTerrainMeshletQuads - 1) / GTerrainMeshletQuads;

            FTerrainRenderParams RenderParams{};
            RenderParams.OriginXZ             = FVector2(WorldOrigin.x - HalfSize, WorldOrigin.z - HalfSize);
            RenderParams.TileWorldSize        = TerrainItem.TileWorldSize;
            RenderParams.MaxHeight            = TerrainItem.MaxHeight;
            RenderParams.Resolution           = (int32)Res;
            RenderParams.ChunkResolution      = TerrainItem.ChunkResolution;
            RenderParams.ChunksPerSide        = ChunksPerSide;
            RenderParams.LayerCount           = TerrainItem.LayerCount;
            RenderParams.WorldOriginY         = FVector3(WorldOrigin.y, 0.0f, 0.0f);
            RenderParams.EntityID             = (uint32)Entity;
            RenderParams.MaterialIndex        = TerrainItem.MaterialIndex;
            RenderParams.MeshletsPerChunkSide = MeshletsPerChunkSide;
            RenderParams.MeshletQuadSide      = GTerrainMeshletQuads;

            const FUIntVector2 Extent = GetNamedImage(ENamedImage::HDR).GetExtent();

            RHI::FRenderAttachment Color;
            Color.Texture = VisRT.Texture;
            Color.LoadOp  = VisLoadOp;
            Color.StoreOp = RHI::EStoreOp::Store;

            RHI::FRenderPassDesc Pass;
            if (StampPS != nullptr)
            {
                Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            }
            Pass.DepthAttachment.Texture  = GetSceneDepthRT().Texture;
            Pass.DepthAttachment.LoadOp   = RHI::ELoadOp::Load;   // cleared by VisBuffer phase 1 or ResetPass
            Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
            Pass.RenderArea               = Extent;

            RHI::CmdBeginRenderPass(CL, Pass);
            SetViewportScissor(CL, Extent);

            RHI::FDepthStencilDesc DepthDesc;
            DepthDesc.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
            DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
            RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

            FGraphicsPipelineKey Key;
            Key.VS          = VertexShader;
            Key.PS          = StampPS;
            Key.SampleCount = MSAASampleCount;
            Key.DepthFormat = EFormat::D32;
            if (StampPS != nullptr)
            {
                Key.ColorTargets.push_back({ VisRT.Desc.Format, {} });
            }
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FTerrainPushConstants Push{};
            Push.ParamsAddr        = RHI::Core::CopyTransient(RenderParams);
            Push.ChunksAddr        = State.ChunkInfoBuffer.GetAddress();
            Push.MeshletsAddr      = State.MeshletInfoBuffer.GetAddress();
            Push.VisibleAddr       = State.VisibleMeshletBuffer.GetAddress();
            Push.HeightmapIndex    = (uint32)State.HeightmapTexture.GetResourceID();
            Push.NormalIndex       = (uint32)State.NormalTexture.GetResourceID();
            Push.LayerWeightsIndex = (uint32)State.LayerWeightTexture.GetResourceID();

            RHI::CmdDrawIndirect(CL, MakeArgs(Push), State.IndirectDrawBuffer.Ptr, 0u, 1u, sizeof(RHI::FDrawIndirectArguments));

            RHI::CmdEndRenderPass(CL);
            VisLoadOp = RHI::ELoadOp::Load;   // only the first terrain may own the clear
        }

        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::TerrainRenderPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (Frame.Extracts.TerrainExtracts.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Terrain Render", tracy::Color::SeaGreen);

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            const entt::entity Entity  = TerrainItem.Entity;
            if (TerrainItem.Resolution < 2 || TerrainItem.ChunkResolution < 2)
            {
                continue;
            }

            auto TerrainStateIt = TerrainGPUStates.find(TerrainItem.Entity);
            if (TerrainStateIt == TerrainGPUStates.end())
            {
                continue;
            }
            FTerrainGPUState& State = TerrainStateIt->second;
            if (!State.HeightmapTexture || !State.NormalTexture || !State.LayerWeightTexture)
            {
                continue;
            }
            if (!State.ChunkInfoBuffer || !State.MeshletInfoBuffer || !State.VisibleMeshletBuffer || !State.IndirectDrawBuffer)
            {
                continue;
            }
            if (State.AllocatedMeshletCount == 0u)
            {
                continue;
            }

            FShaderH VertexShader = TerrainItem.Shaders.VertexShader;
            FShaderH PixelShader  = TerrainItem.Shaders.PixelShader;
            if (!VertexShader || !PixelShader)
            {
                continue;
            }

            const uint32 Res = (uint32)TerrainItem.Resolution;

            const FMatrix4 WorldMat = TerrainItem.WorldMatrix;
            const FVector3 WorldOrigin = FVector3(WorldMat[3]);
            const float HalfSize = TerrainItem.TileWorldSize * 0.5f;

            const int32 QuadsPerChunk        = std::max(1, TerrainItem.ChunkResolution - 1);
            const int32 ChunksPerSide        = std::max(1, ((int32)Res - 1) / QuadsPerChunk);
            const int32 MeshletsPerChunkSide = (QuadsPerChunk + GTerrainMeshletQuads - 1) / GTerrainMeshletQuads;

            FTerrainRenderParams RenderParams{};
            RenderParams.OriginXZ             = FVector2(WorldOrigin.x - HalfSize, WorldOrigin.z - HalfSize);
            RenderParams.TileWorldSize        = TerrainItem.TileWorldSize;
            RenderParams.MaxHeight            = TerrainItem.MaxHeight;
            RenderParams.Resolution           = (int32)Res;
            RenderParams.ChunkResolution      = TerrainItem.ChunkResolution;
            RenderParams.ChunksPerSide        = ChunksPerSide;
            RenderParams.LayerCount           = TerrainItem.LayerCount;
            RenderParams.WorldOriginY         = FVector3(WorldOrigin.y, 0.0f, 0.0f);
            RenderParams.EntityID             = (uint32)Entity;
            RenderParams.MaterialIndex        = TerrainItem.MaterialIndex;
            RenderParams.MeshletsPerChunkSide = MeshletsPerChunkSide;
            RenderParams.MeshletQuadSide      = GTerrainMeshletQuads;

            const bool bHDRWasWritten = !DrawCommands.empty() || RenderSettings.bHasEnvironment;
            const FSceneImage& ColorRT  = GetSceneColorRT();
            const FUIntVector2 Extent   = GetNamedImage(ENamedImage::HDR).GetExtent();

            RHI::FRenderAttachment Colors[2];
            uint32 NumColors = 1;
            Colors[0].Texture        = ColorRT.Texture;
            Colors[0].ResolveTexture = GetSceneColorResolve();
            Colors[0].LoadOp         = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
            Colors[0].StoreOp        = RHI::EStoreOp::Store;
            #if USING(WITH_EDITOR)
            const FSceneImage& PickerRT = GetPickerRT();
            // Deferred clears the picker when meshes exist; terrain-only scenes never ran it (early-out).
            Colors[1].Texture        = PickerRT.Texture;
            Colors[1].ResolveTexture = GetPickerResolve();
            Colors[1].LoadOp         = DrawCommands.empty() ? RHI::ELoadOp::Clear : RHI::ELoadOp::Load;
            Colors[1].StoreOp        = RHI::EStoreOp::Store;
            NumColors = 2;
            #endif

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments               = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
            Pass.DepthAttachment.Texture        = GetSceneDepthRT().Texture;
            Pass.DepthAttachment.ResolveTexture = GetSceneDepthResolve();
            Pass.DepthAttachment.LoadOp         = RHI::ELoadOp::Load;
            Pass.DepthAttachment.StoreOp        = RHI::EStoreOp::Store;
            Pass.RenderArea                     = Extent;

            RHI::CmdBeginRenderPass(CL, Pass);
            SetViewportScissor(CL, Extent);

            RHI::FDepthStencilDesc DepthDesc;
            DepthDesc.DepthMode = RHI::EDepthFlags::Read;
            DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
            RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

            FGraphicsPipelineKey Key;
            Key.VS          = VertexShader;
            Key.PS          = PixelShader;
            Key.SampleCount = MSAASampleCount;
            Key.DepthFormat = EFormat::D32;
            Key.ShadingFeatures = SF_DebugViews | SF_GTAO |
                                  (RenderSettings.bShadowMaskValid ? (uint32)SF_ShadowMask : 0u);
            Key.ColorTargets.push_back({ ColorRT.Desc.Format, {} });
            #if USING(WITH_EDITOR)
            Key.ColorTargets.push_back({ PickerRT.Desc.Format, {} });
            #endif
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FTerrainPushConstants Push{};
            Push.ParamsAddr        = RHI::Core::CopyTransient(RenderParams);
            Push.ChunksAddr        = State.ChunkInfoBuffer.GetAddress();
            Push.MeshletsAddr      = State.MeshletInfoBuffer.GetAddress();
            Push.VisibleAddr       = State.VisibleMeshletBuffer.GetAddress();
            Push.HeightmapIndex    = (uint32)State.HeightmapTexture.GetResourceID();
            Push.NormalIndex       = (uint32)State.NormalTexture.GetResourceID();
            Push.LayerWeightsIndex = (uint32)State.LayerWeightTexture.GetResourceID();

            RHI::CmdDrawIndirect(CL, MakeArgs(Push), State.IndirectDrawBuffer.Ptr, 0u, 1u, sizeof(RHI::FDrawIndirectArguments));

            RHI::CmdEndRenderPass(CL);
        }

        Barriers::RasterToRead(CL);
    }
    
    void FDefaultSceneRenderer::GTAOPass(RHI::FCmdListH CL)
    {
        FFrameData& Frame = *RenderFrame;
        if (Frame.Geometry.DrawCommands.empty() || !Frame.CachedWorldSettings.bEnableGTAO)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("GTAO Pass", tracy::Color::Red);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("GTAOPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = GetNamedImage(ENamedImage::GTAO);
        const FSceneImage& Depth  = GetNamedImage(ENamedImage::DepthAttachment);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Undefined;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FData
        {
            uint32 DepthIndex;
        } PC;

        PC.DepthIndex = (uint32)Depth.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::GTAOBlurPass(RHI::FCmdListH CL)
    {
        FFrameData& Frame = *RenderFrame;
        if (Frame.Geometry.DrawCommands.empty() || !Frame.CachedWorldSettings.bEnableGTAO)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("GTAO Blur", tracy::Color::Red);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH DenoisePS = FShaderLibrary::Get("GTAOBlurPixel.slang");
        static const FShaderH UpsamplePS = FShaderLibrary::Get("GTAOUpsamplePixel.slang");
        if (!VertexShader || !DenoisePS || !UpsamplePS)
        {
            return;
        }

        const FSceneImage& Raw      = GetNamedImage(ENamedImage::GTAO);
        const FSceneImage& Denoised = GetNamedImage(ENamedImage::GTAODenoise);
        const FSceneImage& Output   = GetNamedImage(ENamedImage::GTAOBlur);
        const FSceneImage& Depth    = GetNamedImage(ENamedImage::DepthAttachment);

        struct FData
        {
            uint32 AOIndex;
            uint32 DepthIndex;
        };

        const struct
        {
            FShaderH PS;
            const FSceneImage*  Src;
            const FSceneImage*  Dst;
        } Stages[2] =
        {
            { DenoisePS,  &Raw,      &Denoised },
            { UpsamplePS, &Denoised, &Output   },
        };

        for (const auto& Stage : Stages)
        {
            RHI::FRenderAttachment Color;
            Color.Texture = Stage.Dst->Texture;
            Color.LoadOp  = RHI::ELoadOp::Undefined;
            Color.StoreOp = RHI::EStoreOp::Store;

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            Pass.RenderArea       = Stage.Dst->GetExtent();

            RHI::CmdBeginRenderPass(CL, Pass);
            SetViewportScissor(CL, Stage.Dst->GetExtent());
            RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

            FGraphicsPipelineKey Key;
            Key.VS = VertexShader;
            Key.PS = Stage.PS;
            Key.ColorTargets.push_back({ Stage.Dst->Desc.Format, {} });
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FData PC;
            PC.AOIndex    = (uint32)Stage.Src->GetResourceID();
            PC.DepthIndex = (uint32)Depth.GetResourceID();

            RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);
        }
    }

    void FDefaultSceneRenderer::BillboardPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& BillboardInstances = Frame.Primitives.BillboardInstances;
        const auto& DrawCommands       = Frame.Geometry.DrawCommands;

        if (BillboardInstances.empty() || !RenderSettings.bDrawBillboards)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Billboard Pass", tracy::Color::Red);

        static const FShaderH VertexShader = FShaderLibrary::Get("BillboardVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("BillboardPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR    = GetNamedImage(ENamedImage::HDR);

        const bool bHDRWasWritten = !DrawCommands.empty() || RenderSettings.bHasEnvironment
            || !Frame.Extracts.TerrainExtracts.empty() || !Frame.Primitives.SolidBatches.empty()
            || !Frame.Primitives.LineBatches.empty() || !Frame.Extracts.ParticleExtracts.empty();

        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture = HDR.Texture;
        Colors[0].LoadOp  = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Colors[0].StoreOp = RHI::EStoreOp::Store;
        // Entity picking is editor-only; a packaged game binds no Picker MRT (SV_Target1 writes discard).
        #if USING(WITH_EDITOR)
        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
        Colors[1].Texture = Picker.Texture;
        Colors[1].LoadOp  = RHI::ELoadOp::Load;
        Colors[1].StoreOp = RHI::EStoreOp::Store;
        NumColors = 2;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.push_back({ HDR.Desc.Format, AlphaBlend });
        #if USING(WITH_EDITOR)
        Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
        #endif
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::CmdDraw(CL, MakeArgs(), 6, (uint32)BillboardInstances.size(), 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::WidgetPickerPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& WidgetInstances = Frame.Primitives.WidgetInstances;

        if (WidgetInstances.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Widget Picker Pass", tracy::Color::Magenta);

        static const FShaderH VertexShader = FShaderLibrary::Get("WidgetVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("WidgetPickerPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);

        RHI::FRenderAttachment Color;
        Color.Texture = Picker.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Picker.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Picker.GetExtent());

        // Reversed-Z: GreaterOrEqual keeps fragments at/in front of scene depth.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::CmdDraw(CL, MakeArgs(), 6, (uint32)WidgetInstances.size(), 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::WidgetPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& WidgetInstances = Frame.Primitives.WidgetInstances;

        if (WidgetInstances.empty() || !CurrentView->Output.IsValid())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Widget Pass", tracy::Color::Magenta);

        static const FShaderH VertexShader = FShaderLibrary::Get("WidgetVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("WidgetPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = CurrentView->Output;

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());

        // Reversed-Z: GreaterOrEqual keeps fragments at/in front of scene depth, discards occluded.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.push_back({ Output.Desc.Format, AlphaBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::CmdDraw(CL, MakeArgs(), 6, (uint32)WidgetInstances.size(), 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::TextPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame   = *RenderFrame;
        const auto&       Glyphs  = Frame.Primitives.GlyphInstances;
        const auto&       Batches = Frame.Primitives.TextBatches;

        if (Glyphs.empty() || Batches.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Text Pass", tracy::Color::Yellow);

        static const FShaderH VertexShader = FShaderLibrary::Get("TextVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("TextPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR    = GetNamedImage(ENamedImage::HDR);

        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture = HDR.Texture;
        Colors[0].LoadOp  = RHI::ELoadOp::Load;
        Colors[0].StoreOp = RHI::EStoreOp::Store;
        #if USING(WITH_EDITOR)
        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
        Colors[1].Texture = Picker.Texture;
        Colors[1].LoadOp  = RHI::ELoadOp::Load;
        Colors[1].StoreOp = RHI::EStoreOp::Store;
        NumColors = 2;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // Blend only the color target; the Picker (uint id) must not blend.
        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.push_back({ HDR.Desc.Format, AlphaBlend });
        #if USING(WITH_EDITOR)
        Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
        #endif
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        RHI::FDepthStencilDesc DepthTested;
        DepthTested.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthTested.DepthTest = RHI::EOp::GreaterEqual;

        // All glyphs across every batch share one transient array; batches index it via FirstInstance.
        const RHI::GPUPtr GlyphsAddr = RHI::Core::CopyTransientArray(Glyphs.data(), Glyphs.size());

        struct FTextPushConstants
        {
            uint64 GlyphsAddr;
            uint32 AtlasIndex;
            uint32 AtlasWidth;
            uint32 AtlasHeight;
            float  DistanceRange;
            uint32 ScreenWidth;   // 0 for world text (only the debug screen-space pass uses these)
            uint32 ScreenHeight;
        };
        static_assert(sizeof(FTextPushConstants) == 32, "FTextPushConstants must match TextCommon.slang.");

        auto DrawBatch = [&](const FFrameData::FTextBatch& Batch)
        {
            FTextPushConstants PC = {};
            PC.GlyphsAddr    = GlyphsAddr;
            PC.AtlasIndex    = Batch.AtlasIndex;
            PC.AtlasWidth    = Batch.AtlasWidth;
            PC.AtlasHeight   = Batch.AtlasHeight;
            PC.DistanceRange = Batch.DistanceRange;

            RHI::CmdDraw(CL, MakeArgs(PC), 6, Batch.Count, 0, Batch.FirstInstance);
        };

        // Depth-tested text first (sorts against the scene), then always-on-top text last so it stays on top.
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthTested));
        for (const FFrameData::FTextBatch& Batch : Batches)
        {
            if (Batch.bDepthTest)
            {
                DrawBatch(Batch);
            }
        }

        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        for (const FFrameData::FTextBatch& Batch : Batches)
        {
            if (!Batch.bDepthTest)
            {
                DrawBatch(Batch);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::DebugTextPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame  = *RenderFrame;
        const auto&       Glyphs = Frame.Primitives.DebugTextGlyphs;
        const auto&       Batch  = Frame.Primitives.DebugTextBatch;

        if (Glyphs.empty() || Batch.Count == 0 || !CurrentView->Output.IsValid())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Debug Text Pass", tracy::Color::Yellow);

        static const FShaderH VertexShader = FShaderLibrary::Get("DebugTextVert.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("DebugTextPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = CurrentView->Output;

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, AlphaBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FTextPushConstants
        {
            uint64 GlyphsAddr;
            uint32 AtlasIndex;
            uint32 AtlasWidth;
            uint32 AtlasHeight;
            float  DistanceRange;
            uint32 ScreenWidth;
            uint32 ScreenHeight;
        };
        static_assert(sizeof(FTextPushConstants) == 32, "FTextPushConstants must match TextCommon.slang.");

        const FUIntVector4 PanelSize = Frame.SceneGlobalData.ScreenSize;
        const uint32   ScreenW   = PanelSize.x > 1u ? PanelSize.x : Output.GetSizeX();
        const uint32   ScreenH   = PanelSize.y > 1u ? PanelSize.y : Output.GetSizeY();

        FTextPushConstants PC = {};
        PC.GlyphsAddr    = RHI::Core::CopyTransientArray(Glyphs.data(), Glyphs.size());
        PC.AtlasIndex    = Batch.AtlasIndex;
        PC.AtlasWidth    = Batch.AtlasWidth;
        PC.AtlasHeight   = Batch.AtlasHeight;
        PC.DistanceRange = Batch.DistanceRange;
        PC.ScreenWidth   = ScreenW;
        PC.ScreenHeight  = ScreenH;

        RHI::CmdDraw(CL, MakeArgs(PC), 6, Batch.Count, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::ShadowMaskPass(RHI::FCmdListH CL)
    {
        if (!RenderSettings.bShadowMaskValid)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Shadow Mask", tracy::Color::Red);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("ShadowMaskPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = GetNamedImage(ENamedImage::ShadowMask);
        const FSceneImage& Depth  = GetNamedImage(ENamedImage::DepthAttachment);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Undefined;   // every pixel is written (sky included)
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FData
        {
            uint32 DepthIndex;
        } PC;

        PC.DepthIndex = (uint32)Depth.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    // MBOIT pass 1 (Munstermann et al. 2018): rasterizes exactly what TransparentPass will shade, but
    // runs only the opacity half and accumulates absorbance moments. MUST see the identical fragment set.
    void FDefaultSceneRenderer::MomentGenerationPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        if (TranslucentDrawList.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Moment Generation Pass", tracy::Color::SteelBlue);

        const FSceneImage& MomentZeroth = GetNamedImage(ENamedImage::MomentZeroth);
        const FSceneImage& Moments      = GetNamedImage(ENamedImage::Moments);
        const FUIntVector2 Extent       = GetNamedImage(ENamedImage::HDR).GetExtent();

        // Cleared to zero: an empty pixel has zero absorbance, which the shading pass reads as fully
        // transmissive and the composite as an untouched background.
        RHI::FRenderAttachment Colors[2];
        Colors[0].Texture  = MomentZeroth.Texture;
        Colors[0].LoadOp   = RHI::ELoadOp::Clear;
        Colors[0].StoreOp  = RHI::EStoreOp::Store;
        Colors[0].Color[0] = Colors[0].Color[1] = Colors[0].Color[2] = Colors[0].Color[3] = 0.0f;
        Colors[1].Texture  = Moments.Texture;
        Colors[1].LoadOp   = RHI::ELoadOp::Clear;
        Colors[1].StoreOp  = RHI::EStoreOp::Store;
        Colors[1].Color[0] = Colors[1].Color[1] = Colors[1].Color[2] = Colors[1].Color[3] = 0.0f;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, 2);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        // Depth-test against the opaque scene but never write: translucency behind a wall must not
        // contribute moments, and translucency must not occlude itself.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));

        RHI::FBlendDesc MomentBlend;
        MomentBlend.bBlendEnable   = true;
        MomentBlend.SrcColorFactor = RHI::EFactor::One;
        MomentBlend.DstColorFactor = RHI::EFactor::One;
        MomentBlend.SrcAlphaFactor = RHI::EFactor::One;
        MomentBlend.DstAlphaFactor = RHI::EFactor::One;

        FMeshletPassContext Ctx;
        Ctx.CullViewIndex = CurrentCameraEarlyView;
        Ctx.ViewportW     = (float)Extent.x;
        Ctx.ViewportH     = (float)Extent.y;

        ForEachMeshletBatch(CL, TranslucentDrawList, Ctx,
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand& Batch)
            {
                if (Batch.bAdditive)
                {
                    return false;   // AdditiveTranslucentPass owns these; additive is already order-independent
                }

                // Null means the material fell back to a default (still compiling, or a failed graph). Skipping
                // keeps it out of the moments; TransparentPass then shades it unattenuated.
                if (Batch.MomentPixelShader == nullptr)
                {
                    return false;
                }

                Key.MS          = Batch.MeshShaderBase;
                Key.PS          = Batch.MomentPixelShader;
                Key.DepthFormat = EFormat::D32;
                // TransparentPass must make the identical choice or a fragment gets weighted by a transmittance
                // function that does not describe it. Backface-only: small-prim cull would drop slivers pass 2 shades.
                Key.TriCullMode = (uint8)(Batch.bTwoSided ? 0u : (uint32)TriCull_Backface);
                Key.ColorTargets.push_back({ MomentZeroth.Desc.Format, MomentBlend });
                Key.ColorTargets.push_back({ Moments.Desc.Format, MomentBlend });
                return true;
            },
            [&](const FMeshDrawCommand& Batch)
            {
                // Honour the material's two-sided flag exactly as VisBufferPass does. Rasterizing both faces
                // unconditionally doubles the fragments, and these passes are ROP-bound on fp32 moments.
                RHI::CmdSetCullMode(CL, Batch.bTwoSided ? RHI::ECullMode::None : RHI::ECullMode::Back);
            });

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    // MBOIT pass 2: shade the same geometry, weighting each fragment by the transmittance in front of
    // it. No revealage target -- the background's total transmittance is exp(-b_0) from the zeroth moment.
    void FDefaultSceneRenderer::TransparentPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        if (TranslucentDrawList.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Transparent Pass", tracy::Color::CadetBlue);

        const FSceneImage& Accum     = GetNamedImage(ENamedImage::Accum);
        const FUIntVector2 Extent    = GetNamedImage(ENamedImage::HDR).GetExtent();

        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture  = Accum.Texture;
        Colors[0].LoadOp   = RHI::ELoadOp::Clear;
        Colors[0].StoreOp  = RHI::EStoreOp::Store;
        Colors[0].Color[0] = Colors[0].Color[1] = Colors[0].Color[2] = Colors[0].Color[3] = 0.0f;
        #if USING(WITH_EDITOR)
        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
        Colors[1].Texture  = Picker.Texture;
        Colors[1].LoadOp   = RHI::ELoadOp::Load;
        Colors[1].StoreOp  = RHI::EStoreOp::Store;
        NumColors = 2;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));

        RHI::FBlendDesc AccumBlend;
        AccumBlend.bBlendEnable   = true;
        AccumBlend.SrcColorFactor = RHI::EFactor::One;
        AccumBlend.DstColorFactor = RHI::EFactor::One;
        AccumBlend.SrcAlphaFactor = RHI::EFactor::One;
        AccumBlend.DstAlphaFactor = RHI::EFactor::One;

        FMeshletPassContext Ctx;
        Ctx.CullViewIndex = CurrentCameraEarlyView;
        Ctx.ViewportW     = (float)Extent.x;
        Ctx.ViewportH     = (float)Extent.y;

        ForEachMeshletBatch(CL, TranslucentDrawList, Ctx,
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand& Batch)
            {
                if (Batch.bAdditive)
                {
                    return false;   // AdditiveTranslucentPass owns these
                }

                Key.MS          = Batch.MeshShaderBase;
                Key.PS          = Batch.PixelShader;
                Key.DepthFormat = EFormat::D32;
                // MUST stay byte-identical to MomentGenerationPass' choice -- see the comment there.
                Key.TriCullMode = (uint8)(Batch.bTwoSided ? 0u : (uint32)TriCull_Backface);
                Key.ColorTargets.push_back({ Accum.Desc.Format, AccumBlend });
                #if USING(WITH_EDITOR)
                Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
                #endif
                return true;
            },
            [&](const FMeshDrawCommand& Batch)
            {
                RHI::CmdSetCullMode(CL, Batch.bTwoSided ? RHI::ECullMode::None : RHI::ECullMode::Back);
            });

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::OITResolvePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        if (TranslucentDrawList.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("OIT Resolve Pass", tracy::Color::GreenYellow);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("OITResolve.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR          = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Accum        = GetNamedImage(ENamedImage::Accum);
        const FSceneImage& MomentZeroth = GetNamedImage(ENamedImage::MomentZeroth);
        const FSceneImage& Moments      = GetNamedImage(ENamedImage::Moments);

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // MBOIT composite: HDR = Accum.rgb + HDR * TotalTransmittance. Added, not lerped: each fragment was
        // already pre-weighted by the transmittance in front of it.
        RHI::FBlendDesc CompositeBlend;
        CompositeBlend.bBlendEnable   = true;
        CompositeBlend.SrcColorFactor = RHI::EFactor::One;
        CompositeBlend.DstColorFactor = RHI::EFactor::SrcAlpha;
        CompositeBlend.SrcAlphaFactor = RHI::EFactor::Zero;
        CompositeBlend.DstAlphaFactor = RHI::EFactor::One;

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ HDR.Desc.Format, CompositeBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FOITResolvePushConstants
        {
            uint32 AccumIndex;
            uint32 MomentZerothIndex;
            uint32 MomentsIndex;
            uint32 _Pad0;
        };
        static_assert(sizeof(FOITResolvePushConstants) == 16, "FOITResolvePushConstants must match the slang pass block.");

        FOITResolvePushConstants PC = {};
        PC.AccumIndex        = (uint32)Accum.GetResourceID();
        PC.MomentZerothIndex = (uint32)MomentZeroth.GetResourceID();
        PC.MomentsIndex      = (uint32)Moments.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::AdditiveTranslucentPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands        = Frame.Geometry.DrawCommands;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        bool bHasAdditive = false;
        for (uint32 Idx : TranslucentDrawList)
        {
            if (DrawCommands[Idx].bAdditive)
            {
                bHasAdditive = true;
                break;
            }
        }
        if (!bHasAdditive)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Additive Translucent Pass", tracy::Color::CadetBlue3);

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);
        const FUIntVector2 Extent = HDR.GetExtent();

        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture = HDR.Texture;
        Colors[0].LoadOp  = RHI::ELoadOp::Load;
        Colors[0].StoreOp = RHI::EStoreOp::Store;
        #if USING(WITH_EDITOR)
        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
        Colors[1].Texture = Picker.Texture;
        Colors[1].LoadOp  = RHI::ELoadOp::Load;
        Colors[1].StoreOp = RHI::EStoreOp::Store;
        NumColors = 2;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));

        RHI::FBlendDesc AdditiveBlend;
        AdditiveBlend.bBlendEnable   = true;
        AdditiveBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AdditiveBlend.DstColorFactor = RHI::EFactor::One;
        AdditiveBlend.SrcAlphaFactor = RHI::EFactor::One;
        AdditiveBlend.DstAlphaFactor = RHI::EFactor::One;

        // Resolve for the same reason as the WBOIT pass above: recorded after the mid pyramid rebuild.
        FMeshletPassContext Ctx;
        Ctx.CullViewIndex = CurrentCameraEarlyView;
        Ctx.ViewportW     = (float)Extent.x;
        Ctx.ViewportH     = (float)Extent.y;

        ForEachMeshletBatch(CL, TranslucentDrawList, Ctx,
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand& Batch)
            {
                if (!Batch.bAdditive)
                {
                    return false;   // TranslucentPass owns these
                }

                Key.MS          = Batch.MeshShaderBase;
                Key.PS          = Batch.PixelShader;
                Key.DepthFormat = EFormat::D32;
                Key.ColorTargets.push_back({ HDR.Desc.Format, AdditiveBlend });
                #if USING(WITH_EDITOR)
                Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
                #endif
                return true;
            });

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    namespace
    {
        // Mirrors the FPushConstants in the three VolumetricFog*.slang shaders.
        struct FFroxelInjectPushConstants
        {
            uint32 GridSize[3];
            float  NearPlane;
            float  FogRange;            // froxel far plane (max fog distance, view units)
            uint32 bSunVolumetric;      // 1 if light 0 (sun) opted into volumetrics
            uint32 NumLocalVolumetric;  // <= GFroxelMaxLocalLights
            float  Time;
            uint32 LocalLightIndices[GFroxelMaxLocalLights];
            uint64 FogAddr;             // fog params UBO (transient device address) -- offset 96, 8-aligned
            uint32 ScatterUAV;          // bindless 3D UAV index of the scatter volume
            uint32 bSupersampleLocal;   // 1 = 4x supersample local light in-scatter per froxel

        };
        static_assert(sizeof(FFroxelInjectPushConstants) <= 128, "Froxel inject PC must fit 128B");

        struct FFroxelIntegratePushConstants
        {
            uint32 GridSize[3];
            float  NearPlane;
            float  FogRange;
            uint32 ScatterSRV;     // bindless 3D SRV index of the scatter volume
            uint32 IntegratedUAV;  // bindless 3D UAV index of the integrated volume
            uint32 _Pad0;
        };

        struct FFroxelApplyPushConstants
        {
            uint64 FogAddr;          // device address of the fog-params UBO (transient)
            uint32 DepthIndex;       // bindless 2D SRV: scene depth
            uint32 IntegratedIndex;  // bindless 3D SRV: integrated froxel volume
            uint32 GridZ;
            float  NearPlane;
            float  FogRange;
            uint32 bVolumetric;      // froxel volume valid this frame; 0 = analytic height fog only
        };
        static_assert(sizeof(FFroxelApplyPushConstants) == 32, "Froxel apply PC must match the slang push block.");
    }

    void FDefaultSceneRenderer::FroxelInjectPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog)
        {
            return;
        }

        const auto& LightData       = Frame.Lighting.LightData;
        const auto& SceneGlobalData = Frame.SceneGlobalData;

        bool   bSunVolumetric = false;
        uint32 LocalIndices[GFroxelMaxLocalLights];
        uint32 NumLocal = 0;

        if (LightData.NumLights > 0
            && EnumHasAnyFlags(LightData.Lights[0].Flags, ELightFlags::Directional)
            && EnumHasAnyFlags(LightData.Lights[0].Flags, ELightFlags::Volumetric))
        {
            bSunVolumetric = true;
        }
        for (uint32 i = 1; i < LightData.NumLights; ++i)
        {
            if (!EnumHasAnyFlags(LightData.Lights[i].Flags, ELightFlags::Volumetric))
            {
                continue;
            }
            if (NumLocal >= GFroxelMaxLocalLights)
            {
                break;
            }
            LocalIndices[NumLocal++] = i;
        }

        LUMINA_PROFILE_SECTION_COLORED("Froxel Inject Pass", tracy::Color::SlateBlue);

        static const FShaderH CS = FShaderLibrary::Get("VolumetricFogInject.slang");
        if (!CS)
        {
            return;
        }

        const FSceneImage& Scatter = GetNamedImage(ENamedImage::FroxelScatter);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        const float FogRange = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, SceneGlobalData.FarPlane);

        FFroxelInjectPushConstants PC = {};
        PC.FogAddr            = RHI::Core::CopyTransient(Frame.Volumetrics.FogParams);
        PC.ScatterUAV         = (uint32)Scatter.GetMipUAVIndex(0);
        PC.GridSize[0]        = FroxelGridSize.x;
        PC.GridSize[1]        = FroxelGridSize.y;
        PC.GridSize[2]        = FroxelGridSize.z;
        PC.NearPlane          = Math::Max(SceneGlobalData.NearPlane, 0.05f);
        PC.FogRange           = FogRange;
        PC.bSunVolumetric     = bSunVolumetric ? 1u : 0u;
        PC.NumLocalVolumetric = NumLocal;
        PC.Time               = SceneGlobalData.Time;
        PC.bSupersampleLocal  = 1u;
        if (const CRendererSettings* RS = GetDefault<CRendererSettings>())
        {
            PC.bSupersampleLocal = RS->bSupersampleVolumetricLights ? 1u : 0u;
        }
        for (uint32 i = 0; i < NumLocal; ++i)
        {
            PC.LocalLightIndices[i] = LocalIndices[i];
        }

        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(FroxelGridSize.x, 4),
                         RenderUtils::GetGroupCount(FroxelGridSize.y, 4),
                         RenderUtils::GetGroupCount(FroxelGridSize.z, 4));

        // Integrate reads the scatter volume next.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);
    }

    void FDefaultSceneRenderer::FroxelIntegratePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Froxel Integrate Pass", tracy::Color::MediumPurple);

        static const FShaderH CS = FShaderLibrary::Get("VolumetricFogIntegrate.slang");
        if (!CS)
        {
            return;
        }

        const FSceneImage& Scatter    = GetNamedImage(ENamedImage::FroxelScatter);
        const FSceneImage& Integrated = GetNamedImage(ENamedImage::FroxelIntegrated);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        const float FogRange = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, Frame.SceneGlobalData.FarPlane);

        FFroxelIntegratePushConstants PC = {};
        PC.GridSize[0]    = FroxelGridSize.x;
        PC.GridSize[1]    = FroxelGridSize.y;
        PC.GridSize[2]    = FroxelGridSize.z;
        PC.NearPlane      = Math::Max(Frame.SceneGlobalData.NearPlane, 0.05f);
        PC.FogRange       = FogRange;
        PC.ScatterSRV     = (uint32)Scatter.GetResourceID();
        PC.IntegratedUAV  = (uint32)Integrated.GetMipUAVIndex(0);

        // One thread per (x,y) column; each marches the full Z range.
        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(FroxelGridSize.x, 8),
                         RenderUtils::GetGroupCount(FroxelGridSize.y, 8),
                         1u);

        Barriers::ComputeToAll(CL);
    }

    void FDefaultSceneRenderer::FroxelApplyPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Froxel Apply Pass", tracy::Color::Orange3);

        static const FShaderH VS = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PS = FShaderLibrary::Get("VolumetricFogApply.slang");
        if (!VS || !PS)
        {
            return;
        }

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& Integrated = GetNamedImage(ENamedImage::FroxelIntegrated);

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FBlendDesc OverBlend;
        OverBlend.bBlendEnable   = true;
        OverBlend.SrcColorFactor = RHI::EFactor::One;
        OverBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        OverBlend.SrcAlphaFactor = RHI::EFactor::Zero;
        OverBlend.DstAlphaFactor = RHI::EFactor::One;

        FGraphicsPipelineKey Key;
        Key.VS = VS;
        Key.PS = PS;
        Key.ColorTargets.push_back({ HDR.Desc.Format, OverBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        FFroxelApplyPushConstants PC = {};
        PC.FogAddr         = RHI::Core::CopyTransient(Frame.Volumetrics.FogParams);
        PC.DepthIndex      = (uint32)SceneDepth.GetResourceID();
        PC.IntegratedIndex = (uint32)Integrated.GetResourceID();
        PC.GridZ           = FroxelGridSize.z;
        PC.NearPlane       = Math::Max(Frame.SceneGlobalData.NearPlane, 0.05f);
        PC.FogRange        = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, Frame.SceneGlobalData.FarPlane);
        PC.bVolumetric     = Frame.Volumetrics.bVolumetricFog ? 1u : 0u;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::WaterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const TVector<FGPUWater>& Waters = Frame.Water.Surfaces;
        if (Waters.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Water Pass", tracy::Color::CadetBlue);

        static const FShaderH VS = FShaderLibrary::Get("WaterVert.slang");
        static const FShaderH PS = FShaderLibrary::Get("WaterPixel.slang");
        if (!VS || !PS)
        {
            return;
        }

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneColor = GetNamedImage(ENamedImage::WaterRefraction);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);
        
        Barriers::AllToTransfer(CL);
        RHI::CmdCopyTexture(CL, HDR.Texture, RHI::FTextureSlice{}, SceneColor.Texture, RHI::FTextureSlice{});
        Barriers::TransferToAll(CL);
        
        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        // Double-sided so the surface is visible from below (camera submerged) too.
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // Standard alpha "over": the PS composites scene+water; alpha softens the shoreline edge.
        RHI::FBlendDesc WaterBlend;
        WaterBlend.bBlendEnable   = true;
        WaterBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        WaterBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        WaterBlend.SrcAlphaFactor = RHI::EFactor::One;
        WaterBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS = VS;
        Key.PS = PS;
        Key.ColorTargets.push_back({ HDR.Desc.Format, WaterBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FWaterPushConstants
        {
            uint64 WatersAddr;
            uint32 SceneColorIndex;
            uint32 SceneDepthIndex;
        };
        static_assert(sizeof(FWaterPushConstants) == 16, "FWaterPushConstants must match Includes/Water.slang.");

        FWaterPushConstants PC = {};
        PC.WatersAddr      = RHI::Core::CopyTransientArray(Waters.data(), Waters.size());
        PC.SceneColorIndex = (uint32)SceneColor.GetResourceID();
        PC.SceneDepthIndex = (uint32)SceneDepth.GetResourceID();

        const RHI::GPUPtr Args = MakeArgs(PC);

        for (uint32 i = 0; i < (uint32)Waters.size(); ++i)
        {
            const uint32 Res = Waters[i].GridResolution;
            const uint32 VertexCount = (Res > 1u) ? (Res - 1u) * (Res - 1u) * 6u : 0u;
            if (VertexCount == 0u)
            {
                continue;
            }

            RHI::CmdDraw(CL, Args, VertexCount, 1, 0, i);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::UnderwaterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Water.bUnderwaterActive)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Underwater Pass", tracy::Color::SteelBlue);

        static const FShaderH VS = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PS = FShaderLibrary::Get("WaterUnderwater.slang");
        if (!VS || !PS)
        {
            return;
        }

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneColor = GetNamedImage(ENamedImage::WaterRefraction);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);

        Barriers::AllToTransfer(CL);
        RHI::CmdCopyTexture(CL, HDR.Texture, RHI::FTextureSlice{}, SceneColor.Texture, RHI::FTextureSlice{});
        Barriers::TransferToAll(CL);

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VS;
        Key.PS = PS;
        Key.ColorTargets.push_back({ HDR.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FUnderwaterPushConstants
        {
            uint64 ParamsAddr;
            uint32 SceneColorIndex;
            uint32 SceneDepthIndex;
        };
        static_assert(sizeof(FUnderwaterPushConstants) == 16, "FUnderwaterPushConstants must match WaterUnderwater.slang.");

        FUnderwaterPushConstants PC = {};
        PC.ParamsAddr      = RHI::Core::CopyTransient(Frame.Water.Underwater);
        PC.SceneColorIndex = (uint32)SceneColor.GetResourceID();
        PC.SceneDepthIndex = (uint32)SceneDepth.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
    
    void FDefaultSceneRenderer::SkyCubeCapturePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Sky Cube Capture", tracy::Color::SkyBlue);

        const FFrameData& Frame = *RenderFrame;
        const auto& LightData         = Frame.Lighting.LightData;
        const auto& SceneGlobalData   = Frame.SceneGlobalData;
        const int32 EnvironmentMapID  = Frame.Volumetrics.EnvironmentMapID;
        const bool bIBLDirty          = Frame.Volumetrics.bIBLDirty;

        if (!RenderSettings.bHasEnvironment)
        {
            const float Black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            Barriers::AllToTransfer(CL);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyCube).Texture, Black);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyIrradiance).Texture, Black);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyPrefilter).Texture, Black);
            Barriers::TransferToAll(CL);
            return;
        }

        if (!bIBLDirty)
        {
            return;
        }

        const FSceneImage& SkyCube = GetNamedImage(ENamedImage::SkyCube);
        if (!SkyCube.IsValid())
        {
            return;
        }

        // HDRI path: equirect->cube replaces the procedural fill.
        if (EnvironmentMapID >= 0)
        {
            static const FShaderH ComputeShader = FShaderLibrary::Get("EquirectToCubemap.slang");
            if (!ComputeShader)
            {
                return;
            }

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

            struct FEquirectPC
            {
                uint32 EquirectSRV;
                uint32 SkyCubeUAV;
                float  Intensity;
                float  CosYaw;
                float  SinYaw;
                uint32 _Pad0;
            };
            static_assert(sizeof(FEquirectPC) == 24, "FEquirectPC must match EquirectToCubemap.slang::FPushConstants.");

            const FVector4& HDRIParams = Frame.Volumetrics.EnvironmentParams.HDRIParams;

            FEquirectPC PC = {};
            PC.EquirectSRV = (uint32)EnvironmentMapID;
            PC.SkyCubeUAV  = (uint32)SkyCube.GetMipUAVIndex(0);
            PC.Intensity   = HDRIParams.x;
            PC.CosYaw      = HDRIParams.y;
            PC.SinYaw      = HDRIParams.z;

            constexpr uint32 EquirectTile = 8u;
            const uint32 FaceSize = SkyCube.GetSizeX();
            const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, EquirectTile);
            RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader);
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("SkyCubeCapture.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        struct FSkyCapturePC
        {
            uint64   EnvAddr;
            uint32   SkyCubeUAV;
            float    Time;
            FVector3 SunDirection;
            float    _Pad;
        } PC = {};
        PC.EnvAddr    = RHI::Core::CopyTransient(Frame.Volumetrics.EnvironmentParams);
        PC.SkyCubeUAV = (uint32)SkyCube.GetMipUAVIndex(0);

        if (LightData.bHasSun)
        {
            PC.SunDirection = Math::Normalize(LightData.SunDirection);
        }
        else
        {
            PC.SunDirection = Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));
        }
        PC.Time = SceneGlobalData.Time;

        constexpr uint32 SkyCaptureTile = 8u;
        const uint32 FaceSize = SkyCube.GetSizeX();
        const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, SkyCaptureTile);
        // Z = 6 layers, one per cube face -- each thread owns one (face, x, y).
        RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);

        // Convolution + environment pass read the cube next.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader);
    }

    void FDefaultSceneRenderer::IrradianceConvolutionPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Sky Irradiance Convolution", tracy::Color::SkyBlue1);

        const FFrameData& Frame = *RenderFrame;
        const bool bIBLConvolutionDirty = Frame.Volumetrics.bIBLConvolutionDirty;

        if (!RenderSettings.bHasEnvironment)
        {
            return;
        }

        if (!bIBLConvolutionDirty)
        {
            return;
        }

        const FSceneImage& SkyCube        = GetNamedImage(ENamedImage::SkyCube);
        const FSceneImage& IrradianceCube = GetNamedImage(ENamedImage::SkyIrradiance);
        if (!SkyCube.IsValid() || !IrradianceCube.IsValid())
        {
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("IrradianceConvolution.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        struct FIrradiancePC { uint32 SrcCubeSRV; uint32 OutCubeUAV; uint32 _Pad0; uint32 _Pad1; };
        FIrradiancePC PC = {};
        PC.SrcCubeSRV = (uint32)SkyCube.GetResourceID();
        PC.OutCubeUAV = (uint32)IrradianceCube.GetMipUAVIndex(0);

        constexpr uint32 IrradianceTile = 8u;
        const uint32 FaceSize = IrradianceCube.GetSizeX();
        const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, IrradianceTile);
        RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);

        Barriers::ComputeToAll(CL);
    }

    void FDefaultSceneRenderer::PrefilterEnvMapPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const bool bIBLConvolutionDirty = Frame.Volumetrics.bIBLConvolutionDirty;

        if (!RenderSettings.bHasEnvironment)
        {
            return;
        }

        if (!bIBLConvolutionDirty)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Sky Prefilter Convolution", tracy::Color::SkyBlue2);

        const FSceneImage& SkyCube       = GetNamedImage(ENamedImage::SkyCube);
        const FSceneImage& PrefilterCube = GetNamedImage(ENamedImage::SkyPrefilter);
        if (!SkyCube.IsValid() || !PrefilterCube.IsValid())
        {
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("PrefilterEnvMap.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        const uint32 NumMips      = PrefilterCube.GetNumMips();
        const uint32 BaseFaceSize = PrefilterCube.GetSizeX();

        constexpr uint32 PrefilterTile = 8u;

        for (uint32 Mip = 0; Mip < NumMips; ++Mip)
        {
            FPrefilterPC PC = {};
            PC.SrcCubeSRV = (uint32)SkyCube.GetResourceID();
            PC.OutMipUAV  = (uint32)PrefilterCube.GetMipUAVIndex(Mip);
            PC.Roughness  = (NumMips <= 1u) ? 0.0f
                                            : (float)Mip / (float)(NumMips - 1u);
            PC.NumSamples = GPrefilterSampleCount;

            const uint32 MipFaceSize = eastl::max<uint32>(BaseFaceSize >> Mip, 1u);
            const uint32 GroupsXY    = RenderUtils::GetGroupCount(MipFaceSize, PrefilterTile);
            RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
        }

        Barriers::ComputeToAll(CL);
    }

    void FDefaultSceneRenderer::EnvironmentPass(RHI::FCmdListH CL)
    {
        if (!RenderSettings.bHasEnvironment)
        {
            const FSceneImage& ColorRT = GetSceneColorRT();

            RHI::FRenderAttachment Color;
            Color.Texture        = ColorRT.Texture;
            Color.ResolveTexture = GetSceneColorResolve();
            Color.LoadOp         = RHI::ELoadOp::Clear;
            Color.StoreOp        = RHI::EStoreOp::Store;

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            Pass.RenderArea       = GetNamedImage(ENamedImage::HDR).GetExtent();

            RHI::CmdBeginRenderPass(CL, Pass);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Environment Pass", tracy::Color::Green3);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("Environment.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& ColorRT = GetSceneColorRT();
        const FSceneImage& SkyCube = GetNamedImage(ENamedImage::SkyCube);
        const FUIntVector2 Extent  = GetNamedImage(ENamedImage::HDR).GetExtent();

        const int32  EnvMapID    = RenderFrame->Volumetrics.EnvironmentMapID;
        const uint32 EquirectIdx = EnvMapID >= 0 ? (uint32)EnvMapID : (uint32)GetNamedImage(ENamedImage::BRDFLut).GetResourceID();
        const uint32 EquirectW   = EnvMapID >= 0 ? RenderFrame->Volumetrics.EnvironmentMapWidth : 256u;

        RHI::FRenderAttachment Color;
        Color.Texture        = ColorRT.Texture;
        Color.ResolveTexture = GetSceneColorResolve();
        Color.LoadOp         = RHI::ELoadOp::Clear;
        Color.StoreOp        = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.SampleCount = MSAASampleCount;
        Key.ColorTargets.push_back({ ColorRT.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FEnvPushConstants
        {
            uint64 EnvAddr;
            uint32 SkyCubeIndex;
            uint32 EquirectIndex;
            uint32 EquirectWidth;   // HDRI mode: drives the screen-derivative LOD that anti-aliases the sky.
            uint32 _Pad1;
        };
        static_assert(sizeof(FEnvPushConstants) == 24, "FEnvPushConstants must match the slang pass block.");

        FEnvPushConstants PC = {};
        PC.EnvAddr       = RHI::Core::CopyTransient(RenderFrame->Volumetrics.EnvironmentParams);
        PC.SkyCubeIndex  = (uint32)SkyCube.GetResourceID();
        PC.EquirectIndex = EquirectIdx;
        PC.EquirectWidth = EquirectW;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    namespace
    {
        struct FSimpleElementPassData { uint64 Vertices = 0; };
    }

    void FDefaultSceneRenderer::BatchedLineDraw(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& SimpleVertices     = Frame.Primitives.SimpleVertices;
        const auto& LineBatches        = Frame.Primitives.LineBatches;
        const auto& ImmediateRanges    = Frame.Primitives.ImmediateLines;
        const auto& DrawCommands       = Frame.Geometry.DrawCommands;

        const bool bHasBatched = !SimpleVertices.empty() && !LineBatches.empty();

        bool bHasImmediate = false;
        for (const FImmediateLineRenderer::FDrawRange& Range : ImmediateRanges)
        {
            bHasImmediate |= (Range.Vertices != 0 && Range.VertexCount > 0);
        }

        if (!bHasBatched && !bHasImmediate)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Batched Line Draw", tracy::Color::Red2);

        static const FShaderH VertexShader = FShaderLibrary::Get("SimpleElementVertex.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SimpleElementPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);

        const bool bHDRWasWritten = !DrawCommands.empty() || RenderSettings.bHasEnvironment
            || !Frame.Extracts.TerrainExtracts.empty() || !Frame.Primitives.SolidBatches.empty();

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // No input layout: the VS pulls vertices from PassAddr by SV_VertexID.
        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.Topology    = RHI::ETopology::LineList;
        Key.DepthFormat = EFormat::D32;
        Key.ColorTargets.push_back({ HDR.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        // Depth-tested lines occlude (reversed-Z Greater + depth write); X-ray lines draw on top.
        RHI::FDepthStencilDesc DepthTested;
        DepthTested.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        DepthTested.DepthTest = RHI::EOp::Greater;

        // Re-set only when the depth mode changes between consecutive batches.
        int CurrentDepthMode = -1;

        if (bHasBatched)
        {
            // Vertices live in the transient ring for this submission; the VS reads them by device address.
            const FSimpleElementPassData VertsPass
            {
                RHI::Core::CopyTransientArray(SimpleVertices.data(), SimpleVertices.size())
            };
            const RHI::GPUPtr Args = MakeArgs(VertsPass);

            for (const FLineBatch& Batch : LineBatches)
            {
                const int DepthMode = Batch.bDepthTest ? 1 : 0;
                if (DepthMode != CurrentDepthMode)
                {
                    RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(Batch.bDepthTest ? DepthTested : RHI::FDepthStencilDesc{}));
                    CurrentDepthMode = DepthMode;
                }
                RHI::CmdSetLineWidth(CL, Batch.Thickness);

                // FirstVertex feeds SV_VertexID so the VS indexes into the full vertex array.
                RHI::CmdDraw(CL, Args, Batch.VertexCount, 1, Batch.StartVertex, 0);
            }
        }

        if (bHasImmediate)
        {
            RHI::CmdSetLineWidth(CL, 1.0f);

            for (uint32 Channel = 0; Channel < FImmediateLineRenderer::NumChannels; ++Channel)
            {
                const FImmediateLineRenderer::FDrawRange& Range = ImmediateRanges[Channel];
                if (Range.Vertices == 0 || Range.VertexCount == 0)
                {
                    continue;
                }

                const bool bDepthTest = (Channel == FImmediateLineRenderer::DepthTested);
                const int  DepthMode  = bDepthTest ? 1 : 0;
                if (DepthMode != CurrentDepthMode)
                {
                    RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(bDepthTest ? DepthTested : RHI::FDepthStencilDesc{}));
                    CurrentDepthMode = DepthMode;
                }

                const RHI::GPUPtr ImmediateArgs = MakeArgs(FSimpleElementPassData{ Range.Vertices });
                RHI::CmdDraw(CL, ImmediateArgs, Range.VertexCount, 1, 0, 0);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::BatchedTriangleDraw(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& SolidVertices = Frame.Primitives.SolidVertices;
        const auto& SolidBatches  = Frame.Primitives.SolidBatches;
        const auto& DrawCommands  = Frame.Geometry.DrawCommands;

        if (SolidVertices.empty() || SolidBatches.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Batched Triangle Draw", tracy::Color::Green2);

        static const FShaderH VertexShader = FShaderLibrary::Get("SimpleElementVertex.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SimpleElementPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);

        // First HDR writer in the frame clears; later writers load. Earlier writers here: base pass / terrain.
        const bool bHDRWasWritten = !DrawCommands.empty() || RenderSettings.bHasEnvironment || !Frame.Extracts.TerrainExtracts.empty();

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = HDR.GetExtent();

        bool bWritesDepth = false;
        for (const FSolidBatch& Batch : SolidBatches)
        {
            if (Batch.Mode == ESolidDrawMode::Opaque)
            {
                bWritesDepth = true;
                break;
            }
        }

        if (bWritesDepth)
        {
            RHI::CmdBarrier(CL, RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute, RHI::EStageFlags::FragmentTests);
        }

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        // Two-sided so the surface reads from any angle.
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        // No input layout: the VS pulls vertices from PassAddr by SV_VertexID.
        FGraphicsPipelineKey BlendedKey;
        BlendedKey.VS          = VertexShader;
        BlendedKey.PS          = PixelShader;
        BlendedKey.DepthFormat = EFormat::D32;
        BlendedKey.ColorTargets.push_back({ HDR.Desc.Format, AlphaBlend });

        FGraphicsPipelineKey OpaqueKey = BlendedKey;
        OpaqueKey.ColorTargets[0].Blend = RHI::FBlendDesc{};

        RHI::FDepthStencilDesc TranslucentDepth;
        TranslucentDepth.DepthMode = RHI::EDepthFlags::Read;
        TranslucentDepth.DepthTest = RHI::EOp::Greater;

        RHI::FDepthStencilDesc OpaqueDepth;
        OpaqueDepth.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        OpaqueDepth.DepthTest = RHI::EOp::Greater;

        const FSimpleElementPassData VertsPass{ RHI::Core::CopyTransientArray(SolidVertices.data(), SolidVertices.size()) };
        const RHI::GPUPtr Args = MakeArgs(VertsPass);

        struct FModeGroup
        {
            ESolidDrawMode                  Mode;
            const FGraphicsPipelineKey*     Pipeline;
            const RHI::FDepthStencilDesc*   Depth;
        };

        const RHI::FDepthStencilDesc XRayDepth{};
        const FModeGroup Groups[] =
        {
            { ESolidDrawMode::Opaque,      &OpaqueKey,  &OpaqueDepth      },
            { ESolidDrawMode::Translucent, &BlendedKey, &TranslucentDepth },
            { ESolidDrawMode::XRay,        &BlendedKey, &XRayDepth        },
        };

        for (const FModeGroup& Group : Groups)
        {
            bool bStateBound = false;
            for (const FSolidBatch& Batch : SolidBatches)
            {
                if (Batch.Mode != Group.Mode)
                {
                    continue;
                }

                if (!bStateBound)
                {
                    RHI::CmdSetPipeline(CL, GetOrCreatePipeline(*Group.Pipeline));
                    RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(*Group.Depth));
                    bStateBound = true;
                }

                RHI::CmdDraw(CL, Args, Batch.VertexCount, 1, Batch.StartVertex, 0);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    namespace
    {
        struct FColorGradingConstants
        {
            float    Exposure;
            float    Contrast;
            float    Saturation;
            float    Gamma;

            float    WhiteTemp;
            float    WhiteTint;
            float    VignetteIntensity;
            float    VignetteSmoothness;

            float    VignetteRoundness;
            uint32   TonemapMode;
            float    Time;
            float    BloomIntensity;     // 0 disables the bloom composite path in the shader.

            FVector4 ColorFilter;

            FVector4 Shadows;
            FVector4 Midtones;
            FVector4 Highlights;
            FVector4 VignetteColor;

            // .rgb = bloom tint, .a = chromatic aberration intensity.
            FVector4 BloomTint;

            float    AutoExposureKey;    // middle-grey key; <= 0 disables auto-exposure.
            float    AutoExposureMinMul; // 2^MinEV clamp on the adapted multiplier.
            float    AutoExposureMaxMul; // 2^MaxEV clamp on the adapted multiplier.
            float    _PadAE;
        };
        static_assert(sizeof(FColorGradingConstants) == 160, "FColorGradingConstants layout must match ColorGrading.slang::FColorGradingConstants.");
        
        FColorGradingConstants MakeDefaultColorGrading(float Time)
        {
            FColorGradingConstants PC{};
            PC.Exposure           = 1.0f;
            PC.Contrast           = 1.0f;
            PC.Saturation         = 1.0f;
            PC.Gamma              = 1.0f;
            PC.WhiteTemp          = 0.0f;
            PC.WhiteTint          = 0.0f;
            PC.VignetteIntensity  = 0.0f;
            PC.VignetteSmoothness = 0.5f;
            PC.VignetteRoundness  = 1.0f;
            // Matches SPostProcessSettings::ToneMapper's default, so a view with no settings (thumbnails,
            // any camera-less scene) grades the same as one that has them.
            PC.TonemapMode        = (uint32)EToneMapper::AGX;
            PC.Time               = Time;
            PC.BloomIntensity     = 0.0f;
            PC.ColorFilter        = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
            PC.Shadows            = FVector4(1.0f, 1.0f, 1.0f, 0.0f);
            PC.Midtones           = FVector4(1.0f, 1.0f, 1.0f, 0.0f);
            PC.Highlights         = FVector4(1.0f, 1.0f, 1.0f, 0.0f);
            PC.VignetteColor      = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
            PC.BloomTint          = FVector4(1.0f, 1.0f, 1.0f, 0.0f);
            PC.AutoExposureKey    = 0.0f;
            PC.AutoExposureMinMul = 0.0f;
            PC.AutoExposureMaxMul = 1.0f;
            return PC;
        }

        FColorGradingConstants BuildColorGradingConstants(const SPostProcessSettings* Settings, float Time)
        {
            if (Settings == nullptr || !Settings->bEnabled)
            {
                return MakeDefaultColorGrading(Time);
            }

            FColorGradingConstants PC{};
            PC.Exposure           = std::exp2(Settings->ExposureCompensation);
            PC.Contrast           = Settings->Contrast;
            PC.Saturation         = Settings->Saturation;
            PC.Gamma              = Settings->Gamma;
            PC.WhiteTemp          = Settings->Temperature;
            PC.WhiteTint          = Settings->Tint;
            PC.VignetteIntensity  = Settings->VignetteIntensity;
            PC.VignetteSmoothness = Settings->VignetteSmoothness;
            PC.VignetteRoundness  = Settings->VignetteRoundness;
            PC.TonemapMode        = (uint32)Settings->ToneMapper;
            PC.Time               = Time;
            PC.BloomIntensity     = Settings->BloomIntensity;
            PC.ColorFilter        = FVector4(Settings->ColorFilter, Settings->ColorFilterIntensity);
            PC.Shadows            = FVector4(Settings->Shadows,    Settings->FilmGrainIntensity);
            PC.Midtones           = FVector4(Settings->Midtones,   std::max(Settings->FilmGrainSize, 0.0001f));
            PC.Highlights         = FVector4(Settings->Highlights, Settings->FilmGrainResponse);
            PC.VignetteColor      = FVector4(Settings->VignetteColor, 0.0f);
            PC.BloomTint          = FVector4(Settings->BloomTint, Settings->ChromaticAberration);
            PC.AutoExposureKey    = Settings->bAutoExposure ? 0.18f : 0.0f;
            PC.AutoExposureMinMul = std::exp2(Settings->AutoExposureMinEV);
            PC.AutoExposureMaxMul = std::exp2(std::max(Settings->AutoExposureMaxEV, Settings->AutoExposureMinEV));
            return PC;
        }
    }

    namespace
    {
        // Push constants for BloomDownsample.slang; one dispatch per mip.
        struct FBloomDownPushConstants
        {
            FVector2     SrcTexelSize;
            uint32       SrcIndex;
            float        SrcMip;

            FUIntVector2 DstSize;
            uint32       DstUAV;
            uint32       bFirstPass;

            float        Threshold;
            FVector3     KneeCurve;
        };
        static_assert(sizeof(FBloomDownPushConstants) == 48, "FBloomDownPushConstants must match BloomDownsample.slang::FPushConstants.");

        struct FBloomUpCSPushConstants
        {
            FVector2  SrcTexelSize;
            float      Radius;
            uint32     SrcIndex;

            FUIntVector2 DstSize;
            uint32     DstUAV;
            float      SrcMip;

            float      Scatter;
            uint32     _Pad0;
            uint32     _Pad1;
            uint32     _Pad2;
        };
        static_assert(sizeof(FBloomUpCSPushConstants) == 48,
            "FBloomUpCSPushConstants must match BloomUpsampleCS.slang::FPushConstants.");

        constexpr uint32 BloomTileSize = 8;
    }

    void FDefaultSceneRenderer::BloomPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const SPostProcessSettings* ActivePostProcess = Frame.PostProcess.bHasActivePostProcess ? &Frame.PostProcess.ActivePostProcessStorage : nullptr;

        if (ActivePostProcess == nullptr || !ActivePostProcess->bEnabled || ActivePostProcess->BloomIntensity <= 0.0f)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Bloom Pass", tracy::Color::Yellow3);

        static const FShaderH DownCS = FShaderLibrary::Get("BloomDownsample.slang");
        static const FShaderH UpCS = FShaderLibrary::Get("BloomUpsampleCS.slang");
        if (!DownCS || !UpCS)
        {
            return;
        }

        const FSceneImage& HDR   = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Bloom = CurrentView->BloomChainImage;
        const uint32 HDRWidth = HDR.GetSizeX();
        const uint32 HDRHght  = HDR.GetSizeY();
        const uint32 Mip0W    = eastl::max<uint32>(HDRWidth >> 1u, 1u);
        const uint32 Mip0H    = eastl::max<uint32>(HDRHght  >> 1u, 1u);

        const uint32 MinDim  = eastl::min(Mip0W, Mip0H);
        const uint32 Octaves = MinDim >= 8u ? (uint32)Math::Log2((float)MinDim) - 2u : 1u;
        const uint32 NumMips = Math::Clamp(Octaves, 1u, Math::Max(Bloom.GetNumMips(), 1u));

        const float Threshold = ActivePostProcess->BloomThreshold;
        const float Knee      = ActivePostProcess->BloomSoftKnee * Threshold + 1e-5f;

        // Down chain: 13-tap filtered reduction per mip, prefilter on the first.
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(DownCS));
        for (uint32 Mip = 0; Mip < NumMips; ++Mip)
        {
            const uint32 DstW = eastl::max<uint32>(Mip0W >> Mip, 1u);
            const uint32 DstH = eastl::max<uint32>(Mip0H >> Mip, 1u);
            const uint32 SrcW = (Mip == 0) ? HDRWidth : eastl::max<uint32>(Mip0W >> (Mip - 1u), 1u);
            const uint32 SrcH = (Mip == 0) ? HDRHght  : eastl::max<uint32>(Mip0H >> (Mip - 1u), 1u);

            if (Mip > 0)
            {
                // Order against the previous mip's writes.
                RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);
            }

            FBloomDownPushConstants PC = {};
            PC.SrcTexelSize = FVector2(1.0f / (float)SrcW, 1.0f / (float)SrcH);
            PC.SrcIndex     = (Mip == 0) ? (uint32)HDR.GetResourceID() : (uint32)Bloom.GetResourceID();
            PC.SrcMip       = (Mip == 0) ? 0.0f : (float)(Mip - 1u);
            PC.DstSize      = FUIntVector2(DstW, DstH);
            PC.DstUAV       = (uint32)Bloom.GetMipUAVIndex(Mip);
            PC.bFirstPass   = (Mip == 0) ? 1u : 0u;
            PC.Threshold    = Threshold;
            PC.KneeCurve    = FVector3(Threshold - Knee, 2.0f * Knee, 0.25f / Knee);

            RHI::CmdDispatch(CL, MakeArgs(PC),
                             RenderUtils::GetGroupCount(DstW, BloomTileSize),
                             RenderUtils::GetGroupCount(DstH, BloomTileSize), 1);
        }

        // Up chain: tent-filtered progressive accumulation, scatter-weighted.
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(UpCS));
        for (uint32 i = NumMips - 1; i > 0; --i)
        {
            const uint32 SrcMip = i;
            const uint32 DstMip = i - 1;
            const uint32 SrcW   = eastl::max<uint32>(Mip0W >> SrcMip, 1u);
            const uint32 SrcH   = eastl::max<uint32>(Mip0H >> SrcMip, 1u);
            const uint32 DstW   = eastl::max<uint32>(Mip0W >> DstMip, 1u);
            const uint32 DstH   = eastl::max<uint32>(Mip0H >> DstMip, 1u);

            // Order against the previous mip's writes (down chain, then each up step).
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);

            FBloomUpCSPushConstants PC = {};
            PC.SrcTexelSize = FVector2(1.0f / (float)SrcW, 1.0f / (float)SrcH);
            PC.Radius       = 1.0f;
            PC.SrcIndex     = (uint32)Bloom.GetResourceID();
            PC.DstSize      = FUIntVector2(DstW, DstH);
            PC.DstUAV       = (uint32)Bloom.GetMipUAVIndex(DstMip);
            PC.SrcMip       = (float)SrcMip;
            PC.Scatter      = Math::Clamp(ActivePostProcess->BloomScatter, 0.0f, 1.0f);

            RHI::CmdDispatch(CL, MakeArgs(PC),
                             RenderUtils::GetGroupCount(DstW, BloomTileSize),
                             RenderUtils::GetGroupCount(DstH, BloomTileSize), 1);
        }

        Barriers::ComputeToAll(CL);
    }

    namespace
    {
        // 16 B push block for AutoExposure.slang. Mirrors its FPushConstants.
        struct FAutoExposurePushConstants
        {
            uint32 HDRIndex;
            uint32 AdaptUAV;
            float  DeltaTime;
            float  AdaptationSpeed;
        };
        static_assert(sizeof(FAutoExposurePushConstants) == 16,
            "FAutoExposurePushConstants must match AutoExposure.slang::FPushConstants.");
    }

    void FDefaultSceneRenderer::AutoExposurePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const SPostProcessSettings* ActivePostProcess = Frame.PostProcess.bHasActivePostProcess ? &Frame.PostProcess.ActivePostProcessStorage : nullptr;

        if (ActivePostProcess == nullptr || !ActivePostProcess->bEnabled || !ActivePostProcess->bAutoExposure)
        {
            return;
        }

        static const FShaderH CS = FShaderLibrary::Get("AutoExposure.slang");
        if (!CS)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Auto Exposure Pass", tracy::Color::Orange3);

        const FSceneImage& HDR     = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Adapted = GetNamedImage(ENamedImage::AdaptedLuminance);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        FAutoExposurePushConstants PC = {};
        PC.HDRIndex        = (uint32)HDR.GetResourceID();
        PC.AdaptUAV        = (uint32)Adapted.GetMipUAVIndex(0);
        PC.DeltaTime       = Frame.SceneGlobalData.DeltaTime;
        PC.AdaptationSpeed = ActivePostProcess->AutoExposureSpeed;

        RHI::CmdDispatch(CL, MakeArgs(PC), 1, 1, 1);

        Barriers::ComputeToAll(CL);
    }

    void FDefaultSceneRenderer::ToneMappingPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Color Grading + Tone Map Pass", tracy::Color::Red2);

        const FFrameData& Frame = *RenderFrame;
        const auto& CachedWorldSettings        = Frame.CachedWorldSettings;
        const auto& ActivePostProcessMaterials = Frame.PostProcess.ActivePostProcessMaterials;
        const SPostProcessSettings* ActivePostProcess = Frame.PostProcess.bHasActivePostProcess ? &Frame.PostProcess.ActivePostProcessStorage : nullptr;
        const auto& SceneGlobalData            = Frame.SceneGlobalData;

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("ColorGrading.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const bool bSMAAEnabled = CachedWorldSettings.SMAAQuality != ESMAAQuality::Off;
        const bool bPPMaterials = !ActivePostProcessMaterials.empty();
        const FSceneImage& Output = (bSMAAEnabled || bPPMaterials) ? GetNamedImage(ENamedImage::LDR) : CurrentView->Output;

        const FSceneImage& HDRTex     = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& BloomTex   = CurrentView->BloomChainImage;
        const FSceneImage& AdaptedTex = GetNamedImage(ENamedImage::AdaptedLuminance);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Undefined;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        FColorGradingConstants Constants = BuildColorGradingConstants(ActivePostProcess, SceneGlobalData.Time);

        struct FComposePushConstants
        {
            uint64 ConstantsAddr;
            uint32 HDRIndex;
            uint32 BloomIndex;
            uint32 AdaptedLumIndex;
            uint32 _Pad;
        };
        static_assert(sizeof(FComposePushConstants) == 24, "FComposePushConstants must match the slang pass block.");

        FComposePushConstants PC = {};
        PC.ConstantsAddr   = RHI::Core::CopyTransient(Constants);
        PC.HDRIndex        = (uint32)HDRTex.GetResourceID();
        PC.BloomIndex      = (uint32)BloomTex.GetResourceID();
        PC.AdaptedLumIndex = (uint32)AdaptedTex.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    namespace
    {
        struct FPostProcessMaterialPushConstants
        {
            uint32 MaterialIndex;
            uint32 SceneColorIndex;   // bindless SRV: ping-pong source
            uint32 SceneDepthIndex;   // bindless SRV: opaque depth
            uint32 HDRIndex;          // bindless SRV: pre-tone-map HDR
        };
        static_assert(sizeof(FPostProcessMaterialPushConstants) == 16,
            "FPostProcessMaterialPushConstants must match the slang push block.");
    }

    void FDefaultSceneRenderer::PostProcessMaterialPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& CachedWorldSettings        = Frame.CachedWorldSettings;
        const auto& ActivePostProcessMaterials = Frame.PostProcess.ActivePostProcessMaterials;

        if (ActivePostProcessMaterials.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Post Process Material Pass", tracy::Color::Magenta);

        const FSceneImage& DepthTex = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& HDRTex   = GetNamedImage(ENamedImage::HDR);

        const bool bSMAAEnabled = CachedWorldSettings.SMAAQuality != ESMAAQuality::Off;

        const FSceneImage* Source = &GetNamedImage(ENamedImage::LDR);
        const FSceneImage* Dest   = &GetNamedImage(ENamedImage::PostProcessScratch);

        for (const FFrameData::FPostProcessMaterial& PPMaterial : ActivePostProcessMaterials)
        {
            // Resolved + ref-held at extract; the render phase never touches the CMaterial.
            FShaderH VS = PPMaterial.Shaders.VertexShader;
            FShaderH PS = PPMaterial.Shaders.PixelShader;
            if (VS == nullptr || PS == nullptr)
            {
                continue;
            }

            RHI::FRenderAttachment Color;
            Color.Texture = Dest->Texture;
            Color.LoadOp  = RHI::ELoadOp::Undefined;
            Color.StoreOp = RHI::EStoreOp::Store;

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            Pass.RenderArea       = Dest->GetExtent();

            RHI::CmdBeginRenderPass(CL, Pass);
            SetViewportScissor(CL, Dest->GetExtent());
            RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

            FGraphicsPipelineKey Key;
            Key.VS = VS;
            Key.PS = PS;
            Key.ColorTargets.push_back({ Dest->Desc.Format, {} });
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FPostProcessMaterialPushConstants PC = {};
            PC.MaterialIndex    = PPMaterial.MaterialIndex;
            PC.SceneColorIndex  = (uint32)Source->GetResourceID();
            PC.SceneDepthIndex  = (uint32)DepthTex.GetResourceID();
            PC.HDRIndex         = (uint32)HDRTex.GetResourceID();

            RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);

            eastl::swap(Source, Dest);
        }

        const FSceneImage& LDR = GetNamedImage(ENamedImage::LDR);
        if (bSMAAEnabled)
        {
            if (Source->Texture.Handle != LDR.Texture.Handle)
            {
                Barriers::AllToTransfer(CL);
                RHI::CmdCopyTexture(CL, Source->Texture, RHI::FTextureSlice{}, LDR.Texture, RHI::FTextureSlice{});
                Barriers::TransferToAll(CL);
            }
        }
        else
        {
            Barriers::AllToTransfer(CL);
            RHI::CmdCopyTexture(CL, Source->Texture, RHI::FTextureSlice{}, CurrentView->Output.Texture, RHI::FTextureSlice{});
            Barriers::TransferToAll(CL);
        }
    }

    struct FSMAAPushConstants
    {
        FVector4 RTMetrics;  // x = 1/w, y = 1/h, z = w, w = h
        float     EdgeThreshold;
        float     DebugMode;
        uint32    TexIndex0;  // bindless SRV index of the pass's primary input
        uint32    TexIndex1;  // pass-specific extra input (0 if unused)
        uint32    TexIndex2;  // pass-specific extra input (0 if unused)
        uint32    _Pad0;
        uint32    _Pad1;
        uint32    _Pad2;
    };
    static_assert(sizeof(FSMAAPushConstants) == 48, "FSMAAPushConstants must match the slang push block.");

    static float GetSMAAEdgeThreshold(ESMAAQuality Quality)
    {
        switch (Quality)
        {
        case ESMAAQuality::Low:    return 0.15f;
        case ESMAAQuality::Medium: return 0.12f;
        case ESMAAQuality::High:   return 0.10f;
        case ESMAAQuality::Ultra:  return 0.05f;
        default:                   return 0.10f;
        }
    }

    static FSMAAPushConstants BuildSMAAPushConstants(const FSceneImage& Image, const SDefaultWorldSettings& Settings)
    {
        FSMAAPushConstants PC;
        const float W = (float)Image.GetSizeX();
        const float H = (float)Image.GetSizeY();
        PC.RTMetrics      = FVector4(1.0f / W, 1.0f / H, W, H);
        PC.EdgeThreshold  = GetSMAAEdgeThreshold(Settings.SMAAQuality);
        PC.DebugMode      = 0.0f;
        PC.TexIndex0 = 0; PC.TexIndex1 = 0; PC.TexIndex2 = 0;
        PC._Pad0 = 0; PC._Pad1 = 0; PC._Pad2 = 0;
        return PC;
    }

    void FDefaultSceneRenderer::SMAAEdgeDetectionPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("SMAA Edge Detection", tracy::Color::Red2);

        const FFrameData& Frame = *RenderFrame;
        const auto& CachedWorldSettings = Frame.CachedWorldSettings;

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SMAAEdgeDetection.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output     = GetNamedImage(ENamedImage::SMAAEdges);
        const FSceneImage& InputColor = GetNamedImage(ENamedImage::LDR);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Clear;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        FSMAAPushConstants PC = BuildSMAAPushConstants(Output, CachedWorldSettings);
        PC.TexIndex0 = (uint32)InputColor.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::SMAABlendWeightPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("SMAA Blend Weight", tracy::Color::Red2);

        const FFrameData& Frame = *RenderFrame;
        const auto& CachedWorldSettings = Frame.CachedWorldSettings;

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SMAABlendWeight.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output    = GetNamedImage(ENamedImage::SMAABlend);
        const FSceneImage& EdgesTex  = GetNamedImage(ENamedImage::SMAAEdges);
        const FSceneImage& AreaTex   = GetNamedImage(ENamedImage::SMAAArea);
        const FSceneImage& SearchTex = GetNamedImage(ENamedImage::SMAASearch);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Clear;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        FSMAAPushConstants PC = BuildSMAAPushConstants(Output, CachedWorldSettings);
        PC.TexIndex0 = (uint32)EdgesTex.GetResourceID();
        PC.TexIndex1 = (uint32)AreaTex.GetResourceID();
        PC.TexIndex2 = (uint32)SearchTex.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::SMAANeighborhoodBlendPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("SMAA Neighborhood Blend", tracy::Color::Red2);

        const FFrameData& Frame = *RenderFrame;
        const auto& CachedWorldSettings = Frame.CachedWorldSettings;

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("SMAANeighborhoodBlend.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output     = CurrentView->Output;
        const FSceneImage& InputColor = GetNamedImage(ENamedImage::LDR);
        const FSceneImage& BlendTex   = GetNamedImage(ENamedImage::SMAABlend);

        RHI::FRenderAttachment Color;
        Color.Texture = Output.Texture;
        Color.LoadOp  = RHI::ELoadOp::Undefined;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Output.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Output.GetExtent());
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ Output.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        FSMAAPushConstants PC = BuildSMAAPushConstants(Output, CachedWorldSettings);
        PC.TexIndex0 = (uint32)InputColor.GetResourceID();
        PC.TexIndex1 = (uint32)BlendTex.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
    
    void FDefaultSceneRenderer::InitBuffers()
    {

        // GPU pre-skinning output: written by Skinning.slang, read by every draw VS via BDA.
        PreSkinnedVerticesBuffer = CreateSceneBuffer(sizeof(FPreSkinnedVertex) * 64 * 1024, "Cull.PreSkinnedVertices");

        for (uint32 Slot = 0; Slot < RHI::kFramesInFlight; ++Slot)
        {
            MeshletDrawListRing[Slot] = CreateSceneBuffer(sizeof(uint32) * 2, "Cull.MeshletDrawList");

            // Per-(view, draw) cull layout. Sized for real in CompileDrawCommands_Render.
            RenderBucketRing[Slot] = CreateSceneBuffer(sizeof(FRenderBucketGPU), "Cull.RenderBuckets");

            SpdCounterRing[Slot] = CreateSceneBuffer(sizeof(uint32), "Cull.SpdCounter");

            MeshletBlockRing[Slot] = CreateSceneBuffer(sizeof(uint32) * 2, "Cull.MeshletBlocks");
            // Fixed size: one grid, rewritten by BuildDrawPrefix every frame.
            BlockDispatchArgsRing[Slot] = CreateSceneBuffer(sizeof(RHI::FDispatchIndirectArguments), "Cull.BlockDispatchArgs");
            SkinDispatchArgsRing[Slot]  = CreateSceneBuffer(sizeof(RHI::FDispatchIndirectArguments), "Cull.SkinDispatchArgs");
            SkinWorkBaseRing[Slot]      = CreateSceneBuffer(sizeof(uint32) * 2, "Cull.SkinWorkBase");
            MeshletCullDispatchArgsRing[Slot] = CreateSceneBuffer(sizeof(RHI::FDispatchIndirectArguments), "Cull.MeshletCullDispatchArgs");

            TotalsRing[Slot] = CreateSceneBuffer(sizeof(uint32) * kTotalsSlots, "Cull.Totals");
            TotalsZeroed[Slot] = false;

            // GPU-driven scene per-frame outputs. Sized for real in CompileDrawCommands_Render.
            VisibleInstanceRing[Slot]       = CreateSceneBuffer(sizeof(FGPUInstance), "Cull.VisibleInstances");
            CullCounterRing[Slot]           = CreateSceneBuffer(sizeof(uint32) * 4, "Cull.CullCounters");

            if (MeshletBoundReadback[Slot] == 0)
            {
                MeshletBoundReadback[Slot] = RHI::Malloc(sizeof(uint32) * kTotalsSlots,
                                                         RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
                RHI::SetDebugName(MeshletBoundReadback[Slot], "Readback.MeshletBounds");

                if (void* Host = RHI::ToHost(MeshletBoundReadback[Slot]))
                {
                    Memory::Memzero(Host, sizeof(uint32) * kTotalsSlots);
                }
            }
        }
    }

    void FDefaultSceneRenderer::UpdateMeshletBoundFeedback(uint8 Slot)
    {
        const RHI::GPUPtr Readback = MeshletBoundReadback[Slot];
        if (Readback == 0)
        {
            return;
        }
        static_assert(FDefaultSceneRenderer::kTotalsSlots >= 8, "Totals[7] is read below.");
        if (const uint32* Mapped = static_cast<const uint32*>(RHI::ToHost(Readback)))
        {
            LastVisibleInstances     = Mapped[0];
            LastVisibleOverflowed    = Mapped[1];
            LastDrawListRequired     = Mapped[2];
            LastDrawListOverflowed   = Mapped[3];
            LastBlocksRequested      = Mapped[4];
            LastBlocksOverflowed     = Mapped[5];
            LastPreSkinRequested     = Mapped[6];
            LastPreSkinOverflowed    = Mapped[7];
        }
    }

    void FDefaultSceneRenderer::DispatchGPUSceneCull(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        static const FShaderH CullInstancesShader = FShaderLibrary::Get("CullInstances.slang");
        static const FShaderH DrawPrefixShader = FShaderLibrary::Get("BuildDrawPrefix.slang");
        if (CullInstancesShader == nullptr || DrawPrefixShader == nullptr)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("GPU Scene Cull", tracy::Color::Magenta);
        SCENE_GPU_SCOPE(CL, "GPU Scene Cull");

        const FFrameData::FGeometry::FRetainedUpload& Upload = Frame.Geometry.RetainedUpload;
        const FInstanceCullEntry* SrcCullEntries = ScenePrimitives.GetRetainedCullEntries();
        const FTransform3x4*      SrcTransforms  = ScenePrimitives.GetRetainedTransforms();
        const FInstanceStatic*    SrcStatic      = ScenePrimitives.GetRetainedStatic();

        const uint8  Slot          = CurrentFrameSlot;
        const uint32 RetainedSlots = Upload.SlotCount;
        const uint32 NumBatches    = Math::Max(Frame.Views.NumDrawsPerView, 1u);
        const uint32 NumCullViews  = (uint32)Frame.Views.CullViews.size();

        if (!TotalsZeroed[Slot] && GetTotals())
        {
            RHI::CmdMemset(CL, GetTotals().Ptr, GetTotals().GetSize(), 0u);
            Barriers::TransferToAll(CL);
            TotalsZeroed[Slot] = true;
        }

        {
            LUMINA_PROFILE_SECTION_COLORED("Retained Upload", tracy::Color::Magenta4);
            SCENE_GPU_SCOPE(CL, "Retained Upload");

            const SIZE_T CullBytes      = Math::Max<SIZE_T>(sizeof(FInstanceCullEntry), (SIZE_T)RetainedSlots * sizeof(FInstanceCullEntry));
            const SIZE_T TransformBytes = Math::Max<SIZE_T>(sizeof(FTransform3x4),      (SIZE_T)RetainedSlots * sizeof(FTransform3x4));
            const SIZE_T StaticBytes    = Math::Max<SIZE_T>(sizeof(FInstanceStatic),    (SIZE_T)RetainedSlots * sizeof(FInstanceStatic));

            ResizeBufferIfNeeded(CL, RetainedCullEntryBuffer, CullBytes,      1.5f, RetainedCullEntryLowUsage, Upload.bFull,
                                 EBufferInit::Zeroed, "Retained.CullEntries");
            ResizeBufferIfNeeded(CL, RetainedTransformBuffer, TransformBytes, 1.5f, RetainedTransformLowUsage, Upload.bFull,
                                 EBufferInit::Zeroed, "Retained.Transforms");
            ResizeBufferIfNeeded(CL, RetainedStaticBuffer,    StaticBytes,    1.5f, RetainedStaticLowUsage,    Upload.bFull,
                                 EBufferInit::Zeroed, "Retained.Static");

            // Two-phase occlusion state, keyed by retained slot and therefore sized with these. A grow
            // reallocates, so the contents are undefined until the pre-late clear runs -- harmless, since
            // early and late partition the visible set whatever the flags say, but zeroed anyway so a
            // resize frame is not a different frame.
            const SIZE_T VisBytes = Math::Max<SIZE_T>(sizeof(uint32), (SIZE_T)RetainedSlots * sizeof(uint32));
            const SIZE_T VisBefore = InstanceVisibilityBuffer ? InstanceVisibilityBuffer.GetSize() : 0;
            ResizeBufferIfNeeded(CL, InstanceVisibilityBuffer, VisBytes, 1.5f, InstanceVisibilityLowUsage,
                                 true, EBufferInit::Zeroed, "Retained.InstanceVisibility");

            InstanceVisibilityCapacity = InstanceVisibilityBuffer
                                       ? (uint32)Math::Min<uint64>(InstanceVisibilityBuffer.GetSize() / sizeof(uint32), 0xFFFFFFFFull)
                                       : 0u;

            if (InstanceVisibilityBuffer && InstanceVisibilityBuffer.GetSize() != VisBefore)
            {
                RHI::CmdMemset(CL, InstanceVisibilityBuffer.GetAddress(), InstanceVisibilityBuffer.GetSize(), 0u);
            }

            if (RetainedCullEntryBuffer && RetainedTransformBuffer && RetainedStaticBuffer && RetainedSlots > 0)
            {
                if (Upload.bFull)
                {
                    LUMINA_PROFILE_SECTION_COLORED("FULL resend", tracy::Color::Red3);

                    WriteBuffer(CL, RetainedCullEntryBuffer.GetAddress(),
                                SrcCullEntries, (SIZE_T)RetainedSlots * sizeof(FInstanceCullEntry));
                    WriteBuffer(CL, RetainedTransformBuffer.GetAddress(),
                                SrcTransforms, (SIZE_T)RetainedSlots * sizeof(FTransform3x4));
                    WriteBuffer(CL, RetainedStaticBuffer.GetAddress(),
                                SrcStatic, (SIZE_T)RetainedSlots * sizeof(FInstanceStatic));
                }
                else
                {
                    auto WriteRuns = [&](const TVector<uint32>& Slots, auto&& Emit)
                    {
                        const SIZE_T NumDirty = Slots.size();
                        for (SIZE_T i = 0; i < NumDirty; )
                        {
                            SIZE_T j = i + 1;
                            while (j < NumDirty && Slots[j] == Slots[j - 1] + 1u)
                            {
                                ++j;
                            }
                            // A run is contiguous slots, so it is contiguous in the source arrays too.
                            Emit(Slots[i], j - i);
                            i = j;
                        }
                    };

                    WriteRuns(Upload.DirtySlots, [&](uint32 First, SIZE_T Run)
                    {
                        WriteBuffer(CL, RetainedCullEntryBuffer.GetAddress() + (uint64)First * sizeof(FInstanceCullEntry),
                                    &SrcCullEntries[First], Run * sizeof(FInstanceCullEntry));
                        WriteBuffer(CL, RetainedTransformBuffer.GetAddress() + (uint64)First * sizeof(FTransform3x4),
                                    &SrcTransforms[First], Run * sizeof(FTransform3x4));
                    });

                    WriteRuns(Upload.DirtyStaticSlots, [&](uint32 First, SIZE_T Run)
                    {
                        WriteBuffer(CL, RetainedStaticBuffer.GetAddress() + (uint64)First * sizeof(FInstanceStatic),
                                    &SrcStatic[First], Run * sizeof(FInstanceStatic));
                    });
                }
            }
            const uint32 CullCap      = (uint32)Math::Min<uint64>(RetainedCullEntryBuffer.GetSize() / sizeof(FInstanceCullEntry), 0xFFFFFFFFull);
            const uint32 TransformCap = (uint32)Math::Min<uint64>(RetainedTransformBuffer.GetSize() / sizeof(FTransform3x4), 0xFFFFFFFFull);
            const uint32 StaticCap    = (uint32)Math::Min<uint64>(RetainedStaticBuffer.GetSize() / sizeof(FInstanceStatic), 0xFFFFFFFFull);
            RetainedDeviceCapacity.store(Math::Min(CullCap, Math::Min(TransformCap, StaticCap)), std::memory_order_release);
        }

        // Surface descriptors: interned, so this moves only when a new distinct LOD table appears.
        const uint32 NumDescs = Upload.SurfaceDescCount;
        {
            const SIZE_T DescBytes = Math::Max<SIZE_T>(sizeof(FSurfaceDescGPU), (SIZE_T)NumDescs * sizeof(FSurfaceDescGPU));
            const RHI::GPUPtr PrevDescs = SurfaceDescBuffer.Ptr;
            // Same reasoning as above: a reclaim would drop descriptors this frame may not be re-sending.
            ResizeBufferIfNeeded(CL, SurfaceDescBuffer, DescBytes, 1.5f, SurfaceDescLowUsage, Upload.bSurfaceDescsChanged,
                                 EBufferInit::Zeroed, "Retained.SurfaceDescs");

            if (SurfaceDescBuffer.Ptr != PrevDescs)
            {
                UploadedSurfaceDescs = 0;
            }

            const bool bNeedsDescWrite = SurfaceDescBuffer && NumDescs > 0
                                      && (Upload.bSurfaceDescsChanged || UploadedSurfaceDescs != NumDescs);
            if (bNeedsDescWrite)
            {
                if (ScenePrimitives.GetSurfaceDescCount() == NumDescs)
                {
                    WriteBuffer(CL, SurfaceDescBuffer.GetAddress(),
                                ScenePrimitives.GetSurfaceDescs(), (SIZE_T)NumDescs * sizeof(FSurfaceDescGPU));
                    UploadedSurfaceDescs = NumDescs;
                }
                else
                {
                    RetainedDeviceCapacity.store(0, std::memory_order_release);
                }
            }
        }

        const uint32 VisibleCapacity = FrameVisibleInstanceCapacity;
        const uint32 SeedViews       = Math::Max(NumCullViews, 1u);
        const SIZE_T ViewDrawEntries = (SIZE_T)SeedViews * (SIZE_T)NumBatches;
        // MeshDrawArgsRing is deliberately NOT resized here. It is sized in CompileDrawCommands_Render to
        // NumArgSlots * kMeshletSliceCount * MeshSubDrawsPerSlice, which is what BuildMeshletCullArgs' post
        // pass writes; a second request for ViewDrawEntries alone is 3 * MeshSubDrawsPerSlice too SMALL and
        // fed that ratio straight into the shared low-usage counter.
        ResizeBufferIfNeeded(CL, RenderBucketRing[Slot],
                             ViewDrawEntries * sizeof(FRenderBucketGPU), 1.5f, RenderBucketRingLowUsage[Slot],
                             true, EBufferInit::Zeroed, "Cull.RenderBuckets");

        {
            // Starts at ZERO: there is no CPU-fed head to append past any more, so CullInstances claims
            // every visible slot and therefore writes a range for every pair BuildMeshletBlocks reads.
            // {instance cursor, instance overflow, pre-skin vertex cursor, pad}. The pre-skin cursor runs
            // past capacity by design, so what BuildDrawPrefix reads back off it is demand, not what fit.
            const uint32 Counters[4] = { 0u, 0u, 0u, 0u };
            WriteBuffer(CL, GetCullCounters().GetAddress(), Counters, sizeof(Counters));

            // The whole bucket array in one write: capacities carry the CPU's skinned-batch seeds and every
            // Every field starts at zero. The CPU used to seed DrawCapacity/BlockCapacity here with the
            // skinned batches' contribution, because CullInstances skipped skinned instances and so never
            // accumulated it. It accumulates them like anything else now, so a seed would double-count.
            BucketSeedScratch.assign(ViewDrawEntries, FRenderBucketGPU{});
            WriteBuffer(CL, GetRenderBuckets().GetAddress(),
                        BucketSeedScratch.data(), BucketSeedScratch.size() * sizeof(FRenderBucketGPU));
        }

        Barriers::TransferToCompute(CL);

        //~ Cull + LOD + compaction.
        if (RetainedSlots > 0 && NumCullViews > 0 && NumDescs > 0 && VisibleInstanceRing[Slot])
        {
            LUMINA_PROFILE_SECTION_COLORED("Cull Instances", tracy::Color::Magenta2);
            SCENE_GPU_SCOPE(CL, "Cull Instances");

            struct FCullInstancesPC
            {
                uint32 NumRetained;
                uint32 NumViews;
                uint32 NumBatches;
                uint32 bUseLODs;
                uint32 MaxVisibleInstances;
                uint32 NumSurfaceDescs;
                uint32 SkinFrameTag;
                uint32 PreSkinCapacity;
                uint64 RetainedCullEntriesAddr;
                uint64 RetainedTransformsAddr;
                uint64 RetainedStaticAddr;
                uint64 SurfaceDescsAddr;
                uint64 OutInstancesAddr;
                uint64 OutInstanceCountAddr;
                uint64 OutInstanceViewRangesAddr;
                uint64 OutBucketsAddr;
                uint64 OutOverflowFlagAddr;
                uint64 SkinnedFrameDataAddr;
                uint64 OutPreSkinCursorAddr;
            };
            static_assert(sizeof(FCullInstancesPC) == 120, "FCullInstancesPC must match CullInstances.slang.");

            FCullInstancesPC PC = {};
            PC.NumRetained              = RetainedSlots;
            PC.NumViews                 = NumCullViews;
            PC.NumBatches               = NumBatches;
            PC.bUseLODs                 = RenderSettings.bUseLODs ? 1u : 0u;
            PC.MaxVisibleInstances      = VisibleCapacity;
            PC.NumSurfaceDescs          = UploadedSurfaceDescs;
            PC.SkinFrameTag             = CurrentSkinnedFrameTag;
            PC.PreSkinCapacity          = PreSkinnedVertexCapacity;
            PC.RetainedCullEntriesAddr  = RetainedCullEntryBuffer.GetAddress();
            PC.RetainedTransformsAddr   = RetainedTransformBuffer.GetAddress();
            PC.RetainedStaticAddr       = RetainedStaticBuffer.GetAddress();
            PC.SurfaceDescsAddr         = SurfaceDescBuffer.GetAddress();
            PC.OutInstancesAddr         = VisibleInstanceRing[Slot].GetAddress();
            PC.OutInstanceCountAddr     = GetCullCounters().GetAddress();
            PC.OutInstanceViewRangesAddr = GetInstanceViewRanges().GetAddress();
            PC.OutBucketsAddr           = GetRenderBuckets().GetAddress();
            PC.OutOverflowFlagAddr      = GetCullCounters().GetAddress() + sizeof(uint32);
            PC.SkinnedFrameDataAddr     = SkinnedFrameDataBuffer.GetAddress();
            PC.OutPreSkinCursorAddr     = GetCullCounters().GetAddress() + sizeof(uint32) * 2;

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullInstancesShader));
            // CullInstances.slang undoes the fold with GroupID.y * MAX_DISPATCH_AXIS * LOCAL_SIZE_X.
            const FUIntVector2 Grid = RenderUtils::FoldGroupCount(RenderUtils::GetGroupCount(RetainedSlots, 64u));
            RHI::CmdDispatch(CL, MakeArgs(PC), Grid.x, Grid.y, 1u);

            Barriers::ComputeToGeometry(CL);
        }

        //~ Draw-argument layout, from counts the CPU never sees.
        {
            LUMINA_PROFILE_SECTION_COLORED("Build Draw Prefix", tracy::Color::Magenta3);
            SCENE_GPU_SCOPE(CL, "Build Draw Prefix");

            struct FBuildDrawPrefixPC
            {
                uint32 NumViews;
                uint32 NumDraws;
                uint32 MaxVisibleInstances;
                uint32 DrawListCapacityArg;
                uint32 BlockListCapacityArg;
                uint32 PreSkinCapacityArg;
                uint64 BucketsAddr;
                uint64 InstanceCountAddr;
                uint64 OutTotalsAddr;
                uint64 OutBlockDispatchArgsAddr;
            };
            static_assert(sizeof(FBuildDrawPrefixPC) == 56, "FBuildDrawPrefixPC must match BuildDrawPrefix.slang.");

            FBuildDrawPrefixPC PC = {};
            PC.NumViews                 = SeedViews;
            PC.NumDraws                 = NumBatches;
            PC.MaxVisibleInstances      = VisibleCapacity;
            PC.DrawListCapacityArg      = DrawListCapacity;
            PC.BlockListCapacityArg     = BlockListCapacity;
            PC.PreSkinCapacityArg       = PreSkinnedVertexCapacity;
            PC.BucketsAddr              = GetRenderBuckets().GetAddress();
            PC.InstanceCountAddr        = GetCullCounters().GetAddress();
            PC.OutTotalsAddr            = GetTotals().GetAddress();
            PC.OutBlockDispatchArgsAddr = GetBlockDispatchArgs().GetAddress();

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(DrawPrefixShader));
            RHI::CmdDispatch(CL, MakeArgs(PC), 1u, 1u, 1u);

            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute,
                RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader |
                RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::Transfer);
        }

        {
            static const FShaderH BlocksShader = FShaderLibrary::Get("BuildMeshletBlocks.slang");
            if (BlocksShader && NumCullViews > 0u)
            {
                LUMINA_PROFILE_SECTION_COLORED("Build Meshlet Blocks", tracy::Color::Magenta2);
                SCENE_GPU_SCOPE(CL, "Build Meshlet Blocks");

                struct FBuildMeshletBlocksPC
                {
                    uint32 NumViews;
                    uint32 NumBatches;
                    uint32 MaxVisibleInstances;
                    uint32 _Pad0;
                    uint64 InstanceCountAddr;
                    uint64 InstanceViewRangesAddr;
                    uint64 BucketsAddr;
                    uint64 OutBlockListAddr;
                } BPC = {};
                static_assert(sizeof(FBuildMeshletBlocksPC) == 48, "FBuildMeshletBlocksPC must match BuildMeshletBlocks.slang.");

                BPC.NumViews             = NumCullViews;
                BPC.NumBatches           = NumBatches;
                BPC.MaxVisibleInstances  = VisibleCapacity;
                BPC.InstanceCountAddr    = GetCullCounters().GetAddress();
                BPC.InstanceViewRangesAddr = GetInstanceViewRanges().GetAddress();
                BPC.BucketsAddr          = GetRenderBuckets().GetAddress();
                BPC.OutBlockListAddr     = GetMeshletBlocks().GetAddress();

                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BlocksShader));

                RHI::CmdDispatchIndirect(CL, MakeArgs(BPC), GetBlockDispatchArgs().Ptr, 0u);

                // The block list has exactly one reader, and it is compute now: MeshletCullPass below.
                RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute);
            }
        }

        // Every view's meshlets, culled once. Everything that rasterizes afterwards reads what this
        // wrote rather than culling again, which is the whole point of it being a pass.
        MeshletCullPass(CL, EMeshletSlice::Early);

        if (MeshletBoundReadback[Slot] != 0)
        {
            RHI::CmdMemcpy(CL, MeshletBoundReadback[Slot], GetTotals().GetAddress(), sizeof(uint32) * kTotalsSlots);
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

    void FDefaultSceneRenderer::AllocateMSAAImages(FSceneView& View, const FUIntVector2& Extent)
    {
        if (MSAASampleCount <= 1)
        {
            return;
        }

        // MS scratch RTs are attachment-only (resolved into the 1x images); no heap slots.
        RHI::FTextureDesc Desc;
        Desc.Type        = RHI::ETextureType::Tex2D;
        Desc.Dimension   = FUIntVector3(Extent.x, Extent.y, 1);
        Desc.SampleCount = MSAASampleCount;

        Desc.Format = EFormat::RGBA16_FLOAT;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment;
        View.Images[(int)ENamedImage::HDR_MS] = CreateSceneImage(Desc, /*bSampled*/ false);

        Desc.Format = EFormat::D32;
        Desc.Usage  = RHI::EImageUsageFlags::DepthAttachment;
        View.Images[(int)ENamedImage::Depth_MS] = CreateSceneImage(Desc, /*bSampled*/ false);

        #if USING(WITH_EDITOR)
        Desc.Format = EFormat::R32_UINT;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment;
        View.Images[(int)ENamedImage::Picker_MS] = CreateSceneImage(Desc, /*bSampled*/ false);
        #endif

        // These arrive after InitViewImages has already named the rest of the array.
        NameOwnedImages(View.Images);
    }

    void FDefaultSceneRenderer::SyncMSAAState()
    {
        if (RenderFrame == nullptr)
        {
            return;
        }
        const uint8 Desired = ResolveVisBufferSampleCount(RenderFrame->CachedWorldSettings.MSAASampleCount);

        if (Desired == MSAASampleCount)
        {
            return;
        }

        MSAASampleCount = Desired;

        for (FSceneView& View : SceneViews)
        {
            DeferRelease(View.Images[(int)ENamedImage::HDR_MS]);
            DeferRelease(View.Images[(int)ENamedImage::Depth_MS]);
            DeferRelease(View.Images[(int)ENamedImage::Picker_MS]);

            if (MSAASampleCount > 1)
            {
                AllocateMSAAImages(View, View.Size);
            }
        }
    }

    void FDefaultSceneRenderer::ReleaseViewImages(FSceneView& View, bool bDeferRelease)
    {
        auto Release = [this, bDeferRelease](FSceneImage& Image)
        {
            if (bDeferRelease)
            {
                DeferRelease(Image);
            }
            else
            {
                ReleaseSceneImage(Image);
            }
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

        // Shared aliases: just drop the copies, the owners release them.
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
        case ENamedImage::HDR_MS:             return "Scene.HDR_MS";
        case ENamedImage::Depth_MS:           return "Scene.Depth_MS";
        case ENamedImage::Picker_MS:          return "Scene.Picker_MS";
        case ENamedImage::BRDFLut:            return "Scene.BRDFLut";
        case ENamedImage::SkyCube:            return "Scene.SkyCube";
        case ENamedImage::SkyIrradiance:      return "Scene.SkyIrradiance";
        case ENamedImage::SkyPrefilter:       return "Scene.SkyPrefilter";
        case ENamedImage::ProbeCaptureCube:   return "Scene.ProbeCaptureCube";
        case ENamedImage::ProbePrefiltered:   return "Scene.ProbePrefiltered";

        #if USING(WITH_EDITOR)
        case ENamedImage::PointLightIcon:       return "Scene.PointLightIcon";
        case ENamedImage::DirectionalLightIcon: return "Scene.DirectionalLightIcon";
        case ENamedImage::SkyLightIcon:         return "Scene.SkyLightIcon";
        case ENamedImage::SpotLightIcon:        return "Scene.SpotLightIcon";
        case ENamedImage::CameraIcon:           return "Scene.CameraIcon";
        case ENamedImage::CharacterIcon:        return "Scene.CharacterIcon";
        case ENamedImage::ParticleSystemIcon:   return "Scene.ParticleSystemIcon";
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

        // Additively blended and fp32: the reconstruction inverts a Hankel matrix that is ill-conditioned
        // enough that fp16 needs the paper's quantization matrices to stay stable.
        case ENamedImage::MomentZeroth:
            OutDesc.Format = EFormat::R32_FLOAT;
            return true;

        case ENamedImage::Moments:
            OutDesc.Format = EFormat::RGBA32_FLOAT;
            return true;

        // Never rendered into -- WaterPass copies HDR here and samples it, so it is a transfer
        // destination rather than an attachment.
        case ENamedImage::WaterRefraction:
            OutDesc.Format = EFormat::RGBA16_FLOAT;
            OutDesc.Usage  = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;
            return true;

        case ENamedImage::DBufferA:
        case ENamedImage::DBufferB:
        case ENamedImage::DBufferC:
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

        // Exactly the conditions the consuming passes early-out on, so a target exists precisely when a
        // pass is going to touch it. Drifting from those tests is what would produce a null attachment.
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
        Want(ENamedImage::WaterRefraction, bWater);
        Want(ENamedImage::DBufferA,        bDecals);
        Want(ENamedImage::DBufferB,        bDecals);
        Want(ENamedImage::DBufferC,        bDecals);

        ReleaseIdleOptionalImages(View);
    }

    void FDefaultSceneRenderer::ReleaseIdleOptionalImages(FSceneView& View)
    {
        // Long enough that walking in and out of a room with glass in it does not reallocate every few
        // seconds, short enough that a level with no translucency stops paying for it.
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
                // Frame-deferred: a frame already recorded against this image is still in flight.
                DeferRelease(Slot);
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

        // HDR scene color; copy source for the water/underwater refraction snapshot, and a storage image
        // because the deferred lighting pass writes it from compute.
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

        Desc.Format    = EFormat::R8_UNORM;
        Desc.Dimension = FUIntVector3(Math::Max(Extent.x / 2, 1u), Math::Max(Extent.y / 2, 1u), 1);
        View.Images[(int)ENamedImage::GTAO]        = CreateSceneImage(Desc);
        View.Images[(int)ENamedImage::GTAODenoise] = CreateSceneImage(Desc);
        Desc.Dimension = FUIntVector3(Extent.x, Extent.y, 1);
        View.Images[(int)ENamedImage::GTAOBlur]    = CreateSceneImage(Desc);

        View.Images[(int)ENamedImage::ShadowMask]  = CreateSceneImage(Desc);

        // Scene depth; transfer-dst for the no-occluder clear.
        Desc.Format = EFormat::D32;
        Desc.Usage  = RHI::EImageUsageFlags::DepthAttachment | RHI::EImageUsageFlags::Sampled |
                      RHI::EImageUsageFlags::TransferDst;
        View.Images[(int)ENamedImage::DepthAttachment] = CreateSceneImage(Desc);

        {
            const uint32 Width  = PreviousPow2(Extent.x);
            const uint32 Height = PreviousPow2(Extent.y);

            // R16_FLOAT HZB: reverse-Z [0,1], min-reduced; quantization error is conservative.
            RHI::FTextureDesc PyramidDesc;
            PyramidDesc.Type      = RHI::ETextureType::Tex2D;
            PyramidDesc.Dimension = FUIntVector3(Width, Height, 1);
            PyramidDesc.Format    = EFormat::R16_FLOAT;
            PyramidDesc.MipCount  = RenderUtils::CalculateMipCount(Width, Height);
            PyramidDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.Images[(int)ENamedImage::DepthPyramid] = CreateSceneImage(PyramidDesc, true, /*bMipUAVs*/ true);
        }

        // Entity picker; copy source for the click readback. Editor-only: a packaged game never picks.
        #if USING(WITH_EDITOR)
        Desc.Format = EFormat::R32_UINT;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferSrc;
        View.Images[(int)ENamedImage::Picker] = CreateSceneImage(Desc);
        #endif

        Desc.Format = EFormat::R32_UINT;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled;
        View.Images[(int)ENamedImage::VisBuffer] = CreateSceneImage(Desc);

        // Written from compute, never a render target, so Storage + Sampled only; channel assignment lives
        // in Includes/GBuffer.slang. Never cleared -- lighting walks the same compacted pixel list.
        Desc.Usage  = RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::Sampled;
        Desc.Format = EFormat::RGBA8_UNORM;
        View.Images[(int)ENamedImage::GBufferA] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);
        View.Images[(int)ENamedImage::GBufferC] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);
        Desc.Format = EFormat::RGBA16_FLOAT;
        View.Images[(int)ENamedImage::GBufferB] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);
        Desc.Format = EFormat::R11G11B10_FLOAT;
        View.Images[(int)ENamedImage::GBufferD] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);

        AllocateMSAAImages(View, Extent);

        // The MBOIT, water and decal targets used to be created here. They are ~230 MB at 1080p for
        // features a scene may contain none of, so they now arrive through EnsureOptionalViewImages on
        // the first frame that actually draws one. A resize lands here with them absent, which is the
        // correct state -- the next frame that needs one rebuilds it at the new extent.

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
            const uint32 BloomW = eastl::max<uint32>(Extent.x / 2u, 1u);
            const uint32 BloomH = eastl::max<uint32>(Extent.y / 2u, 1u);

            RHI::FTextureDesc BloomDesc;
            BloomDesc.Type      = RHI::ETextureType::Tex2D;
            BloomDesc.Dimension = FUIntVector3(BloomW, BloomH, 1);
            BloomDesc.Format    = EFormat::R11G11B10_FLOAT;
            BloomDesc.MipCount  = Math::Min(BLOOM_MIP_COUNT, RenderUtils::CalculateMipCount(BloomW, BloomH));
            BloomDesc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            View.BloomChainImage = CreateSceneImage(BloomDesc, true, true);
        }

        {
            // Auto-exposure adapted luminance: 1x1 persistent R32F carrying eye-adaptation across frames.
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
        RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
        RHI::CmdSetPipeline(CL, Pipeline);

        struct FBRDFArgs { uint32 OutUAV; uint32 Width; uint32 Height; uint32 _Pad0; };
        const FBRDFArgs Args{ Shared.BRDFLutUAV, BRDFLutSize, BRDFLutSize, 0 };
        const RHI::GPUPtr ArgsPtr = RHI::Core::CopyTransient(FRootConstants{ 0, RHI::Core::CopyTransient(Args) });

        constexpr uint32 BRDFLutTile = 8u;
        const uint32 Groups = RenderUtils::GetGroupCount(BRDFLutSize, BRDFLutTile);
        RHI::CmdDispatch(CL, ArgsPtr, Groups, Groups, 1);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::AllCommands);

        RHI::Submit(CL);
        RHI::WaitDeviceIdle();
        RHI::ResetCommandList(CL);
        RHI::FreeH(Pipeline);
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
        Barriers::AllToTransfer(CL);
        RHI::CmdClearTexture(CL, NamedImages[(int)ENamedImage::ProbePrefiltered].Texture, Black);
        Barriers::TransferToAll(CL);
        RHI::SubmitAndWait(CL);

        NameOwnedImages(NamedImages);
    }

    void FDefaultSceneRenderer::SyncProbeCaptureCube(uint32 FaceSize)
    {
        if (FaceSize == 0 || FaceSize == ProbeCaptureCubeSize)
        {
            return;
        }

        RHI::WaitDeviceIdle();
        ReleaseSceneImage(NamedImages[(int)ENamedImage::ProbeCaptureCube]);

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

        RHI::WaitDeviceIdle();

        ReleaseSceneImage(NamedImages[(int)ENamedImage::SkyCube]);
        ReleaseSceneImage(NamedImages[(int)ENamedImage::SkyIrradiance]);
        ReleaseSceneImage(NamedImages[(int)ENamedImage::SkyPrefilter]);

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

        ReleaseViewImages(Primary, /*bDeferRelease*/ true);
        InitViewImages(Primary, OutputSlot);
    }

    void FDefaultSceneRenderer::EnsureStreamingFeedbackBuffer()
    {
        // One uint per MATERIAL slot, not per texture: the lanes report once per pixel against the
        // material they are shading, and the streamer expands that to textures through the material
        // manager's uniform mirror. That makes this ~8x smaller than the per-heap-slot buffer it replaced,
        // and its per-frame copy and clear correspondingly cheaper.
        if (StreamingFeedbackBuffer)
        {
            return;
        }

        // Fixed rather than tracking the material table's capacity, so the address never moves under an
        // in-flight readback. The shader's range check drops anything past it, and a scene with more live
        // materials than this has bigger problems than one texture staying at its inline tail.
        StreamingFeedbackSlots = 4096;
        const uint64 Bytes     = (uint64)StreamingFeedbackSlots * sizeof(uint32);

        StreamingFeedbackBuffer = CreateSceneBuffer(Bytes, "Streaming.Feedback");
        for (uint32 i = 0; i < RHI::kFramesInFlight; ++i)
        {
            StreamingFeedbackReadback[i] = FSceneBuffer{ RHI::Malloc(Bytes, RHI::kDefaultAlign, RHI::EMemoryType::CPURead), Bytes };
            RHI::SetDebugName(StreamingFeedbackReadback[i].Ptr, "Readback.StreamingFeedback");
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

        // Everything that samples a material has run by now, so the accumulated mask is complete.
        Barriers::AllToTransfer(CL);
        RHI::CmdMemcpy(CL, StreamingFeedbackReadback[Slot].Ptr, StreamingFeedbackBuffer.Ptr, StreamingFeedbackBuffer.Size);

        // Zero AFTER the copy: this is the article's fetch_and(0) -- read and reset in one pass, so the
        // next frame's mask is what the next frame actually sampled rather than an ever-growing union.
        RHI::Barriers::TransferToTransfer(CL);
        RHI::CmdMemzero(CL, StreamingFeedbackBuffer.Ptr, StreamingFeedbackBuffer.Size);
        Barriers::TransferToAll(CL);

        StreamingFeedbackStamp[Slot] = ++StreamingFeedbackFrame;
    }

    void FDefaultSceneRenderer::PublishStreamingFeedback()
    {
        FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet();
        if (Streaming == nullptr || StreamingFeedbackSlots == 0)
        {
            return;
        }

        // The slot written kFramesInFlight ago: its copy has certainly completed, because the frame ring
        // has come all the way round and waited on that slot's timeline before reusing it.
        const uint32 Slot = (CurrentFrameSlot + 1u) % RHI::kFramesInFlight;
        if (StreamingFeedbackStamp[Slot] == 0 || !StreamingFeedbackReadback[Slot])
        {
            return;
        }

        const uint32* Masks = static_cast<const uint32*>(RHI::ToHost(StreamingFeedbackReadback[Slot].Ptr));
        if (Masks == nullptr)
        {
            return;
        }

        Streaming->SubmitMaterialFeedback(Masks, StreamingFeedbackSlots);
    }

    uint64 FDefaultSceneRenderer::BuildViewSceneRoot(FSceneView& View, uint64 SceneDataAddr)
    {
        RHI::FTransientAlloc Alloc = RHI::Core::AllocTransient(sizeof(FSceneRoot), alignof(FSceneRoot));
        FSceneRoot* Root = static_cast<FSceneRoot*>(Alloc.Cpu);

        *Root = SceneRootShared;
        Root->SceneData          = SceneDataAddr;
        Root->Clusters           = View.ClusterBuffer.GetAddress();
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

        Root->Splines       = SplineBufferAddr;
        Root->SplinePoints  = SplinePointBufferAddr;
        Root->SplineSamples = SplineSampleBufferAddr;
        Root->NumSplines    = NumActiveSplines;

        // Re-read every frame rather than cached: the slab moves when it grows, and the scene root is the
        // one place its address is allowed to live. Instances carry slots, so nothing else has to be told.
        Root->MeshletHeaders = MeshletHeaderSlab::GetAddress();

        // Only the primary view reports. A capture or a probe bake samples the same materials from a
        // camera the player is not looking through, and letting those drive residency would hold mips for
        // a 64px cube face -- or, worse, promote the world because a thumbnail rendered it up close.
        // Count travels with the pointer and is zeroed with it: a shader that sees no buffer also sees a
        // capacity of 0, so its bounds check rejects every slot rather than reading a stale capacity
        // against a null pointer.
        const bool bWantsFeedback = View.bIsPrimary && !bCapturingProbe && StreamingFeedbackBuffer;
        Root->StreamingFeedback      = bWantsFeedback ? StreamingFeedbackBuffer.GetAddress() : 0;
        Root->StreamingFeedbackCount = bWantsFeedback ? StreamingFeedbackSlots : 0;

        const FSceneImage& ProbeArray = NamedImages[(int)ENamedImage::ProbePrefiltered];
        if (!bCapturingProbe && NumActiveProbes > 0 && ProbeArray.IsValid())
        {
            Root->ReflectionProbes    = ProbeBufferAddr;
            Root->NumReflectionProbes = NumActiveProbes;
            Root->ProbeCubeArrayIndex = ((uint32)ProbeArray.GetResourceID() & 0x00FFFFFFu) | (ProbeArray.GetNumMips() << 24);
        }
        return Alloc.Gpu;
    }

    //~ Begin new-RHI helpers

    void FDefaultSceneRenderer::MeshletCullPass(RHI::FCmdListH CL, EMeshletSlice Slice)
    {
        static const FShaderH ArgsShader = FShaderLibrary::Get("BuildMeshletCullArgs.slang");
        static const FShaderH CullShader = FShaderLibrary::Get("MeshletCull.slang");
        if (ArgsShader == nullptr || CullShader == nullptr || RenderFrame == nullptr)
        {
            return;
        }

        const uint32 NumViews = (uint32)RenderFrame->Views.CullViews.size();
        const uint32 NumDraws = Math::Max(RenderFrame->Views.NumDrawsPerView, 1u);
        if (NumViews == 0u || !GetRenderBuckets() || !GetMeshletBlocks())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Meshlet Cull", tracy::Color::Magenta);
        SCENE_GPU_SCOPE(CL, "Meshlet Cull");

        struct FCullArgsPC
        {
            uint32 NumViews;
            uint32 NumDraws;
            uint32 Slice;
            uint32 bPost;
            uint32 MaxMeshGroups;
            uint32 SubDrawsPerSlice;
            uint32 _Pad0;
            uint32 _Pad1;
            uint64 BucketsAddr;
            uint64 OutCullDispatchArgsAddr;
            uint64 OutMeshDrawArgsAddr;
        } APC = {};
        static_assert(sizeof(FCullArgsPC) == 56, "FCullArgsPC must match BuildMeshletCullArgs.slang.");

        APC.NumViews                = NumViews;
        APC.NumDraws                = NumDraws;
        APC.Slice                   = (uint32)Slice;
        APC.MaxMeshGroups           = Math::Max(RHI::GetMaxMeshWorkGroupCount(), 1u);
        APC.SubDrawsPerSlice        = MeshSubDrawsPerSlice;
        APC.BucketsAddr             = GetRenderBuckets().GetAddress();
        APC.OutCullDispatchArgsAddr = GetMeshletCullDispatchArgs().GetAddress();
        APC.OutMeshDrawArgsAddr     = GetMeshDrawArgs().GetAddress();

        // Lay out the flat dispatch and record where this phase starts appending. Serial prefix, so one
        // group; the post pass below is per-bucket and takes a real grid.
        constexpr uint32 kArgsGroupSize = 64;
        const uint32 PostGroups = (NumViews * NumDraws + kArgsGroupSize - 1u) / kArgsGroupSize;

        APC.bPost = 0u;
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ArgsShader));
        RHI::CmdDispatch(CL, MakeArgs(APC), 1u, 1u, 1u);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute,
            RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments);

        struct FMeshletCullPC
        {
            uint32 NumViews;
            uint32 NumDraws;
            uint32 Slice;
            uint32 VisibilityCapacity;
            uint64 BucketsAddr;
            uint64 BlockListAddr;
            uint64 InstanceVisibilityAddr;
        } CPC = {};
        static_assert(sizeof(FMeshletCullPC) == 40, "FMeshletCullPC must match MeshletCull.slang.");

        CPC.NumViews      = NumViews;
        CPC.NumDraws      = NumDraws;
        CPC.Slice         = (uint32)Slice;
        CPC.VisibilityCapacity     = InstanceVisibilityCapacity;
        CPC.BucketsAddr            = GetRenderBuckets().GetAddress();
        CPC.BlockListAddr          = GetMeshletBlocks().GetAddress();
        CPC.InstanceVisibilityAddr = GetInstanceVisibility().GetAddress();

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullShader));
        RHI::CmdDispatchIndirect(CL, MakeArgs(CPC), GetMeshletCullDispatchArgs().Ptr, 0u);
        Barriers::ComputeToAll(CL);

        // Turn what it appended into the slice every draw indexes, and the counts they draw from.
        APC.bPost = 1u;
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ArgsShader));
        RHI::CmdDispatch(CL, MakeArgs(APC), PostGroups, 1u, 1u);

        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute,
            RHI::EStageFlags::Compute | RHI::EStageFlags::MeshShader |
            RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::PixelShader);
    }

    void FDefaultSceneRenderer::DrawMeshletBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch,
                                               const FMeshletPassContext& Ctx)
    {
        const uint32 NumDrawsPerView = RenderFrame->Views.NumDrawsPerView;
        const uint32 ArgIndex = Ctx.CullViewIndex * NumDrawsPerView + Batch.IndirectDrawOffset;
        const uint32 Slice    = (uint32)Ctx.Slice;

        struct FMeshletPassPush
        {
            uint64 BucketsAddr;
            uint32 ArgBase;
            uint32 Slice;
            uint32 MaxMeshGroups;
            uint32 CullViewIndex;
            int32  ShadowDataIndex;
            int32  ViewIndex;
            float  ViewportW;
            float  ViewportH;
        } Push;
        static_assert(sizeof(FMeshletPassPush) == 40, "FMeshletPassPush must match FMeshletPassArgs in MeshletGeometry.slang.");

        Push.BucketsAddr          = GetRenderBuckets().GetAddress();
        Push.ArgBase              = ArgIndex;
        Push.Slice                = Slice;
        Push.MaxMeshGroups        = Math::Max(RHI::GetMaxMeshWorkGroupCount(), 1u);
        Push.CullViewIndex        = Ctx.CullViewIndex;
        Push.ShadowDataIndex      = Ctx.ShadowDataIndex;
        Push.ViewIndex            = Ctx.ShadowViewIndex;
        Push.ViewportW            = Ctx.ViewportW;
        Push.ViewportH            = Ctx.ViewportH;

        // One mesh workgroup per meshlet MeshletCull already kept, so the grid IS the survivor count.
        // Both it and the sub-draw count come from the bucket, so the CPU never learns either.
        const uint32 SliceArgBase = (ArgIndex * kMeshletSliceCount + Slice) * MeshSubDrawsPerSlice;

        RHI::CmdDrawMeshTasksIndirectCount(CL, MakeArgs(Push),
            GetMeshDrawArgs().Ptr,
            SliceArgBase * sizeof(RHI::FDrawMeshTasksIndirectArguments),
            GetRenderBuckets().Ptr,
            ArgIndex * sizeof(FRenderBucketGPU) + offsetof(FRenderBucketGPU, SubDrawCount) + Slice * sizeof(uint32),
            MeshSubDrawsPerSlice, sizeof(RHI::FDrawMeshTasksIndirectArguments));
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

        // Checked: a handle that no longer resolves means the entry was freed since this key was built, so
        // there is nothing to compile. Bail rather than feed the driver an empty module.
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
            MakeUInt(5, (uint32)Key.SkinnedMode),   // SPEC_SKINNED: 0=static, 1=skinned, 2=dynamic
            MakeUInt(6, (Key.ShadingFeatures & SF_ShadowMask) ? 1u : 0u),
            MakeUInt(7, (uint32)Key.TriCullMode),   // SPEC_TRI_CULL: per-triangle rejects in the mesh shaders
        };
        const TSpan<const RHI::FSpecializationConstant> Consts(SpecConsts, 7);

        FWriteScopeLock Lock(PipelineCacheMutex);
        if (auto Existing = PipelineCache.find(Seed); Existing != PipelineCache.end())
        {
            return Existing->second;
        }

        // Task-less by construction: MeshletCull.slang culled and compacted before any draw was recorded,
        // so a mesh pipeline reads the survivor list directly.
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
        #endif

        return Pipeline;
    }

    RHI::FDepthStencilH FDefaultSceneRenderer::GetOrCreateDepthState(const RHI::FDepthStencilDesc& Desc)
    {
        auto HashStencil = [](size_t& Seed, const RHI::FStencil& S)
        {
            Hash::HashCombine(Seed, ((uint64)S.Test) | ((uint64)S.FailOp << 8) |
                                    ((uint64)S.PassOp << 16) | ((uint64)S.DepthFailOp << 24) |
                                    ((uint64)S.Reference << 32));
        };

        auto FloatBits = [](float V) -> size_t
        {
            return (size_t)std::bit_cast<uint32>(V);
        };

        size_t Seed = 0;
        Hash::HashCombine(Seed, ((uint64)Desc.DepthMode) | ((uint64)Desc.DepthTest << 8) |
                                ((uint64)Desc.StencilReadMask << 16) | ((uint64)Desc.StencilWriteMask << 24));
        Hash::HashCombine(Seed, FloatBits(Desc.DepthBias));
        Hash::HashCombine(Seed, FloatBits(Desc.DepthBiasSlopeFactor));
        Hash::HashCombine(Seed, FloatBits(Desc.DepthBiasClamp));
        HashStencil(Seed, Desc.StencilFront);
        HashStencil(Seed, Desc.StencilBack);

        {
            FReadScopeLock Lock(PipelineCacheMutex);
            auto It = DepthStateCache.find(Seed);
            if (It != DepthStateCache.end())
            {
                return It->second;
            }
        }

        FWriteScopeLock Lock(PipelineCacheMutex);
        if (auto Existing = DepthStateCache.find(Seed); Existing != DepthStateCache.end())
        {
            return Existing->second;
        }

        RHI::FDepthStencilH State = RHI::CreateDepthStencil(Desc);
        DepthStateCache.emplace(Seed, State);
        return State;
    }

    void FDefaultSceneRenderer::SetViewportScissor(RHI::FCmdListH CL, const FUIntVector2& Extent)
    {
        const RHI::FRect Rect{ 0, (int)Extent.x, 0, (int)Extent.y };
        RHI::CmdSetViewport(CL, Rect);
        RHI::CmdSetScissor(CL, Rect);
    }

    void FDefaultSceneRenderer::WriteBuffer(RHI::FCmdListH CL, RHI::GPUPtr Dst, const void* Data, uint64 Size)
    {
        RHI::FTransientAlloc Staging = RHI::Core::AllocTransient(Size);
        Memory::Memcpy(Staging.Cpu, Data, Size);
        RHI::CmdMemcpy(CL, Dst, Staging.Gpu, Size);
    }

    void FDefaultSceneRenderer::ResizeBufferIfNeeded(RHI::FCmdListH CL, FSceneBuffer& Buffer, uint64 NeededSize,
                                                   float SlackFactor, uint32& LowUsageCounter,
                                                   bool bAllowShrink, EBufferInit Init, const char* DebugName)
    {
        NeededSize = Math::Max<uint64>(NeededSize, 16ull);

        auto AlignUp16 = [](uint64 Size) { return (Size + 15ull) & ~15ull; };

        const auto Reallocate = [&]()
        {
            DeferFree(Buffer.Ptr);
            Buffer = CreateSceneBuffer(AlignUp16((uint64)((double)NeededSize * SlackFactor)), DebugName);
            LowUsageCounter = 0;

            if (Buffer && Init == EBufferInit::Zeroed)
            {
                RHI::CmdMemzero(CL, Buffer.Ptr, Buffer.GetSize());
                Barriers::TransferToAll(CL);
            }
        };

        if (NeededSize > Buffer.Size)
        {
            Reallocate();

            if (!Buffer)
            {
                LOG_ERROR("RenderScene: scene buffer allocation of {} MiB failed; the pass using it will run degraded this frame.",
                          NeededSize / (1024ull * 1024ull));
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

    void FDefaultSceneRenderer::DeferFree(RHI::GPUPtr Ptr)
    {
        RHI::Core::Retire(Ptr);
    }

    void FDefaultSceneRenderer::DeferRelease(FSceneImage& Image)
    {
        RetireSceneImage(Image);
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

    entt::entity FDefaultSceneRenderer::GetEntityAtPixel(uint32 X, uint32 Y) const
    {
    #if USING(WITH_EDITOR)
        int32 BestSlotIdx = -1;
        uint64 BestFrame = 0;
        for (uint32 i = 0; i < PickerReadbackRingSize; ++i)
        {
            const FPickerReadbackSlot& Slot = PickerReadbackRing[i];
            if (!Slot.bPending || Slot.Readback == 0)
            {
                continue;
            }
            if (PickerReadbackFrame - Slot.SubmittedFrame <= RHI::kFramesInFlight)
            {
                continue;
            }
            if (X < Slot.OriginX || X >= Slot.OriginX + Slot.Width ||
                Y < Slot.OriginY || Y >= Slot.OriginY + Slot.Height)
            {
                continue;
            }
            if (BestSlotIdx == -1 || Slot.SubmittedFrame > BestFrame)
            {
                BestSlotIdx = static_cast<int32>(i);
                BestFrame = Slot.SubmittedFrame;
            }
        }

        if (BestSlotIdx == -1)
        {
            return entt::null;
        }

        const FPickerReadbackSlot& Slot = PickerReadbackRing[BestSlotIdx];
        const uint32 LocalX = X - Slot.OriginX;
        const uint32 LocalY = Y - Slot.OriginY;

        // CPURead allocations are persistently mapped; the readback is tightly packed (RowLength = Width).
        const uint32* Pixels = static_cast<const uint32*>(RHI::ToHost(Slot.Readback));
        if (Pixels == nullptr)
        {
            return entt::null;
        }

        const uint32 PixelValue = Pixels[LocalY * Slot.Width + LocalX];

        if (PixelValue == 0)
        {
            return entt::null;
        }

        return static_cast<entt::entity>(PixelValue);
    #else
        (void)X;
        (void)Y;
        return entt::null;
    #endif
    }

    #if USING(WITH_EDITOR)
    void FDefaultSceneRenderer::SetPickerCursor(uint32 X, uint32 Y, bool bOverViewport)
    {
        const uint64 Packed = (bOverViewport ? 1ull : 0ull)
                            | ((uint64(X) & 0x1FFFFF) << 1)
                            | ((uint64(Y) & 0x1FFFFF) << 22);
        PickerCursorPacked.store(Packed, std::memory_order_relaxed);
    }

    void FDefaultSceneRenderer::IssuePickerReadback(RHI::FCmdListH CL)
    {
        const uint64 Packed = PickerCursorPacked.load(std::memory_order_relaxed);
        const bool bOverViewport = (Packed & 1ull) != 0;
        if (!bOverViewport)
        {
            // Cursor isn't over the viewport: no pick can happen this frame, so skip the copy entirely.
            return;
        }

        const FSceneImage& PickerImage = GetNamedImage(ENamedImage::Picker);
        if (!PickerImage.IsValid())
        {
            return;
        }

        const uint32 ImgW = PickerImage.GetSizeX();
        const uint32 ImgH = PickerImage.GetSizeY();
        if (ImgW == 0 || ImgH == 0)
        {
            return;
        }

        const uint32 CursorX = Math::Min((uint32)((Packed >> 1) & 0x1FFFFF), ImgW - 1);
        const uint32 CursorY = Math::Min((uint32)((Packed >> 22) & 0x1FFFFF), ImgH - 1);

        const uint32 RegionW = Math::Min(PickerRegionExtent, ImgW);
        const uint32 RegionH = Math::Min(PickerRegionExtent, ImgH);
        const uint32 OriginX = Math::Min(CursorX - Math::Min(CursorX, RegionW / 2), ImgW - RegionW);
        const uint32 OriginY = Math::Min(CursorY - Math::Min(CursorY, RegionH / 2), ImgH - RegionH);

        FPickerReadbackSlot& Slot = PickerReadbackRing[PickerReadbackWriteIndex];

        if (Slot.Readback == 0 || Slot.Width != RegionW || Slot.Height != RegionH)
        {
            if (Slot.Readback != 0)
            {
                DeferFree(Slot.Readback);
            }
            Slot.Readback = RHI::Malloc((uint64)RegionW * RegionH * sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            RHI::SetDebugName(Slot.Readback, "Readback.Picker");
            Slot.Width = RegionW;
            Slot.Height = RegionH;
        }

        RHI::FTextureSlice SrcSlice;
        SrcSlice.Offset = FUIntVector3(OriginX, OriginY, 0);
        SrcSlice.Extent = FUIntVector3(RegionW, RegionH, 1);

        // Picker writes -> transfer read, then host read of the packed region.
        Barriers::AllToTransfer(CL);
        RHI::CmdCopyTextureToMemory(CL, PickerImage.Texture, SrcSlice, Slot.Readback, RegionW);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Host);

        Slot.OriginX = OriginX;
        Slot.OriginY = OriginY;
        Slot.SubmittedFrame = PickerReadbackFrame;
        Slot.bPending = true;

        ++PickerReadbackFrame;
        PickerReadbackWriteIndex = (PickerReadbackWriteIndex + 1) % PickerReadbackRingSize;
    }
    #endif
}
