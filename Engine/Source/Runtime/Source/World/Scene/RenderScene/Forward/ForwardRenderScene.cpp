#include "RuntimePCH.h"
#include "ForwardRenderScene.h"
#include <algorithm>
#include "Animation/SkeletalMeshUtils.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "assets/assettypes/mesh/skeleton/skeleton.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Config/EngineSettings.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Windows/Window.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "Renderer/RendererUtils.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Renderer/RHITexture.h"
#include "Renderer/RenderManager.h"
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
#include "world/entity/components/charactercontrollercomponent.h"
#include "World/Entity/Components/EditorComponent.h"
#include "world/entity/components/entitytags.h"
#include "world/entity/components/environmentcomponent.h"
#include "World/Entity/Components/ExponentialHeightFogComponent.h"
#include "world/entity/components/lightcomponent.h"
#include "World/Entity/Components/LineBatcherComponent.h"
#include "World/Entity/Components/TriangleBatcherComponent.h"
#include "World/Entity/Components/ParticleSystemComponent.h"
#include "World/Entity/Components/DecalComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/EnvironmentComponent.h"
#include "World/Entity/Components/SkyLightComponent.h"
#include "World/Entity/Components/ReflectionProbeComponent.h"
#include "world/entity/components/staticmeshcomponent.h"
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

namespace Lumina
{
    namespace
    {
        constexpr uint32 GFroxelGridX = 160;
        constexpr uint32 GFroxelGridY = 90;
        constexpr uint32 GFroxelGridZ = 128;

        // Hard cap on non-sun volumetric lights per frame; packed into the inject push constants.
        constexpr uint32 GFroxelMaxLocalLights = 16;

        static TConsoleVar<bool> CVarVolFogEnabled(
            "r.VolFog.Enabled",
            true,
            "Enable froxel volumetric fog. Still requires an enabled ExponentialHeightFog component.");
        
        static TConsoleVar<bool> CVarMeshShaders(
            "r.MeshShaders",
            true,
            "Use the mesh-shader geometry path (MeshletMesh.slang) instead of the vertex-emulation path.");
        
        // Every GPU cull stage already reports its count through the Totals readback; this is the only
        // thing that shows them. Off by default, and one log line every N frames when on.
        static TConsoleVar<int32> CVarCullStats(
            "r.Scene.CullStats",
            0,
            "Log the GPU cull's per-stage counts every N frames (0 = off): retained slots -> visible "
            "instances -> meshlet pairs -> draw-list entries. Shows which stage a missing scene died at.");

        static TConsoleVar<bool> CVarCPUInstanceCull(
            "r.Scene.CPUInstanceCull",
            true,
            "Reject instances on the CPU before upload. Disable to leave all culling to the GPU.");

        // Bumped by RequestReflectionProbeRebake / the console command; each scene compares it against
        // its own last-seen value on the next Extract, so one request reaches every live scene exactly
        // once without the command needing a handle to any of them.
        static TAtomic<uint32> GReflectionProbeRebakeRequests{0};

        static TConsoleVar<bool> CVarProbeDebugLog(
            "r.ReflectionProbes.DebugLog",
            false,
            "Log reflection-probe extract/upload/bake state on change. Pairs with the Probe Influence "
            "and Probe Radiance view modes for tracking down probes that read as sky.");

        static FAutoConsoleCommand GCmdRebakeReflectionProbes(
            "r.ReflectionProbes.Rebake",
            "Recapture every reflection probe. Needed after moving world geometry, which does not itself "
            "invalidate a bake (only changing a probe does).",
            []{ RequestReflectionProbeRebake(); });
        
        static TConsoleVar<float> CVarPOMSampleScale(
            "r.POM.SampleScale",
            1.0f,
            "Scales every Parallax Occlusion Mapping material's sample counts. 0 disables POM (surfaces fall back to flat normal mapping).");

        static TConsoleVar<float> CVarPOMLODBias(
            "r.POM.LODBias",
            0.0f,
            "Added to every POM material's LOD threshold. Negative values fade POM to normal mapping closer to the camera.");

        static TConsoleVar<float> CVarPOMShadowSampleScale(
            "r.POM.ShadowSampleScale",
            1.0f,
            "Scales POM self-shadow sample counts. 0 disables height-field self-shadowing globally.");

        // Cascade culling is configured per-light on SDirectionalLightComponent (CascadeMinTexels,
        // bCascadeOcclusionCull), alongside the cascade splits and bias those knobs interact with.

        // One grain for the whole gather: all primitive types share a single dense array.
        static TConsoleVar<int32> CVarPrimitiveGrain(
            "r.Scene.PrimitiveGrain",
            256,
            "Scene primitives per task in the cull/emit gather.");
        
        static TConsoleVar<float> CVarSyncSlowMs(
            "r.Scene.SyncSlowMs",
            0.0f,
            "Log when FScenePrimitiveSet::Sync exceeds this many milliseconds (0 = off).");
        
        static TConsoleVar<int32> CVarValidateSkinSlices(
            "r.Skinning.Validate",
            0,
            "Validate GPU pre-skinning slice arithmetic on the CPU and log violations (1 = on).");

        static TConsoleVar<int32> CVarMaxPreSkinnedVertices(
            "r.Skinning.MaxVertices",
            12 * 1024 * 1024,
            "Per-frame budget for the GPU pre-skinning buffer, in vertices (28 B each). Skinned entities "
            "past the budget blend bones in the draw path instead. Without a cap a large skinned crowd "
            "asks for tens of GB and the skinning compute writes off the end of the allocation.");

        // A zero grain would ask for an unbounded task split; floor it.
        FORCEINLINE uint32 ResolveGrain(const TConsoleVar<int32>& CVar)
        {
            return (uint32)Math::Max(1, CVar.GetValue());
        }
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

    FForwardRenderScene::FForwardRenderScene(CWorld* InWorld)
        : IRenderScene(InWorld)
        , ShadowAtlas(FShadowAtlasConfig())
    {
    }

    // The VisBuffer resolves opaque geometry at 1x (visibility IDs cannot be MSAA-averaged) and uses SMAA
    // for edge AA, so hardware MSAA is unsupported. A >1x count would bind a 1x VisBuffer color against an
    // Nx depth target: an invalid mixed-sample subpass. Clamp to 1, warn once.
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

    void FForwardRenderScene::Init()
    {
        LUMINA_MEMORY_SCOPE("Render Scene");

        RHI::WaitDeviceIdle();

        // Scene-global setting; sizes every view's MS scratch images. Init runs before any Extract,
        // so read straight from the world.
        const SDefaultWorldSettings& InitSettings = World ? World->GetDefaultWorldSettings() : SDefaultWorldSettings{};
        MSAASampleCount = ResolveVisBufferSampleCount(InitSettings.MSAASampleCount);

        // Shared (view-independent) buffers + images first.
        InitBuffers();

        InitSharedResources();

        AppliedIBLResolution = FIBLBakeResolution{};
        InitSkyCube(AppliedIBLResolution.SkyCube);
        InitIBLConvolutionTargets(AppliedIBLResolution);

        ShadowAtlas.InitImage();

        // Cascade shadow atlas: shared across all views. Capture (preview) cameras reuse the
        // primary camera's CSM cascades rather than fitting their own, so it's created once here.
        {
            RHI::FTextureDesc Desc;
            Desc.Type      = RHI::ETextureType::Tex2D;
            Desc.Dimension = FUIntVector3(GCSMAtlasWidth, GCSMAtlasHeight, 1);
            Desc.Format    = EFormat::D32;
            Desc.Usage     = RHI::EImageUsageFlags::DepthAttachment | RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;
            NamedImages[(int)ENamedImage::Cascade] = CreateSceneImage(Desc);
        }

        // Cascade Hi-Z, shared with the atlas above. Exactly HALF the atlas per axis, which is not a memory
        // compromise but a correctness one: SPD's base case covers one 2x2 source block per output texel with
        // a single reduction tap, so any other ratio would sample a subset of the block and report an extent
        // smaller than the truth -- and an under-reported extent over-culls.
        //
        // The 2x2 cascade packing survives the mip chain because both the tile size and the tile origins are
        // powers of two: every reduction stays inside one tile until the tile is a single texel. R32 rather
        // than the camera pyramid's R16: rounding a max DOWN loses occluders, and near 1.0 R16 has a ~5e-4 step.
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

        // Reserve so capture-view registration never reallocates SceneViews -- the render
        // thread holds raw FSceneView* (CurrentView) and indexes SceneViews by snapshot.
        SceneViews.reserve(MaxSceneViews);

        // Primary view (index 0) tracks the swapchain size.
        AddSceneView(Windowing::GetPrimaryWindowHandle()->GetExtent(), /*bPrimary*/ true);

        SwapchainResizedHandle = FRenderManager::OnSwapchainResized.AddMember(this, &FForwardRenderScene::SwapchainResized);

        if (World != nullptr)
        {
            FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

            // Opt in to the resolve publishing which entities it moved. Nothing accumulates until this
            // is on, so worlds without a render scene pay nothing.
            ECS::Utils::SetPublishMovedTransforms(Registry, true);

            // The world's components were loaded before this scene existed, so their construct hooks
            // fired into a tracker nobody was reading. Rescan once.
            ScenePrimitives.Reset(&Registry);
        }
    }

    FForwardRenderScene::FSceneView& FForwardRenderScene::AddSceneView(const FUIntVector2& Size, bool bPrimary)
    {
        // SceneViews backing storage is stable during a frame; views are only added/removed
        // at controlled points (init / capture register), never mid-RenderView.
        SceneViews.emplace_back();
        FSceneView& View = SceneViews.back();
        View.bIsPrimary = bPrimary;
        View.Size       = Math::Max(Size, FUIntVector2(1));

        // Per-view clustered-lighting grid (built from this view's projection).
        View.ClusterBuffer = CreateSceneBuffer(sizeof(FCluster) * NumClusters);
        View.bClusterGridDirty = true;   // fresh buffer has undefined contents.

        InitViewImages(View);

        return View;
    }

    int32 FForwardRenderScene::RegisterCaptureView(const FUIntVector2& Size)
    {
        // Reuse a disabled capture view of the same size if one exists (avoids growing
        // SceneViews on repeated select/deselect); else allocate a new one.
        const FUIntVector2 ClampedSize = Math::Max(Size, FUIntVector2(1));
        for (int32 i = 1; i < (int32)SceneViews.size(); ++i)
        {
            if (!SceneViews[i].bEnabled && !SceneViews[i].bReservedForProbeBake && SceneViews[i].Size == ClampedSize)
            {
                return i;
            }
        }

        // Cap capture views so the reserved SceneViews never reallocates (which would dangle CurrentView).
        // The editor reuses one preview slot, so this only trips on many simultaneous captures.
        if (SceneViews.size() >= MaxSceneViews)
        {
            return -1;
        }

        // No WaitIdle: resource creation is mutex-serialized, the new view isn't referenced by any
        // in-flight frame, and SceneViews is reserved so the push-back can't reallocate under the
        // render phase.
        const int32 Handle = (int32)SceneViews.size();
        AddSceneView(ClampedSize, /*bPrimary*/ false);
        return Handle;
    }

    bool FForwardRenderScene::SetCaptureView(int32 Handle, const FViewVolume& View, bool bEnabled)
    {
        if (Handle <= 0 || Handle >= (int32)SceneViews.size())
        {
            return false;
        }
        SceneViews[Handle].PendingViewVolume = View;
        SceneViews[Handle].bEnabled          = bEnabled;
        return true;
    }

    int32 FForwardRenderScene::GetCaptureDisplayResourceID(int32 Handle) const
    {
        if (Handle <= 0 || Handle >= (int32)SceneViews.size())
        {
            return -1;
        }
        return SceneViews[Handle].Output.GetResourceID();
    }

    void FForwardRenderScene::InitSharedResources()
    {
        FSharedRenderResources& Shared = GRenderManager->GetSharedRenderResources();

        if (!Shared.bInitialized)
        {
            BakeBRDFLUT();

            Shared.SMAAArea = RHI::Textures::Create(RHI::FTexture2DDesc{ .Width = AREATEX_WIDTH, .Height = AREATEX_HEIGHT, .Format = EFormat::RG8_UNORM });
            RHI::Textures::Upload(Shared.SMAAArea, 0, areaTexBytes, AREATEX_SIZE, AREATEX_WIDTH);

            Shared.SMAASearch = RHI::Textures::Create(RHI::FTexture2DDesc{ .Width = SEARCHTEX_WIDTH, .Height = SEARCHTEX_HEIGHT, .Format = EFormat::R8_UNORM });
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
                    Shared.EditorIcons[i] = RHI::Textures::Create(RHI::FTexture2DDesc
                    {
                        .Width  = Imported->Dimensions.x,
                        .Height = Imported->Dimensions.y,
                        .Format = Imported->Format
                    });
                    RHI::Textures::Upload(Shared.EditorIcons[i], 0, Imported->Pixels.data(), Imported->Pixels.size(), Imported->Dimensions.x);
                }
            }
            #endif

            Shared.bInitialized = true;
        }

        // Alias the shared (manager-owned) textures into the scene's named-image table; the
        // scene never releases these slots (ReleaseViewImages only touches per-view entries).
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

        FSharedRenderResources& SharedNow = GRenderManager->GetSharedRenderResources();
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

        // Only the scene's own images (cascade atlas, pyramid). The aliases above belong to the
        // render manager's shared resources and are named where they are created.
        NameOwnedImages(NamedImages);
    }

    FForwardRenderScene::~FForwardRenderScene()
    {
        RHI::WaitDeviceIdle();

        FRenderManager::OnSwapchainResized.Remove(SwapchainResizedHandle);

        // Stop the resolve recording who moved; this scene was its only consumer, and an unread queue
        // would grow for as long as the world outlives its renderer.
        if (World != nullptr)
        {
            FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);
            ECS::Utils::SetPublishMovedTransforms(Registry, false);
            MovedTransformScratch.clear();
            ECS::Utils::DrainMovedTransforms(Registry, MovedTransformScratch);
            MovedTransformScratch.clear();

            // Primitives hold entity keys and refs into the resolve table; a later scene on this world
            // must rebuild rather than inherit them.
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

        // Scene-owned shared images (cascade atlas, sky cubes). Slots that merely alias
        // manager-owned resources carry bOwned = false and are skipped, so this releases exactly
        // what the scene created -- and keeps releasing it when new shared images are added.
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

        // Retained GPU scene. NOT ringed (uploads are ordered on the GPU timeline), so these are freed once
        // rather than per slot. RetainedInstanceBuffer alone is 128 B per primitive surface -- at ~900k
        // entities that is well over 100 MB, and every one of these was leaking a full copy on every scene
        // teardown because the GPU-driven cutover added them without adding them here.
        FreeBuffer(RetainedCullEntryBuffer);
        FreeBuffer(RetainedTransformBuffer);
        FreeBuffer(RetainedStaticBuffer);
        FreeBuffer(SurfaceDescBuffer);

        for (uint32 Slot = 0; Slot < RHI::kFramesInFlight; ++Slot)
        {
            FreeBuffer(IndirectArgsRing[Slot]);
            FreeBuffer(MeshletDrawListRing[Slot]);
            FreeBuffer(MeshDrawArgsRing[Slot]);
            FreeBuffer(MeshletDeferListRing[Slot]);
            FreeBuffer(DeferCountRing[Slot]);
            FreeBuffer(CullDispatchArgsRing[Slot]);
            FreeBuffer(SpdCounterRing[Slot]);
            FreeBuffer(InstancePrefixRing[Slot]);
            FreeBuffer(MaterialBinTileBitsRing[Slot]);
            FreeBuffer(MaterialTileListRing[Slot]);
            FreeBuffer(MaterialTileArgsRing[Slot]);

            // GPU-driven scene per-frame outputs.
            FreeBuffer(VisibleInstanceRing[Slot]);
            FreeBuffer(CullCounterRing[Slot]);
            FreeBuffer(BatchMeshletCountRing[Slot]);
            FreeBuffer(ViewDrawCountRing[Slot]);
            FreeBuffer(ViewDrawOffsetRing[Slot]);
            FreeBuffer(TotalsRing[Slot]);
            FreeBuffer(ScanBlockSumRing[Slot]);
            FreeBuffer(ScanDispatchArgsRing[Slot]);

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

        // Anything still pending deferred destruction.
        for (uint32 Slot = 0; Slot < RHI::kFramesInFlight; ++Slot)
        {
            for (RHI::GPUPtr Ptr : DeferredBufferFrees[Slot])
            {
                RHI::Free(Ptr);
            }
            DeferredBufferFrees[Slot].clear();
            for (FSceneImage& Image : DeferredImageReleases[Slot])
            {
                ReleaseSceneImage(Image);
            }
            DeferredImageReleases[Slot].clear();
        }

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
        void PrepareTerrainExtract(STerrainComponent& Terrain, const FMatrix4& WorldMatrix, FForwardRenderScene::FFrameData::FTerrainExtract& Out);
    }

    void FForwardRenderScene::Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess)
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

        // Resolve post-process materials here (extract phase, alive) and ref-hold their shaders, so a
        // deleted PP material can't dangle the render phase. Invalid/wrong-domain entries are dropped.
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
            const FShaderEntry* VS = PPMaterial->GetVertexShader();
            const FShaderEntry*  PS = PPMaterial->GetPixelShader();
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
        // SSAO (GTAO): per-world tuning only, no CPU kernel. AOTextureIndex stays the ~0u sentinel here;
        // the render phase patches it (or leaves the sentinel when SSAO is off) before upload.
        SceneGlobalData.SSAOSettings                    = FSSAOSettings{};
        SceneGlobalData.SSAOSettings.Radius             = Frame.CachedWorldSettings.SSAORadius;
        SceneGlobalData.SSAOSettings.Intensity          = Frame.CachedWorldSettings.SSAOIntensity;
        SceneGlobalData.SSAOSettings.Power              = Frame.CachedWorldSettings.SSAOPower;
        // POM global quality. Negative CVar values would invert the sample-count lerp, so floor at 0.
        SceneGlobalData.ParallaxSettings.SampleScale       = Math::Max(CVarPOMSampleScale.GetValue(), 0.0f);
        SceneGlobalData.ParallaxSettings.LODBias           = CVarPOMLODBias.GetValue();
        SceneGlobalData.ParallaxSettings.ShadowSampleScale = Math::Max(CVarPOMShadowSampleScale.GetValue(), 0.0f);
        Frame.CameraFrustum                             = ViewVolume.GetFrustum();
        SceneGlobalData.CullData.Frustum                = AsGPU(Frame.CameraFrustum);
        SceneGlobalData.CullData.ShadowFrustum          = SceneGlobalData.CullData.Frustum; // Rebuilt after directional light is processed.
        SceneGlobalData.CullData.bHasDirectional        = 0u;
        // InstanceNum / MeshletDrawListCapacity are published by CompileDrawCommands_Render, which
        // is where the buffers they bound are actually sized. Nothing on this thread knows either value.
        SceneGlobalData.CullData.bFrustumCull           = RenderSettings.bFrustumCull;
        SceneGlobalData.CullData.bOcclusionCull         = RenderSettings.bOcclusionCull;
        // Fallback; ProcessDirectionalLight overrides this from the active sun's
        // ShadowMaxDistance. Only matters when no directional light is present.
        SceneGlobalData.CullData.ShadowMaxDistance      = 5000.0f;
        // Per-scene switch; ProcessDirectionalLight ands in the active sun's own bCascadeOcclusionCull, so a
        // scene that turned it off stays off whatever the light asks for. The thumbnail scene turns it off
        // because it renders a single frame and there is no previous cascade depth to test against.
        SceneGlobalData.CullData.bShadowOcclusionCull   = RenderSettings.bShadowOcclusionCull;
        // Shadow LOD inputs for BOTH cull passes. CullInstances reserves each cascade's draw-list region
        // from these and CullMeshlets re-derives the same pick when it walks, so they are published once
        // here rather than pushed per pass.
        SceneGlobalData.CullData.ShadowLODBias          = RenderSettings.ShadowLODBias;
        SceneGlobalData.CullData.ShadowCoarseLODDistSq  = RenderSettings.ShadowCoarseLODDistance
                                                        * RenderSettings.ShadowCoarseLODDistance;
        // Fallback, same contract as ShadowMaxDistance above. With no sun no cascade view is pushed, so
        // nothing reads it.
        CascadeMinTexels                                = 1.0f;
        SceneGlobalData.CullData.DebugMode              = (uint32)RenderSettings.Flags;
        // Cleared here, raised by ProcessDirectionalLight (transforms) and the render phase (pyramid).
        // A scene with no sun leaves it at 0, which is what makes every cascade occlusion test pass.
        SceneGlobalData.CullData.bCascadeHZBValid       = 0u;
        // Same, for the mid-frame pyramid the phase-2 re-test reads. Raised only by ProcessDirectionalLight,
        // so a sunless scene leaves phase 2 testing nothing (it also never runs: no cascade views exist).
        SceneGlobalData.CullData.bCascadeHZBMidValid    = 0u;


        CMaterial* FallbackMaterial = CMaterial::GetDefaultMaterial();
        if (!IsValid(FallbackMaterial) || !FallbackMaterial->IsReadyForRender())
        {
            ExtractFrame = nullptr;

            // Nothing rendered, so next frame's HZB is not this frame's depth.
            bDepthPyramidValid.store(false, std::memory_order_release);
            return;
        }

        // Clear CPU scene state before the gather, so the render phase
        // never sees half-populated containers.
        ResetPass_Extract();

        // Probes are gathered (and a bake scheduled) before the gather for the same reason capture
        // views are snapshotted here: the six face cameras need cull views, and only BuildCullViews
        // (inside CompileDrawCommands_Extract) can allocate them.
        ExtractReflectionProbes(ECS::GetWorldRegistry(*World), Frame);

        // Snapshot the enabled capture views (camera + RT) before the gather, so BuildCullViews
        // (inside CompileDrawCommands_Extract) can append a frustum-only cull view for each.
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

        // Finalize each capture view's per-view constants: inherit the primary's shared state
        // (debug mode, time, instance count) then override the camera-specific fields.
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

        // Same finalize for the probe bake's six faces. They share one FSceneView, so the size is one
        // square face for all six; only the camera basis differs.
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

        Frame.bExtractedThisFrame = true;

        ExtractFrame = nullptr;
    }

    void FForwardRenderScene::ExtractReflectionProbes(FEntityRegistry& Registry, FFrameData& Frame)
    {
        LUMINA_PROFILE_SECTION("Extract Reflection Probes");

        auto& Probes   = Frame.ReflectionProbes.Probes;
        auto& Captures = Frame.ReflectionProbes.Captures;
        Probes.clear();
        Captures.clear();
        Frame.ReflectionProbes.bNeedsRebake = false;

        auto ProbeView = Registry.view<SReflectionProbeComponent, STransformComponent>(entt::exclude<SDisabledTag>);

        // Collected with their priority so the whole set can be ordered before slices are handed out;
        // slice assignment must follow the final order, since the shader walks the array front-to-back.
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

            // Sphere mode is uniform by definition (it has one radius), so collapse the extents rather
            // than letting a non-uniform entity scale turn the "sphere" into an ellipsoid the shader's
            // unit-sphere intersection would not match.
            const FVector3 Extent = (Probe.Shape == EReflectionProbeShape::Sphere)
                                        ? FVector3(Math::Max(Probe.Extent.x, 0.001f))
                                        : Math::Max(Probe.Extent, FVector3(0.001f));

            const FMatrix4 WorldMatrix = Transform.GetWorldMatrix();

            FProbeSortEntry Entry;
            // Baking Extent into the matrix is what makes the volume the unit box/sphere in probe
            // space, which the shader relies on for the inside test, blend, and parallax intersect.
            Entry.Gpu.ProbeToWorld    = Math::Scale(WorldMatrix, Extent);
            Entry.Gpu.WorldToProbe    = Math::Inverse(Entry.Gpu.ProbeToWorld);

            // The capture origin is offset in the entity's own frame, so a rotated probe's offset
            // rotates with it; the influence volume stays centered on the entity either way.
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

        // Descending priority: the shader consumes influence front-to-back, so the probe that should
        // win an overlap has to be seen first. stable_sort keeps equal-priority probes in registry
        // order, which keeps slice assignment (and therefore bakes) stable frame to frame.
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

        // Two kinds of change, which must NOT be treated alike:
        //
        //  LAYOUT (probe count changed): a probe's slice is its position in the priority ordering, so
        //  inserting or removing one shifts every lower-priority probe onto a slice holding someone
        //  else's radiance. That data is genuinely wrong, so the baked mask has to be cleared.
        //
        //  CONTENT (a probe moved or resized): only its own capture is stale, and a stale capture is
        //  far closer to right than none. Requeue it but KEEP its baked flag, so it keeps showing the
        //  previous capture until the new one lands. Clearing here is what made a probe read as
        //  unbaked for every frame it was being dragged, and the shader renders unbaked as pure sky --
        //  so the probe appeared to do nothing at all for as long as you were positioning it.
        //
        // The content test is epsilon-based rather than an exact memcmp so that movement too small to
        // change the capture does not spend a bake on it.
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

        // An explicit rebuild request covers the case the comparison above cannot see: the probes are
        // unchanged but the WORLD around them moved, which leaves every capture stale with nothing in
        // the probe set to signal it. Compared rather than exchanged so one request fans out to every
        // live scene exactly once.
        const uint32 RebakeRequests = GReflectionProbeRebakeRequests.load(std::memory_order_relaxed);
        if (RebakeRequests != LastSeenRebakeRequest)
        {
            LastSeenRebakeRequest = RebakeRequests;
            Frame.ReflectionProbes.bNeedsRebake = true;
        }

        // Fold in whatever the render phase finished since the last Extract. Bakes complete at the end
        // of RenderView, so a probe captured last frame becomes samplable now; doing it the other way
        // (marking at schedule time) would let this frame's shading read a slice its own bake has not
        // written yet.
        BakedProbeMask |= CompletedProbeBakes.exchange(0, std::memory_order_acq_rel);

        if (Frame.ReflectionProbes.bNeedsRebake)
        {
            // Only a LAYOUT change invalidates existing captures, because that is what reshuffles which
            // probe owns which slice. A probe that merely moved keeps its baked flag and therefore keeps
            // displaying its previous capture while the new one is queued.
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

        // Stamp the baked flag so the shader skips probes still waiting their turn. An unbaked probe
        // must be skipped rather than sampled-as-black: consuming influence and returning black would
        // darken the reflection instead of leaving the sky to cover that pixel.
        for (uint32 i = 0; i < (uint32)Probes.size(); ++i)
        {
            Probes[i].CapturePosition.w = ((BakedProbeMask >> i) & 1u) ? 1.0f : 0.0f;
        }

        ScheduleReflectionProbeBake(Frame);

        // Change-gated so it stays quiet once things settle, but catches the transitions that matter:
        // probes extracted but never marked baked, a mask that keeps getting wiped, a bake that never
        // gets scheduled. Behind r.ReflectionProbes.DebugLog.
        {
            const uint64 State = ((uint64)Probes.size())
                               | ((uint64)BakedProbeMask << 8)
                               | ((uint64)PendingProbeBakes.size() << 40)
                               | ((uint64)(Frame.ReflectionProbes.BakingProbe + 1) << 48);
            if (State != LastProbeDiagState && CVarProbeDebugLog.GetValue())
            {
                LastProbeDiagState = State;
                LOG_WARN("[Probe] Extract: count={} bakedMask=0x{:X} queued={} scheduled={} view={} needsRebake={}",
                         Probes.size(), BakedProbeMask, PendingProbeBakes.size(),
                         Frame.ReflectionProbes.BakingProbe, Frame.ReflectionProbes.BakeViewIndex,
                         Frame.ReflectionProbes.bNeedsRebake ? 1 : 0);
            }
        }
    }

    void FForwardRenderScene::ScheduleReflectionProbeBake(FFrameData& Frame)
    {
        auto& Bake = Frame.ReflectionProbes;
        Bake.BakingProbe   = -1;
        Bake.BakeViewIndex = -1;

        const uint32 NumProbes = (uint32)Bake.Captures.size();

        // The explicit queue drains first: a probe that has never been captured, or one invalidated by
        // a change, matters more than refreshing an Always probe that already has usable radiance.
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
            // Nothing queued, so give one Always probe its turn. Round-robin from where the last one
            // left off rather than always starting at 0, or with several Always probes only the first
            // would ever refresh.
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

        // One FSceneView renders all six faces in sequence, rather than six views each owning a full
        // per-view image set. Held across bakes and reserved, so the editor's camera preview can never
        // be handed the same view (both would then render into its images in the same frame).
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

        // Six 90-degree cameras from the capture origin. Forward/up pairs must match
        // CubeFaceDirection() in Sky.slang or the baked faces land rotated or swapped, which reads as
        // reflections that jump discontinuously across a surface as the view direction crosses a face
        // boundary. Derived against the engine's left-handed basis (right = cross(up, forward)).
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

        // Dequeued optimistically: the render phase completes any bake Extract schedules, and a frame
        // that schedules always sets bExtractedThisFrame, so RenderView cannot skip it. An Always probe
        // was never queued, so there is nothing to pop for it; it just captures over its own slice,
        // which is already flagged baked and stays continuously samplable.
        if (bFromQueue)
        {
            PendingProbeBakes.erase(PendingProbeBakes.begin());
        }
    }

    void FForwardRenderScene::PrepareRender(uint8 /*FrameIndex*/)
    {
        LUMINA_PROFILE_SCOPE();

        FFrameData& Frame = FrameData;
        if (!Frame.bExtractedThisFrame)
        {
            return;
        }

        // Recreating the IBL cubes calls WaitDeviceIdle, so it can't run while sibling scenes record.
        // RenderWorlds runs this serially for every scene before the parallel RenderView pass.
        SyncIBLResolution(Frame.Volumetrics.IBLResolution);

        // Same reason: the scratch cube may need resizing for this frame's bake.
        if (Frame.ReflectionProbes.BakingProbe >= 0)
        {
            SyncProbeCaptureCube(Frame.ReflectionProbes.BakeFaceSize);
        }
    }

    void FForwardRenderScene::RenderView(uint8 FrameIndex)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Render Scene");

        const uint8 Slot = (uint8)(FrameIndex % RHI::kFramesInFlight);
        RenderFrame = &FrameData;
        FFrameData& Frame = FrameData;

        // This slot's previous GPU work completed AND its command lists were recycled (RHI::Core::BeginFrame
        // waits the frame timeline, then resets them), so nothing executes or still names these resources.
        for (RHI::GPUPtr Ptr : DeferredBufferFrees[Slot])
        {
            RHI::Free(Ptr);
        }
        DeferredBufferFrees[Slot].clear();
        for (FSceneImage& Image : DeferredImageReleases[Slot])
        {
            ReleaseSceneImage(Image);
        }
        DeferredImageReleases[Slot].clear();

        // SyncMSAAState reads Frame.CachedWorldSettings, so RenderFrame must be set first.
        SyncMSAAState();

        if (!Frame.bExtractedThisFrame)
        {
            RenderFrame = nullptr;
            return;
        }

        CurrentFrameSlot = Slot;

        // IBL cube reconciliation already ran serially in PrepareRender (it issues WaitDeviceIdle).

        PointAtView(SceneViews[0]);
        CurrentCameraEarlyView = 0u;                                // primary's early/frustum cull view
        CurrentCameraLateView  = Frame.Views.CameraLateViewIndex;   // primary's late/occlusion cull view

        Frame.SceneGlobalData.CullData.PyramidWidth      = (float)GetNamedImage(ENamedImage::DepthPyramid).GetSizeX();
        Frame.SceneGlobalData.CullData.PyramidHeight     = (float)GetNamedImage(ENamedImage::DepthPyramid).GetSizeY();
        Frame.SceneGlobalData.CullData.DepthPyramidIndex = (uint32)GetNamedImage(ENamedImage::DepthPyramid).GetResourceID();

        // Cascade Hi-Z. Extract already decided whether the reprojection transforms are usable; this ANDs in
        // whether the pyramid behind them has actually been built, which only the render phase knows.
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

            // Constant tile rects: the pyramid is the atlas at half resolution, so a cascade's tile maps
            // straight through. Kept as data rather than recomputed in the shader because the packing table
            // is a CPU constant and nothing in the shader should have to know it.
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
            SCENE_GPU_SCOPE(CL, "RenderView");

            // Order against last frame's reads of our targets (editor viewport sampling the
            // primary Output) before this frame's writes.
            RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EStageFlags::AllCommands);

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
                // Everything the GPU-driven scene does before the first raster lives in here: the retained
                // uploads and the CullInstances / BuildDrawPrefix / SeedIndirectArgs / ScanPrefix*
                // dispatches. It had no GPU marker, so on the GPU timeline it was an unlabelled gap between
                // "RmlUi Widgets" and "Cull Early" -- which is where the frame was actually going.
                SCENE_GPU_SCOPE(CL, "Compile Draw Commands");
                CompileDrawCommands_Render(CL);
            }

            {
                SCENE_GPU_SCOPE(CL, "Texture Paint");
                TexturePaintPass(CL);
            }

            {
                LUMINA_PROFILE_SECTION("RenderPasses");

                // Phase 0 cull: frustum + cone all views, Hi-Z occlusion vs LAST frame's pyramid for the
                // camera; meshlets the stale pyramid hides are deferred (not dropped). The cull writes the
                // mesh-task GroupCountX directly into the {0,1,1}-seeded args (no ConvertMeshDrawArgs pass).
                {
                    SCENE_GPU_SCOPE(CL, "Cull Early");
                    CullPassEarly(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Skinning");
                    SkinningPass(CL);
                }

                // VisBuffer phase 1: rasterize the early (non-occluded) camera meshlets into triangle IDs +
                // depth (clearing both). This is the opaque geometry the deferred pass shades from.
                {
                    SCENE_GPU_SCOPE(CL, "VisBuffer Phase 1");
                    VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ true);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Terrain Update");
                    TerrainUpdatePass(CL);
                }

                // Terrain cull tests against LAST frame's end pyramid (which includes terrain); chunks are
                // large, so the one-frame lag is a conservative, low-risk trade.
                {
                    SCENE_GPU_SCOPE(CL, "Terrain Cull");
                    TerrainCullPass(CL);
                }

                // Terrain depth + VisBuffer 'empty' stamp, BEFORE the mid pyramid: terrain occludes the
                // late meshlet re-test, SSAO/decals see full opaque depth, and the deferred pass skips
                // mesh pixels terrain covers (their VisID is stamped empty).
                {
                    SCENE_GPU_SCOPE(CL, "Terrain Depth");
                    TerrainDepthPrePass(CL);
                }

                // Rebuild the depth pyramid from this frame's partial depth (meshes + terrain) so phase 1's
                // occluders are up to date for the late re-test.
                {
                    SCENE_GPU_SCOPE(CL, "Depth Pyramid (Mid)");
                    DepthPyramidPass(CL);
                }

                // Phase 1 cull: re-test the deferred meshlets against the rebuilt pyramid; emit the
                // disoccluded ones to the camera-late view.
                {
                    SCENE_GPU_SCOPE(CL, "Cull Late");
                    CullPassLate(CL);
                }

                // VisBuffer phase 2: rasterize the disoccluded meshlets, loading + accumulating into the
                // same VisBuffer + depth (no clear). Removes the one-frame disocclusion lag.
                {
                    SCENE_GPU_SCOPE(CL, "VisBuffer Phase 2");
                    VisBufferPass(CL, CurrentCameraLateView, /*bClear*/ false);
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

                // Cascade phase 1: raster the casters the phase-0 cull kept. Casters that last frame's
                // cascade pyramid hid were DEFERRED, not dropped, so they are still pending below.
                {
                    SCENE_GPU_SCOPE(CL, "Cascaded Shadows");
                    CascadedShowPass(CL, Frame.Views.CascadeViewBase);
                }

                // Rebuild the cascade pyramid from what phase 1 just rastered, so the re-test below has
                // this frame's occluders rather than last frame's.
                {
                    SCENE_GPU_SCOPE(CL, "Cascade Pyramid (Mid)");
                    CascadePyramidPass(CL);
                }

                // Cascade phase 2: re-test the deferred casters against that rebuilt pyramid and emit the
                // disoccluded ones. This is what removes the one-frame lag that made shadows flicker open
                // under motion; a caster the stale pyramid wrongly hid gets its shadow back in-frame.
                {
                    SCENE_GPU_SCOPE(CL, "Cull Cascade Late");
                    CullPassCascadeLate(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Cascaded Shadows Phase 2");
                    CascadedShowPass(CL, Frame.Views.CascadeLateViewBase);
                }

                // Cascade HZB for NEXT frame's phase-0 cull. Rebuilt AGAIN because phase 2 added depth the
                // mid pyramid above does not describe; publishing that one would make next frame's cull
                // test against an atlas missing every disoccluded caster.
                {
                    SCENE_GPU_SCOPE(CL, "Cascade Pyramid");
                    CascadePyramidPass(CL);
                }

                {
                    // Sky cube capture here keeps the IBL cube in lockstep with the rendered background.
                    SCENE_GPU_SCOPE(CL, "Sky Cube Capture");
                    SkyCubeCapturePass(CL);
                }

                {
                    // Convolves IBL diffuse + GGX specular cubemaps from the cube SkyCubeCapturePass wrote.
                    SCENE_GPU_SCOPE(CL, "Sky Irradiance");
                    IrradianceConvolutionPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Sky Prefilter");
                    PrefilterEnvMapPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Environment");
                    EnvironmentPass(CL);
                }

                {
                    // DBuffer decals: project onto the full opaque depth (meshes + terrain).
                    SCENE_GPU_SCOPE(CL, "Decals");
                    DecalPass(CL);
                }

                // Full opaque depth (meshes + terrain) is in place, before the passes that sample it.
                {
                    SCENE_GPU_SCOPE(CL, "SSAO");
                    SSAOPass(CL);
                    SSAOBlurPass(CL);
                }

                // Resolve the sun's cascade PCSS into a screen-space mask so the opaque shading passes
                // below read one texel instead of carrying the PCSS live range in their whole-shader
                // register allocation. Both cascade raster phases and the full opaque depth are done.
                {
                    SCENE_GPU_SCOPE(CL, "Shadow Mask");
                    ShadowMaskPass(CL);
                }

                // Stamp each covered pixel's owning material slot into MaterialDepth and bin the tiles, then
                // shade one indirect tile draw per material against it. Both passes emit their own GPU marker.
                {
                    MaterialDepthPass(CL);
                    DeferredMaterialPass(CL);
                }

                // Early-Z vs the pre-pass depth: the heavy terrain PS shades each visible pixel once.
                {
                    SCENE_GPU_SCOPE(CL, "Terrain Render");
                    TerrainRenderPass(CL);
                }

                {
                    SCENE_GPU_SCOPE(CL, "Depth Pyramid (End)");
                    DepthPyramidPass(CL);
                }

                // After the opaque scene (so HDR holds the lit scene to refract/SSR), before translucency.
                {
                    SCENE_GPU_SCOPE(CL, "Water");
                    WaterPass(CL);
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
                    // World-space widgets stamp their entity id into the Picker buffer here (their
                    // color is drawn later, post-tone-map), so they stay click-selectable.
                    SCENE_GPU_SCOPE(CL, "Widget Picker");
                    WidgetPickerPass(CL);
                }
                {
                    // After the last picker RT write; readback happens lazily in GetEntityAtPixel.
                    SCENE_GPU_SCOPE(CL, "Picker Readback");
                    IssuePickerReadback(CL);
                }
                #endif

                // Underwater absorption/distortion over the fully-composited HDR (per-ray path length, so the
                // half-submerged waterline falls out and above-water pixels are untouched). Before bloom/exposure.
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
                    CurrentCameraLateView  = ~0u;

                    CurrentSceneRootAddr = BuildViewSceneRoot(View,
                        RHI::Core::CopyTransient(MakeSecondaryViewGlobals(Capture.SceneGlobalData)));

                    RenderCaptureView(CL);
                }

                // Last, so the shared shadow atlas and cascades this frame's primary sequence rendered
                // are available to light the capture. Note the cascades are fit to the PRIMARY camera,
                // so a probe far outside the camera's shadow range bakes with unshadowed sun -- same
                // limitation capture views already carry.
                ReflectionProbeBakePass(CL);
            }

            {
                // Screen-space world UI composites onto the primary display-referred output.
                SCENE_GPU_SCOPE(CL, "RmlUi World UI");
                RmlUi::RenderWorldUI(World, CL);
            }

            // Make the final Output writes visible to ImGui's later same-queue submit, which
            // samples them (by ResourceID) in the editor viewport / capture preview panels.
            Barriers::RasterToRead(CL);
        }

        RHI::Core::Submit(CL);

        RenderFrame = nullptr;
    }

    static constexpr uint32 GPrefilterSampleCount = 256;

    // Shared by the sky prefilter and the reflection-probe prefilter; both run PrefilterEnvMap.slang,
    // differing only in DstLayerOffset (0 for the sky's own cube, ProbeSlice * 6 for the probe array).
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

    void FForwardRenderScene::ReflectionProbeBakePass(RHI::FCmdListH CL)
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

        // Read the shared targets from NamedImages, not GetNamedImage: both are allocated lazily, after
        // InitViewImages already snapshotted the shared slots into every view, so the per-view copies of
        // these two are empty.
        const FSceneImage& CaptureCube = NamedImages[(int)ENamedImage::ProbeCaptureCube];
        const FSceneImage& ProbeArray  = NamedImages[(int)ENamedImage::ProbePrefiltered];
        if (!CaptureCube.IsValid() || !ProbeArray.IsValid())
        {
            LOG_WARN("[Probe] Bake SKIPPED: captureCube={} probeArray={} (targets not allocated)",
                     CaptureCube.IsValid() ? 1 : 0, ProbeArray.IsValid() ? 1 : 0);
            return;
        }

        // One line per bake: confirms the pass runs, which cull views the faces got, and how much
        // geometry the shared cull actually produced for them to draw.
        if (CVarProbeDebugLog.GetValue())
        {
            LOG_WARN("[Probe] Bake RUN: probe={} faceSize={} cullViews=[{},{},{},{},{},{}] drawCmds={} capturePos=({:.2f},{:.2f},{:.2f})",
                     Bake.BakingProbe, Bake.BakeFaceSize,
                     Bake.FaceCullViews[0], Bake.FaceCullViews[1], Bake.FaceCullViews[2],
                     Bake.FaceCullViews[3], Bake.FaceCullViews[4], Bake.FaceCullViews[5],
                     Frame.Geometry.DrawCommands.size(),
                     Bake.FaceVolumes[0].GetViewPosition().x,
                     Bake.FaceVolumes[0].GetViewPosition().y,
                     Bake.FaceVolumes[0].GetViewPosition().z);
        }

        LUMINA_PROFILE_SECTION_COLORED("Reflection Probe Bake", tracy::Color::SkyBlue3);
        SCENE_GPU_SCOPE(CL, "Reflection Probe Bake");

        FSceneView& View = SceneViews[Bake.BakeViewIndex];

        // Suppresses probe sampling for the duration: a capture that sampled the probe array would fold
        // the previous bake's reflections into this one, compounding on every rebuild.
        bCapturingProbe = true;

        const uint32 FaceSize = Bake.BakeFaceSize;

        // Clear policy for this probe. Read once: Captures is parallel to Probes and BakingProbe
        // indexes both.
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
            CurrentCameraLateView  = ~0u;   // frustum-only, no late re-test

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

            // Opaque + sky, lit with clustered lights and the shared shadow atlas/cascades (already
            // rendered this frame by the primary sequence, which is why the bake runs last).
            //
            // Deliberately excluded: bloom/exposure/tonemap/SMAA, because the cube has to stay linear
            // HDR -- baking display-referred data in would double-apply at runtime and break energy
            // conservation. Also excluded: translucency and volumetric fog, which the runtime scene
            // applies itself and would otherwise be counted twice.
            VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ true);
            TerrainCullPass(CL);
            TerrainDepthPrePass(CL);
            ClusterBuildPass(CL);
            LightCullPass(CL);

            if (bClearToColor)
            {
                // Stand-in for EnvironmentPass, which owns the scene-color clear and then draws sky
                // into it. Clearing to a flat color and skipping the sky draw is what makes a sealed
                // interior capture only its own geometry.
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
            MaterialDepthPass(CL);
            DeferredMaterialPass(CL);
            TerrainRenderPass(CL);

            // HDR scene color -> this face of the scratch cube. Same format and same extent, so it is a
            // straight copy; SyncProbeCaptureCube is what guarantees the extents match.
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

        // Prefilter the finished cube into this probe's slice. Same shader the sky prefilter runs; the
        // destination layer offset is what redirects it into the array.
        {
            static const FShaderEntry* const ComputeShader = FShaderLibrary::Get("PrefilterEnvMap.slang");
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

        // Leave the live per-view members on the primary. The passes after this (RmlUi world UI, the
        // final read barrier) composite onto the primary output, and the loop above left CurrentView
        // pointing at the probe's view.
        PointAtView(SceneViews[0]);
        CurrentCameraEarlyView = 0u;
        CurrentCameraLateView  = Frame.Views.CameraLateViewIndex;

        // Publish for the next Extract to fold into BakedProbeMask. Deferred by a frame on purpose:
        // the primary view for THIS frame already shaded above, so making the slice samplable now
        // would be a lie for everything already recorded.
        CompletedProbeBakes.fetch_or(1u << (uint32)Bake.BakingProbe, std::memory_order_acq_rel);
    }

    // Extract snapshots each secondary view's globals from the primary's, then overrides the camera. Four
    // CullData fields are not known yet at that point -- CompileDrawCommands_Render stamps this frame's
    // meshlet draw-list tag and publishes the three resolve bounds (draw-list capacity, visible-instance
    // capacity, bone count) onto the primary only. A secondary view that keeps the snapshot's values carries
    // a stale tag and zeroed bounds, so every meshlet fails IsFromFrame in MeshletGeometry.slang (nothing
    // rasterizes) and every covered texel fails the same check in MaterialDepth / DeferredMaterial (nothing
    // shades). Symptom: a capture that renders sky and terrain but no meshes.
    FSceneGlobalData FForwardRenderScene::MakeSecondaryViewGlobals(const FSceneGlobalData& ViewGlobals)
    {
        FSceneGlobalData Globals = ViewGlobals;

        const FCullData& Primary = RenderFrame->SceneGlobalData.CullData;
        Globals.CullData.MeshletDrawTag            = Primary.MeshletDrawTag;
        Globals.CullData.MeshletDrawListCapacity   = Primary.MeshletDrawListCapacity;
        Globals.CullData.InstanceNum               = Primary.InstanceNum;
        Globals.CullData.BoneNum                   = Primary.BoneNum;

        // Secondary views (scene captures, probe cube faces) never run ShadowMaskPass -- their opaque
        // sequence is a trimmed subset of the primary's -- so they fall back to the inline cascade PCSS.
        // Clearing BOTH here is the point of routing every secondary view through this one function: the
        // index is what the shader reads, the RenderSettings bit is what the pipeline key specializes on,
        // and a view whose two disagreed would either sample an unwritten mask or shade unshadowed.
        // Both secondary paths run AFTER the whole primary sequence, so nothing re-reads the primary's
        // value this frame; CompileDrawCommands_Render sets it again unconditionally next frame.
        Globals.ShadowMaskIndex         = ~0u;
        RenderSettings.bShadowMaskValid = false;

        return Globals;
    }

    void FForwardRenderScene::RenderCaptureView(RHI::FCmdListH CL)
    {
        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            // No geometry -> VisBufferPass returns early without clearing depth, so clear it here for the
            // downstream depth consumers (water, transparent, fog).
            Barriers::AllToTransfer(CL);
            const float DepthClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::DepthAttachment).Texture, DepthClear);
            Barriers::TransferToAll(CL);
        }

        // Capture views are frustum-only (single phase): rasterize the camera view, no late re-test.
        // Terrain depth (+ VisBuffer stamp) before decals/deferred, mirroring the primary path.
        VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ true);
        TerrainCullPass(CL);
        TerrainDepthPrePass(CL);
        ClusterBuildPass(CL);
        LightCullPass(CL);
        EnvironmentPass(CL);
        DecalPass(CL);
        MaterialDepthPass(CL);
        DeferredMaterialPass(CL);
        TerrainRenderPass(CL);
        WaterPass(CL);
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

    void FForwardRenderScene::SwapchainResized(FVector2 NewSize)
    {
        // Rare, editor-driven: drain the GPU so the per-view images can be released and
        // recreated at the new size without racing in-flight frames.
        RHI::WaitDeviceIdle();

        // Only the primary view tracks the swapchain; capture views keep their own size.
        FSceneView& Primary = SceneViews[0];
        Primary.Size = FUIntVector2(Math::Max((uint32)NewSize.x, 1u), Math::Max((uint32)NewSize.y, 1u));

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

    // Maps the authored IBL quality tier to concrete cube/prefilter resolutions. The Mips counts keep
    // roughness=1 on the smallest face >= 8px so the GGX lobe stays well sampled.
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

    FForwardRenderScene::FThreadLocalDrawData& FForwardRenderScene::AcquireThreadLocalDrawData(uint32 Slot)
    {
        FThreadLocalDrawData& Local = ThreadLocalStorage[Slot];

        // First touch this frame on this worker: bind the slot to this thread's frame arena and reserve.
        // CompileDrawCommands_Extract cleared every slot's arena to null up front, so a null arena here
        // is the once-per-frame guard; later chunks on the same worker see the bound arena and accumulate.
        if (Local.Arena.GetArena() == nullptr)
        {
            Local.ResetForFrame(FFrameArenaAllocator(&GetThreadFrameAllocator(), "RenderGather"));
            Local.Items.reserve(CurrentReservePerThread);

            // Clears only the slots this worker touched last time it ran a gather. A worker that emitted
            // into 40 draws pays for 40 entries, not for the scene's whole slot space.
            Local.PrepareCounters(ScenePrimitives.GetBatches().Num());
            Local.bTouched = true;
        }
        return Local;
    }

    // Routes this frame's transform + component changes into the persistent primitive table.
    void FForwardRenderScene::SyncScenePrimitives()
    {
        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        // The transform resolve above published exactly the entities whose world matrix it rewrote
        // (including descendants dragged along by a parent). Route them to the primitives that mirror
        // that transform -- the only per-frame link between the ECS and the render scene.
        MovedTransformScratch.clear();
        if (ECS::Utils::DrainMovedTransforms(Registry, MovedTransformScratch))
        {
            FRenderDirtyTracker& Tracker = FRenderDirtyTracker::Ensure(Registry);
            for (entt::entity Entity : MovedTransformScratch)
            {
                Tracker.MarkAllSources(Entity, EPrimitiveDirty::Transform);
            }
        }

        const float SyncSlowMs = CVarSyncSlowMs.GetValue();
        const auto  SyncStart  = std::chrono::steady_clock::now();

        ScenePrimitives.Sync(*World);

        if (SyncSlowMs > 0.0f)
        {
            const double ElapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - SyncStart).count();
            if (ElapsedMs >= (double)SyncSlowMs)
            {
                LOG_WARN("ScenePrimitiveSet::Sync took {:.2f}ms for {} primitives.",
                         ElapsedMs, ScenePrimitives.Num());
            }
        }

        PublishRetainedUpload();
    }

    // Collects what changed in the retained scene, before the render phase uploads it.
    void FForwardRenderScene::PublishRetainedUpload()
    {
        FFrameData::FGeometry::FRetainedUpload& Out = ExtractFrame->Geometry.RetainedUpload;

        const uint32 SlotCount = ScenePrimitives.GetRetainedSlotCount();
        Out.SlotCount = SlotCount;

        Out.DirtySlots.clear();
        Out.DirtyStaticSlots.clear();

        // A full re-send is needed exactly when the device allocation is about to be replaced, because a
        // replacement loses the accumulated contents. That happens when the slot count outgrows the capacity
        // the render phase last published; that value only ever grows.
        const uint32 DeviceCapacity = RetainedDeviceCapacity.load(std::memory_order_acquire);
        Out.bFull = ScenePrimitives.NeedsFullInstanceUpload() || SlotCount > DeviceCapacity;

        // The raw count upper-bounds the deduped one, so a list already past the threshold below can be
        // rejected without the filter-copy and sort. Sync's MarkInstanceDirty normally decides this much
        // earlier and hands us an empty list; this covers what lands under its floor.
        if (!Out.bFull
            && (ScenePrimitives.GetDirtyInstanceSlots().size() * 4 >= (SIZE_T)SlotCount
                || ScenePrimitives.GetDirtyStaticSlots().size() * 4 >= (SIZE_T)SlotCount))
        {
            Out.bFull = true;
        }

        if (!Out.bFull)
        {
            // Sort and dedupe so the upload can coalesce runs of adjacent slots into ONE copy each.
            // The channel legitimately repeats a slot (a re-bind frees then re-stamps it) and hands them out
            // in whatever order the sweep hit them.
            //
            // Both lists get the same treatment, and either one crossing the threshold takes the whole
            // upload full -- the buffers share one device capacity and one bFull decision, so there is no
            // meaning to re-sending two of three wholesale.
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

            // Past a quarter of the scene, one contiguous upload beats scattered per-run copies outright.
            // This matters because "incremental" is badly named for a material edit: the resolve entry is
            // shared, so invalidating one mesh dirties EVERY primitive drawing it. Uploading that as
            // thousands of separate ranges was O(dirty) transient allocations, each one an insert into the
            // RHI's block list -- which is quadratic and hung the frame outright.
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
        /**
         * True when this component's cached copy of its resolve entry is still current.
         *
         * Two independent things can invalidate it, and both are checked here because they fail in
         * different ways:
         *  - the component now points at a DIFFERENT mesh (CachedMeshKey). The gather refuses to draw
         *    anything whose key disagrees with its live mesh, so missing this strands the component --
         *    it bails every frame and nothing ever frees it.
         *  - the ENTRY it copied from was rebuilt (CachedEntryState). Its bounds, meshlet header and
         *    surface list all came from that entry and are now wrong.
         *
         * The second test is one indexed load from FMeshResolveCache's dense state mirror. It used to be
         * a comparison against the cache's global epoch, which meant any asset finishing its load failed
         * this gate for EVERY mesh component in the world.
         */
        template <typename TComponent>
        FORCEINLINE bool IsResolveCurrent(const TComponent& Component, const CMesh* Mesh,
                                          const FMeshResolveCache& Cache)
        {
            if (Component.CachedMeshKey != (const void*)Mesh
                || Component.CachedEntryState == MESH_RESOLVE_STATE_STALE)
            {
                return false;
            }

            // No handle is only settled when there is genuinely nothing to resolve. A non-null mesh with
            // no handle failed to resolve and has to keep retrying.
            if (Component.ResolveHandle == INVALID_MESH_RESOLVE_HANDLE)
            {
                return Component.CachedEntryState == MESH_RESOLVE_STATE_NO_MESH;
            }

            return Component.CachedEntryState == Cache.GetEntryState(Component.ResolveHandle);
        }

        /**
         * FNV-1a over the override list the resolve is keyed on.
         *
         * A direct write to MaterialOverrides from C++ or C# signals nothing, and the resolve entry is
         * interned by (mesh, overrides) -- so without this the component keeps its old handle forever.
         * The global epoch used to cover this only by accident: the component re-resolved the next time
         * any unrelated asset happened to load. Checking the content is both cheaper and actually correct.
         *
         * 0 is the never-resolved sentinel, so a real hash must not land on it.
         */
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
                Component.CachedMeshletHeaderAddress = 0;
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
            Component.CachedMeshletHeaderAddress = Entry.MeshletHeaderAddress;

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

        // Returns how many components were actually re-resolved. In a settled scene that is zero even
        // while the pass is running, which is the number to watch: it used to be the whole pool every
        // time any asset in the process finished loading.
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

                // This pass visits exactly the components whose resolve was stale, for every reason the
                // cache tracks (material recompiled, mesh GPU buffers landed, slot override edited, epoch
                // bumped, mesh assigned directly). Routing them here is what keeps those invalidations
                // flowing into the render scene without a second change-detection mechanism.
                Tracker.Mark(Entity, Source, EPrimitiveDirty::Data);
                ++Refreshed;
            }

            return Refreshed;
        }
    }

    // Serial: runs before the parallel gather so workers only read the resolve table.
    // Dynamic meshes own their resolve outright, so they never go through FMeshResolveCache and its
    // pending-work generation. That is why this is NOT part of ResolveDirtyMeshComponents: that pass
    // early-returns unless the generation moved, so anything routed through it only runs when the cache
    // says so -- and a material swapped for an already-compiled one moves nothing.
    //
    // Detection is by CONTENT, not by notification. The signals (PostEditChange, SetMaterialAtSlot,
    // MarkRenderStateDirty) all work, but this has to hold even when nothing signals: a direct field write
    // from C++ or C#, or an editor path that reports the edit against the array element rather than the
    // owning component. Comparing the resolved materials against what the component last resolved with
    // costs O(surfaces) per dynamic mesh per frame, and dynamic meshes are counted in the tens.
    void FForwardRenderScene::ResolveDynamicMeshMaterials(FEntityRegistry& Registry, FRenderDirtyTracker& Tracker)
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

            // One atomic ref for the whole hash: Commit can swap this from a worker, so re-reading the
            // member per surface could walk two different snapshots (or one being freed).
            const TSharedPtr<FDynamicMeshRenderData> Data = Component.LoadRenderData();
            if (!Data)
            {
                continue;
            }

            // FNV-1a over the material each surface would resolve to right now. GetMaterialForSlot folds in
            // the override AND the fallback, so this changes exactly when the outcome would change.
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

            if (Hash == Component.CachedMaterialHash && Data->bAllMaterialsReady)
            {
                continue;
            }

            Component.RefreshResolvedMaterials();
            Component.CachedMaterialHash = Hash;

            // RefreshResolvedMaterials rewrites RenderData->Surfaces IN PLACE, so the Surfaces pointer that
            // PollUnhookedSources compares is unchanged and cannot see this. Without an explicit mark the
            // primitive is never re-bound and the retained instance keeps its original MaterialIndex/batch.
            Tracker.Mark(Entity, EPrimitiveSource::DynamicMesh, EPrimitiveDirty::Data);

            if (!Data->bAllMaterialsReady)
            {
                // Still compiling: come back next frame. The hash is already stored, so the retry is driven
                // by bAllMaterialsReady rather than by the hash differing again.
                FMeshResolveCache::MarkPendingWork();
            }
        }
    }

    void FForwardRenderScene::SettleResolveWork(int32 MaxIterations)
    {
        // ResolveDirtyMeshComponents re-marks pending work for anything it could not finish (a material
        // still compiling, GPU buffers not landed), which normally means "retry next frame". Loop until
        // a pass adds nothing new. Bounded: something genuinely unresolvable must not spin forever, and
        // rendering it half-ready is still better than not rendering at all.
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

    void FForwardRenderScene::ResolveDirtyMeshComponents()
    {
        // Per-scene watermark, not a shared flag: this scene owns one world, and every other world's
        // scene runs this same pass. Consuming a global flag here starved theirs (see MarkPendingWork).
        const uint32 PendingGeneration = FMeshResolveCache::GetPendingGeneration();
        if (PendingGeneration == LastResolvedPendingGeneration)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        // Sampled up front; anything that fails to fully resolve below re-marks, pushing the generation
        // past this value so the next frame runs again.
        LastResolvedPendingGeneration = PendingGeneration;

        FMeshResolveCache& Cache = FMeshResolveCache::Get();

        // Turns the queued "this asset changed" keys into per-entry staleness, before anything reads a
        // token. Every gate below is a comparison against those tokens, so this has to run first.
        Cache.ApplyPendingInvalidations();

        // Compared at the end. It moves only when an ALREADY-RESOLVED entry is rebuilt, which is the only
        // case the primitive set's O(primitives) sweep exists for. Interning a newly added mesh does not
        // move it, so adding meshes no longer drags the whole scene through that sweep.
        const uint32 TableGenerationBefore = Cache.GetTableGeneration();

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        TVector<CMaterialInterface*>& Scratch = ResolveOverrideScratch;
        FRenderDirtyTracker& Tracker = FRenderDirtyTracker::Ensure(Registry);

        uint32 Refreshed = ResolveMeshPool(Registry.storage<SStaticMeshComponent>(), EInstanceFlags::None, Scratch,
            [](const SStaticMeshComponent& C) -> CMesh* { return C.StaticMesh; },
            Tracker, EPrimitiveSource::StaticMesh);


        // Skeletal assets always carry FSkinnedVertex, so Skinned is unconditional here.
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
                    Type.CachedMeshletHeaderAddress = 0;
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
                Type.CachedMeshletHeaderAddress = Entry.MeshletHeaderAddress;
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

        // Belt and braces on top of the per-component marks above: a shared entry can be rebuilt by
        // ANOTHER world's resolve pass, whose components this one never visits. Gated, so the sweep is
        // paid on a real rebuild rather than on every frame anything is streaming.
        const uint32 EntriesRebuilt = Cache.GetTableGeneration() - TableGenerationBefore;
        if (EntriesRebuilt != 0)
        {
            ScenePrimitives.NotifyResolveTableChanged();
        }

        // Both should sit at zero once a scene settles, and should track what actually changed while it
        // is loading. Either one scaling with the SIZE of the scene rather than with the change is the
        // signature of a global invalidation leaking back in.
        LUMINA_PROFILE_VALUE("Resolve/ComponentsRefreshed", (int64)Refreshed);
        LUMINA_PROFILE_VALUE("Resolve/EntriesRebuilt",      (int64)EntriesRebuilt);
    }

    void FForwardRenderScene::CompileDrawCommands_Extract()
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Render Scene");

        FFrameData& Frame = *ExtractFrame;
        auto& Instances              = Frame.Geometry.Instances;
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

            // The mesh pools are no longer walked here at all: membership lives in ScenePrimitives and
            // is maintained from the dirty channel (see SyncScenePrimitives).

            ECS::Utils::ResolveAllDirtyTransforms(Registry);

            // Unconditional: dynamic-mesh materials are polled by content, not gated on the resolve
        // cache's generation (see ResolveDynamicMeshMaterials).
        {
            FEntityRegistry& DynRegistry = ECS::GetWorldRegistry(*World);
            ResolveDynamicMeshMaterials(DynRegistry, FRenderDirtyTracker::Ensure(DynRegistry));
        }

        ResolveDirtyMeshComponents();

            // Apply this frame's changes to the persistent primitive table. O(changed): on a frame where
            // nothing moved and nothing was added or removed, this is one atomic load and a return.
            SyncScenePrimitives();

            // Per-frame CPU reject volumes built before parallel gather so workers query lock-free.
            BuildSceneCullContext();

            // Geometry is recompiled every frame now. It used to be gated on a serial that hashed the
            // camera matrices to detect "nothing changed, reuse this slot" -- which only made sense while
            // the CPU culled. The gather is skinned-only (rigid primitives are culled and compacted on the
            // GPU) and the serial force-disabled reuse whenever any skinned primitive existed, so it could
            // only ever skip work that was already empty, while hashing two 4x4 matrices a byte at a time
            // every frame to decide that.
            ResetGeometry_Extract();

            const uint32 NumPrimitives     = ScenePrimitives.Num();
            const size_t EstimatedProxies  = (size_t)NumPrimitives * 2;

            Instances.reserve(EstimatedProxies);
            DrawCommands.reserve(EstimatedProxies);

            const uint32 NumThreads = GTaskSystem->GetNumTaskThreads();

            // Persistent: outer storage keeps capacity, slots are (re)bound to per-worker thread frame
            // arenas lazily on each worker's first gather touch. NumThreads is process-constant, grows once.
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

            // SKELETAL MESHES ONLY. Every other source is culled on the GPU by CullInstances.slang off the
            // retained arrays; skeletal primitives are excluded from that cull (their FGPUInstance fields
            // can't be filled at bind time), so this is what draws them -- and what gathers their bone rows,
            // pre-skin sizing and last-rendered feedback, none of which can be retained because the pose
            // changes every frame.
            //
            // With none in the scene the node is pure cost: a fan-out over every primitive that rejects all
            // of them. Skip it and let the merge run on the empty thread-local data (it early-outs).
            {
                FTaskGraph::FNodeHandle MergeNode = Graph.Add([&]
                {
                    MergeMeshDrawData(ThreadLocal);
                }, ETaskPriority::High);

                if (ScenePrimitives.GetSkinnedPrimitiveCount() > 0)
                {
                    FTaskGraph::FNodeHandle CullNode = Graph.AddParallelFor(NumPrimitives, ResolveGrain(CVarPrimitiveGrain), [&](const Task::FParallelRange& Range)
                    {
                        LUMINA_PROFILE_SECTION("Cull And Emit Primitives");
                        FThreadLocalDrawData& Local = AcquireThreadLocalDrawData(Range.Thread);
                        CullAndEmitPrimitives(Range, Local);
                    }, ETaskPriority::High); // critical path: MergeNode waits on this, so it runs ahead of emitters

                    Graph.AddDependency(MergeNode, CullNode);
                }
            }

            // Kick the mesh critical path NOW: the workers chew on the gather while Extract
            // builds the emitter graph below (lights/primitives/extract snapshots), whose tasks share
            // no state with the mesh nodes. This pulls that build time off the serial window.
            Graph.Dispatch();

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

            // Triangles are low-volume; one node, Medium priority so it overlaps the mesh fan-out without
            // stealing the first workers from the High critical-path nodes that gate MergeNode.
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

                    const FMatrix4 World = TransformStorage.get(Entity).GetWorldMatrix();
                    const FVector3 Center = FVector3(World[3]);
                    const float ScaleXY = Math::Max(Math::Length(FVector3(World[0])), Math::Length(FVector3(World[1])));
                    const float Radius  = 0.5f * Math::Length(WidgetComponent.WorldSize) * Math::Max(1.0f, ScaleXY);

                    Runtime.bVisible = !bCullWidgets || WidgetFrustum.IntersectsSphere(Center, Radius);

                    if (!Runtime.bVisible || Runtime.ResourceID < 0)
                    {
                        return;
                    }

                    FWidgetInstance& Inst = WidgetInstances.emplace_back();
                    Inst.Transform    = World;
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

                    const FMatrix4 World  = TransformStorage.get(Entity).GetWorldMatrix();
                    const FVector3 Origin = FVector3(World[3]);

                    const float HAlign = (TextComponent.HorizontalAlign == ETextHorizontalAlign::Left)   ? 0.0f
                                       : (TextComponent.HorizontalAlign == ETextHorizontalAlign::Center) ? 0.5f : 1.0f;
                    // Top places the text above the origin, Bottom below (block bottom/top anchored at origin).
                    const float VAlign = (TextComponent.VerticalAlign == ETextVerticalAlign::Top)        ? 1.0f
                                       : (TextComponent.VerticalAlign == ETextVerticalAlign::Middle)     ? 0.5f : 0.0f;

                    // Reshape only when an input that affects layout changed (text/font/align/spacing). Color,
                    // size, billboard and transform are applied per-frame below and never invalidate the cache.
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

                        // Cache the bounding extent (em units) for the cull sphere alongside the glyphs, so the
                        // per-frame path skips this scan too.
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

                    // Frustum cull on a bounding sphere sized from the SHAPED extent (em units * WorldSize),
                    // so long / multi-line text isn't culled by a fixed radius that ignores its real width.
                    if (bCullText && !TextFrustum.IntersectsSphere(Origin, Cache.EmExtent * TextComponent.WorldSize * 1.5f))
                    {
                        return;
                    }

                    // World axes for the text plane. Oriented text uses the entity's X/Y (Y is up, matching
                    // the widget oriented convention); billboard text uses the camera's right/up.
                    FVector3 RightDir, UpDir;
                    if (TextComponent.bBillboard)
                    {
                        RightDir = CamRight;
                        UpDir    = CamUp;
                    }
                    else
                    {
                        RightDir = Math::Normalize(FVector3(World[0]));
                        UpDir    = Math::Normalize(FVector3(World[1]));
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
                    Billboard.Position              = TransformStorage.get(Entity).WorldTransform.GetLocation();
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
                        EmplaceVisualizer(Entity, TransformStorage.get(Entity).WorldTransform.GetLocation(), ENamedImage::CameraIcon, FColor::White);
                    });

                    CharacterView.each([&](entt::entity Entity, SCharacterControllerComponent&)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.get(Entity).WorldTransform.GetLocation(), ENamedImage::CharacterIcon, FColor::White);
                    });

                    PointLightView.each([&](entt::entity Entity, const SPointLightComponent& Light)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.get(Entity).WorldTransform.GetLocation(), ENamedImage::PointLightIcon, FVector4(Light.LightColor, 1.0f));
                    });

                    SpotLightView.each([&](entt::entity Entity, const SSpotLightComponent& Light)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.get(Entity).WorldTransform.GetLocation(), ENamedImage::SpotLightIcon, FVector4(Light.LightColor, 1.0f));
                    });

                    DirectionalView.each([&](entt::entity Entity, const SDirectionalLightComponent& Light)
                    {
                        const auto& Transform = Registry.get<STransformComponent>(Entity);
                        EmplaceVisualizer(Entity, Transform.WorldTransform.GetLocation(), ENamedImage::DirectionalLightIcon, FVector4(Light.Color, 1.0f));
                    });

                    SkyLightView.each([&](entt::entity Entity, const SSkyLightComponent&)
                    {
                        const auto& Transform = Registry.get<STransformComponent>(Entity);
                        EmplaceVisualizer(Entity, Transform.WorldTransform.GetLocation(), ENamedImage::SkyLightIcon, FVector4(1.0f));
                    });

                    ParticleView.each([&](entt::entity Entity, const SParticleSystemComponent&)
                    {
                        EmplaceVisualizer(Entity, TransformStorage.get(Entity).WorldTransform.GetLocation(), ENamedImage::ParticleSystemIcon, FVector4(1.0f));
                    });
                }
                #endif
            }, ETaskPriority::High);

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

                    // Consumed once for the whole system and then stamped onto every emitter. Reading them
                    // per emitter would let the first emitter clear a burst the rest never saw.
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
                        // A disabled emitter extracts nothing at all, but still holds its index: slots stay
                        // aligned with PS->Emitters, so muting one does not renumber the others' GPU state.
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
                        // Filled unconditionally: only the custom-shader branch below resolves real slots,
                        // and a partially-initialised array would leave 0 (a valid slot) meaning "undeclared".
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
                                // Stamp the component's live parameter values over the slots the module
                                // stack bound. This is where ParticleComponent.SetFloat("Size", x) from
                                // C++/C# actually reaches the simulation: the asset supplies the authored
                                // constants, the component overrides the bound ones on the way to the GPU.
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
                // Resolve RHI shaders here (material is alive); the batch stores refs, never the
                // CMaterial*, so deleting the decal asset can't dangle the render phase.
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

            // Screen-space debug text (World::DrawDebugText): drain the queued lines and lay them out top-left
            // in pixels (the debug pass converts to NDC). Default font, single batch. Dev/Debug only. Runs here,
            // after both dispatches, so the serial shaping overlaps the parallel gather.
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
                    // Fixed pixel size in the (fixed-resolution) world RT, so the text is a consistent size
                    // regardless of viewport aspect/size.
                    const float PxSize = 32.0f;   // pixels per em
                    const float Margin = 12.0f;
                    float       PenY   = Margin;

                    TVector<FShapedGlyph> DebugShaped;
                    for (const FDebugTextLine& Line : DebugLines)
                    {
                        const uint32 Color = PackColor(Line.Color);
                        if (DebugFont->ShapeText(Line.Text, 0.0f /*left*/, 0.0f, 1.0f, DebugShaped))
                        {
                            // ShapeText anchors the first line's top near em y=0 and stacks downward (em y<=0);
                            // larger em y = higher on screen, so screen Y = PenY - em*PxSize.
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

                // Local, assigned once below: the render phase reads RenderSettings.bHasEnvironment live,
                // and a transient false (reset-then-set) makes SkyCubeCapturePass clear the IBL cubes.
                bool bHasEnvironment           = false;
                RenderSettings.bSSAO           = false;
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

                    // Only honor HDRI assignment when SkyMode == HDRI; otherwise IBL would convolve
                    // from HDRI even though visible sky is procedural.
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

                    // Yaw resolved to cos/sin once here so the sky pass and the equirect->cube bake read
                    // identical values; they must agree or reflections slide against the background.
                    const float HDRIYaw = Math::Radians(Env.HDRIRotation);
                    EnvironmentParams.HDRIParams    = FVector4(Math::Max(Env.HDRIIntensity, 0.0f),
                                                                std::cos(HDRIYaw),
                                                                std::sin(HDRIYaw),
                                                                0.0f);
                });

                RenderSettings.bHasEnvironment = bHasEnvironment;

                // Resolve the IBL bake resolution; the render phase rebuilds the cubes when it changes
                // (handled alongside the dirty-flag bookkeeping below). Keep the last tier if no env.
                Frame.Volumetrics.IBLResolution = ActiveEnv
                    ? ResolveIBLQuality(ActiveEnv->IBLQuality)
                    : LastExtractedIBLResolution;
            }

            // Skylight (ambient fill + IBL scale). Last enabled SSkyLightComponent wins.
            // bAmbientFromSky derives the color from the active sky (needs ActiveEnv + SunDirection).
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

                // IBL cubes are only baked when an environment is present (otherwise cleared to black). Tell
                // the shader so a skylight-only scene falls back to a flat ambient instead of black.
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
                        BaseHeight += Transform->WorldTransform.GetLocation().y;
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

        // Populates CullViews[]/IndirectArgs[]; runs after AllocateShadowTiles so shadow VPs are settled.
        // Use the snapshot's view volume -- SceneViewport is render-phase state and may be the capture's.
        BuildCullViews(ExtractFrame->ViewVolume);
    }

    void FForwardRenderScene::CompileDrawCommands_Render(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Render Scene");

        FFrameData& Frame = *RenderFrame;
        auto& SceneGlobalData            = Frame.SceneGlobalData;
        const auto& Instances            = Frame.Geometry.Instances;
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
        // Frame tag stamped into every meshlet draw-list entry this frame (see FMeshletDraw). Cycles through
        // 1..4095 -- never 0, because a freshly allocated draw list is zeroed and untouched memory must
        // never pass the check. A monotonic counter rather than CurrentFrameSlot: a stale entry sitting in
        // this slot is from kFramesInFlight frames ago and would carry the slot's own value.
        SceneGlobalData.CullData.MeshletDrawTag    = (MeshletDrawTagCounter++ % 4095u) + 1u;

        // Sizes for the persistent per-frame buffers (all other CPU-dynamic data is transient): the
        // GPU-written pre-skinned vertex buffer. Debug line/triangle geometry is ring-allocated at draw time.
        const SIZE_T PreSkinnedSize       = Math::Max<SIZE_T>(sizeof(FPreSkinnedVertex),
                                            (SIZE_T)Frame.Geometry.TotalPreSkinnedVertices * sizeof(FPreSkinnedVertex));

        // Shared draw list. Regions are packed per (view, draw), each sized on the GPU to exactly what that
        // pair can emit, so the requirement is the SUM over views of what each view really needs -- not
        // NumViews copies of the global worst case. BuildDrawPrefix publishes that sum as Totals[2] and this
        // sizes from the value fed back kFramesInFlight frames ago.
        //
        // The NumCullViews multiply that used to be here is exactly what made this allocation enormous: a
        // shadow cascade that rejects almost everything was reserving as much room as the camera.
        UpdateMeshletBoundFeedback(CurrentFrameSlot);

        // Every stage of the GPU cull already publishes its count into Totals and the CPU already reads
        // them back -- but nothing logged them except on overflow, so "nothing renders" gave no clue
        // WHICH stage emptied. Reading one of these lines localizes it immediately:
        //   visible == 0                  -> CullInstances rejected everything (frustum/distance/flags)
        //   visible > 0, domain == 0      -> instances survived but carry no meshlets (LOD counts/geometry flag)
        //   domain > 0, drawlist == 0     -> CullMeshlets rejected every meshlet (frustum/cone/occlusion)
        //   drawlist > 0 but blank screen -> the cull is fine; the fault is in raster/pipeline/material
        // Values lag by kFramesInFlight (they are the readback), which does not matter for a steady scene.
        if (const int32 StatsEvery = CVarCullStats.GetValue(); StatsEvery > 0)
        {
            static uint32 CullStatsCounter = 0;
            if ((CullStatsCounter++ % (uint32)StatsEvery) == 0u)
            {
                LOG_DISPLAY("Cull: {} retained -> {} visible instances -> {} meshlet pairs (clamped {}) -> "
                            "{} draw-list entries. Overflow: instances {}, drawlist {}, defer {}.",
                            ScenePrimitives.GetRetainedSlotCount(), LastVisibleInstances,
                            LastMeshletWorkRequested, LastMeshletBound, LastDrawListRequired,
                            LastVisibleOverflowed, LastDrawListOverflowed, LastDeferOverflowed);
            }
        }

        // Lead the measured requirement by 50%: undershooting costs a frame of dropped meshlets,
        // overshooting only costs address space, so the asymmetry says bias high.
        const uint32 PredictedBound    = Math::Max(LastMeshletBound + LastMeshletBound / 2u, 65536u);
        const uint32 PredictedDrawList = Math::Max(LastDrawListRequired + LastDrawListRequired / 2u, 65536u);
        const SIZE_T MeshletDrawListSize = Math::Max<SIZE_T>(
            sizeof(uint32) * 2,
            (SIZE_T)PredictedDrawList * sizeof(uint32) * 2);

        // Shared indirect args: NumViews * NumDraws FDrawIndirectArguments, GPU-seeded below.
        const SIZE_T NumArgSlots = (SIZE_T)NumCullViews * (SIZE_T)NumDraws;
        const SIZE_T IndirectArgsSize = Math::Max<SIZE_T>(
            sizeof(RHI::FDrawIndirectArguments),
            NumArgSlots * sizeof(RHI::FDrawIndirectArguments));

        // Mesh-task args: one FDrawMeshTasksIndirectArguments per arg slot (same count, 12B stride).
        const SIZE_T MeshDrawArgsSize = Math::Max<SIZE_T>(
            sizeof(RHI::FDrawMeshTasksIndirectArguments),
            NumArgSlots * sizeof(RHI::FDrawMeshTasksIndirectArguments));

        // Deferred meshlets, sized from measured demand (Totals[6]) with the meshlet work domain as the
        // ceiling -- a meshlet can only be deferred once, so it can never exceed the domain. Sizing this
        // purely off PredictedBound was what let a stale HZB (every meshlet occluded, e.g. resuming after a
        // shader-compile stall) overflow it, and the append had no bound to catch it.
        const uint32 PredictedDefer = Math::Min(
            Math::Max(LastDeferRequested + LastDeferRequested / 2u, 65536u),
            Math::Max(PredictedBound, 65536u));
        const SIZE_T DeferListSize = Math::Max<SIZE_T>(
            sizeof(uint32) * 4,
            (SIZE_T)PredictedDefer * sizeof(uint32) * 4);

        // Buffers are reached only by device address; a resize swaps the allocation (old one
        // retires on this slot's deferred-free list) with no descriptor work. Only the current
        // frame slot is sized -- the others are untouched by this frame's GPU work and get
        // sized to their own frame's needs when their turn comes around.
        ResizeBufferIfNeeded(PreSkinnedVerticesBuffer, PreSkinnedSize, 1.2f, PreSkinnedVerticesLowUsage);
        {
            const uint8 Slot = CurrentFrameSlot;
            ResizeBufferIfNeeded(IndirectArgsRing[Slot], IndirectArgsSize, 1.2f, IndirectArgsRingLowUsage[Slot]);
            ResizeBufferIfNeeded(MeshDrawArgsRing[Slot], MeshDrawArgsSize, 1.2f, MeshDrawArgsRingLowUsage[Slot]);
            const RHI::GPUPtr PrevDrawList = MeshletDrawListRing[Slot].Ptr;
            ResizeBufferIfNeeded(MeshletDrawListRing[Slot], MeshletDrawListSize, 1.2f, MeshletDrawListRingLowUsage[Slot]);
            if (MeshletDrawListRing[Slot] && MeshletDrawListRing[Slot].Ptr != PrevDrawList)
            {
                // A new allocation holds undefined bytes, which could pass the frame-tag check by chance
                // (1 in 4095 per entry). Zeroing once on (re)allocation makes tag 0 mean "never written"
                // for the life of the buffer, so the check is exact rather than probabilistic. Rare enough
                // to be free -- only growth, and growth is geometric.
                RHI::CmdMemset(CL, MeshletDrawListRing[Slot].Ptr, MeshletDrawListRing[Slot].GetSize(), 0u);
            }
            // From the allocation we got, not the size we asked for: a failed or short allocation must
            // shrink the clamp, not be trusted.
            DrawListCapacity = (uint32)Math::Min<uint64>(MeshletDrawListRing[Slot].GetSize() / (sizeof(uint32) * 2), 0xFFFFFFFFull);

            // The early cull's indirect dispatch args used to be zeroed here, guarding a real hazard:
            // BuildDrawPrefix was the only writer and lived behind different conditions than the
            // dispatch, so a disagreement fed a garbage group count to vkCmdDispatchIndirect and hung
            // the device. The early cull now dispatches a CPU-sized grid, so there is no such buffer
            // and no such hazard. The late cull keeps its indirect dispatch, bounded by DeferListCapacity.

            // Draw-list overflow, reported by the GPU itself rather than inferred: BuildDrawPrefix sets
            // Totals[3] when the packed requirement exceeded the allocation it was given, which is the only
            // condition that can still drop a meshlet now that each region is exactly sized.
            if (LastVisibleOverflowed != 0u)
            {
                static uint32 InstanceOverflowLogCounter = 0;
                if ((InstanceOverflowLogCounter++ % 60u) == 0u)
                {
                    LOG_WARN("RenderScene: visible-instance buffer too small -- {} instances requested, "
                             "capacity {}. Whole instances were dropped; the next allocation grows.",
                             LastVisibleInstances, FrameVisibleInstanceCapacity);
                }
            }

            if (LastDrawListOverflowed != 0u)
            {
                static uint32 OverflowLogCounter = 0;
                if ((OverflowLogCounter++ % 60u) == 0u)
                {
                    LOG_WARN("RenderScene: meshlet draw list too small -- required {} entries, allocation holds {} "
                             "({} views x {} draws). Meshlets were dropped; the next allocation grows.",
                             LastDrawListRequired, DrawListCapacity, NumCullViews, NumDraws);
                }
            }

            // The GPU-measured domain, not the CPU ceiling that bounds it. That ceiling is a worst case
            // (retained slots x the densest LOD any ONE surface has), so a single dense surface makes it
            // enormous while real demand stays small -- it used to log an error on that product alone,
            // which said meshlets were dropped on frames where nothing was.
            if (LastMeshletWorkClamped != 0u)
            {
                static uint32 DomainClampLogCounter = 0;
                if ((DomainClampLogCounter++ % 60u) == 0u)
                {
                    LOG_ERROR("RenderScene: meshlet cull domain truncated -- {} (instance, meshlet) pairs measured, "
                              "ceiling {}. Meshlets are being dropped off the tail of the domain.",
                              LastMeshletWorkRequested, (uint64)GMaxMeshletCullDomain);
                }
            }
            ResizeBufferIfNeeded(MeshletDeferListRing[Slot], DeferListSize, 1.2f, MeshletDeferListRingLowUsage[Slot]);
            // From the allocation we got, not the size asked for -- the shader bounds its append with this.
            DeferListCapacity = (uint32)Math::Min<uint64>(MeshletDeferListRing[Slot].GetSize() / (sizeof(uint32) * 4), 0xFFFFFFFFull);

            if (LastDeferOverflowed != 0u)
            {
                static uint32 DeferOverflowLogCounter = 0;
                if ((DeferOverflowLogCounter++ % 60u) == 0u)
                {
                    LOG_WARN("RenderScene: meshlet defer list too small -- {} deferred, capacity {}. Those "
                             "meshlets skip the late HZB re-test for one frame; the next allocation grows.",
                             LastDeferRequested, DeferListCapacity);
                }
            }

            // Visible-instance capacity, derived ONCE here and reused everywhere for the rest of the
            // frame so the buffer, the prefix allocation and the GPU-side clamps cannot disagree.
            //
            // This used to be "every retained slot could survive" -- 64 MB per frame slot at 500k
            // primitives, for a buffer that in practice holds the visible set. It is now sized from the
            // demand BuildDrawPrefix measured (Totals[4], which counts past the cap on an overflow frame,
            // so it is real demand and not just what fit), on the same feedback the draw list uses.
            const uint32 NumSkinnedHead = (uint32)Instances.size();
            // Lead the measurement by 50%. Undershooting drops whole instances, which is far more visible
            // than dropping meshlets, so bias high.
            const uint32 PredictedVisible = LastVisibleInstances + LastVisibleInstances / 2u;
            // Floors: the CPU-written skinned head is copied in unconditionally, so capacity must always
            // cover it, plus headroom for a scene that is only just starting to populate.
            uint32 VisibleCapacityWanted = Math::Max(PredictedVisible,
                                                     Math::Max(NumSkinnedHead + 1024u, 4096u));

            // Ceiling: the cull appends at most one survivor per active retained slot, so the old
            // worst-case figure becomes the upper bound rather than the default.
            const uint32 VisibleCapacityMax = Math::Max(Frame.Geometry.RetainedUpload.SlotCount + NumSkinnedHead, 1u);

            // Until a measurement exists, start AT the ceiling rather than at the floor. Feedback lags by
            // kFramesInFlight frames, so guessing low on a freshly loaded 500k-primitive scene would drop
            // most of it for the first few frames -- a visible pop-in. Starting high and letting the
            // low-usage reclaim shrink it converges from the correct side.
            if (LastVisibleInstances == 0u)
            {
                VisibleCapacityWanted = VisibleCapacityMax;
            }

            FrameVisibleInstanceCapacity = Math::Min(VisibleCapacityWanted, VisibleCapacityMax);

            // The visible-instance buffer is sized HERE, not in DispatchGPUSceneCull, because the SceneRoot
            // published below bakes in its device address. ResizeBufferIfNeeded swaps the allocation on
            // growth and retires the old one, so sizing it after the publish left every shader dereferencing
            // a freed address the moment the scene grew -- an MMU read fault in whichever pass touched
            // Instances() first. Anything the SceneRoot points at must reach its final size before the
            // publish; nothing downstream may resize it again.
            ResizeBufferIfNeeded(VisibleInstanceRing[Slot],
                                 (SIZE_T)FrameVisibleInstanceCapacity * sizeof(FGPUInstance), 1.25f,
                                 VisibleInstanceLowUsage[Slot]);

            // GPU-built per-instance meshlet prefix: N+1 uints (last entry = total). N is the GPU's
            // survivor count, so this has to cover the whole capacity CullInstances may append into --
            // sizing it from the CPU-written skinned head would be a guaranteed overrun.
            const SIZE_T InstancePrefixSize = Math::Max<SIZE_T>(
                sizeof(uint32),
                ((SIZE_T)FrameVisibleInstanceCapacity + 1) * sizeof(uint32));
            ResizeBufferIfNeeded(InstancePrefixRing[Slot], InstancePrefixSize, 1.2f, InstancePrefixRingLowUsage[Slot]);

            // Per-block sums for the hierarchical scan: one per 256-element block, plus the grand total.
            // Must match SCAN_BLOCK in the ScanPrefix* shaders.
            constexpr uint32 kScanBlock = 256u;
            const SIZE_T ScanBlockSumSize = Math::Max<SIZE_T>(
                sizeof(uint32) * 2,
                ((SIZE_T)((FrameVisibleInstanceCapacity + kScanBlock - 1u) / kScanBlock) + 1) * sizeof(uint32));
            ResizeBufferIfNeeded(ScanBlockSumRing[Slot], ScanBlockSumSize, 1.2f, ScanBlockSumLowUsage[Slot]);
        }

        // The VisBuffer resolve (MaterialDepth / DeferredMaterial) recovers a meshlet-draw slot
        // and an instance id from each covered texel, and bounds-checks both so a stale draw-list gap can
        // never page-fault a device-address load. Both bounds used to be exact per-frame counts the CPU
        // computed during the gather. The GPU owns those counts now, so the bound becomes the CAPACITY of
        // the allocation the index addresses -- which is both the correct meaning of "past the end" and
        // the only version the CPU still knows. Publishing the stale CPU counts here (zero, and the
        // skinned-only instance head) is what made the resolve classify every covered pixel as background:
        // geometry rastered into depth and the VisBuffer, then nothing shaded it.
        SceneGlobalData.CullData.MeshletDrawListCapacity = DrawListCapacity;
        SceneGlobalData.CullData.InstanceNum             = FrameVisibleInstanceCapacity;
        // Published for the same reason as the two above: the bone index SkinVertex builds comes partly
        // from vertex data, so nothing upstream of the shader bounds it.
        SceneGlobalData.CullData.BoneNum                 = (uint32)BonesData.size();

        {
            LUMINA_PROFILE_SECTION_COLORED("Write Scene Buffers", tracy::Color::OrangeRed3);

            // Env/fog params are uploaded to the transient ring at their pass sites (Environment /
            // VolumetricFog*); here we only track changes to gate the costly IBL convolution below.
            const bool bEnvParamsChanged = !bEnvironmentParamsUploaded || std::memcmp(&EnvironmentParams, &LastUploadedEnvironmentParams, sizeof(FEnvironmentParams)) != 0;
            if (bEnvParamsChanged)
            {
                LastUploadedEnvironmentParams = EnvironmentParams;
                bEnvironmentParamsUploaded   = true;
            }

            const bool bSunChanged = LastIBLSunDirection != LightData.SunDirection || bLastIBLHasSun != (LightData.bHasSun != 0);
            const bool bMapChanged = LastIBLEnvironmentMapID != EnvironmentMapID;

            // Quality-tier change forces a full re-bake into the freshly-sized cubes (recreated on
            // the render phase by SyncIBLResolution before SkyCubeCapturePass runs).
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

            // Gate the costly irradiance + GGX prefilter on an angular sun threshold so
            // TOD animation doesn't pay full convolution every frame. cos(0.5 deg) ~ 0.99996.
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
            // Instances are GPU-produced now: CullInstances writes the compacted survivors into this
            // slot's visible buffer (past the CPU-written skinned head), so there is nothing to upload
            // and nothing whose size the CPU knows.
            if (VisibleInstanceRing[CurrentFrameSlot])
            {
                SceneRootShared.Instances = VisibleInstanceRing[CurrentFrameSlot].GetAddress();
            }
            if (!BonesData.empty())
            {
                SceneRootShared.Bones = RHI::Core::CopyTransientArray(BonesData.data(), BonesData.size());
            }
            if (!BillboardInstances.empty())
            {
                SceneRootShared.Billboards = RHI::Core::CopyTransientArray(BillboardInstances.data(), BillboardInstances.size());
            }
            if (!CullViews.empty())
            {
                // No per-view draw-list slice is patched in here any more. Each (view, draw) gets its own
                // packed region, whose offset BuildDrawPrefix computes and SeedIndirectArgs writes straight
                // into FirstInstance, so FCullView carries no draw-list geometry at all.
                SceneRootShared.CullViews = RHI::Core::CopyTransientArray(CullViews.data(), CullViews.size());
            }
            if (!Frame.Geometry.SkinDescriptors.empty())
            {
                SceneRootShared.SkinDescriptors = RHI::Core::CopyTransientArray(Frame.Geometry.SkinDescriptors.data(), Frame.Geometry.SkinDescriptors.size());
            }
            if (!Frame.Primitives.WidgetInstances.empty())
            {
                SceneRootShared.Widgets = RHI::Core::CopyTransientArray(Frame.Primitives.WidgetInstances.data(), Frame.Primitives.WidgetInstances.size());
            }
            // Probe array. Uploaded even when every probe is still waiting to bake: an unbaked slice
            // reads black (cleared at allocation), which is a smooth blend toward no reflection rather
            // than the garbage an unwritten slice would give.
            // Probes arrive fully resolved (sorted, sliced, baked-flag stamped) from Extract; this is a
            // straight upload. Uploaded even when every probe is still queued: an unbaked probe carries
            // a clear baked flag and the shader skips it, leaving the sky to cover that pixel.
            NumActiveProbes = (uint32)Frame.ReflectionProbes.Probes.size();
            ProbeBufferAddr = 0;
            if (NumActiveProbes > 0)
            {
                InitReflectionProbeTargets();
                ProbeBufferAddr = RHI::Core::CopyTransientArray(Frame.ReflectionProbes.Probes.data(),
                                                                Frame.ReflectionProbes.Probes.size());
            }

            // Temporary diagnostic: the render phase's half of the plumbing. Change-gated.
            {
                const FSceneImage& DiagArray = NamedImages[(int)ENamedImage::ProbePrefiltered];
                const uint64 State = ((uint64)NumActiveProbes)
                                   | ((uint64)(ProbeBufferAddr != 0 ? 1 : 0) << 8)
                                   | ((uint64)(DiagArray.IsValid() ? 1 : 0) << 9)
                                   | ((uint64)DiagArray.GetNumMips() << 16)
                                   | ((uint64)(uint32)(DiagArray.GetResourceID() + 1) << 24);
                if (State != LastProbeRenderDiagState && CVarProbeDebugLog.GetValue())
                {
                    LastProbeRenderDiagState = State;
                    LOG_WARN("[Probe] Render: numActive={} bufferAddr={} arrayValid={} arraySRV={} mips={}",
                             NumActiveProbes, ProbeBufferAddr != 0 ? "set" : "NULL",
                             DiagArray.IsValid() ? 1 : 0, DiagArray.GetResourceID(), DiagArray.GetNumMips());
                }
            }

            // GPU-built this frame (ScanPrefix* dispatches below), read by the cull's binary search.
            SceneRootShared.InstanceMeshletPrefix = GetInstancePrefix().GetAddress();

            SceneRootShared.Materials          = GRenderManager->GetMaterialManager().GetMaterialBuffer();
            SceneRootShared.MeshletDrawList    = GetMeshletDrawList().GetAddress();
            SceneRootShared.PreSkinnedVertices = GetPreSkinnedVerticesBuffer().GetAddress();
            if (Frame.CachedWorldSettings.bEnableSSAO)
            {
                SceneGlobalData.SSAOSettings.AOTextureIndex = (uint32)CurrentView->Images[(int)ENamedImage::SSAOBlur].GetResourceID();
            }

            // Screen-space sun-shadow mask. Decided ONCE, here, and consumed by two places that must
            // never disagree: ShadowMaskPass (does the resolve run?) and every opaque pipeline key
            // (is the shader specialized to read a mask instead of running the PCSS inline?). A shader
            // keyed for the mask on a view that never produced one would read fully-lit everywhere.
            //
            // Only the sun is deferred, so the gate is "is there a shadow-casting sun": light 0 is the
            // sun whenever bHasSun, matching ShadeSurface's GetLightAt(0). Deliberately NOT gated on
            // DrawCommands like SSAO is -- a terrain-only scene has no mesh draws and still needs sun
            // shadows on the terrain.
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
            CurrentSceneRootAddr = BuildViewSceneRoot(*CurrentView, RHI::Core::CopyTransient(SceneGlobalData));

            // Cull, LOD-select and compact the retained scene, then build the draw-argument layout --
            // all GPU-side. This is what produces the totals SeedIndirectArgs, the prefix scan and
            // CullMeshlets read; the CPU no longer computes any of them.
            DispatchGPUSceneCull(CL, Frame);

            bool bPrepDispatched = false;

            // Seed the per-view indirect + mesh-task args GPU-side (SeedIndirectArgs.slang): one thread per
            // (view, draw) slot expands the D-entry per-draw meshlet prefix, replacing the CPU-built V*D
            // upload. FirstInstance is pre-seeded so each atomic append in CullMeshlets lands in its own
            // slice; mesh-task Y/Z seed to 1 and the cull accumulates GroupCountX (no ConvertMeshDrawArgs).
            if (NumCullViews > 0u && NumDraws > 0u && GetViewDrawOffsets())
            {
                static const FShaderEntry* const SeedShader = FShaderLibrary::Get("SeedIndirectArgs.slang");
                if (SeedShader)
                {
                    LUMINA_PROFILE_SECTION_COLORED("Seed Indirect Args", tracy::Color::Purple);
                    SCENE_GPU_SCOPE(CL, "Seed Indirect Args");

                    struct FSeedIndirectArgsPC
                    {
                        uint32 NumViews;
                        uint32 NumDraws;
                        uint32 Pad0;
                        uint32 Pad1;
                        uint64 IndirectArgsAddr;
                        uint64 MeshDrawArgsAddr;
                        uint64 ViewDrawOffsetsAddr;
                    };
                    static_assert(sizeof(FSeedIndirectArgsPC) == 40, "FSeedIndirectArgsPC must match SeedIndirectArgs.slang.");

                    FSeedIndirectArgsPC PC = {};
                    PC.NumViews          = NumCullViews;
                    PC.NumDraws          = NumDraws;
                    PC.IndirectArgsAddr  = GetIndirectArgs().GetAddress();
                    PC.MeshDrawArgsAddr  = GetMeshDrawArgs().GetAddress();
                    // GPU-built by BuildDrawPrefix; there is no CPU array to upload any more.
                    PC.ViewDrawOffsetsAddr = GetViewDrawOffsets().GetAddress();

                    RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(SeedShader));
                    // 1D grid: even GMaxCullViews * thousands of draws stays far under the 65535-group X cap.
                    const uint32 TotalSlots = NumCullViews * NumDraws;
                    RHI::CmdDispatch(CL, MakeArgs(PC), (TotalSlots + 63u) / 64u, 1u, 1u);
                    bPrepDispatched = true;
                }
            }

            // Per-instance meshlet prefix, hierarchical three-pass scan.
            //
            // This was one workgroup walking every 256-element chunk in sequence, which measured 1.94 ms of
            // GPU self time at ~500k instances -- ~2000 dependent iterations pinned to a single SM. Split
            // into scan-blocks / scan-the-block-sums / add-offsets-back, only the middle pass is serial and
            // its domain is block COUNT rather than element count (~2000 elements -> ~8 iterations).
            //
            // Passes 1 and 3 dispatch indirectly off BuildDrawPrefix's scan args, so the domain follows the
            // GPU-side survivor count with no readback and no dispatching over the worst case.
            if (NumDraws > 0u)
            {
                static const FShaderEntry* const ScanBlocksShader = FShaderLibrary::Get("ScanPrefixBlocks.slang");
                static const FShaderEntry* const ScanSumsShader   = FShaderLibrary::Get("ScanPrefixSums.slang");
                static const FShaderEntry* const ScanAddShader    = FShaderLibrary::Get("ScanPrefixAdd.slang");
                if (ScanBlocksShader && ScanSumsShader && ScanAddShader)
                {
                    LUMINA_PROFILE_SECTION_COLORED("Build Instance Prefix", tracy::Color::Purple2);
                    SCENE_GPU_SCOPE(CL, "Build Instance Prefix");

                    // Shared by all three passes; the trailing pointer is unused by pass 2.
                    struct FScanPC
                    {
                        uint32 Pad0;
                        uint32 Pad1;
                        uint64 CountAddr;        // [1] of the totals block: surviving instance count
                        uint64 PrefixAddr;
                        uint64 BlockSumsAddr;
                    };
                    static_assert(sizeof(FScanPC) == 32, "FScanPC must match the ScanPrefix* shaders.");

                    FScanPC PC = {};
                    // +4 bytes: the count is the SECOND uint of the totals block, read through a pointer so
                    // BuildDrawPrefix can produce it GPU-side.
                    PC.CountAddr     = GetTotals().GetAddress() + sizeof(uint32);
                    PC.PrefixAddr    = GetInstancePrefix().GetAddress();
                    PC.BlockSumsAddr = GetScanBlockSums().GetAddress();

                    {
                        SCENE_GPU_SCOPE(CL, "Scan Blocks");
                        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ScanBlocksShader));
                        RHI::CmdDispatchIndirect(CL, MakeArgs(PC), GetScanDispatchArgs().Ptr, 0);
                    }

                    // Pass 2 reads every block sum pass 1 wrote, and pass 3 reads pass 2's output.
                    Barriers::ComputeToAll(CL);

                    {
                        SCENE_GPU_SCOPE(CL, "Scan Block Sums");
                        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ScanSumsShader));
                        RHI::CmdDispatch(CL, MakeArgs(PC), 1u, 1u, 1u);
                    }

                    Barriers::ComputeToAll(CL);

                    {
                        SCENE_GPU_SCOPE(CL, "Scan Add Offsets");
                        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ScanAddShader));
                        RHI::CmdDispatchIndirect(CL, MakeArgs(PC), GetScanDispatchArgs().Ptr, 0);
                    }

                    bPrepDispatched = true;
                }
            }

            if (bPrepDispatched)
            {
                // Seeds + prefix must land before the cull's reads/atomics and the indirect-draw consumption.
                Barriers::ComputeToAll(CL);
            }
        }
    }

    // Compares DistSq against Threshold^2 * RadiusSq, which is the squared form of the old
    // (distance/radius >= threshold) test. Monotonic, so stop at the first that fails.
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

    // Mirrors CullInstances.slang exactly: both seed the cull's dispatch domain, so a disagreement drops
    // meshlets off its tail. CoarseDistSq <= 0 keeps the tight cap everywhere.
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

    namespace
    {
        // Running total of the entity's distinct rendered meshlet blocks, accumulated across the
        // surface loop; drives the GPU pre-skinning slice.
        struct FSkinSizeAccum
        {
            uint32 SliceSize = 0u;
        };
    }

    // Per-surface emit, shared by every primitive source.
    //
    // Unlike the old gather this does no identity work: the batch each surface belongs to was resolved
    // once when the primitive was bound and is read straight out of FSurfaceBinding. All that is left
    // here is the camera-dependent part -- the LOD pick and the per-instance flags.
    static void EmitPrimitiveSurfaces(FForwardRenderScene::FThreadLocalDrawData& Local,
                                      const FScenePrimitive& Prim,
                                      const FSurfaceBinding* Bindings,
                                      uint32 EntityRecordIdx,
                                      const FSceneRenderSettings& Settings,
                                      float DistSq,
                                      float RadiusSq,
                                      FSkinSizeAccum* SkinSize = nullptr,
                                      const TVector<FMeshlet>* Meshlets = nullptr)
    {
        // Block vertex extents come from the live meshlet table, never from anything cached at mesh-resolve
        // time: a mesh can resolve while its surfaces are populated but the meshlet array is not yet
        // resident, and a cached zero there silently turns into base 0 -- a valid-looking index into
        // unwritten pre-skin memory. Contiguous per (surface, LOD) because packing is meshlet-ordered.
        auto BlockExtent = [Meshlets](uint32 MeshletOffset, uint32 MeshletCount,
                                      uint32& OutVertexOffset, uint32& OutVertexCount) -> bool
        {
            OutVertexOffset = 0u;
            OutVertexCount  = 0u;

            if (MeshletCount == 0u || Meshlets == nullptr
                || (SIZE_T)MeshletOffset + MeshletCount > Meshlets->size())
            {
                return false;
            }

            const FMeshlet& First = (*Meshlets)[MeshletOffset];
            const FMeshlet& Last  = (*Meshlets)[MeshletOffset + MeshletCount - 1u];
            OutVertexOffset = First.VertexOffset;
            OutVertexCount  = (Last.VertexOffset + Last.VertexCount) - First.VertexOffset;
            
            if (CVarValidateSkinSlices.GetValue() != 0)
            {
                for (uint32 m = 0; m < MeshletCount; ++m)
                {
                    const FMeshlet& M = (*Meshlets)[MeshletOffset + m];
                    const bool bInside = (M.VertexOffset >= OutVertexOffset)
                                      && ((M.VertexOffset + M.VertexCount) <= (OutVertexOffset + OutVertexCount));
                    if (!bInside)
                    {
                        static uint32 SpanViolationLogCount = 0;
                        if (SpanViolationLogCount++ < 16u)
                        {
                            LOG_ERROR("Skinning: meshlet {} of block [{}, {}) is OUTSIDE the block's vertex span. "
                                      "Meshlet verts [{}, {}), span [{}, {}). The pre-skin base for this block is a "
                                      "wrap around the span start, so this meshlet skins and reads out of slice.",
                                      MeshletOffset + m, MeshletOffset, MeshletOffset + MeshletCount,
                                      M.VertexOffset, M.VertexOffset + M.VertexCount,
                                      OutVertexOffset, OutVertexOffset + OutVertexCount);
                        }
                        break;
                    }
                }
            }

            return OutVertexCount > 0u;
        };

        const TVector<FResolvedSurface>& Surfaces = *Prim.Surfaces;
        const uint64 MeshletHeaderAddress = Prim.MeshletHeaderAddress;
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
            const uint32 SurfaceMeshletCount  = MeshletHeaderAddress ? Surface.LODMeshletCount[LODIndex]       : 0u;
            const uint32 SurfaceMeshletOffset = Surface.LODMeshletOffset[LODIndex];
            const uint32 ShadowMeshletCount   = MeshletHeaderAddress ? Surface.LODMeshletCount[ShadowLODIndex] : 0u;
            const uint32 ShadowMeshletOffset  = Surface.LODMeshletOffset[ShadowLODIndex];

            // End of the last LOD: what bounds the mesh-global meshlet index the raster resolves. A skinned
            // instance's LOD is fixed (its pre-skin blocks are built for one specific range, so no view may
            // re-select), but it emits the same kind of draw entry and so carries the same bound.
            const uint32 LastLOD = Surface.NumLODs > 0u ? Math::Min(Surface.NumLODs, (uint32)MAX_MESH_LODS) - 1u : 0u;
            const uint32 MeshletTotalCount = MeshletHeaderAddress
                                           ? Surface.LODMeshletOffset[LastLOD] + Surface.LODMeshletCount[LastLOD]
                                           : 0u;

            const uint32 BatchIndex = Binding.BatchIndex;

            if (Local.DrawInstanceCounts[BatchIndex]++ == 0u)
            {
                Local.TouchedSlots.push_back(BatchIndex);
            }
            // max, matching ScanPrefixBlocks and CullInstances: a shadow view walks the SHADOW LOD, whose
            // MAX_SHADOW_LOD cap can make it finer -- and larger -- than the camera LOD. This seeds the
            // cull's dispatch domain for the CPU-fed skinned instances, and the prefix that indexes into
            // that domain is computed from the same max on the GPU, so the two must agree.
            Local.DrawMeshletCounts[BatchIndex] += Math::Max(SurfaceMeshletCount, ShadowMeshletCount);
            Local.BatchSkinFlags[Binding.BatchIndex] |=
                EnumHasAnyFlags(Flags, EInstanceFlags::Skinned) ? 1u : 2u;

            FForwardRenderScene::FProcessedDrawItem& Item = Local.Items.emplace_back();
            Item.EntityRecordIndex    = EntityRecordIdx;
            Item.BatchIndex           = BatchIndex;
            Item.SurfaceMeshletOffset = SurfaceMeshletOffset;
            Item.SurfaceMeshletCount  = SurfaceMeshletCount;
            Item.ShadowMeshletOffset  = ShadowMeshletOffset;
            Item.ShadowMeshletCount   = ShadowMeshletCount;
            Item.MeshletTotalCount    = MeshletTotalCount;
            Item.Flags                = Flags;
            Item.MaterialIndex        = Binding.MaterialIndex;
            Item._Pad                 = 0;

            // Skinned items start at the sentinel, not 0: anything that fails to get a real slice below
            // then falls back to the in-draw blend and still renders, instead of silently reading
            // whatever sits at index 0 of the pre-skin buffer.
            Item.SurfaceVertexOffset = SkinSize ? kNoPreSkinBase : 0u;
            Item.SurfaceVertexCount  = 0u;
            Item.ShadowVertexOffset  = SkinSize ? kNoPreSkinBase : 0u;
            Item.ShadowVertexCount   = 0u;

            if (SkinSize)
            {
                // Each rendered (surface, LOD) meshlet block gets its own compacted slice. The shadow
                // block is free when it resolved to the same LOD, which is the common case.
                if (BlockExtent(SurfaceMeshletOffset, SurfaceMeshletCount,
                                Item.SurfaceVertexOffset, Item.SurfaceVertexCount))
                {
                    SkinSize->SliceSize += Item.SurfaceVertexCount;
                }
                else
                {
                    Item.SurfaceVertexOffset = kNoPreSkinBase;
                }

                // Non-casters never reach a shadow view (the cull gates them on the same flag), so a
                // shadow slice for one would be written and never read.
                const bool bNeedsShadowBlock = ShadowMeshletOffset != SurfaceMeshletOffset
                                            && EnumHasAnyFlags(Flags, EInstanceFlags::CastShadow);
                if (bNeedsShadowBlock
                    && BlockExtent(ShadowMeshletOffset, ShadowMeshletCount,
                                   Item.ShadowVertexOffset, Item.ShadowVertexCount))
                {
                    SkinSize->SliceSize += Item.ShadowVertexCount;
                }
                else
                {
                    Item.ShadowVertexOffset = kNoPreSkinBase;
                    Item.ShadowVertexCount  = 0u;
                }
            }
        }
    }

    /**
     * The per-frame gather for SKELETAL meshes. Everything else is culled on the GPU.
     *
     * This is not the general mesh gather it once was: CullInstances.slang culls static, dynamic and
     * foliage primitives off the retained instance arrays, and skeletal primitives are deliberately
     * excluded from that cull because their FGPUInstance fields cannot be filled at bind time. What is
     * left here is the work that genuinely cannot be retained -- the pose changes every frame, so bone
     * rows, pre-skin slice sizing and the animation system's last-rendered feedback are all gathered
     * fresh, from the live component, after the cull.
     *
     * The range still spans the whole primitive array (the sources are interleaved in one dense array),
     * so the source filter is the hot path, not the cull. It reads the dense key array via GetSource --
     * see the note there for why FScenePrimitive::Source is the wrong place to read it from. The caller
     * skips this node entirely when the scene holds no skeletal meshes.
     */
    void FForwardRenderScene::CullAndEmitPrimitives(const Task::FParallelRange& Range, FThreadLocalDrawData& Local)
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

        const double WorldTime = World->GetTimeSinceWorldCreation();

        for (uint32 i = Range.Start; i < Range.End; ++i)
        {
            // Filter before touching Prims: this rejects every non-skeletal primitive in the scene, and
            // FScenePrimitive is ~160 bytes with Source in its last cache line.
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
            EntityRecord.MeshletHeaderAddress = Prim.MeshletHeaderAddress;
            EntityRecord.CustomData           = Prim.CustomData;
            EntityRecord.EntityID             = Prim.EntityID;
            EntityRecord.LocalBoneOffset      = ~0u;
            EntityRecord.SkinSliceSize        = 0u;
            EntityRecord.GlobalSkinnedBase    = 0u;
            EntityRecord.SkinCursor           = 0u;

            //~ Everything below needs the live component.

            if (!SkeletalStorage.contains(Prim.Entity))
            {
                continue;
            }
            SSkeletalMeshComponent& MeshComponent = SkeletalStorage.get(Prim.Entity);

            // Shadow-only casters intentionally excluded: a mesh that only reaches a shadow view isn't
            // "rendered", so the anim system may stop ticking its pose.
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

            // The mesh bakes u8 joint indices at import; the bone slice is sized from the SKELETON at
            // runtime. Those are separate assets and nothing on the GPU bounds
            // Bones()[BoneOffset + JointIndices.x], so if the mesh references more bones than the skeleton
            // provides, every vertex weighted to the excess reads past this entity's slice -- garbage
            // matrices, wildly displaced vertices. A rig's highest indices are its leaf bones, so it shows
            // up on fingers and toes first while the torso stays correct.
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

            uint32 LocalBoneOffset = ~0u;
            if (SkeletonBoneCount > 0)
            {
                LocalBoneOffset = Local.BonesData.Size();
                if ((uint32)MeshComponent.BoneTransforms.size() == SkeletonBoneCount)
                {
                    // The animation system packs when it evaluates a pose; writers that bypass it
                    // (ragdoll readback, editor bind-pose fills) set the dirty flag and we repack here.
                    // Frozen/off-screen skeletons hit neither and just bulk-copy the cached rows.
                    if (MeshComponent.bRenderBonesDirty || (uint32)MeshComponent.RenderBones.size() != SkeletonBoneCount * 3u)
                    {
                        SkeletalUtils::PackRenderBones(MeshComponent.BoneTransforms, MeshComponent.RenderBones);
                        MeshComponent.bRenderBonesDirty = false;
                    }

                    const FBoneTransform* Packed = reinterpret_cast<const FBoneTransform*>(MeshComponent.RenderBones.data());
                    Local.BonesData.Append(Packed, SkeletonBoneCount);
                }
                else
                {
                    // No active animation: BoneWorld * InvBindMatrix collapses to identity for every bone.
                    Local.BonesData.AppendIdentity(SkeletonBoneCount);
                }
            }

            EntityRecord.LocalBoneOffset = LocalBoneOffset;

            // Screen-size proxy for the animation system's update-rate optimization. The sqrt survives only
            // here because the anim system wants the real ratio; LOD selection uses the squared form.
            MeshComponent.LastDistanceOverRadius = (Radius > 0.0f) ? (Math::Sqrt(DistSq) / Radius) : 0.0f;

            const bool bAccumulateSkinSize = (LocalBoneOffset != ~0u && Prim.MeshletHeaderAddress != 0ull);
            FSkinSizeAccum SkinSize;
            EmitPrimitiveSurfaces(Local, Prim, Bindings + Prim.BindingBase,
                                  EntityRecordIdx, RenderSettings, DistSq, RadiusSq,
                                  bAccumulateSkinSize ? &SkinSize : nullptr,
                                  &Resource.MeshletData.Meshlets);

            EntityRecord.SkinSliceSize = SkinSize.SliceSize;
        }
    }

    void FForwardRenderScene::AssignPreSkinSlices(FFrameData& Frame,
                                                  TVector<FSkinCandidate>& Candidates,
                                                  TVector<FThreadLocalDrawData>& ThreadLocal)
    {
        if (Candidates.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION("Assign Pre-Skin Slices");

        const uint32 Budget = (uint32)Math::Max(0, CVarMaxPreSkinnedVertices.GetValue());

        uint64 TotalDemand = 0;
        for (const FSkinCandidate& Candidate : Candidates)
        {
            TotalDemand += Candidate.Record->SkinSliceSize;
        }

        // Ranking only matters once someone has to lose; the common case keeps merge order.
        if (TotalDemand > Budget)
        {
            eastl::sort(Candidates.begin(), Candidates.end(),
                        [](const FSkinCandidate& A, const FSkinCandidate& B) { return A.Priority > B.Priority; });
        }

        // Grant whole entities, largest on screen first, so one is never half pre-skinned.
        uint32 Total       = Frame.Geometry.TotalPreSkinnedVertices;
        uint32 NumDeferred = 0;

        for (const FSkinCandidate& Candidate : Candidates)
        {
            FEntityRecord& Rec = *Candidate.Record;

            if ((uint64)Total + Rec.SkinSliceSize > Budget)
            {
                Rec.GlobalSkinnedBase = kNoPreSkinBase;
                Rec.SkinCursor        = kNoPreSkinBase;
                ++NumDeferred;
                continue;
            }

            Rec.GlobalSkinnedBase = Total;
            Rec.SkinCursor        = Total;
            Total                += Rec.SkinSliceSize;
        }

        Frame.Geometry.TotalPreSkinnedVertices = Total;

        // Sub-allocate each granted entity's blocks and emit their descriptors. Serial because the
        // descriptor list and the per-entity cursor are shared; the walk order only has to be stable,
        // not meaningful. Resolved block bases are written back over the item's *VertexOffset fields.
        for (FThreadLocalDrawData& Local : ThreadLocal)
        {
            if (Local.BonesData.IsEmpty())
            {
                continue;
            }

            for (FProcessedDrawItem& Item : Local.Items)
            {
                FEntityRecord& Rec = Local.EntityRecords[Item.EntityRecordIndex];
                if (Rec.LocalBoneOffset == ~0u || Rec.SkinSliceSize == 0u)
                {
                    continue;
                }

                if (Rec.GlobalSkinnedBase == kNoPreSkinBase)
                {
                    Item.SurfaceVertexOffset = kNoPreSkinBase;
                    Item.ShadowVertexOffset  = kNoPreSkinBase;
                    continue;
                }

                // A block's base folds in its vertex-span start so Base + M.VertexOffset lands inside
                // the compacted slice (unsigned wrap; every meshlet in the block is at or past it).
                auto AllocateBlock = [&](uint32 VertexOffset, uint32 VertexCount,
                                         uint32 MeshletOffset, uint32 MeshletCount) -> uint32
                {
                    const uint32 Base = Rec.SkinCursor - VertexOffset;

                    // The block must fit inside the slice this entity was granted. Overrunning it walks into
                    // the NEXT entity's slice, so two entities skin and read the same vertices -- one of them
                    // renders with the other's pose.
                    if (CVarValidateSkinSlices.GetValue() != 0)
                    {
                        const uint32 SliceBegin = Rec.GlobalSkinnedBase;
                        const uint32 SliceEnd   = Rec.GlobalSkinnedBase + Rec.SkinSliceSize;
                        if (Rec.SkinCursor < SliceBegin || (uint64)Rec.SkinCursor + VertexCount > SliceEnd)
                        {
                            static uint32 SliceViolationLogCount = 0;
                            if (SliceViolationLogCount++ < 16u)
                            {
                                LOG_ERROR("Skinning: block [{}, {}) escapes its entity slice [{}, {}). "
                                          "MeshletRange [{}, {}), BlockVertexOffset {}, Base {}. Grant is "
                                          "SkinSliceSize {} -- the per-block consumption and the grant disagree.",
                                          Rec.SkinCursor, Rec.SkinCursor + VertexCount, SliceBegin, SliceEnd,
                                          MeshletOffset, MeshletOffset + MeshletCount, VertexOffset, Base,
                                          Rec.SkinSliceSize);
                            }
                        }
                    }

                    Rec.SkinCursor   += VertexCount;

                    // One descriptor per meshlet -> the dispatch runs one workgroup per meshlet,
                    // so meshlets skin concurrently instead of looping serially within one group.
                    const uint32 MeshletEnd = MeshletOffset + MeshletCount;
                    for (uint32 m = MeshletOffset; m < MeshletEnd; ++m)
                    {
                        FSkinDescriptor& Desc     = Frame.Geometry.SkinDescriptors.emplace_back();
                        Desc.MeshletHeaderAddress = Rec.MeshletHeaderAddress;
                        Desc.BoneOffset           = Rec.SkinBoneOffset;
                        Desc.SkinnedVertexBase    = Base;
                        Desc.MeshletIndex         = m;
                        Desc.Pad                  = 0u;
                    }
                    return Base;
                };

                const uint32 SurfaceBase = Item.SurfaceVertexCount > 0u
                    ? AllocateBlock(Item.SurfaceVertexOffset, Item.SurfaceVertexCount,
                                    Item.SurfaceMeshletOffset, Item.SurfaceMeshletCount)
                    : kNoPreSkinBase;

                uint32 ShadowBase;
                if (Item.ShadowVertexCount > 0u)
                {
                    ShadowBase = AllocateBlock(Item.ShadowVertexOffset, Item.ShadowVertexCount,
                                               Item.ShadowMeshletOffset, Item.ShadowMeshletCount);
                }
                else if (Item.ShadowMeshletOffset == Item.SurfaceMeshletOffset)
                {
                    // Shadow LOD resolved to the surface block; same vertices, same base.
                    ShadowBase = SurfaceBase;
                }
                else
                {
                    ShadowBase = kNoPreSkinBase;
                }

                Item.SurfaceVertexOffset = SurfaceBase;
                Item.ShadowVertexOffset  = ShadowBase;
            }
        }

        // Closing the loop on the grant: every granted entity must have consumed EXACTLY the slice it was
        // budgeted. Under-consumption means the size accumulation counted a block the allocation pass then
        // skipped (or vice versa) -- the two run under separate conditions in separate passes, so they can
        // drift silently; over-consumption is the same drift in the direction that corrupts the next entity.
        if (CVarValidateSkinSlices.GetValue() != 0)
        {
            for (FThreadLocalDrawData& Local : ThreadLocal)
            {
                for (FEntityRecord& Rec : Local.EntityRecords)
                {
                    if (Rec.LocalBoneOffset == ~0u || Rec.SkinSliceSize == 0u
                        || Rec.GlobalSkinnedBase == kNoPreSkinBase)
                    {
                        continue;
                    }

                    const uint32 Expected = Rec.GlobalSkinnedBase + Rec.SkinSliceSize;
                    if (Rec.SkinCursor != Expected)
                    {
                        static uint32 CursorMismatchLogCount = 0;
                        if (CursorMismatchLogCount++ < 16u)
                        {
                            LOG_ERROR("Skinning: entity slice consumption mismatch -- cursor ended at {} but the "
                                      "grant [{}, {}) expected {}. Delta {} vertices. The size pass and the "
                                      "allocation pass disagree about which blocks exist.",
                                      Rec.SkinCursor, Rec.GlobalSkinnedBase, Expected, Expected,
                                      (int64)Rec.SkinCursor - (int64)Expected);
                        }
                    }
                }
            }
        }

        if (NumDeferred != LastPreSkinDeferredCount)
        {
            LastPreSkinDeferredCount = NumDeferred;
            if (NumDeferred > 0)
            {
                LOG_WARN("Skinning: {} of {} skinned entities exceed the {}-vertex pre-skin budget and fall back to "
                         "in-draw skinning. Raise r.Skinning.MaxVertices or reduce the visible skinned set.",
                         NumDeferred, Candidates.size(), Budget);
            }
        }
    }

    /**
     * Collapses the per-thread gather output into this frame's GPU-facing arrays.
     *
     * The old merge had to *discover* the draw structure every frame: union the per-thread batch maps,
     * dedup draws inside each batch, then lay out draw args. None of that happens now -- the (batch,
     * draw) slot space is persistent scene state, so this is pure arithmetic over per-slot counters plus
     * one scatter of the visible instances. Everything here is O(visible instances + draw slots), with no
     * hashing and no allocation.
     */
    void FForwardRenderScene::MergeMeshDrawData(TVector<FThreadLocalDrawData>& ThreadLocal)
    {
        LUMINA_PROFILE_SECTION("Merge Mesh Draw Data");

        FFrameData& Frame               = *ExtractFrame;
        auto& Instances                 = Frame.Geometry.Instances;
        auto& BonesData                 = Frame.Geometry.BonesData;
        auto& DrawCommands              = Frame.Geometry.DrawCommands;
        auto& OpaqueDrawList            = Frame.Geometry.OpaqueDrawList;
        auto& TranslucentDrawList       = Frame.Geometry.TranslucentDrawList;
        auto& DeferredMaterials         = Frame.Geometry.DeferredMaterials;
        auto& FrameStats                = Frame.FrameStats;
        uint32& NumDrawsPerView         = Frame.Views.NumDrawsPerView;

        const uint32 NumThreads = (uint32)ThreadLocal.size();
        const uint32 NumSlots   = ScenePrimitives.GetBatches().Num();   // one draw per batch

        DeferredMaterials.clear();

        // Bones merged serially: skinned meshes reference by absolute index.
        // Persistent member; assign keeps capacity across frames.
        TVector<uint32>& ThreadBoneBase = MergeThreadBoneBase;
        ThreadBoneBase.assign(NumThreads, 0u);

        TVector<FSkinCandidate>& SkinCandidates = MergeSkinCandidates;
        SkinCandidates.clear();
        const FVector3 CameraPos = FVector3(Frame.SceneGlobalData.CameraData.Location);

        uint32 TotalInstances = 0;
        uint64 TotalInstancesCulled = 0;
        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FThreadLocalDrawData& Local = ThreadLocal[t];
            ThreadBoneBase[t] = (uint32)BonesData.size();
            TotalInstances += (uint32)Local.Items.size();
            TotalInstancesCulled += Local.Stats.NumInstancesCulled;

            // A skinned record always appends bones, so a bone-less thread has no skinned
            // records and its full EntityRecords scan below can be skipped.
            if (Local.BonesData.IsEmpty())
            {
                continue;
            }
            for (const TFrameVector<FBoneTransform>& Page : Local.BonesData.Pages)
            {
                BonesData.insert(BonesData.end(), Page.begin(), Page.end());
            }

            // Bases are not assigned here: the pre-skin buffer is budgeted, so which entities get a
            // slice depends on all of them. Collect now, rank and hand out slices after the bone pass.
            for (FEntityRecord& Rec : Local.EntityRecords)
            {
                if (Rec.LocalBoneOffset == ~0u || Rec.SkinSliceSize == 0u)
                {
                    continue;
                }

                Rec.SkinBoneOffset = ThreadBoneBase[t] + Rec.LocalBoneOffset;

                const FVector3 ToCamera = FVector3(Rec.SphereBounds) - CameraPos;
                const float    DistSq   = Math::Dot(ToCamera, ToCamera);
                const float    RadiusSq = Rec.SphereBounds.w * Rec.SphereBounds.w;

                FSkinCandidate& Candidate = SkinCandidates.emplace_back();
                Candidate.Record   = &Rec;
                Candidate.Priority = RadiusSq / Math::Max(DistSq, 1e-4f);
            }
        }
        FrameStats.NumInstancesCulled += TotalInstancesCulled;

        AssignPreSkinSlices(Frame, SkinCandidates, ThreadLocal);

        // ResetPass leaves Instances at last frame's size (see there); consumers read size(), so a frame
        // with no skinned meshes must drop it explicitly. It does NOT return early any more: the draw
        // list below covers the rigid instances the GPU appends, which exist regardless.
        if (TotalInstances == 0)
        {
            Instances.clear();
        }

        // Sum the per-thread slot counters and, in the same pass, record where each thread's instances
        // for that slot will start. Threads that never ran a gather chunk have empty counter arrays.
        // Sized by batch count, which is what the draw-slot space collapsed to. Max(1) so an empty
        // scene still has addressable storage for the guards below.
        const uint32 SlotCapacity = Math::Max(NumSlots, 1u);
        MergeDrawInstanceCounts.assign(SlotCapacity, 0u);
        MergeMeshletCountsPerDraw.assign(SlotCapacity, 0u);
        MergeDrawInstanceOffsets.assign(SlotCapacity, 0u);

        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FThreadLocalDrawData& Local = ThreadLocal[t];
            if (!Local.bTouched)
            {
                continue;
            }

            // Only the slots this thread actually emitted into, so a scene with thousands of distinct
            // materials but few visible ones doesn't pay a dense sweep per thread. DrawWriteBase is
            // already sized by PrepareCounters and is written before it is read, so it needs no clear.
            for (uint32 Slot : Local.TouchedSlots)
            {
                // Running total so far == this thread's offset within the slot's instance block.
                Local.DrawWriteBase[Slot]        = MergeDrawInstanceCounts[Slot];
                MergeDrawInstanceCounts[Slot]   += Local.DrawInstanceCounts[Slot];
                MergeMeshletCountsPerDraw[Slot] += Local.DrawMeshletCounts[Slot];
            }
        }

        // Draws come from the persistent batch registry now, not from this frame's visible set: a draw
        // is one PSO bucket, and which of them have instances is a GPU-side answer. A batch the cull
        // finds empty simply gets InstanceCount = 0 and rasterizes nothing.
        FSceneBatchRegistry& Registry = ScenePrimitives.GetBatches();
        const uint32 NumBatches = Registry.Num();

        DrawCommands.clear();
        Frame.Geometry.BatchMeshletSeed.assign(Math::Max(NumBatches, 1u), 0u);

        uint32 InstanceRunning = 0u;

        for (uint32 b = 0; b < NumBatches; ++b)
        {
            // Counters are indexed by batch directly now, so there is no cached layout that can lag
            // behind the registry and no stale-slot guard to get wrong.
            if (b < SlotCapacity)
            {
                // Skinned instances occupy the head of the visible buffer in emission order, so their
                // per-batch offsets are just a running total; the GPU appends after them.
                MergeDrawInstanceOffsets[b] = InstanceRunning;
                InstanceRunning += MergeDrawInstanceCounts[b];

                Frame.Geometry.BatchMeshletSeed[b] = MergeMeshletCountsPerDraw[b];
            }

            const FSceneBatchRegistry::FBatch& Batch = Registry.Get(b);

            uint8 SkinFlags = 0u;
            for (uint32 t = 0; t < NumThreads; ++t)
            {
                if (ThreadLocal[t].bTouched)
                {
                    SkinFlags |= ThreadLocal[t].BatchSkinFlags[b];
                }
            }
            // A batch with no CPU-side instances can still receive rigid ones from the cull, so it must
            // advertise a static variant rather than nothing.
            if (SkinFlags == 0u)
            {
                SkinFlags = 2u;
            }

            FMeshDrawCommand& Cmd = DrawCommands.emplace_back();
            Cmd.VertexShader                   = Batch.VertexShader;
            Cmd.PixelShader                    = Batch.PixelShader;
            Cmd.MeshShader                     = Batch.MeshShader;
            Cmd.VisBufferMeshShader            = Batch.VisBufferMeshShader;
            Cmd.VisBufferMeshShaderMasked      = Batch.VisBufferMeshShaderMasked;
            Cmd.VisBufferVertexShader          = Batch.VisBufferVertexShader;
            Cmd.MaskedVisBufferPixelShader     = Batch.MaskedVisBufferPixelShader;
            Cmd.MaskedVisBufferPixelShaderPrim = Batch.MaskedVisBufferPixelShaderPrim;
            Cmd.DeferredShader                 = Batch.DeferredShader;
            Cmd.MaterialIndex                  = Batch.MaterialIdx;
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

        DEBUG_ASSERT(InstanceRunning == TotalInstances);

        // Skinned instances only; the GPU appends the rigid survivors after them.
        Instances.resize(TotalInstances);

        // Fold each slot's global instance offset into the per-thread bases, so the scatter is a plain
        // post-increment with no indirection.
        for (uint32 t = 0; t < NumThreads; ++t)
        {
            FThreadLocalDrawData& Local = ThreadLocal[t];
            if (!Local.bTouched)
            {
                continue;
            }
            for (uint32 Slot : Local.TouchedSlots)
            {
                Local.DrawWriteBase[Slot] += MergeDrawInstanceOffsets[Slot];
            }
        }

        // Scheduling dominates the merge for typical scenes, so each dispatch+join costs more than the
        // work below it. Fan out only above the threshold; below it runs inline.
        const bool bParallelMerge = TotalInstances > 4096;

        // Each worker only touches its own Local data; in-place cursor advance needs no sync.
        {
            LUMINA_PROFILE_SECTION("Parallel Instance Write");

            auto InstanceWriteBody = [&](const Task::FParallelRange& Range)
            {
                for (uint32 t = Range.Start; t < Range.End; ++t)
                {
                    FThreadLocalDrawData& Local = ThreadLocal[t];
                    if (!Local.bTouched)
                    {
                        continue;
                    }
                    const uint32 BoneBase = ThreadBoneBase[t];

                    for (FProcessedDrawItem& Item : Local.Items)
                    {
                        const uint32 WriteIdx   = Local.DrawWriteBase[Item.BatchIndex]++;
                        const uint32 GlobalDraw = Item.BatchIndex;   // DrawCommands is filled one per batch

                        const FEntityRecord& Entity = Local.EntityRecords[Item.EntityRecordIndex];
                        const uint32 GlobalBoneOffset = Entity.LocalBoneOffset != ~0u ? (BoneBase + Entity.LocalBoneOffset) : 0u;

                        FGPUInstance& Out = Instances[WriteIdx];
                        Out.Transform                  = PackTransform3x4(Entity.Transform);
                        Out.SphereBounds               = Entity.SphereBounds;
                        Out.ShadowMeshletOffset        = Item.ShadowMeshletOffset;
                        Out.ShadowMeshletCount         = Item.ShadowMeshletCount;
                        Out.MeshletHeaderAddress       = Entity.MeshletHeaderAddress;
                        Out.DrawIDAndFlags             = PackDrawIDAndFlags(GlobalDraw, Item.Flags);
                        Out.SurfaceMeshletOffset       = Item.SurfaceMeshletOffset;
                        Out.SurfaceMeshletCount        = Item.SurfaceMeshletCount;
                        Out.CustomData                 = Entity.CustomData;
                        Out.BoneOffset                 = GlobalBoneOffset;
                        Out.MaterialIndex              = Item.MaterialIndex;
                        Out.EntityID                   = Entity.EntityID;
                        // Per-block bases, resolved in AssignPreSkinSlices and stashed on the item.
                        Out.SkinnedVertexBase          = Item.SurfaceVertexOffset;
                        Out.ShadowSkinnedVertexBase    = Item.ShadowVertexOffset;
                        // Skinned instances are the one producer that does NOT let a view re-select the LOD:
                        // the pre-skin blocks the draw path reads were built for these exact ranges, so a
                        // view picking a different LOD would skin one range and raster another.
                        Out.SurfaceDescIndex           = kNoSurfaceDescIndex;
                        // The walk domain the meshlet cull and the prefix scan share. No per-view maximum to
                        // fold in here for the same reason: both ranges are fixed.
                        Out.MeshletWalkCount           = Math::Max(Item.SurfaceMeshletCount, Item.ShadowMeshletCount);
                        Out.MeshletTotalCount          = Item.MeshletTotalCount;
                    }
                }
            };
            if (bParallelMerge) { Task::ParallelFor(NumThreads, InstanceWriteBody, 1); }
            else                { InstanceWriteBody(Task::FParallelRange{ 0u, NumThreads, 0u }); }
        }

        // The per-instance meshlet prefix the cull binary-searches is GPU-built
        // (ScanPrefix* passes) from the uploaded instances; no CPU pass here.

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

    bool FForwardRenderScene::ShouldRequestShadow(const FVector3& LightPosition, float LightRadius) const
    {
        return ExtractFrame->CameraFrustum.IntersectsSphere(LightPosition, LightRadius);
    }

    void FForwardRenderScene::BuildSceneCullContext()
    {
        LUMINA_PROFILE_SCOPE();

        FFrameData& Frame = *ExtractFrame;
        auto& SceneCullContext = Frame.Geometry.SceneCullContext;
        auto& SceneGlobalData  = Frame.SceneGlobalData;

        SceneCullContext.Reset();
        // FSceneRenderSettings::bCPUInstanceCull existed but nothing read it -- only the CVar did, so the
        // per-scene setting was dead and a viewport that turned it off still culled. Both now gate it, the
        // same way bShadowOcclusionCull pairs with its CVar: the CVar is the global kill switch, the
        // setting is this scene's.
        SceneCullContext.bEnabled = RenderSettings.bCPUInstanceCull && CVarCPUInstanceCull.GetValue();
        SceneCullContext.Frustum  = Frame.CameraFrustum;

        if (!SceneCullContext.bEnabled)
        {
            return;
        }

        // Union in each active capture (preview camera) frustum so instances visible only to
        // a preview survive the CPU pre-cull and reach the GPU per-view cull.
        for (const FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            SceneCullContext.CaptureFrusta.push_back(Capture.ViewVolume.GetFrustum());
        }

        // Same for a probe bake's six faces. Without this the CPU pre-cull drops everything outside the
        // primary camera's frustum before upload, and the probe bakes a cube that only contains what the
        // camera happened to be looking at -- baked in permanently, since captures are on-demand. The six
        // faces together are a full sphere, so this effectively disables the pre-cull for the bake frame.
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
            // Sweep camera frustum along sun so off-screen casters between sun and view stay.
            // Distance MUST match ShadowSweepDistance in CompileDrawCommands.
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
            if (!SceneCullContext.Frustum.IntersectsSphere(Transform.WorldTransform.Location, Radius))
            {
                continue;
            }
            SceneCullContext.ShadowLights.push_back({ Transform.WorldTransform.GetLocation(), Radius });
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
            if (!SceneCullContext.Frustum.IntersectsSphere(Transform.WorldTransform.Location, Radius))
            {
                continue;
            }
            SceneCullContext.ShadowLights.push_back({ Transform.WorldTransform.GetLocation(), Radius });
        }
    }

    void FForwardRenderScene::ProcessPointLight(const SPointLightComponent& PointLight, const STransformComponent& TransformComponent, TAtomic<uint32>& LightCount)
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
        Light.Position              = TransformComponent.WorldTransform.GetLocation();
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

    void FForwardRenderScene::ProcessSpotLight(const SSpotLightComponent& SpotLight, const STransformComponent& TransformComponent, TAtomic<uint32>& LightCount)
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
        Light.Position              = TransformComponent.WorldTransform.GetLocation();
        // Store the to-light direction (surface->spot), matching the sun/directional convention so
        // the shader cone test dot(Direction, L) peaks on the beam axis. -UpdatedForward = aim reversed.
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

    void FForwardRenderScene::AllocateShadowTiles()
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

        // Drop farthest shadow views first to fit GMaxCullViews
        // (camera + cascades + 6/point + 1/spot). Overflow crashes the GPU.
        {
            const uint32 SunViews        = LightData.bHasSun ? (uint32)NumCascades : 0u;
            // Everything BuildCullViews pushes that is not a shadow view. Under-reserving here does not
            // merely trip its ASSERT: shadow views are dropped to fit whatever is left, so any view not
            // counted overflows the cull-view buffer on the GPU. The camera-late view and the capture
            // cameras were already missing from this before probes added six more.
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
                // Sort descending by distance; stable eastl::sort so equal-distance
                // requests keep their input order (deterministic across frames).
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

        // pow2 area budget: all tiles are pow2 so sum(area) <= AtlasSize^2
        // is a sufficient packing guarantee for the quad-tree allocator.
        const uint64 Budget = (uint64)AtlasSize * (uint64)AtlasSize;

        const uint32 NumRequests = (uint32)ShadowRequests.size();

        // Round-up-pow2 clamped to [Min, Max], matching FShadowAtlas::AllocateTile's
        // quantization so the area sum equals what the allocator consumes.
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

        // Point lights cost six tiles (one per cube face), spots one; reflect that
        // in the area accounting so the shrink loop doesn't underestimate point cost.
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

        // Halve the largest tile until the set fits budget; that's the biggest
        // single-step area reduction available.
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
                // All at the floor and still over budget; let the overflow request
                // drop via AllocateTile INDEX_NONE rather than spin.
                break;
            }
            Sizes[LargestIdx] = LargestVal >> 1;
        }

        // Largest-first allocation keeps the quad-tree from fragmenting; small-first
        // would waste root splits on leaves.
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
                // Allocate all six cube-face tiles up front so a partial allocation
                // doesn't leave the light half-shadowed.
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

                // Near plane scales with radius; a fixed 0.01 collapses NDC z to the
                // last ~0.001 of the depth buffer, leaving no precision for PCF.
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

    void FForwardRenderScene::BuildCullViews(const FViewVolume& ViewVolume)
    {
        FFrameData& Frame = *ExtractFrame;
        auto& CullViews                = Frame.Views.CullViews;
        auto& LightData                = Frame.Lighting.LightData;
        auto& PackedShadows            = Frame.Lighting.PackedShadows;
        auto& PointShadowCullViewBases = Frame.Views.PointShadowCullViewBases;
        auto& SpotShadowCullViewBases  = Frame.Views.SpotShadowCullViewBases;
        uint32& CascadeViewBase        = Frame.Views.CascadeViewBase;
        uint32& CascadeLateViewBase    = Frame.Views.CascadeLateViewBase;
        uint32& CameraLateViewIndex    = Frame.Views.CameraLateViewIndex;

        // IndirectArgs slot (v,d) = v*NumDraws + d. CullMeshlets owns all atomic appends.
        // There is no per-view draw-list slice: each (view, draw) owns a packed region whose offset
        // BuildDrawPrefix computes on the GPU and SeedIndirectArgs writes into FirstInstance.
        const uint32 NumDraws = Frame.Views.NumDrawsPerView;

        auto PushView = [&](const FMatrix4& ViewProjection, const FVector3& Origin, uint32 Flags,
                            uint32 CascadeIndex = ~0u, float MinBoundsDiameter = 0.0f)
        {
            // AllocateShadowTiles guarantees the total view count fits in
            // GMaxCullViews before we get here, so no runtime clamp is needed.
            const uint32 ViewIndex = (uint32)CullViews.size();
            FFrustum Frustum = FFrustum::FromViewProjection(ViewProjection);

            FCullView View = {};
            for (int p = 0; p < 6; ++p)
            {
                View.FrustumPlanes[p] = Frustum.Planes[p];
            }
            // Reinterpret flag bits through the w channel; matches the
            // shader's asuint(ViewOriginAndFlags.w) unpack.
            float FlagsAsFloat;
            std::memcpy(&FlagsAsFloat, &Flags, sizeof(float));
            View.ViewOriginAndFlags = FVector4(Origin, FlagsAsFloat);
            View.CascadeIndex       = CascadeIndex;
            View.MinBoundsDiameter  = MinBoundsDiameter;
            View.IndirectArgsOffset = ViewIndex * NumDraws;
            View.NumDraws           = NumDraws;
            CullViews.push_back(View);

            // This view's indirect + mesh-task arg slice is seeded GPU-side
            // (SeedIndirectArgs.slang) from the GPU-built per-draw meshlet prefix.
            return ViewIndex;
        };

        // NumViews <= GMaxCullViews (guaranteed by AllocateShadowTiles). The +1 camera-late view is
        // appended last so earlier view-base indices stay valid; capture views come after that.
        const uint32 NumViews =
            1u +                                                        // Camera (early)
            (LightData.bHasSun ? (uint32)NumCascades : 0u) +            // CSM cascades
            (uint32)PackedShadows[(uint32)ELightType::Point].size() * 6u +
            (uint32)PackedShadows[(uint32)ELightType::Spot].size() +
            1u +                                                        // Camera (late, phase 1)
            (uint32)Frame.Views.CaptureViews.size() +                         // Capture cameras (frustum-only)
            (Frame.ReflectionProbes.BakingProbe >= 0 ? 6u : 0u);              // Reflection-probe cube faces

        ASSERT(NumViews <= (uint32)GMaxCullViews);

        CullViews.reserve(NumViews);

        CascadeViewBase = ~0u;
        CascadeLateViewBase = ~0u;
        CameraLateViewIndex = ~0u;
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
            PushView(CameraVP, ViewVolume.GetViewPosition(), CameraFlags);
        }
        
        if (LightData.bHasSun)
        {
            const int32 SunShadowIndex = LightData.Lights[0].ShadowDataIndex;
            if (SunShadowIndex != INDEX_NONE)
            {
                const FLightShadowData& SunShadow = LightData.Shadows[SunShadowIndex];
                // Frustum and Cone honour the render settings the same way the camera view above does.
                // They used to be unconditional, so disabling either setting silently left the cascades
                // culling -- which makes the settings useless for bisecting a missing-shadow bug, since
                // the one view you wanted to take out of the picture kept going.
                const uint32 CascadeFlags =
                    (RenderSettings.bFrustumCull ? (uint32)ECullViewFlags::Frustum : 0u) |
                    ConeFlag |
                    ECullViewFlags::SunAligned |
                    ECullViewFlags::CastShadowOnly |
                    ECullViewFlags::Distance |
                    ECullViewFlags::Cascade;

                // Published by ProcessDirectionalLight from the active sun, already clamped.
                const float MinTexels = CascadeMinTexels;

                // Phase-2 views, one per cascade, receiving casters the stale cascade Hi-Z hid in phase 0.
                // Deliberately NOT flagged Frustum/Cone/SunAligned/Distance: those rejects already ran in
                // phase 0 and the deferred entry passed them. PhaseLate keeps the early walk from emitting
                // here directly, while CastShadowOnly + Cascade are what CullInstances reads to reserve the
                // same draw-list room the early cascade view got -- without them the late emits are all
                // rejected by EmitMeshlet's bound and the pass silently does nothing.
                const uint32 CascadeLateFlags =
                    ECullViewFlags::PhaseLate |
                    ECullViewFlags::CastShadowOnly |
                    ECullViewFlags::Cascade;

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

                // Contiguous and in the same cascade order, so a defer tag indexes both bases identically.
                // MinBoundsDiameter is left at 0: the micro-poly reject already ran in phase 0, and a
                // deferred entry that passed it there must not be re-tested and dropped here.
                CascadeLateViewBase = (uint32)CullViews.size();
                for (int32 c = 0; c < NumCascades; ++c)
                {
                    PushView(SunShadow.ViewProjection[c], ViewVolume.GetViewPosition(), CascadeLateFlags, (uint32)c);
                }
            }
        }

        // Point lights: 6 views each (one per cube face), cone apex at light position.
        // Parallel array records each face-0 view index for draw-pass lookup.
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

        // Camera-late view: phase 1 re-tests the defer list against the rebuilt HZB and rasters the
        // disoccluded meshlets into the VisBuffer.
        {
            const FMatrix4 CameraVP = ViewVolume.GetProjectionMatrix() * ViewVolume.GetViewMatrix();
            const uint32 CameraLateFlags =
                ECullViewFlags::Occlusion |
                ECullViewFlags::PhaseLate;

            CameraLateViewIndex = (uint32)CullViews.size();
            PushView(CameraVP, ViewVolume.GetViewPosition(), CameraLateFlags);
        }

        // Capture cameras: frustum-only (no occlusion, no two-pass HZB, no shadow flags). The shared cull
        // fills each one's draw-list slice, indexed via CameraViewIndex. Appended last so indices stay valid.
        for (FFrameData::FCaptureViewData& Capture : Frame.Views.CaptureViews)
        {
            const FMatrix4 CaptureVP = Capture.ViewVolume.GetProjectionMatrix() * Capture.ViewVolume.GetViewMatrix();
            const uint32 CaptureFlags = ECullViewFlags::Frustum | ConeFlag;
            Capture.CameraViewIndex = PushView(CaptureVP, Capture.ViewVolume.GetViewPosition(), CaptureFlags);
        }

        // Reflection-probe cube faces: frustum-only, same as capture cameras. Occlusion culling is
        // deliberately off -- the probe view has no depth pyramid of its own, and testing against the
        // primary camera's would cull geometry the probe can see but the camera cannot, punching holes
        // into the baked cube that persist until the next rebuild.
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

    // CCT -> linear RGB tint (Tanner Helland approx), normalized so the brightest channel is 1.0 -- tints
    // the sun without changing intensity (the separate Intensity multiplier does that). ~6500K â‰ˆ white.
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

    void FForwardRenderScene::ProcessDirectionalLight(const SDirectionalLightComponent& DirectionalLight, TAtomic<uint32>& LightCount)
    {
        FFrameData& Frame = *ExtractFrame;
        auto& LightData            = Frame.Lighting.LightData;
        auto& ShadowDataCount      = Frame.Lighting.ShadowDataCount;
        auto& SceneGlobalData      = Frame.SceneGlobalData;

        LightData.bHasSun = true;
        // Primary camera (frame snapshot), not the render-phase SceneViewport: CSM cascades
        // fit to the primary view; capture views reuse these cascades rather than fitting their own.
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

        // Allocate a shadow slot only when the light casts shadows; ShadowDataIndex stays
        // INDEX_NONE otherwise, the sentinel everything downstream uses to skip shadow work.
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
        
        // Cascade shadows are now configured per-light. Feed the cull pass this
        // light's max distance so shadow-caster culling matches the cascades.
        SceneGlobalData.CullData.ShadowMaxDistance = DirectionalLight.ShadowMaxDistance;

        // The cascade cull knobs travel with the light too. Only ever narrows: the scene switch set during
        // extract already decided whether occlusion culling is allowed at all here.
        if (!DirectionalLight.bCascadeOcclusionCull)
        {
            SceneGlobalData.CullData.bShadowOcclusionCull = 0u;
        }
        // Read by BuildCullViews, which runs later in the same extract. Clamped here so the consumer
        // does not have to care that the property is user-authored.
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

        // Republish the transforms that produced the cascade pyramid's CURRENT contents before this frame's
        // loop overwrites them. The pyramid is built after the shadow raster, so a cull can only ever test
        // against the previous frame's -- projecting with this frame's matrices would misregister the tap by
        // however far the cascade re-snapped.
        if (bCascadeHZBTransformsValid)
        {
            for (int i = 0; i < NumCascades; ++i)
            {
                SceneGlobalData.CullData.CascadeHZBViewProjection[i] = CascadeHZBViewProjection[i];
                SceneGlobalData.CullData.CascadeHZBNdcScale[i]       = CascadeHZBNdcScale[i];
            }
            SceneGlobalData.CullData.bCascadeHZBValid = 1u;
        }

        // Blend-band widening for the caster volumes below. A pixel in band i-1 samples cascade i over the
        // last ShadowParams.w of its band (see ComputeDirectionalShadow), so cascade i is responsible for a
        // slice that starts BEFORE its own near split. Culling to the split alone would strip the occluders
        // out of exactly the region the two cascades cross-fade over, and the seam would show as shadows
        // fading out and back in.
        const float CascadeBlendFraction = Math::Clamp(DirectionalLight.CascadeBlend, 0.0f, 1.0f);

        float LastSplitDistance = ShadowNear;
        for (int i = 0; i < NumCascades; ++i)
        {
            const float SplitNear = LastSplitDistance;
            const float SplitFar  = CascadeFarDistances[i];

            // Per-cascade resolution (outer cascades smaller); texel-size math below
            // reads it so snap step and world-space texel pitch shift accordingly.
            const int   CascadeRes        = GCSMCascadeSizes[i];
            const float CascadeResFloat   = (float)CascadeRes;
            LightData.CascadeResolutions[i] = CascadeResFloat;

            // World-space corners of sub-frustum [SplitNear, SplitFar]. Standard-Z perspective
            // so ComputeFrustumCorners un-projects the canonical NDC cube despite reverse-Z.
            const FMatrix4 SliceProj = Math::Perspective(Math::Radians(CamFOV), CamAspect, SplitNear, SplitFar);
            const FMatrix4 SliceVP   = SliceProj * CamView;

            FVector3 Corners[8];
            FFrustum::ComputeFrustumCorners(SliceVP, Corners);

            // Bound the slice with a sphere, rotation-invariant, so the cascade
            // size doesn't pulse as the camera turns.
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

            // BackDistance pushes the light eye behind the cascade so off-screen occluders
            // still write depth; low sun angles need larger values (D/tan(theta) light-space height).
            const float BackDistance = Math::Max(DirectionalLight.CascadeBackDistance, 1.0f);
            const float OrthoRange   = Radius * 2.0f + BackDistance;

            // lookAt target = origin (not SphereCenter) so the rotation
            // depends only on LightDir; otherwise the texel snap below collapses.
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

            // Expose half-extent so the lit pixel shader converts a shadow texel to a
            // world-space length for normal-offset bias.
            LightData.CascadeRadii[i] = Radius;

            // The cull volume, which is deliberately NOT this cascade's ortho box. The box is fitted to a
            // sphere around the slice, and that sphere's radius is set by the slice's lateral extent at its
            // far plane -- for any reasonable FOV that makes it wider than the slice is deep, so cascade i's
            // box swallows every box before it. Culling on the box alone therefore let essentially the whole
            // shadow-casting scene into all four cascades, which is the 4x geometry this replaces.
            //
            // What a cascade actually needs is: everything that can occlude a pixel it shades. That is the
            // camera sub-frustum slice it owns, swept toward the sun. Slices do not nest.
            {
                // Length of the PRECEDING band, measured the way the shader measures it: cascade 0's band
                // starts at 0, not at the shadow near plane (ComputeDirectionalShadow hardcodes that).
                const float PrevBandNear = (i >= 2) ? CascadeFarDistances[i - 2] : 0.0f;
                const float BlendBack    = (i == 0) ? 0.0f : CascadeBlendFraction * (SplitNear - PrevBandNear);

                // Cascade 0 culls from the camera's own near plane, not from ShadowNear. The cascade FIT
                // starts at ShadowNear because that is where the texel budget is worth spending, but pixels
                // in front of it still select cascade 0 and still want their occluders.
                const float MinCullNear = Math::Max(NearClip, 0.01f);
                const float CullNear    = (i == 0) ? MinCullNear : Math::Max(SplitNear - BlendBack, MinCullNear);
                const FMatrix4 CullProj = Math::Perspective(Math::Radians(CamFOV), CamAspect, CullNear, SplitFar);

                // Swept by the full ortho depth: a caster farther from the receivers than that is behind the
                // light eye and outside the box test above anyway, so there is nothing to gain by sweeping
                // further and correctness to lose by sweeping less.
                const FFrustum SliceFrustum = FFrustum::FromViewProjection(CullProj * CamView);
                SceneGlobalData.CullData.CascadeFrustum[i] = AsGPU(SliceFrustum.Extruded(LightDir, OrthoRange));
            }

            // Record the transform + NDC scale that this frame's raster will write into the pyramid, for
            // NEXT frame's occlusion test. Scale is carried rather than derived from the matrix so the
            // shader never has to care whether it is indexing rows or columns.
            CascadeHZBViewProjection[i] = CascadeVP;
            CascadeHZBNdcScale[i]       = FVector4(1.0f / Radius, 1.0f / Radius, 1.0f / OrthoRange, 0.0f);

            // Same values, published for THIS frame rather than next: the phase-2 re-test reads the pyramid
            // rebuilt mid-frame from this raster, so it needs the matrices that raster used. The pair above
            // is the previous frame's by the time the GPU sees it, which is exactly what phase 0 wants and
            // exactly what phase 2 must not use.
            SceneGlobalData.CullData.CascadeHZBViewProjectionMid[i] = CascadeVP;
            SceneGlobalData.CullData.CascadeHZBNdcScaleMid[i]       = CascadeHZBNdcScale[i];

            LastSplitDistance = SplitFar;
        }

        // Only true once the loop above has run at least once; the republish at the top reads it.
        bCascadeHZBTransformsValid = true;

        // This frame's cascade transforms are now published, so the phase-2 re-test has matrices matching
        // the pyramid it will read. Ordering within the frame is enforced on the command list, not here:
        // CascadePyramidPass is recorded before CullPassCascadeLate, which also gates on the pyramid.
        SceneGlobalData.CullData.bCascadeHZBMidValid = 1u;

        LightCount.fetch_add(1, std::memory_order_acquire);
        LightData.Lights[0] = Light;
    }

    uint32 FForwardRenderScene::PrepareBatchedLines(FLineBatcherComponent& Batcher)
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
    
    void FForwardRenderScene::BatchLineChunks(const Task::FParallelRange& Range)
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
                        // Clamp to the last bucket if the (thickness, depth-test) combos ever exceed
                        // the cap; the original serial path made the same <= kMaxBuckets assumption.
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

    // Runs after the batch node: merge per-worker buckets, lay out a contiguous vertex range, scatter, and
    // rebuild the persistent line list. Reads the same LineBatchScratch the batch node filled.
    void FForwardRenderScene::FinalizeBatchedLines(FLineBatcherComponent& Batcher)
    {
        using FLineInstance = FLineBatcherComponent::FLineInstance;
        constexpr uint32 kMaxBuckets = FLineBatchScratch::kMaxBuckets;

        FFrameData& Frame       = *ExtractFrame;
        auto& SimpleVertices    = Frame.Primitives.SimpleVertices;
        auto& LineBatches       = Frame.Primitives.LineBatches;

        TVector<FLineInstance>& Lines = Batcher.Lines;
        auto& ThreadBuffers           = Batcher.ThreadBuffers;

        const uint32 NumThreads = GTaskSystem->GetNumTaskThreads();

        // Merge per-worker buckets into a global table keyed by (thickness, depth-test), and
        // accumulate each global bucket's vertex count.
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

    void FForwardRenderScene::ProcessBatchedTriangles(FTriangleBatcherComponent& Batcher)
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

    void FForwardRenderScene::NotifyMaxLightsHit()
    {
        LOG_WARN("[Rendering] - Maximum Lights Hit! {}", MAX_LIGHTS);
    }

    void FForwardRenderScene::DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale)
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

    void FForwardRenderScene::ResetPass_Extract()
    {
        FFrameData& Frame = *ExtractFrame;

        Frame.Primitives.SimpleVertices.clear();
        Frame.Primitives.LineBatches.clear();
        Frame.Primitives.SolidVertices.clear();
        Frame.Primitives.SolidBatches.clear();
        Frame.Views.CullViews.clear();
        Frame.Views.CaptureViews.clear();
        Frame.Views.CameraLateViewIndex = ~0u;
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
    void FForwardRenderScene::ResetGeometry_Extract()
    {
        FFrameData& Frame = *ExtractFrame;

        Frame.Geometry.DrawCommands.clear();
        Frame.Geometry.OpaqueDrawList.clear();
        Frame.Geometry.TranslucentDrawList.clear();
        Frame.Geometry.BonesData.clear();
        Frame.Geometry.SkinDescriptors.clear();
        Frame.Geometry.TotalPreSkinnedVertices = 0;
        Frame.Views.NumDrawsPerView   = 0;
        // Instances is intentionally NOT cleared: the merge resizes it to the exact count and the
        // parallel instance write overwrites every slot, so leaving the size in place means EASTL
        // value-initializes (memsets) only growth beyond the previous frame, not the whole array.
    }

    void FForwardRenderScene::ResetPass_Render(RHI::FCmdListH CL)
    {
        // VisBuffer phase 1 clears depth when it runs (LoadOp); with no geometry it early-outs, so clear
        // here for the downstream depth consumers (terrain, water, transparent, fog).
        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            const float DepthClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::DepthAttachment).Texture, DepthClear);
        }

        // Atlas/cascade clears stay unconditional: shadow passes only clear their own
        // tiles, so regions no light renders into still need clearing here.
        const float ShadowClear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        RHI::CmdClearTexture(CL, ShadowAtlas.GetImage().Texture, ShadowClear);
        RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::Cascade).Texture, ShadowClear);

        Barriers::TransferToAll(CL);
    }


    struct FCullMeshletPushConstants
    {
        uint32 NumViews;
        uint32 Phase;
        uint32 CameraLateViewIndex;
        uint32 NumSurfaceDescs;     // bounds the per-view LOD lookup; what the DEVICE buffer holds
        // Device addresses of the cull-private scratch buffers (matches the pointer
        // fields in CullMeshlets.slang's pass block, 8-byte aligned).
        uint64 IndirectArgsAddr;
        uint64 DeferListAddr;
        uint64 DeferCountAddr;
        uint64 MeshDrawArgsAddr;   // GroupCountX accumulated here (mesh path; replaces ConvertMeshDrawArgs)
        uint64 TotalsAddr;          // {MeshletWork, InstanceCount, DrawListRequired, Overflowed}
        uint64 ViewDrawCountsAddr;  // per-(view, draw) region length; EmitMeshlet exact bound
        uint64 ViewDrawOffsetsAddr; // per-(view, draw) region base; read-only, off the contended args line
        // Interned per-surface LOD tables. The same buffer CullInstances reserved draw-list space from, so
        // both passes make each view's LOD pick from identical inputs. Grouped with the other pointers so
        // the trailing uint32 pair needs no alignment padding on either side.
        uint64 SurfaceDescsAddr;
        uint32 DrawListCapacity;    // entries in the whole allocation; outer backstop
        uint32 DeferListCapacity;   // entries the defer list holds; bounds the unbounded defer append
        uint32 GridThreads;         // early phase only: the stride of its grid-stride walk
        uint32 CascadeLateViewBase; // cascade-late phase only: first of NumCascades contiguous views
    };
    static_assert(sizeof(FCullMeshletPushConstants) == 96, "FCullMeshletPushConstants must match CullMeshlets.slang FPushConstants.");

    // Early-cull grid, sized to the DEVICE rather than to the domain.
    //
    // The shader strides over the whole domain whatever grid it gets, so this decides only how much
    // parallelism the scheduler is handed. Both bounds are therefore performance knobs, not correctness
    // ones: too small costs extra loop iterations, too large costs a few empty workgroup launches.
    // Neither can drop a meshlet, and no scene value reaches a dispatch dimension.
    //
    // The max is "comfortably saturates any consumer GPU" -- past full occupancy extra groups just queue,
    // which is what the loop already does for free. 8192 x 64 = 524288 threads, against ~4.2M GROUPS the
    // old exact-size indirect dispatch could ask for. The min covers the frames before the first feedback
    // readback lands, where the measured demand still reads zero.
    static constexpr uint32 kMinEarlyCullGroups = 256;
    static constexpr uint32 kMaxEarlyCullGroups = 8192;

    void FForwardRenderScene::CullPassEarly(RHI::FCmdListH CL)
    {
        const FFrameData& Frame             = *RenderFrame;
        const auto& DrawCommands            = Frame.Geometry.DrawCommands;
        const auto& CullViews               = Frame.Views.CullViews;
        const uint32 CameraLateViewIndex    = Frame.Views.CameraLateViewIndex;

        if (DrawCommands.empty() || CullViews.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cull Pass (Early)", tracy::Color::Pink2);

        static const FShaderEntry* const CullShader = FShaderLibrary::Get("CullMeshlets.slang");
        if (!CullShader)
        {
            return;
        }
        
        {
            const struct { const char* Name; RHI::GPUPtr Addr; } Required[] =
            {
                { "IndirectArgs",          GetIndirectArgs().GetAddress()          },
                { "MeshletDeferList",      GetMeshletDeferList().GetAddress()      },
                { "DeferCount",            GetDeferCount().GetAddress()            },
                { "MeshDrawArgs",          GetMeshDrawArgs().GetAddress()          },
                { "Totals",                GetTotals().GetAddress()                },
                { "ViewDrawCounts",        GetViewDrawCounts().GetAddress()        },
                { "ViewDrawOffsets",       GetViewDrawOffsets().GetAddress()       },
            };

            for (const auto& Buffer : Required)
            {
                if (Buffer.Addr == 0)
                {
                    // Throttled: if one allocation is failing they all will, every frame.
                    static uint32 NullBufferLogCounter = 0;
                    if ((NullBufferLogCounter++ % 120u) == 0u)
                    {
                        LOG_ERROR("RenderScene: skipping early cull -- scene buffer '{}' has no allocation. "
                                  "Dispatching would read device address 0 and fault the GPU.", Buffer.Name);
                    }
                    return;
                }
            }
        }

        RHI::CmdMemset(CL, GetDeferCount().Ptr, GetDeferCount().Size, 0u);
        Barriers::TransferToAll(CL);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullShader));

        FCullMeshletPushConstants PC = {};
        PC.NumViews            = (uint32)CullViews.size();
        PC.Phase               = (uint32)ECullPhase::Early;
        PC.CameraLateViewIndex = CameraLateViewIndex;
        PC.IndirectArgsAddr    = GetIndirectArgs().GetAddress();
        PC.DeferListAddr       = GetMeshletDeferList().GetAddress();
        PC.DeferCountAddr      = GetDeferCount().GetAddress();
        PC.MeshDrawArgsAddr    = GetMeshDrawArgs().GetAddress();
        PC.TotalsAddr          = GetTotals().GetAddress();
        PC.ViewDrawCountsAddr  = GetViewDrawCounts().GetAddress();
        PC.ViewDrawOffsetsAddr = GetViewDrawOffsets().GetAddress();
        // What the device buffer actually holds, not what the game thread has interned -- same bound
        // CullInstances uses, so an instance whose binding is newer than the last upload falls back to its
        // fixed range in both passes instead of reading past the table in one of them.
        PC.NumSurfaceDescs     = UploadedSurfaceDescs;
        PC.SurfaceDescsAddr    = SurfaceDescBuffer.GetAddress();
        PC.DrawListCapacity    = DrawListCapacity;
        PC.DeferListCapacity   = DeferListCapacity;

        // Grid from the measured domain, clamped to the device. LastMeshletWorkRequested is the
        // PRE-clamp total (Totals[8]) from kFramesInFlight ago; the post-clamp one would be
        // self-reinforcing. Staleness is harmless here -- a scene that just got heavier only makes the
        // loop iterate more, where it used to make the dispatch drop the tail.
        const uint32 DemandGroups = (LastMeshletWorkRequested + 63u) / 64u;
        const uint32 Groups       = Math::Clamp(DemandGroups, kMinEarlyCullGroups, kMaxEarlyCullGroups);
        PC.GridThreads            = Groups * 64u;

        RHI::CmdDispatch(CL, MakeArgs(PC), Groups, 1u, 1u);
        Barriers::ComputeToAll(CL);
        
        static const FShaderEntry* const DispatchArgsShader = FShaderLibrary::Get("BuildCullDispatchArgs.slang");
        if (DispatchArgsShader && CameraLateViewIndex != ~0u)
        {
            struct FBuildCullDispatchArgsPC
            {
                uint32 DeferListCapacity;
                uint32 Pad0;
                uint64 DeferCountAddr;
                uint64 DispatchArgsAddr;
                uint64 OutTotalsAddr;
            } DPC = {};
            static_assert(sizeof(FBuildCullDispatchArgsPC) == 32, "FBuildCullDispatchArgsPC must match BuildCullDispatchArgs.slang.");
            DPC.DeferListCapacity = DeferListCapacity;
            DPC.DeferCountAddr    = GetDeferCount().GetAddress();
            DPC.DispatchArgsAddr  = GetCullDispatchArgs().GetAddress();
            DPC.OutTotalsAddr     = GetTotals().GetAddress();
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(DispatchArgsShader));
            RHI::CmdDispatch(CL, MakeArgs(DPC), 1u, 1u, 1u);
            Barriers::ComputeToAll(CL);   // orders the args write before the late CmdDispatchIndirect reads it
        }

        // The EARLY raster is issued before the late cull runs, so its args have to be trimmed here.
        ClampDrawArgs(CL);
    }

    void FForwardRenderScene::ClampDrawArgs(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const uint32 NumCullViews = (uint32)Frame.Views.CullViews.size();
        const uint32 NumDraws     = Frame.Views.NumDrawsPerView;
        if (NumCullViews == 0u || NumDraws == 0u)
        {
            return;
        }

        static const FShaderEntry* const ClampShader = FShaderLibrary::Get("ClampDrawArgs.slang");
        if (!ClampShader)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Clamp Draw Args", tracy::Color::Pink4);
        SCENE_GPU_SCOPE(CL, "Clamp Draw Args");

        struct FClampDrawArgsPC
        {
            uint32 NumViews;
            uint32 NumDraws;
            uint32 Pad0;
            uint32 DrawListCapacityArg;
            uint64 IndirectArgsAddr;
            uint64 MeshDrawArgsAddr;
            uint64 ViewDrawCountsAddr;
            uint64 ViewDrawOffsetsAddr;
        } PC = {};
        static_assert(sizeof(FClampDrawArgsPC) == 48, "FClampDrawArgsPC must match ClampDrawArgs.slang.");

        PC.NumViews         = NumCullViews;
        PC.NumDraws         = NumDraws;
        PC.DrawListCapacityArg = DrawListCapacity;
        PC.IndirectArgsAddr    = GetIndirectArgs().GetAddress();
        PC.MeshDrawArgsAddr    = GetMeshDrawArgs().GetAddress();
        PC.ViewDrawCountsAddr  = GetViewDrawCounts().GetAddress();
        PC.ViewDrawOffsetsAddr = GetViewDrawOffsets().GetAddress();

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ClampShader));
        const uint32 TotalSlots = NumCullViews * NumDraws;
        RHI::CmdDispatch(CL, MakeArgs(PC), (TotalSlots + 63u) / 64u, 1u, 1u);

        // The indirect args are consumed by the raster right after this.
        Barriers::ComputeToAll(CL);
    }

    void FForwardRenderScene::CullPassLate(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands     = Frame.Geometry.DrawCommands;
        const auto& CullViews        = Frame.Views.CullViews;
        const uint32 CameraLateViewIndex = Frame.Views.CameraLateViewIndex;

        if (DrawCommands.empty() || CullViews.empty())
        {
            return;
        }

        if (CameraLateViewIndex == ~0u)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cull Pass (Late)", tracy::Color::Pink3);

        static const FShaderEntry* const CullShader = FShaderLibrary::Get("CullMeshlets.slang");
        if (!CullShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullShader));

        FCullMeshletPushConstants PC = {};
        PC.NumViews            = (uint32)CullViews.size();
        PC.Phase               = (uint32)ECullPhase::Late;
        PC.CameraLateViewIndex = CameraLateViewIndex;
        PC.IndirectArgsAddr    = GetIndirectArgs().GetAddress();
        PC.DeferListAddr       = GetMeshletDeferList().GetAddress();
        PC.DeferCountAddr      = GetDeferCount().GetAddress();
        PC.MeshDrawArgsAddr    = GetMeshDrawArgs().GetAddress();
        PC.TotalsAddr          = GetTotals().GetAddress();
        PC.ViewDrawCountsAddr  = GetViewDrawCounts().GetAddress();
        PC.ViewDrawOffsetsAddr = GetViewDrawOffsets().GetAddress();
        // The late phase re-tests deferred entries that already name their meshlet, so it makes no LOD pick
        // of its own; passed anyway so the two dispatches differ only in Phase.
        PC.NumSurfaceDescs     = UploadedSurfaceDescs;
        PC.SurfaceDescsAddr    = SurfaceDescBuffer.GetAddress();
        PC.DrawListCapacity    = DrawListCapacity;
        PC.DeferListCapacity   = DeferListCapacity;

        // Indirect dispatch sized to the deferred set (BuildCullDispatchArgs wrote {ceil(DeferCount/64),1,1} from
        // DeferCount after the early cull). The 1D grid keeps RunLatePhase's flat-index = GlobalID.x; the
        // in-shader idx>=DeferCount guard backstops, and GroupCountX==0 (nothing deferred) launches no waves.
        RHI::CmdDispatchIndirect(CL, MakeArgs(PC), GetCullDispatchArgs().Ptr, 0);

        Barriers::ComputeToAll(CL);

        // Late phase appended into the camera-late slice; trim before that slice is rasterized.
        ClampDrawArgs(CL);
    }

    // Phase 2 of the CASCADE cull. Runs after CascadedShowPass rastered the early casters and
    // CascadePyramidPass rebuilt the cascade pyramid from them, which is the whole point: phase 0 tested
    // against a pyramid a frame old and DEFERRED what it hid rather than dropping it, so this is where a
    // caster the stale pyramid wrongly hid gets its shadow back in the same frame.
    //
    // A separate dispatch from CullPassLate because the camera pyramid and the cascade pyramid are rebuilt
    // at different points in the frame. Both walk the same defer list and filter on the entry's tag.
    void FForwardRenderScene::CullPassCascadeLate(RHI::FCmdListH CL)
    {
        const FFrameData& Frame              = *RenderFrame;
        const auto& DrawCommands             = Frame.Geometry.DrawCommands;
        const auto& CullViews                = Frame.Views.CullViews;
        const uint32 CascadeLateViewBase     = Frame.Views.CascadeLateViewBase;

        if (DrawCommands.empty() || CullViews.empty() || CascadeLateViewBase == ~0u)
        {
            return;
        }

        // Nothing was deferred if the cascade Hi-Z never ran, and the pyramid it re-tests against is only
        // meaningful if this frame actually rastered cascades.
        if (!bCascadePyramidValid.load(std::memory_order_acquire))
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cull Pass (Cascade Late)", tracy::Color::Pink4);

        static const FShaderEntry* const CullShader = FShaderLibrary::Get("CullMeshlets.slang");
        if (!CullShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullShader));

        FCullMeshletPushConstants PC = {};
        PC.NumViews             = (uint32)CullViews.size();
        PC.Phase                = (uint32)ECullPhase::CascadeLate;
        PC.CameraLateViewIndex  = Frame.Views.CameraLateViewIndex;
        PC.CascadeLateViewBase  = CascadeLateViewBase;
        PC.IndirectArgsAddr     = GetIndirectArgs().GetAddress();
        PC.DeferListAddr        = GetMeshletDeferList().GetAddress();
        PC.DeferCountAddr       = GetDeferCount().GetAddress();
        PC.MeshDrawArgsAddr     = GetMeshDrawArgs().GetAddress();
        PC.TotalsAddr           = GetTotals().GetAddress();
        PC.ViewDrawCountsAddr   = GetViewDrawCounts().GetAddress();
        PC.ViewDrawOffsetsAddr  = GetViewDrawOffsets().GetAddress();
        PC.NumSurfaceDescs      = UploadedSurfaceDescs;
        PC.SurfaceDescsAddr     = SurfaceDescBuffer.GetAddress();
        PC.DrawListCapacity     = DrawListCapacity;
        PC.DeferListCapacity    = DeferListCapacity;

        // Same args the camera-late dispatch used: BuildCullDispatchArgs sized them from the whole defer
        // list, which is what this phase walks too. It is written once after the early cull and read by
        // both late dispatches, so nothing has to re-derive it.
        RHI::CmdDispatchIndirect(CL, MakeArgs(PC), GetCullDispatchArgs().Ptr, 0);

        Barriers::ComputeToAll(CL);

        // Trim the cascade-late slices before they are rasterized.
        ClampDrawArgs(CL);
    }


    void FForwardRenderScene::SkinningPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const uint32 DescriptorCount = (uint32)Frame.Geometry.SkinDescriptors.size();
        if (DescriptorCount == 0)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Skinning Pass", tracy::Color::SkyBlue);

        static const FShaderEntry* const SkinShader = FShaderLibrary::Get("Skinning.slang");
        if (!SkinShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(SkinShader));

        // Capacity comes from the allocation, not from the demand that sized it: if the allocation
        // came back short (or failed outright) the shader drops those stores instead of faulting.
        const uint32 VertexCapacity = (uint32)Math::Min<uint64>(
            GetPreSkinnedVerticesBuffer().GetSize() / sizeof(FPreSkinnedVertex), 0xFFFFFFFFull);

        struct FSkinningPushConstants { uint32 DescriptorCount; uint32 VertexCapacity; }
        PC{ DescriptorCount, VertexCapacity };

        // One workgroup per skinned entity; fold across X/Y past the 65535 per-axis cap.
        constexpr uint32 MaxDispatchAxis = 65535u;
        const uint32 DispatchX = DescriptorCount < MaxDispatchAxis ? DescriptorCount : MaxDispatchAxis;
        const uint32 DispatchY = (DescriptorCount + MaxDispatchAxis - 1u) / MaxDispatchAxis;
        RHI::CmdDispatch(CL, MakeArgs(PC), DispatchX, DispatchY, 1u);

        // Pre-skinned vertices feed every draw VS.
        Barriers::ComputeToAll(CL);
    }

    void FForwardRenderScene::TexturePaintPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.Extracts.PaintOps.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Texture Paint Pass", tracy::Color::Red);

        static const FShaderEntry* const PaintShader = FShaderLibrary::Get("TexturePaint.slang");
        if (!PaintShader)
        {
            return;
        }

        RHI::FPipelineH Pipeline = GetOrCreateComputePipeline(PaintShader);

        // Push-constant block mirroring TexturePaint.slang; fields aligned so offsets match under scalar
        // and std430. Color is four scalars (not a vec4) to keep it that way.
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

    void FForwardRenderScene::VisBufferPass(RHI::FCmdListH CL, uint32 ViewIndex, bool bClear)
    {
        const FFrameData& Frame             = *RenderFrame;
        const auto& DrawCommands            = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList          = Frame.Geometry.OpaqueDrawList;
        const bool   bMeshShaders           = CVarMeshShaders.GetValue() && RHI::SupportsMeshShaders();
        const uint32 NumDrawsPerView        = Frame.Views.NumDrawsPerView;

        if (DrawCommands.empty() || ViewIndex == ~0u)
        {
            return;
        }

        // Every device address this pass's shaders dereference, checked while it still has a name.
        // MeshMain's first instruction indexes ViewDrawOffsets, and ResolveMeshlet walks
        // MeshletDrawList and Instances immediately after; none of the three is guarded GPU-side.
        //
        // Instances is not a theoretical null: BuildSceneRoot only fills it when the visible-instance
        // ring is allocated, and leaves it zero otherwise. Reaching a shader that way costs the device
        // -- Aftermath reports "mesh_02, MMU Fault Error, GPU virtual address 0", which names neither
        // the buffer nor the pass, so the fault has to be caught here or not at all.
        {
            const RHI::GPUPtr DrawListAddr  = GetMeshletDrawList().GetAddress();
            const RHI::GPUPtr InstancesAddr = SceneRootShared.Instances;
            const RHI::GPUPtr OffsetsAddr   = GetViewDrawOffsets().GetAddress();

            const char* MissingBuffer = DrawListAddr == 0  ? "MeshletDrawList"
                                      : InstancesAddr == 0 ? "Instances (visible-instance ring)"
                                      : OffsetsAddr == 0   ? "ViewDrawOffsets"
                                                           : nullptr;
            if (MissingBuffer != nullptr)
            {
                // Once per occurrence rather than once per frame would spam a stuck frame forever;
                // once ever would hide a second, different buffer going null later.
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

        static const FShaderEntry* const VisPixel = FShaderLibrary::Get("VisBufferPixel.slang");
        // (slang #7019)
        static const FString VisPrimIdDefine("VISBUFFER_PRIMID");
        static const FShaderEntry* const VisPixelPrim = FShaderLibrary::Get("VisBufferPixel.slang", TSpan<const FString>(&VisPrimIdDefine, 1));
        if (!VisPixel)
        {
            return;
        }

        // Cull output (draw list + indirect args) and skinning output feed this pass; the depth target
        // may also have been transfer-cleared. Phase 2 also reads the pyramid the late cull just used.
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

        const uint32 ViewBase = ViewIndex * NumDrawsPerView;

        for (uint32 Idx : OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = DrawCommands[Idx];
            
            const bool bUseMesh  = bMeshShaders && Batch.VisBufferMeshShader != nullptr;
            const FShaderEntry* MaskedPS = bUseMesh ? Batch.MaskedVisBufferPixelShaderPrim : Batch.MaskedVisBufferPixelShader;
            // The mesh path needs the MASKED geometry variant too: the opaque one emits position only, and
            // pairing it with the masked PS would interpolate attributes nothing wrote. Requiring both is
            // what keeps that combination unreachable -- without the geometry shader the batch renders
            // unclipped rather than reading garbage.
            const bool bMaskedClip = Batch.bMasked && MaskedPS != nullptr
                                  && (!bUseMesh || Batch.VisBufferMeshShaderMasked != nullptr);

            FGraphicsPipelineKey Key;
            Key.VS               = bUseMesh ? nullptr : Batch.VisBufferVertexShader;
            Key.MS               = bUseMesh ? (bMaskedClip ? Batch.VisBufferMeshShaderMasked : Batch.VisBufferMeshShader) : nullptr;
            Key.PS               = bMaskedClip ? MaskedPS : (bUseMesh ? VisPixelPrim : VisPixel);
            Key.bVisBufferMasked = bMaskedClip;   // geometry emits interpolants only when actually masked-clipping
            Key.bWireframe       = RenderSettings.bWireframe;
            Key.SkinnedMode      = (Batch.bAnySkinned && Batch.bAnyStatic) ? 2u : (Batch.bAnySkinned ? 1u : 0u);
            Key.SampleCount      = MSAASampleCount;
            Key.DepthFormat = EFormat::D32;
            Key.ColorTargets.push_back({ VisRT.Desc.Format, {} });
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));
            
            if (RenderSettings.bWireframe)
            {
                RHI::CmdSetLineWidth(CL, 1.5f);
            }

            // Two-sided materials must rasterize both faces into the VisBuffer.
            RHI::CmdSetCullMode(CL, Batch.bTwoSided ? RHI::ECullMode::None : RHI::ECullMode::Back);

            if (bUseMesh)
            {
                struct { uint64 ViewDrawOffsetsAddr; uint32 ArgBase; } VisArgs;
                VisArgs.ViewDrawOffsetsAddr = GetViewDrawOffsets().GetAddress();
                VisArgs.ArgBase             = ViewBase + Batch.IndirectDrawOffset;
                RHI::CmdDrawMeshTasksIndirect(CL, MakeArgs(VisArgs),
                    GetMeshDrawArgs().Ptr, (ViewBase + Batch.IndirectDrawOffset) * sizeof(RHI::FDrawMeshTasksIndirectArguments),
                    Batch.DrawCount, sizeof(RHI::FDrawMeshTasksIndirectArguments));
            }
            else
            {
                // VS-emulation path: FirstInstance carries the meshlet base, so no push constant is needed.
                RHI::CmdDrawIndirect(CL, MakeArgs(),
                    GetIndirectArgs().Ptr, (ViewBase + Batch.IndirectDrawOffset) * sizeof(RHI::FDrawIndirectArguments),
                    Batch.DrawCount, sizeof(RHI::FDrawIndirectArguments));
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FForwardRenderScene::BuildDepthPyramid(RHI::FCmdListH CL, const FSceneImage& Source, const FSceneImage& Pyramid, bool bReduceMax)
    {
        static const FShaderEntry* const ComputeShader = FShaderLibrary::Get("DepthPyramidSPD.slang");
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
        // GetResourceID and GetMipUAVIndex both report "no slot" as -1, and both are cast to uint32 on
        // the way into a bindless index. That cast turns the sentinel into 0xFFFFFFFF, which the shader
        // indexes the texture heap with -- a descriptor read four billion entries past the end, whose
        // contents are whatever memory follows. The resulting unmapped access is a device loss inside
        // the SPD dispatch, attributed to a shader that did nothing wrong.
        //
        // Neither GPU-assisted validation check catches it: the read is through a descriptor, not a
        // buffer device address, so the buffer-address range check never sees it.
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

    void FForwardRenderScene::DepthPyramidPass(RHI::FCmdListH CL)
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

    void FForwardRenderScene::CascadePyramidPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& LightData   = Frame.Lighting.LightData;

        // Mirrors CascadedShowPass's guards exactly. If it rendered nothing, the atlas still holds the 1.0
        // clear and pyramiding it would publish a "nothing occludes anything" HZB -- harmless but pointless.
        // If it DID render and we skipped the pyramid, next frame would test against a stale one, so these
        // two must agree.
        if (!LightData.bHasSun ||
            Frame.Geometry.DrawCommands.empty() ||
            LightData.Lights[0].ShadowDataIndex == INDEX_NONE ||
            Frame.Views.CascadeViewBase == ~0u)
        {
            bCascadePyramidValid.store(false, std::memory_order_release);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cascade Pyramid Pass (SPD)", tracy::Color::Orange3);

        // Standard-Z cascade atlas (Less test, cleared to 1.0): MAX reduction keeps the farthest-from-light
        // occluder, which is the one a caster has to be behind to contribute nothing.
        BuildDepthPyramid(CL,
            GetNamedImage(ENamedImage::Cascade),
            GetNamedImage(ENamedImage::CascadePyramid),
            /*bReduceMax*/ true);

        bCascadePyramidValid.store(true, std::memory_order_release);
    }

    void FForwardRenderScene::ClusterBuildPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        const bool bHasTerrain = !Frame.Extracts.TerrainExtracts.empty();
        if (DrawCommands.empty() && !bHasTerrain)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cluster Build Pass", tracy::Color::Pink2);
        
        static const FShaderEntry* const ComputeShader = FShaderLibrary::Get("ClusterBuild.slang");
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

    void FForwardRenderScene::LightCullPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        const bool bHasTerrain = !Frame.Extracts.TerrainExtracts.empty();
        if (DrawCommands.empty() && !bHasTerrain)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Light Cull Pass", tracy::Color::Pink2);

        static const FShaderEntry* const ComputeShader = FShaderLibrary::Get("LightCull.slang");
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

    bool FForwardRenderScene::BindShadowBatchPipeline(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, const FShaderEntry* PixelShader)
    {
        const bool bUseMesh = CVarMeshShaders.GetValue() && RHI::SupportsMeshShaders() && Batch.MeshShader != nullptr;

        FGraphicsPipelineKey Key;
        Key.VS          = bUseMesh ? nullptr : Batch.VertexShader;
        Key.MS          = bUseMesh ? Batch.MeshShader : nullptr;
        Key.PS          = PixelShader;
        Key.PassVariant = EMeshPass::Shadow;
        Key.DepthFormat = EFormat::D32;
        // SPEC_SKINNED: homogeneous batch -> dead-strip the unused vertex-load path; mixed -> runtime branch (2).
        Key.SkinnedMode = (Batch.bAnySkinned && Batch.bAnyStatic) ? 2u : (Batch.bAnySkinned ? 1u : 0u);
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));
        return bUseMesh;
    }

    void FForwardRenderScene::DrawShadowBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, bool bUseMesh,
        uint32 CullViewIndex, int32 ShadowDataIndex, int32 ShadowViewIndex)
    {
        const uint32 ViewBase = CullViewIndex * RenderFrame->Views.NumDrawsPerView;
        if (bUseMesh)
        {
            struct { uint64 ViewDrawOffsetsAddr; uint32 ArgBase; int32 ShadowDataIndex; int32 ViewIndex; } MP;
            MP.ViewDrawOffsetsAddr = GetViewDrawOffsets().GetAddress();
            MP.ArgBase             = ViewBase + Batch.IndirectDrawOffset;
            MP.ShadowDataIndex  = ShadowDataIndex;
            MP.ViewIndex        = ShadowViewIndex;
            RHI::CmdDrawMeshTasksIndirect(CL, MakeArgs(MP),
                GetMeshDrawArgs().Ptr, (ViewBase + Batch.IndirectDrawOffset) * sizeof(RHI::FDrawMeshTasksIndirectArguments),
                Batch.DrawCount, sizeof(RHI::FDrawMeshTasksIndirectArguments));
        }
        else
        {
            // MeshletVertex EPass=Shadow reads { ShadowDataIndex, ViewIndex }; FirstInstance carries the meshlet base.
            struct { int32 ShadowDataIndex; int32 ViewIndex; } Push;
            Push.ShadowDataIndex = ShadowDataIndex;
            Push.ViewIndex       = ShadowViewIndex;
            RHI::CmdDrawIndirect(CL, MakeArgs(Push),
                GetIndirectArgs().Ptr, (ViewBase + Batch.IndirectDrawOffset) * sizeof(RHI::FDrawIndirectArguments),
                Batch.DrawCount, sizeof(RHI::FDrawIndirectArguments));
        }
    }

    void FForwardRenderScene::PointShadowPass(RHI::FCmdListH CL)
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

        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("ShadowMappingPixel.slang");
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

        // Batch-outer: the batch's pipeline binds once, then every light/face reuses it with just
        // viewport/scissor (cheap dynamic state) changing per tile.
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

                    // ViewIndex indexes ShadowData.ViewProjection[]; here the cube face.
                    DrawShadowBatch(CL, Batch, bUseMesh, ViewBase + (uint32)Face, LightShadow.ShadowDataIndex, Face);
                }
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FForwardRenderScene::SpotShadowPass(RHI::FCmdListH CL)
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

        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("ShadowMappingPixel.slang");
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
                DrawShadowBatch(CL, Batch, bUseMesh, ViewIndex, Shadow.ShadowDataIndex, 0);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    // ViewBase selects which set of cascade views is rastered: the phase-0 views for the first call, the
    // phase-2 views for the disoccluded re-test after the pyramid rebuild. Both load the atlas rather than
    // clearing it (ResetPass owns the clear), so the second call accumulates into the first's depth.
    void FForwardRenderScene::CascadedShowPass(RHI::FCmdListH CL, uint32 CascadeViewBase)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands     = Frame.Geometry.DrawCommands;
        const auto& OpaqueDrawList   = Frame.Geometry.OpaqueDrawList;
        const auto& LightData        = Frame.Lighting.LightData;

        // No work without a shadow-casting sun or caster meshes; terrain-only scenes
        // still read valid (cleared 1.0) shadow data from ResetPass.
        if (!LightData.bHasSun || DrawCommands.empty())
        {
            return;
        }
        if (LightData.Lights[0].ShadowDataIndex == INDEX_NONE)
        {
            return;
        }

        // Each cascade maps to its own cull view; BuildCullViews recorded the base.
        // Bail if the sun got no shadow slot (MaxShadows exceeded).
        if (CascadeViewBase == ~0u)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cascaded Shadow Map Pass", tracy::Color::DeepPink2);


        // ResetPass cleared the whole cascade atlas to 1.0; every cascade loads and
        // rasterizes only its own viewport tile.
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

        // Batch-outer: one depth-only pipeline bind per batch, then all four cascades draw with just
        // the per-cascade tile viewport (GCSMCascadeOrigin/Sizes packing table) changing.
        const int32 SunShadowDataIndex = LightData.Lights[0].ShadowDataIndex;
        for (uint32 OpaqueIdx : OpaqueDrawList)
        {
            const FMeshDrawCommand& Batch = DrawCommands[OpaqueIdx];
            const bool bUseMesh = BindShadowBatchPipeline(CL, Batch, nullptr);

            for (uint32 c = 0; c < (uint32)NumCascades; ++c)
            {
                const int32 TileX = GCSMCascadeOriginX[c];
                const int32 TileY = GCSMCascadeOriginY[c];
                const int32 TileW = GCSMCascadeSizes[c];

                const RHI::FRect TileRect{ TileX, TileX + TileW, TileY, TileY + TileW };
                RHI::CmdSetViewport(CL, TileRect);
                RHI::CmdSetScissor(CL, TileRect);

                // Cascade is depth-only (no pixel shader); ViewIndex indexes the cascade's ViewProjection.
                DrawShadowBatch(CL, Batch, bUseMesh, CascadeViewBase + c, SunShadowDataIndex, (int32)c);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FForwardRenderScene::DecalPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const TVector<FGPUDecal>& Decals = Frame.Primitives.DecalExtracts;

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

        // No decals: still run the clear-only pass so the base pass DBuffer sample is a guaranteed no-op.
        if (Decals.empty())
        {
            RHI::CmdBeginRenderPass(CL, Pass);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Decal Pass", tracy::Color::Orange);

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, DBufferA.GetExtent());

        // Render back faces (robust when the camera is inside the box -- its far faces still fill the
        // screen); no depth test -- the pixel shader reconstructs the surface from depth and rejects
        // out-of-box pixels itself.
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

        // The decal array is read by device address and the scene depth by bindless index,
        // both carried in the pass block (matches FDecalPushConstants in DecalCommon.slang).
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
            const FShaderEntry* VS = Batch.Shaders.VertexShader;
            const FShaderEntry*  PS = Batch.Shaders.PixelShader;
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

    // Mirrors of the shader-side contract in Includes/MaterialTiles.slang. C++ cannot include the slang
    // header, so these are the one duplicated pair; if either moves, both move.
    static constexpr uint32 GMaterialTileSizePx = 16u;   // MATERIAL_TILE_SIZE_PX
    static constexpr uint32 GMaterialMaxSlots   = 64u;   // MATERIAL_MAX_SLOTS

    bool FForwardRenderScene::BuildDeferredMaterialBinning()
    {
        const FFrameData& Frame = *RenderFrame;

        // Cleared up front so a bail-out leaves NumSlots == 0, which is what DeferredMaterialPass reads to
        // decide it has nothing to draw. Never leave last frame's layout standing.
        MaterialBinLayout = FMaterialBinLayout{};

        const FUIntVector2 Extent = GetNamedImage(ENamedImage::HDR).GetExtent();
        if (Extent.x == 0u || Extent.y == 0u)
        {
            return false;
        }

        // Each distinct master DeferredShader gets ONE dense slot (0..N-1); EVERY opaque material instance
        // that shares that shader maps its own GPU MaterialIndex to the same slot. So instances of one master
        // share both the geometry batch AND the shading draw -- O(distinct visible master shaders) draws,
        // not per-instance and not fullscreen.
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
                // The cap bounds the bitmask width, the compacted tile list (TotalTiles entries per slot) and
                // the depth encoding's usable range. Past it a material simply gets no deferred draw:
                // MaterialDepth discards its pixels, so they keep the background instead of shading. Visible
                // and logged, rather than indexing off the end of the bitmask.
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

        MaterialBinLayout.ScreenW       = Extent.x;
        MaterialBinLayout.ScreenH       = Extent.y;
        MaterialBinLayout.NumSlots      = NumSlots;
        MaterialBinLayout.TileCountX    = (Extent.x + GMaterialTileSizePx - 1u) / GMaterialTileSizePx;
        MaterialBinLayout.TotalTiles    = MaterialBinLayout.TileCountX
                                        * ((Extent.y + GMaterialTileSizePx - 1u) / GMaterialTileSizePx);
        MaterialBinLayout.TileWordCount = (NumSlots + 31u) / 32u;

        // Binning scratch: device-address only, per-frame ring. The tile list is strided by TotalTiles per
        // slot, which is the exact worst case (the bitmask holds one bit per (tile, slot), so a slot can be
        // appended at most once per tile) and so needs no prefix sum and no feedback sizing.
        const uint64 TileBitsSize = Math::Max<uint64>(sizeof(uint32),
            (uint64)MaterialBinLayout.TotalTiles * (uint64)MaterialBinLayout.TileWordCount * sizeof(uint32));
        const uint64 TileListSize = Math::Max<uint64>(sizeof(uint32),
            (uint64)MaterialBinLayout.TotalTiles * (uint64)NumSlots * sizeof(uint32));
        const uint64 TileArgsSize = Math::Max<uint64>(sizeof(RHI::FDrawIndirectArguments),
            (uint64)NumSlots * sizeof(RHI::FDrawIndirectArguments));

        ResizeBufferIfNeeded(MaterialBinTileBitsRing[CurrentFrameSlot], TileBitsSize, 1.2f, MaterialBinTileBitsRingLowUsage[CurrentFrameSlot]);
        ResizeBufferIfNeeded(MaterialTileListRing[CurrentFrameSlot],    TileListSize, 1.2f, MaterialTileListRingLowUsage[CurrentFrameSlot]);
        ResizeBufferIfNeeded(MaterialTileArgsRing[CurrentFrameSlot],    TileArgsSize, 1.2f, MaterialTileArgsRingLowUsage[CurrentFrameSlot]);

        return true;
    }

    // Stamps every covered pixel's owning material slot into MaterialDepth as an encoded depth value, then
    // compacts the per-tile slot bitmask it also produces into one packed tile list per slot.
    //
    // Both outputs exist so the material draws that follow can be exact. The depth lets FIXED-FUNCTION
    // hardware reject pixels owned by other materials, before any pixel shader launches and at coarse
    // depth-block granularity; the tile list makes each draw's instance count its own tile count instead of
    // every tile on screen. Between them they replace a per-pixel buffer load plus `discard` in every
    // material shader, and a full-screen instanced draw per material.
    void FForwardRenderScene::MaterialDepthPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        // Zeroed before ANY early return, not just inside BuildDeferredMaterialBinning: DeferredMaterialPass
        // reads NumSlots to decide whether it has draws, so bailing here on a missing shader while leaving the
        // previous frame's layout standing would have it issue draws against a tile grid nothing built.
        MaterialBinLayout = FMaterialBinLayout{};

        if (Frame.Geometry.DrawCommands.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Material Depth + Tile Bin", tracy::Color::Orange3);

        static const FShaderEntry* const FullscreenVS   = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const StampPS        = FShaderLibrary::Get("MaterialDepth.slang");
        static const FShaderEntry* const BuildTileListCS = FShaderLibrary::Get("BuildMaterialTileList.slang");
        if (!FullscreenVS || !StampPS || !BuildTileListCS)
        {
            return;
        }

        if (!BuildDeferredMaterialBinning())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Material Depth");

        const FMaterialBinLayout Layout = MaterialBinLayout;
        const uint32 NumSlots = Layout.NumSlots;

        const FSceneImage& VisRT    = GetNamedImage(ENamedImage::VisBuffer);
        const FSceneImage& MatDepth = GetNamedImage(ENamedImage::MaterialDepth);
        const FUIntVector2 Extent   = FUIntVector2(Layout.ScreenW, Layout.ScreenH);

        const FSceneBuffer TileBits = GetMaterialBinTileBits();
        const FSceneBuffer TileList = GetMaterialTileList();
        const FSceneBuffer TileArgs = GetMaterialTileArgs();

        // MaterialIndex -> dense slot; uploaded to the transient ring and read by device address.
        const RHI::GPUPtr SlotByMaterialAddr =
            RHI::Core::CopyTransientArray(BinnedDeferredSlotByMaterial.data(), BinnedDeferredSlotByMaterial.size());

        // Exactly the region the shaders address: indices are Tile * TileWordCount + word with THIS frame's
        // word count, so anything beyond it in a larger prior allocation is unreachable.
        const uint64 TileBitsSize = (uint64)Layout.TotalTiles * (uint64)Layout.TileWordCount * sizeof(uint32);

        // Seed one indirect draw per slot: 6 vertices (the tile quad), instance count filled by the
        // compaction below, FirstInstance = this slot's base in the tile list. The compaction recomputes that
        // base arithmetically rather than reading it back, so it never loads from the 16 bytes it is
        // atomically incrementing.
        RHI::FDrawIndirectArguments ArgSeed[GMaterialMaxSlots] = {};
        for (uint32 Slot = 0; Slot < NumSlots; ++Slot)
        {
            ArgSeed[Slot].VertexCount   = 6u;
            ArgSeed[Slot].InstanceCount = 0u;
            ArgSeed[Slot].FirstVertex   = 0u;
            ArgSeed[Slot].FirstInstance = Slot * Layout.TotalTiles;
        }

        RHI::CmdMemset(CL, TileBits.Ptr, TileBitsSize, 0u);
        RHI::CmdWriteMemory(CL, TileArgs.GetAddress(), ArgSeed, (uint64)NumSlots * sizeof(RHI::FDrawIndirectArguments));
        Barriers::TransferToAll(CL);

        // Depth-only: no color attachments. The slot goes out through SV_Depth and the tile bits through
        // buffer atomics.
        RHI::FRenderPassDesc StampPass;
        StampPass.DepthAttachment.Texture  = MatDepth.Texture;
        StampPass.DepthAttachment.LoadOp   = RHI::ELoadOp::Clear;
        StampPass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        StampPass.DepthAttachment.Color[0] = 0.0f;   // 0 is what "no material owns this pixel" encodes to
        StampPass.RenderArea               = Extent;

        RHI::CmdBeginRenderPass(CL, StampPass);
        SetViewportScissor(CL, Extent);

        // Test ALWAYS but with the test *enabled*: Vulkan does not write depth when depthTestEnable is false,
        // so EDepthFlags::Read has to be set for the stamp to land even though nothing is being compared.
        RHI::FDepthStencilDesc StampDepth;
        StampDepth.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        StampDepth.DepthTest = RHI::EOp::Always;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(StampDepth));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        struct FMaterialDepthPC
        {
            uint32      VisBufferIndex;
            uint32      TileCountX;
            uint32      TileSizePx;
            uint32      TileWordCount;
            uint32      DrawListCount;
            uint32      SlotByMaterialCount;
            uint32      _Pad0;
            uint32      _Pad1;
            RHI::GPUPtr TileBitsAddr;
            RHI::GPUPtr SlotByMaterialAddr;
        } StampPC = {};
        static_assert(sizeof(FMaterialDepthPC) == 48, "FMaterialDepthPC must match MaterialDepth.slang FMaterialDepthArgs.");
        StampPC.VisBufferIndex      = (uint32)VisRT.GetResourceID();
        StampPC.TileCountX          = Layout.TileCountX;
        StampPC.TileSizePx          = GMaterialTileSizePx;
        StampPC.TileWordCount       = Layout.TileWordCount;
        // Absolute bound on a MeshletDrawList() index: the whole allocation across every view's slice.
        // Not a per-frame meshlet total -- see FCullData::MeshletDrawListCapacity.
        StampPC.DrawListCount       = DrawListCapacity;
        StampPC.SlotByMaterialCount = (uint32)BinnedDeferredSlotByMaterial.size();
        StampPC.TileBitsAddr        = TileBits.GetAddress();
        StampPC.SlotByMaterialAddr  = SlotByMaterialAddr;

        FGraphicsPipelineKey StampKey;
        StampKey.VS          = FullscreenVS;
        StampKey.PS          = StampPS;
        StampKey.DepthFormat = EFormat::D32;
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(StampKey));

        RHI::CmdDraw(CL, MakeArgs(StampPC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);

        // The tile-bit atomics are PIXEL-SHADER writes, not color-attachment writes, so Barriers::RasterToRead
        // would not order them against the compaction below -- its source stage set is RasterColorOut |
        // FragmentTests. Name both real producers: PixelShader for the atomics, FragmentTests for the depth
        // stamp the material draws will test against.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::FragmentTests,
            RHI::EStageFlags::Compute | RHI::EStageFlags::FragmentTests | RHI::EStageFlags::IndirectArguments);

        struct FBuildTileListPC
        {
            RHI::GPUPtr ArgsAddr;
            RHI::GPUPtr TileListAddr;
            RHI::GPUPtr TileBitsAddr;
            uint32      TotalTiles;
            uint32      NumSlots;
            uint32      TileWordCount;
            uint32      _Pad0;
        } BuildPC = {};
        static_assert(sizeof(FBuildTileListPC) == 40, "FBuildTileListPC must match BuildMaterialTileList.slang FBuildTileListArgs.");
        BuildPC.ArgsAddr      = TileArgs.GetAddress();
        BuildPC.TileListAddr  = TileList.GetAddress();
        BuildPC.TileBitsAddr  = TileBits.GetAddress();
        BuildPC.TotalTiles    = Layout.TotalTiles;
        BuildPC.NumSlots      = NumSlots;
        BuildPC.TileWordCount = Layout.TileWordCount;

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BuildTileListCS));
        RHI::CmdDispatch(CL, MakeArgs(BuildPC), RenderUtils::GetGroupCount(Layout.TotalTiles, 64u), 1u, 1u);

        // Compaction feeds both the indirect argument fetch and the tile-quad vertex shader.
        Barriers::ComputeToAll(CL);
    }

    void FForwardRenderScene::DeferredMaterialPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame    = *RenderFrame;
        const auto& DrawCommands   = Frame.Geometry.DrawCommands;

        if (DrawCommands.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Deferred Material Pass", tracy::Color::Red);

        static const FShaderEntry* const TileVS = FShaderLibrary::Get("DeferredMaterialTileVS.slang");
        if (!TileVS)
        {
            return;
        }

        const FSceneImage& VisRT    = GetNamedImage(ENamedImage::VisBuffer);
        const FSceneImage& MatDepth = GetNamedImage(ENamedImage::MaterialDepth);
        const FSceneImage& ColorImg = GetSceneColorRT();
        const FUIntVector2 Extent   = GetNamedImage(ENamedImage::HDR).GetExtent();

        // Set by MaterialDepthPass, which runs immediately before this and zeroes the layout on any bail-out.
        const FMaterialBinLayout Layout = MaterialBinLayout;
        const uint32 NumSlots = Layout.NumSlots;

        // Sky/environment already populated HDR for background pixels; the deferred draws overwrite the
        // covered pixels and leave the rest.
        RHI::FRenderAttachment Colors[2];
        uint32 NumColors = 1;
        Colors[0].Texture        = ColorImg.Texture;
        Colors[0].ResolveTexture = GetSceneColorResolve();
        Colors[0].LoadOp         = RenderSettings.bHasEnvironment ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Colors[0].StoreOp        = RHI::EStoreOp::Store;
        // Editor: Picker is cleared here (it had no VisBuffer-stage write). A packaged game has no picker.
        #if USING(WITH_EDITOR)
        const FSceneImage& PickerImg = GetPickerRT();
        Colors[1].Texture        = PickerImg.Texture;
        Colors[1].ResolveTexture = GetPickerResolve();
        Colors[1].LoadOp         = RHI::ELoadOp::Clear;
        Colors[1].StoreOp        = RHI::EStoreOp::Store;
        NumColors = 2;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.RenderArea       = Extent;

        // No deferred-drawing materials (e.g. a terrain-only scene), or MaterialDepthPass bailed: still run
        // the clear-only pass so Picker is cleared and HDR keeps its background/sky.
        if (NumSlots == 0u)
        {
            RHI::CmdBeginRenderPass(CL, Pass);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);
            return;
        }

        SCENE_GPU_SCOPE(CL, "Deferred Material");

        // MaterialDepth carries each covered pixel's owning slot as an encoded depth value. Bound read-only
        // and tested EQUAL against the depth each tile quad is emitted at, so hardware rejects every pixel
        // this draw does not own.
        Pass.DepthAttachment.Texture = MatDepth.Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        RHI::FDepthStencilDesc MatchDepth;
        MatchDepth.DepthMode = RHI::EDepthFlags::Read;   // test only: the stamp is the single writer
        MatchDepth.DepthTest = RHI::EOp::Equal;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(MatchDepth));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        const FSceneBuffer TileList = GetMaterialTileList();
        const FSceneBuffer TileArgs = GetMaterialTileArgs();

        // Layout overlays FDBufferPushConstants on offsets 0-12 so ShadeSurface's ApplyDBuffer reads the
        // decal indices from this same push constant. Shared with the tile-quad vertex shader (same
        // pipeline, same push block): the fields from SlotIndex on are the VS's.
        struct FDeferredPushConstants
        {
            uint32      VisBufferIndex;
            uint32      DBufferAIndex;
            uint32      DBufferBIndex;
            uint32      DBufferCIndex;
            uint32      DrawListCount;
            uint32      SlotIndex;
            uint32      TileCountX;
            uint32      TileSizePx;
            uint32      ScreenW;
            uint32      ScreenH;
            RHI::GPUPtr TileListAddr;
        } PC = {};
        static_assert(sizeof(FDeferredPushConstants) == 48, "FDeferredPushConstants must match DeferredMaterial.slang FDeferredPassArgs.");
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
        PC.TileCountX    = Layout.TileCountX;
        PC.TileSizePx    = GMaterialTileSizePx;
        PC.ScreenW       = Layout.ScreenW;
        PC.ScreenH       = Layout.ScreenH;
        PC.TileListAddr  = TileList.GetAddress();

        // One indirect tile-quad draw per master DeferredShader, over exactly the tiles that shader covers.
        // The pixel shader reads each pixel's own MaterialIndex for its instance uniforms, so every instance
        // of the master shades in this one draw.
        // ShadowMaskPass resolved the sun for this view, so specialize the sun's cascade PCSS out of every
        // deferred material shader and read its mask instead. This is the pass the gate exists for: these
        // shaders are the register-pressure hot spot (PS warp launch stalled on register allocation), and
        // the PCSS live range was setting their floor even for pixels the sun never reaches.
        const uint32 ShadingFeatures = SF_All | (RenderSettings.bShadowMaskValid ? (uint32)SF_ShadowMask : 0u);

        for (uint32 Slot = 0; Slot < NumSlots; ++Slot)
        {
            FGraphicsPipelineKey Key;
            Key.VS          = TileVS;
            Key.PS          = BinnedDeferredSlotShaders[Slot];
            Key.ShadingFeatures = ShadingFeatures;
            Key.DepthFormat = EFormat::D32;
            Key.ColorTargets.push_back({ ColorImg.Desc.Format, {} });
            #if USING(WITH_EDITOR)
            Key.ColorTargets.push_back({ PickerImg.Desc.Format, {} });
            #endif
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            PC.SlotIndex = Slot;
            RHI::CmdDrawIndirect(CL, MakeArgs(PC),
                TileArgs.Ptr, (uint64)Slot * sizeof(RHI::FDrawIndirectArguments),
                1, sizeof(RHI::FDrawIndirectArguments));
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }


    void FForwardRenderScene::ParticleSimulatePass(RHI::FCmdListH CL)
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
            // Sized to the system's emitter count, both directions. Growing covers a newly added emitter;
            // shrinking frees the buffers of one that was deleted, which would otherwise sit allocated
            // until the entity itself died.
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

            // Attribute stride is part of the allocation identity: a recompile that declares a new
            // attribute changes it, and the old buffer would then be indexed with the wrong stride.
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

                State.AttributeBufferSize = (uint64)MaxParticles * (uint64)Item.AttributeFloatCount * sizeof(float);
                State.AttributeBuffer     = RHI::Malloc(State.AttributeBufferSize, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);

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

            // Upper bound on remaining alive-particle lifetime; a spawn resets it to max
            // lifetime, else it counts down. At zero with no spawn, skip the dispatch.
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

            // Zero the spawn counter only when we are actually going to
            // dispatch. Idle systems no longer pay for it.
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
            // Pack EmitterVelocity.xyz into the w components of the basis vectors to
            // avoid growing the CBV layout. The shader reconstructs it from these slots.
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

            static const FShaderEntry* const DefaultSimShader = FShaderLibrary::Get("ParticleSimulate.slang");
            const FShaderEntry* ComputeShader = Item.bUsesCustomShader ? Item.CustomComputeShader : DefaultSimShader;

            if (ComputeShader == nullptr || !ComputeShader->IsValid())
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
            // Module-stack slots ride the same per-frame transient ring as SimParams: they are small,
            // rewritten whenever the user edits an input, and need no lifetime tracking. 0 for the legacy
            // data-driven sim, whose shader never calls MP().
            SimArgs.ModuleParamsAddr = Item.ModuleParamValues.empty()
                ? 0ull
                : RHI::Core::CopyTransientArray(Item.ModuleParamValues.data(), Item.ModuleParamValues.size());
            SimArgs.AttributesAddr   = State.AttributeBuffer;

            RHI::CmdDispatch(CL, MakeArgs(SimArgs), (MaxParticles + 63u) / 64u, 1, 1);
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

    void FForwardRenderScene::ParticleRenderPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Particle Render", tracy::Color::OrangeRed);

        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (Frame.Extracts.ParticleExtracts.empty())
        {
            return;
        }

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("ParticleVertex.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("ParticlePixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR   = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Depth = GetNamedImage(ENamedImage::DepthAttachment);

        // Only Load when an earlier pass wrote these targets; in the particle preview world
        // BasePass/DepthPrePass early-return, so the first pass must clear them itself.
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

        // Particle array read by device address; per-emitter render params inlined into the pass
        // block (matches FParticlePushConstants in ParticleVertex.slang).
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

        // Resize CPU backing stores to declared dimensions (lazy, so designers tweak Resolution/LayerCount
        // without rebooting). A resolution change resamples existing height + weights rather than wiping them.
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

        // Terrain prep (during Extract, exclusive ECS access): rebuild dirty metadata and copy
        // the bytes the render phase needs into the extract, so a concurrent sculpt can't realloc under it.
        static void PrepareTerrainExtract(STerrainComponent& Terrain, const FMatrix4& WorldMatrix,
                                          FForwardRenderScene::FFrameData::FTerrainExtract& Out)
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

            // Resolve shaders here (extract phase, material alive) and ref-hold them; the render
            // thread never touches the CMaterial. A wrong-domain material leaves the shaders null
            // (terrain skipped); null/not-ready falls back to the default terrain material.
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

            // A dimension change vs what we last prepared forces a full re-seed and a
            // GPU texture realloc on the render side.
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

        static FSceneImage CreateTerrainImage(uint32 Size, uint16 ArraySize, EFormat Format, bool bUav, bool bArrayView = false)
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
            return CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ bUav);
        }
    }

    void FForwardRenderScene::TerrainUpdatePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Terrain Update", tracy::Color::SeaGreen);

        static const FShaderEntry* const NormalShader = FShaderLibrary::Get("TerrainNormalCompute.slang");

        const FFrameData& Frame = *RenderFrame;

        // Reclaim GPU state for destroyed terrains (disabled ones stay in LiveTerrainEntities).
        // Releases are slot-deferred so in-flight frames reading the resources finish first.
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

            // GPUState is render-phase-owned (this map); the CPU payload comes entirely
            // from the extract-phase snapshot in TerrainItem, never from the live component.
            FTerrainGPUState& State = TerrainGPUStates[TerrainItem.Entity];
            const uint32 Res        = (uint32)TerrainItem.Resolution;
            const uint32 LayerCount = (uint32)std::max(TerrainItem.LayerCount, 1);
            const size_t SlicePixels = size_t(Res) * size_t(Res);

            // (Re)allocate GPU textures when the GPU dimensions no longer match what the
            // Extract prepared. A structural change always ships Full payloads below.
            const bool bRealloc = State.AllocatedResolution != Res || State.AllocatedLayerCount != LayerCount;
            if (bRealloc)
            {
                DeferRelease(State.HeightmapTexture);
                DeferRelease(State.NormalTexture);
                DeferRelease(State.LayerWeightTexture);

                State.HeightmapTexture   = CreateTerrainImage(Res, 1u,          EFormat::R32_FLOAT, false);
                State.NormalTexture      = CreateTerrainImage(Res, 1u,          EFormat::RGBA8_UNORM, true);
                State.LayerWeightTexture = CreateTerrainImage(Res, (uint16)std::max(LayerCount, 1u), EFormat::R8_UNORM, false, true);
                State.AllocatedResolution = Res;
                State.AllocatedLayerCount = LayerCount;

                // Vulkan doesn't zero new image memory; with no weight payload this frame, clear every
                // slice so a sampler can't read garbage (otherwise the weight upload below seeds them).
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

                // Central-difference normals read one neighbor each side, so dilate the
                // recompute region by a texel and clamp to the map.
                const int32 NMinX = std::max(RectMin.x - 1, 0);
                const int32 NMinY = std::max(RectMin.y - 1, 0);
                const int32 NMaxX = std::min(RectMax.x + 1, ResI - 1);
                const int32 NMaxY = std::min(RectMax.y + 1, ResI - 1);
                const int32 NW    = NMaxX - NMinX + 1;
                const int32 NH    = NMaxY - NMinY + 1;

                // Mirrors FTerrainNormalArgs in TerrainNormalCompute.slang: scalar params plus
                // the heightmap SRV / normal UAV heap indices.
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
                RHI::CmdDispatch(CL, MakeArgs(NormalArgs), (uint32(NW) + 7u) / 8u, (uint32(NH) + 7u) / 8u, 1u);

                // Normals are sampled by the terrain VS/PS.
                Barriers::ComputeToAll(CL);
            }

            // Upload chunk + meshlet metadata Extract rebuilt this frame; the
            // cull pass tests these AABBs, so it must land before the next cull.
            if (TerrainItem.bGeometryRebuilt)
            {
                const uint32 ChunkCount   = (uint32)TerrainItem.Chunks.size();
                const uint32 MeshletCount = (uint32)TerrainItem.Meshlets.size();

                if (ChunkCount > 0 && MeshletCount > 0)
                {
                    auto AllocSSBO = [&](FSceneBuffer& Buffer, uint64 SizeBytes)
                    {
                        if (!Buffer || Buffer.Size < SizeBytes)
                        {
                            if (Buffer)
                            {
                                DeferFree(Buffer.Ptr);
                            }
                            Buffer = CreateSceneBuffer(std::max<uint64>(SizeBytes, 16ull));
                        }
                    };

                    AllocSSBO(State.ChunkInfoBuffer,      uint64(ChunkCount)   * sizeof(FTerrainChunkInfo));
                    AllocSSBO(State.MeshletInfoBuffer,    uint64(MeshletCount) * sizeof(FTerrainMeshletInfo));
                    AllocSSBO(State.VisibleMeshletBuffer, uint64(MeshletCount) * sizeof(FTerrainVisibleMeshlet));
                    AllocSSBO(State.IndirectDrawBuffer,   sizeof(RHI::FDrawIndirectArguments));

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

    void FForwardRenderScene::TerrainCullPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.Extracts.TerrainExtracts.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Terrain Cull", tracy::Color::SeaGreen);

        static const FShaderEntry* const CullShader = FShaderLibrary::Get("TerrainCull.slang");
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

            // Seed the indirect args slot. Six verts per quad * meshletQuads^2
            // is the per-meshlet vertex count the terrain VS expects.
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

            // One workgroup per chunk, one thread per meshlet; the chunk-level test on
            // thread 0 gates the rest via groupshared.
            RHI::CmdDispatch(CL, MakeArgs(Push), State.AllocatedChunkCount, 1u, 1u);
            bAnyDispatched = true;
        }

        if (bAnyDispatched)
        {
            Barriers::ComputeToAll(CL);
        }
    }

    // Depth pre-pass over culled terrain meshlets (reverse-Z, DepthWrite on) so the heavy shaded pass
    // early-Z rejects overdraw and runs its ~80-tap PBR once per pixel. Runs BEFORE the mid pyramid /
    // SSAO / deferred pass: alongside depth it stamps 'empty' (0) into the VisBuffer wherever terrain is
    // the closest surface, so classify/deferred never shade mesh pixels the terrain covers.
    void FForwardRenderScene::TerrainDepthPrePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (Frame.Extracts.TerrainExtracts.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Terrain Depth", tracy::Color::SeaGreen);

        static const FShaderEntry* const StampPS = FShaderLibrary::Get("TerrainDepthPixel.slang");
        const FSceneImage& VisRT = GetNamedImage(ENamedImage::VisBuffer);

        // With no meshes, VisBufferPass didn't run, so the first terrain pass owns the VisBuffer clear
        // (0 = empty). Depth was already transfer-cleared by ResetPass in that case, so it always loads.
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
            const FShaderEntry* VertexShader = TerrainItem.Shaders.VertexShader;
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

            // VisBuffer rides along so terrain-covered pixels read as empty in classify/deferred. The
            // stamp only lands where the depth test passes (terrain closest).
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

    void FForwardRenderScene::TerrainRenderPass(RHI::FCmdListH CL)
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
            // Cull output makes the indirect draw legal; bail when it isn't built
            // yet (first frame, or heightmap dirty before TerrainUpdatePass ran).
            if (!State.ChunkInfoBuffer || !State.MeshletInfoBuffer || !State.VisibleMeshletBuffer || !State.IndirectDrawBuffer)
            {
                continue;
            }
            if (State.AllocatedMeshletCount == 0u)
            {
                continue;
            }


            // The extract enforced the terrain domain + default fallback and ref-held the shaders on the
            // extract phase, so the render phase never touches the CMaterial. Null VS/PS => skip.
            const FShaderEntry* VertexShader = TerrainItem.Shaders.VertexShader;
            const FShaderEntry*  PixelShader  = TerrainItem.Shaders.PixelShader;
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

            // TerrainDepthPrePass laid down terrain depth (and cleared when there
            // were no meshes), so always load and let early-Z drop the overdraw.
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
            // Terrain binds FTerrainPushConstants (not the DBuffer overlay), so decals stay specialized
            // off. SSAO is on: terrain depth now precedes the SSAO pass, and the fetch reads the scene
            // globals (no push input needed); views without SSAO (captures) fall back via AOTextureIndex.
            Key.ShadingFeatures = SF_DebugViews | SF_SSAO |
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

            // Cull populated the single indirect args slot (VertexCount = MeshletQuads^2*6,
            // InstanceCount = surviving meshlets). One GPU-driven draw.
            RHI::CmdDrawIndirect(CL, MakeArgs(Push), State.IndirectDrawBuffer.Ptr, 0u, 1u, sizeof(RHI::FDrawIndirectArguments));

            RHI::CmdEndRenderPass(CL);
        }

        Barriers::RasterToRead(CL);
    }
    
    void FForwardRenderScene::SSAOPass(RHI::FCmdListH CL)
    {
        FFrameData& Frame = *RenderFrame;
        if (Frame.Geometry.DrawCommands.empty() || !Frame.CachedWorldSettings.bEnableSSAO)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("SSAO Pass", tracy::Color::Red);

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("SSAOPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = GetNamedImage(ENamedImage::SSAO);
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

    void FForwardRenderScene::SSAOBlurPass(RHI::FCmdListH CL)
    {
        FFrameData& Frame = *RenderFrame;
        if (Frame.Geometry.DrawCommands.empty() || !Frame.CachedWorldSettings.bEnableSSAO)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("SSAO Blur", tracy::Color::Red);

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const DenoisePS    = FShaderLibrary::Get("SSAOBlurPixel.slang");
        static const FShaderEntry* const UpsamplePS   = FShaderLibrary::Get("SSAOUpsamplePixel.slang");
        if (!VertexShader || !DenoisePS || !UpsamplePS)
        {
            return;
        }

        const FSceneImage& Raw      = GetNamedImage(ENamedImage::SSAO);
        const FSceneImage& Denoised = GetNamedImage(ENamedImage::SSAODenoise);
        const FSceneImage& Output   = GetNamedImage(ENamedImage::SSAOBlur);
        const FSceneImage& Depth    = GetNamedImage(ENamedImage::DepthAttachment);

        struct FData
        {
            uint32 AOIndex;
            uint32 DepthIndex;
        };

        // Stage 1: plane-aware 5x5 denoise, half res (SSAO -> SSAODenoise).
        // Stage 2: joint bilateral upsample, full res (SSAODenoise -> SSAOBlur).
        const struct
        {
            const FShaderEntry* PS;
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

    void FForwardRenderScene::BillboardPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& BillboardInstances = Frame.Primitives.BillboardInstances;
        const auto& DrawCommands       = Frame.Geometry.DrawCommands;

        if (BillboardInstances.empty() || !RenderSettings.bDrawBillboards)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Billboard Pass", tracy::Color::Red);

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("BillboardVert.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("BillboardPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR    = GetNamedImage(ENamedImage::HDR);

        // Only Load when an earlier pass wrote HDR; debug tris/lines and particles render before this
        // pass, so clearing here would erase them (editor grid + a light billboard hit this).
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

    void FForwardRenderScene::WidgetPickerPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& WidgetInstances = Frame.Primitives.WidgetInstances;

        if (WidgetInstances.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Widget Picker Pass", tracy::Color::Magenta);

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("WidgetVert.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("WidgetPickerPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        // Before the picker readback: stamp the widget's entity id into the Picker buffer where it's opaque.
        // Depth-tested (no write) to match WidgetPass, so a widget picks only where it's visible.
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

    void FForwardRenderScene::WidgetPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& WidgetInstances = Frame.Primitives.WidgetInstances;

        if (WidgetInstances.empty() || !CurrentView->Output.IsValid())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Widget Pass", tracy::Color::Magenta);

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("WidgetVert.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("WidgetPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = CurrentView->Output;

        // Drawn AFTER tone mapping onto the display-referred target so widget colors match the screen UI.
        // RT/HDR/depth share one extent, so we still depth-test against scene depth (occluded). No depth write.
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

    void FForwardRenderScene::TextPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame   = *RenderFrame;
        const auto&       Glyphs  = Frame.Primitives.GlyphInstances;
        const auto&       Batches = Frame.Primitives.TextBatches;

        if (Glyphs.empty() || Batches.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Text Pass", tracy::Color::Yellow);

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("TextVert.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("TextPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR    = GetNamedImage(ENamedImage::HDR);

        // Drawn pre-tone-map into HDR like billboards: one MRT pass writes color (SV_Target0) and, in the
        // editor, stamps the glyph's entity id into the Picker buffer (SV_Target1) so text stays
        // click-selectable without a second pass. Per-component bDepthTest selects depth-tested+written vs
        // always-on-top; depth state is dynamic so both share one render pass and one pipeline.
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

        // Reversed-Z: GreaterOrEqual keeps fragments at/in front of scene depth, and writes depth so
        // the text occludes things behind it.
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

    void FForwardRenderScene::DebugTextPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame  = *RenderFrame;
        const auto&       Glyphs = Frame.Primitives.DebugTextGlyphs;
        const auto&       Batch  = Frame.Primitives.DebugTextBatch;

        if (Glyphs.empty() || Batch.Count == 0 || !CurrentView->Output.IsValid())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Debug Text Pass", tracy::Color::Yellow);

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("DebugTextVert.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("DebugTextPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& Output = CurrentView->Output;

        // Screen-space overlay onto the final display-referred target (post-tone-map, like the screen UI),
        // top-left stack. No depth, alpha blend, colors written as authored.
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

        // Use the DISPLAY/panel size (not the RT extent) for px->NDC: the world RT is a fixed size stretched
        // to the editor panel, so converting against the RT would let that stretch distort the text. The
        // panel size cancels the RT->panel stretch, keeping glyphs square and a true pixel size on screen.
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

    // Deferred sun-shadow projection: resolve the directional cascade PCSS once per screen pixel into a
    // full-res R8 visibility mask, which the opaque shading passes then read with a single texel fetch.
    //
    // The win is REGISTER PRESSURE, not arithmetic -- it is the same PCSS over roughly the same pixels.
    // Whole-shader VGPR allocation reserves the peak live range across every path, so the cascade PCSS
    // (two inlined SampleDirectionalCascade bodies for the cross-fade, each holding a 4x4 cascade VP, the
    // receiver-plane Jacobian, an atlas tile and a rolling tap loop) set the register floor for the whole
    // deferred material shader -- graph, clustered light loop, IBL -- including for pixels that never
    // touch the sun. Here the same code IS the whole shader, so it allocates few registers, reaches high
    // occupancy, and the extra resident warps hide its own texture latency.
    //
    // Ordering: needs the full opaque depth (meshes from both VisBuffer phases + terrain) AND both
    // cascade raster phases, so it sits after "Cascade Pyramid" alongside the other screen-space passes
    // that consume opaque depth.
    void FForwardRenderScene::ShadowMaskPass(RHI::FCmdListH CL)
    {
        // Set by CompileDrawCommands_Render, which also decides ShadowMaskIndex and SF_ShadowMask from
        // it. Not gated on DrawCommands.empty() the way SSAO is: a terrain-only scene has no mesh draws
        // and its terrain still receives sun shadows.
        if (!RenderSettings.bShadowMaskValid)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Shadow Mask", tracy::Color::Red);

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const PixelShader  = FShaderLibrary::Get("ShadowMaskPixel.slang");
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

    void FForwardRenderScene::TransparentPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands        = Frame.Geometry.DrawCommands;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;
        const uint32 NumDrawsPerView    = Frame.Views.NumDrawsPerView;
        const uint32 ViewBase           = CurrentCameraEarlyView * NumDrawsPerView;
        // Camera-LATE view slice: meshlets the stale phase-0 HZB wrongly occluded, re-emitted by the
        // phase-1 cull (a meshlet is in early XOR late, never both). The VisBuffer rasters both views;
        // transparency must too, or deferred glass meshlets vanish for a frame and pop back -- with a
        // moving camera the defer set changes every frame, which reads as heavy flicker. ~0u = no late
        // view this frame (scene captures).
        const uint32 LateViewBase       = (CurrentCameraLateView != ~0u) ? CurrentCameraLateView * NumDrawsPerView : ~0u;

        if (TranslucentDrawList.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Transparent Pass", tracy::Color::CadetBlue);

        const FSceneImage& Accum     = GetNamedImage(ENamedImage::Accum);
        const FSceneImage& Revealage = GetNamedImage(ENamedImage::Revealage);
        const FUIntVector2 Extent    = GetNamedImage(ENamedImage::HDR).GetExtent();

        RHI::FRenderAttachment Colors[3];
        uint32 NumColors = 2;
        Colors[0].Texture  = Accum.Texture;
        Colors[0].LoadOp   = RHI::ELoadOp::Clear;
        Colors[0].StoreOp  = RHI::EStoreOp::Store;
        Colors[0].Color[0] = Colors[0].Color[1] = Colors[0].Color[2] = Colors[0].Color[3] = 0.0f;
        Colors[1].Texture  = Revealage.Texture;
        Colors[1].LoadOp   = RHI::ELoadOp::Clear;
        Colors[1].StoreOp  = RHI::EStoreOp::Store;
        Colors[1].Color[0] = Colors[1].Color[1] = Colors[1].Color[2] = Colors[1].Color[3] = 1.0f;
        #if USING(WITH_EDITOR)
        const FSceneImage& Picker = GetNamedImage(ENamedImage::Picker);
        Colors[2].Texture  = Picker.Texture;
        Colors[2].LoadOp   = RHI::ELoadOp::Load;
        Colors[2].StoreOp  = RHI::EStoreOp::Store;
        NumColors = 3;
        #endif

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(Colors, NumColors);
        Pass.DepthAttachment.Texture = GetNamedImage(ENamedImage::DepthAttachment).Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencilState(CL, GetOrCreateDepthState(DepthDesc));

        // WBOIT: accum adds, revealage multiplies in (1 - coverage). Both are commutative, so the
        // atomic (frame-varying) instance order from the cull cannot change the result. Additive
        // batches are EXCLUDED: their materials compile the opaque-variant shader (Color+Picker),
        // whose outputs misalign against these three attachments (the Picker uint landed in the R16F
        // revealage with blending disabled, stomping the coverage product at an order-dependent point
        // in the per-pixel blend sequence -- overlap pixels strobed frame to frame). They composite
        // additively onto HDR after the resolve instead (AdditiveTranslucentPass).
        RHI::FBlendDesc AccumBlend;
        AccumBlend.bBlendEnable   = true;
        AccumBlend.SrcColorFactor = RHI::EFactor::One;
        AccumBlend.DstColorFactor = RHI::EFactor::One;

        RHI::FBlendDesc RevealBlend;
        RevealBlend.bBlendEnable   = true;
        RevealBlend.SrcColorFactor = RHI::EFactor::Zero;
        RevealBlend.DstColorFactor = RHI::EFactor::OneMinusSrcColor;

        for (uint32 Idx : TranslucentDrawList)
        {
            const FMeshDrawCommand& Batch = DrawCommands[Idx];
            if (Batch.bAdditive)
            {
                continue;
            }

            FGraphicsPipelineKey Key;
            Key.VS          = Batch.VertexShader;
            Key.PS          = Batch.PixelShader;
            Key.DepthFormat = EFormat::D32;
            Key.ColorTargets.push_back({ Accum.Desc.Format, AccumBlend });
            Key.ColorTargets.push_back({ Revealage.Desc.Format, RevealBlend });
            #if USING(WITH_EDITOR)
            Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
            #endif
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            RHI::CmdDrawIndirect(CL, MakeArgs(),
                GetIndirectArgs().Ptr, (ViewBase + Batch.IndirectDrawOffset) * sizeof(RHI::FDrawIndirectArguments),
                Batch.DrawCount, sizeof(RHI::FDrawIndirectArguments));

            if (LateViewBase != ~0u)
            {
                RHI::CmdDrawIndirect(CL, MakeArgs(),
                    GetIndirectArgs().Ptr, (LateViewBase + Batch.IndirectDrawOffset) * sizeof(RHI::FDrawIndirectArguments),
                    Batch.DrawCount, sizeof(RHI::FDrawIndirectArguments));
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FForwardRenderScene::OITResolvePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        if (TranslucentDrawList.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("OIT Resolve Pass", tracy::Color::GreenYellow);

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("OITResolve.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR       = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Accum     = GetNamedImage(ENamedImage::Accum);
        const FSceneImage& Revealage = GetNamedImage(ENamedImage::Revealage);

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

        RHI::FBlendDesc AlphaBlend;
        AlphaBlend.bBlendEnable   = true;
        AlphaBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AlphaBlend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        AlphaBlend.SrcAlphaFactor = RHI::EFactor::One;
        AlphaBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ HDR.Desc.Format, AlphaBlend });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FOITResolvePushConstants
        {
            uint32 AccumIndex;
            uint32 RevealageIndex;
            uint32 _Pad0;
            uint32 _Pad1;
        };
        static_assert(sizeof(FOITResolvePushConstants) == 16, "FOITResolvePushConstants must match the slang pass block.");

        FOITResolvePushConstants PC = {};
        PC.AccumIndex     = (uint32)Accum.GetResourceID();
        PC.RevealageIndex = (uint32)Revealage.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FForwardRenderScene::AdditiveTranslucentPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands        = Frame.Geometry.DrawCommands;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;
        const uint32 NumDrawsPerView    = Frame.Views.NumDrawsPerView;
        const uint32 ViewBase           = CurrentCameraEarlyView * NumDrawsPerView;
        const uint32 LateViewBase       = (CurrentCameraLateView != ~0u) ? CurrentCameraLateView * NumDrawsPerView : ~0u;

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

        // Additive surfaces add light without occluding, so they don't belong in the WBOIT average at
        // all: pure addition is order-independent by itself, and their materials compile the
        // opaque-variant shader (Color + Picker), which matches these attachments exactly. Runs after
        // the OIT resolve so they layer on top of the composited translucency, like the particle pass.
        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);

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

        for (uint32 Idx : TranslucentDrawList)
        {
            const FMeshDrawCommand& Batch = DrawCommands[Idx];
            if (!Batch.bAdditive)
            {
                continue;
            }

            FGraphicsPipelineKey Key;
            Key.VS          = Batch.VertexShader;
            Key.PS          = Batch.PixelShader;
            Key.DepthFormat = EFormat::D32;
            Key.ColorTargets.push_back({ HDR.Desc.Format, AdditiveBlend });
            #if USING(WITH_EDITOR)
            Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
            #endif
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            RHI::CmdDrawIndirect(CL, MakeArgs(),
                GetIndirectArgs().Ptr, (ViewBase + Batch.IndirectDrawOffset) * sizeof(RHI::FDrawIndirectArguments),
                Batch.DrawCount, sizeof(RHI::FDrawIndirectArguments));

            if (LateViewBase != ~0u)
            {
                RHI::CmdDrawIndirect(CL, MakeArgs(),
                    GetIndirectArgs().Ptr, (LateViewBase + Batch.IndirectDrawOffset) * sizeof(RHI::FDrawIndirectArguments),
                    Batch.DrawCount, sizeof(RHI::FDrawIndirectArguments));
            }
        }

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

    void FForwardRenderScene::FroxelInjectPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog || !CVarVolFogEnabled.GetValue())
        {
            return;
        }

        const auto& LightData       = Frame.Lighting.LightData;
        const auto& SceneGlobalData = Frame.SceneGlobalData;

        // Same volumetric-light policy as the lit passes: sun (light 0) is special-cased;
        // local point/spot lights opt in via ELightFlags::Volumetric.
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

        static const FShaderEntry* const CS = FShaderLibrary::Get("VolumetricFogInject.slang");
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

    void FForwardRenderScene::FroxelIntegratePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog || !CVarVolFogEnabled.GetValue())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Froxel Integrate Pass", tracy::Color::MediumPurple);

        static const FShaderEntry* const CS = FShaderLibrary::Get("VolumetricFogIntegrate.slang");
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

    void FForwardRenderScene::FroxelApplyPass(RHI::FCmdListH CL)
    {
        // Runs whenever fog is enabled: composites the froxel volume when volumetrics are on,
        // and always continues the medium analytically past the froxel range / over the sky.
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Froxel Apply Pass", tracy::Color::Orange3);

        static const FShaderEntry* const VS = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const PS = FShaderLibrary::Get("VolumetricFogApply.slang");
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

        // result = inScatter + HDR * transmittance, via src=(One), dst=(InvSrcAlpha)
        // with the shader writing alpha = 1 - transmittance.
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
        PC.bVolumetric     = (Frame.Volumetrics.bVolumetricFog && CVarVolFogEnabled.GetValue()) ? 1u : 0u;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FForwardRenderScene::WaterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const TVector<FGPUWater>& Waters = Frame.Water.Surfaces;
        if (Waters.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Water Pass", tracy::Color::CadetBlue);

        static const FShaderEntry* const VS = FShaderLibrary::Get("WaterVert.slang");
        static const FShaderEntry* const PS = FShaderLibrary::Get("WaterPixel.slang");
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

        // One draw per water body; the body index arrives via SV_VulkanInstanceID (FirstInstance). Grid is
        // (Res-1)^2 cells, 6 verts each, generated procedurally from SV_VertexID in the VS.
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

    void FForwardRenderScene::UnderwaterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Water.bUnderwaterActive)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Underwater Pass", tracy::Color::SteelBlue);

        static const FShaderEntry* const VS = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const PS = FShaderLibrary::Get("WaterUnderwater.slang");
        if (!VS || !PS)
        {
            return;
        }

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneColor = GetNamedImage(ENamedImage::WaterRefraction);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);

        // Sample the fully-composited scene from a copy; the PS recomputes every pixel (above-water pixels
        // pass through unchanged), so the half-submerged screen split falls out of the per-ray path length.
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
    
    void FForwardRenderScene::SkyCubeCapturePass(RHI::FCmdListH CL)
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
            static const FShaderEntry* const ComputeShader = FShaderLibrary::Get("EquirectToCubemap.slang");
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

            // Intensity/yaw come from the same HDRIParams the sky pass reads, so the baked cube (and
            // therefore irradiance + prefilter) matches the visible background exactly.
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

        static const FShaderEntry* const ComputeShader = FShaderLibrary::Get("SkyCubeCapture.slang");
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

        // Same source EnvironmentPass uses (SunDirection points FROM surface TO sun);
        // falls back to a daytime direction if no sun so the cube still has IBL structure.
        if (LightData.bHasSun)
        {
            PC.SunDirection = Math::Normalize(LightData.SunDirection);
        }
        else
        {
            PC.SunDirection = Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));
        }
        // Star twinkle samples Time, but the cube only re-bakes on env/sun change so
        // it effectively freezes (stars blur to nothing in convolution anyway).
        PC.Time = SceneGlobalData.Time;

        constexpr uint32 SkyCaptureTile = 8u;
        const uint32 FaceSize = SkyCube.GetSizeX();
        const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, SkyCaptureTile);
        // Z = 6 layers, one per cube face -- each thread owns one (face, x, y).
        RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);

        // Convolution + environment pass read the cube next.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader);
    }

    void FForwardRenderScene::IrradianceConvolutionPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Sky Irradiance Convolution", tracy::Color::SkyBlue1);

        const FFrameData& Frame = *RenderFrame;
        const bool bIBLConvolutionDirty = Frame.Volumetrics.bIBLConvolutionDirty;

        if (!RenderSettings.bHasEnvironment)
        {
            return;
        }

        // Skip the per-texel convolution when the source cube hasn't moved; the wide
        // diffuse lobe makes sub-degree sun deltas invisible in the persistent cube.
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

        static const FShaderEntry* const ComputeShader = FShaderLibrary::Get("IrradianceConvolution.slang");
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

    void FForwardRenderScene::PrefilterEnvMapPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const bool bIBLConvolutionDirty = Frame.Volumetrics.bIBLConvolutionDirty;

        if (!RenderSettings.bHasEnvironment)
        {
            return;
        }

        // Same dirty gate as irradiance: re-running 256 GGX samples per texel per mip
        // is wasted when the persistent prefilter cube's source hasn't moved enough.
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

        static const FShaderEntry* const ComputeShader = FShaderLibrary::Get("PrefilterEnvMap.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        const uint32 NumMips      = PrefilterCube.GetNumMips();
        const uint32 BaseFaceSize = PrefilterCube.GetSizeX();

        constexpr uint32 PrefilterTile = 8u;

        // One dispatch per mip, each writing a bindless 2D-array UAV view of just that mip; roughness is
        // uniform across the dispatch, threaded in via the pass block. SkyCube read as a bindless cube SRV.
        for (uint32 Mip = 0; Mip < NumMips; ++Mip)
        {
            FPrefilterPC PC = {};
            PC.SrcCubeSRV = (uint32)SkyCube.GetResourceID();
            PC.OutMipUAV  = (uint32)PrefilterCube.GetMipUAVIndex(Mip);
            // Roughness even across mips (mip 0 mirror, last fully rough); matches
            // SamplePrefilter()'s runtime mip select.
            PC.Roughness  = (NumMips <= 1u) ? 0.0f
                                            : (float)Mip / (float)(NumMips - 1u);
            PC.NumSamples = GPrefilterSampleCount;

            const uint32 MipFaceSize = eastl::max<uint32>(BaseFaceSize >> Mip, 1u);
            const uint32 GroupsXY    = RenderUtils::GetGroupCount(MipFaceSize, PrefilterTile);
            RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
        }

        Barriers::ComputeToAll(CL);
    }

    void FForwardRenderScene::EnvironmentPass(RHI::FCmdListH CL)
    {
        if (!RenderSettings.bHasEnvironment)
        {
            // This pass owns the HDR clear (LoadOp::Clear below). With no environment AND no
            // geometry nothing else writes scene color, and the viewport shows uninitialized
            // memory -- so run a clear-only pass instead of skipping outright.
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

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("Environment.slang");
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
        // Mirrors FSimpleElementPass in SimpleElementVertex.slang: device address of the transient debug
        // vertex array, passed through the per-pass args block.
        struct FSimpleElementPassData { uint64 Vertices = 0; };
    }

    void FForwardRenderScene::BatchedLineDraw(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& SimpleVertices     = Frame.Primitives.SimpleVertices;
        const auto& LineBatches        = Frame.Primitives.LineBatches;
        const auto& DrawCommands       = Frame.Geometry.DrawCommands;

        if (SimpleVertices.empty() || LineBatches.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Batched Line Draw", tracy::Color::Red2);

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("SimpleElementVertex.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("SimpleElementPixel.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);

        // Only Load when an earlier pass wrote HDR (base pass / terrain / solid tris); billboards render
        // after this pass, so they must not count.
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

        // Vertices live in the transient ring for this submission; the VS reads them by device address.
        const FSimpleElementPassData VertsPass
        {
            RHI::Core::CopyTransientArray(SimpleVertices.data(), SimpleVertices.size())
        };
        const RHI::GPUPtr Args = MakeArgs(VertsPass);

        // Re-set only when the depth mode changes between consecutive batches.
        int CurrentDepthMode = -1;
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

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FForwardRenderScene::BatchedTriangleDraw(RHI::FCmdListH CL)
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

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("SimpleElementVertex.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("SimpleElementPixel.slang");
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

        // Opaque batches write scene depth, which the froxel-fog and pyramid passes sampled earlier this
        // frame: order those reads ahead of the writes. Only paid when something opaque is queued.
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

        // Opaque batches take the source color as-is; blending an alpha-1 fragment is a no-op anyway,
        // and skipping it keeps a stray sub-1 alpha from leaking a depth-writing fragment into the blend.
        FGraphicsPipelineKey OpaqueKey = BlendedKey;
        OpaqueKey.ColorTargets[0].Blend = RHI::FBlendDesc{};

        // Translucent batches read reversed-Z but never write depth, so the overlay is occluded by solid
        // geometry without occluding itself. Opaque batches also write, so their own faces sort like real
        // geometry instead of resolving to whichever triangle was submitted last. XRay does neither, and
        // draws on top. (Vulkan ignores depth writes when the test is off, hence no write-only mode.)
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

        // Grouped by mode rather than drawn in submission order: opaque lays down depth first so the
        // blended modes sort against it. Submission order still holds within a group, and the state is
        // only bound for groups that actually have batches.
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

            // .a slots carry film-grain knobs (Shadows.a=Intensity, Midtones.a=Size,
            // Highlights.a=Response) -- avoids growing past Vulkan's 128B push guarantee.
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
            PC.TonemapMode        = (uint32)EToneMapper::ACES;
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
            // ExposureCompensation is in stops; 2^EV gives the linear
            // multiplier the shader expects.
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
            // 0.18 == photographic middle grey. Key <= 0 tells the shader to
            // ignore the adapted luminance and use the manual exposure alone.
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

        // Push constants for BloomUpsampleCS.slang. SrcIndex is the all-mips
        // bindless SRV of BloomChain; SrcMip picks the level via SampleLevel.
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

    void FForwardRenderScene::BloomPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const SPostProcessSettings* ActivePostProcess = Frame.PostProcess.bHasActivePostProcess ? &Frame.PostProcess.ActivePostProcessStorage : nullptr;

        if (ActivePostProcess == nullptr || !ActivePostProcess->bEnabled || ActivePostProcess->BloomIntensity <= 0.0f)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Bloom Pass", tracy::Color::Yellow3);

        static const FShaderEntry* const DownCS = FShaderLibrary::Get("BloomDownsample.slang");
        static const FShaderEntry* const UpCS = FShaderLibrary::Get("BloomUpsampleCS.slang");
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

        // Use as many octaves as the resolution supports (smallest mip ~8px on the short axis);
        // the deep mips are what give the wide cinematic veil.
        const uint32 MinDim  = eastl::min(Mip0W, Mip0H);
        // Guard the -2 before it happens: on a sub-8px axis it underflows unsigned and the clamp then
        // pins to the MAXIMUM octave count instead of the minimum. Bound by the chain's real mip count
        // rather than BLOOM_MIP_COUNT, since a small extent gives the image fewer mips than that.
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

    void FForwardRenderScene::AutoExposurePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const SPostProcessSettings* ActivePostProcess = Frame.PostProcess.bHasActivePostProcess ? &Frame.PostProcess.ActivePostProcessStorage : nullptr;

        // Skipped entirely when disabled; ColorGrading reads the persistent
        // AdaptedLuminance image but ignores it (AutoExposureKey <= 0).
        if (ActivePostProcess == nullptr || !ActivePostProcess->bEnabled || !ActivePostProcess->bAutoExposure)
        {
            return;
        }

        static const FShaderEntry* const CS = FShaderLibrary::Get("AutoExposure.slang");
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

    void FForwardRenderScene::ToneMappingPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Color Grading + Tone Map Pass", tracy::Color::Red2);

        const FFrameData& Frame = *RenderFrame;
        const auto& CachedWorldSettings        = Frame.CachedWorldSettings;
        const auto& ActivePostProcessMaterials = Frame.PostProcess.ActivePostProcessMaterials;
        const SPostProcessSettings* ActivePostProcess = Frame.PostProcess.bHasActivePostProcess ? &Frame.PostProcess.ActivePostProcessStorage : nullptr;
        const auto& SceneGlobalData            = Frame.SceneGlobalData;

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("ColorGrading.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        // Render to an LDR intermediate when SMAA or a post-process chain is active
        // (both need to ping-pong before the final blit); otherwise straight to the view output.
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
        // 16 B push block for the PostProcess material template.
        // Mirrors PostProcessPixelPass.slang::FPostProcessPushConstants.
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

    void FForwardRenderScene::PostProcessMaterialPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& CachedWorldSettings        = Frame.CachedWorldSettings;
        const auto& ActivePostProcessMaterials = Frame.PostProcess.ActivePostProcessMaterials;

        if (ActivePostProcessMaterials.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Post Process Material Pass", tracy::Color::Magenta);

        // Post-process inputs (chain source, opaque depth, pre-tonemap HDR) are read bindlessly via
        // pass-block indices. Depth's point-clamp vs the linear-clamp others is selected by sampler
        // index inside the shader.
        const FSceneImage& DepthTex = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& HDRTex   = GetNamedImage(ENamedImage::HDR);

        const bool bSMAAEnabled = CachedWorldSettings.SMAAQuality != ESMAAQuality::Off;

        // Chain reads Source, writes Dest, swapping each pass. ToneMappingPass forced its
        // output into LDR when PP materials are present, so the first read is always LDR.
        const FSceneImage* Source = &GetNamedImage(ENamedImage::LDR);
        const FSceneImage* Dest   = &GetNamedImage(ENamedImage::PostProcessScratch);

        for (const FFrameData::FPostProcessMaterial& PPMaterial : ActivePostProcessMaterials)
        {
            // Resolved + ref-held at extract; the render phase never touches the CMaterial.
            const FShaderEntry* VS = PPMaterial.Shaders.VertexShader;
            const FShaderEntry*  PS = PPMaterial.Shaders.PixelShader;
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
            // Interface's index (resolved at extract): instances own their own buffer slot where
            // parameter overrides live, so the parent's slot would ignore them.
            PC.MaterialIndex    = PPMaterial.MaterialIndex;
            PC.SceneColorIndex  = (uint32)Source->GetResourceID();
            PC.SceneDepthIndex  = (uint32)DepthTex.GetResourceID();
            PC.HDRIndex         = (uint32)HDRTex.GetResourceID();

            RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);

            eastl::swap(Source, Dest);
        }

        // Source holds the latest result; copy it where each consumer expects --
        // LDR for SMAA, the view output for the no-SMAA path.
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

    void FForwardRenderScene::SMAAEdgeDetectionPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("SMAA Edge Detection", tracy::Color::Red2);

        const FFrameData& Frame = *RenderFrame;
        const auto& CachedWorldSettings = Frame.CachedWorldSettings;

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("SMAAEdgeDetection.slang");
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

    void FForwardRenderScene::SMAABlendWeightPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("SMAA Blend Weight", tracy::Color::Red2);

        const FFrameData& Frame = *RenderFrame;
        const auto& CachedWorldSettings = Frame.CachedWorldSettings;

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("SMAABlendWeight.slang");
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

    void FForwardRenderScene::SMAANeighborhoodBlendPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("SMAA Neighborhood Blend", tracy::Color::Red2);

        const FFrameData& Frame = *RenderFrame;
        const auto& CachedWorldSettings = Frame.CachedWorldSettings;

        static const FShaderEntry* const VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderEntry* const PixelShader = FShaderLibrary::Get("SMAANeighborhoodBlend.slang");
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
    
    void FForwardRenderScene::InitBuffers()
    {
        // Cluster grid is per-view (created in AddSceneView). All CPU-dynamic scene data (instances,
        // bones, lights, billboards, widgets, cull views, skin descriptors, env/fog params, meshlet
        // prefix) is uploaded to the transient ring each frame -- no persistent buffer. What remains
        // persistent: GPU-written rings + pre-skinned vertices, all plain device-local allocations
        // reached by address. Debug line/triangle geometry is transient at its draw site.

        // GPU pre-skinning output: written by Skinning.slang, read by every draw VS via BDA.
        PreSkinnedVerticesBuffer = CreateSceneBuffer(sizeof(FPreSkinnedVertex) * 64 * 1024);

        for (uint32 Slot = 0; Slot < RHI::kFramesInFlight; ++Slot)
        {
            // Unified meshlet draw list (NumViews * TotalMeshletBound); CullMeshlets appends
            // surviving meshlets into packed per-(view, draw) regions laid out by BuildDrawPrefix.
            MeshletDrawListRing[Slot] = CreateSceneBuffer(sizeof(uint32) * 2);

            // Unified indirect draw args (NumViews * NumDraws), manually ringed: GPU-atomic-written
            // by the cull and consumed by DrawIndirect.
            IndirectArgsRing[Slot] = CreateSceneBuffer(sizeof(RHI::FDrawIndirectArguments));

            // Two-phase cull defer list: phase 0 appends prev-frame-HZB rejects, phase 1
            // re-tests them. Stride matches FMeshletDeferred (4x uint32).
            MeshletDeferListRing[Slot] = CreateSceneBuffer(sizeof(uint32) * 4);

            // Atomic counter paired with MeshletDeferList; zeroed via CmdMemset before phase 0.
            DeferCountRing[Slot] = CreateSceneBuffer(sizeof(uint32));

            // {GroupCountX,Y,Z} for the late-cull indirect dispatch; written GPU-side from DeferCount each
            // frame so the late cull launches O(deferred) workgroups, not the worst-case full-scene grid.
            CullDispatchArgsRing[Slot] = CreateSceneBuffer(sizeof(uint32) * 3);

            // SPD hand-off counter: phase 1 (per-tile mips 0..5) to phase 2 (last workgroup,
            // mips 6..11). Zeroed before each dispatch; phase 2 resets it so it stays zero.
            SpdCounterRing[Slot] = CreateSceneBuffer(sizeof(uint32));

            // Per-instance meshlet prefix (N+1 uints), GPU-built by the ScanPrefix* three-pass scan.
            InstancePrefixRing[Slot] = CreateSceneBuffer(sizeof(uint32));

            // {TotalMeshletBound, InstanceCount}: the meshlet cull's dispatch domain and its
            // binary-search bound. GPU-resident so they can be produced GPU-side.
            TotalsRing[Slot] = CreateSceneBuffer(sizeof(uint32) * kTotalsSlots);
            // CreateSceneBuffer is a bare device allocation; the zero happens on this slot's first
            // dispatch, where there is a command list to do it with.
            TotalsZeroed[Slot] = false;

            // GPU-driven scene per-frame outputs. Sized for real in CompileDrawCommands_Render.
            VisibleInstanceRing[Slot]       = CreateSceneBuffer(sizeof(FGPUInstance));
            CullCounterRing[Slot]           = CreateSceneBuffer(sizeof(uint32) * 2);
            BatchMeshletCountRing[Slot]     = CreateSceneBuffer(sizeof(uint32));
            ViewDrawCountRing[Slot]         = CreateSceneBuffer(sizeof(uint32));
            ViewDrawOffsetRing[Slot]        = CreateSceneBuffer(sizeof(uint32));
            ScanBlockSumRing[Slot]          = CreateSceneBuffer(sizeof(uint32) * 2);
            ScanDispatchArgsRing[Slot]      = CreateSceneBuffer(sizeof(uint32) * 3);

            // CPU-visible copy of the whole Totals block, picked up kFramesInFlight frames later to size
            // the next meshlet draw list, defer list and visible-instance buffer. Sized off kTotalsSlots
            // like the ring it mirrors -- this held 2 uints while the copy and the read both moved 8.
            if (MeshletBoundReadback[Slot] == 0)
            {
                MeshletBoundReadback[Slot] = RHI::Malloc(sizeof(uint32) * kTotalsSlots, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);

                // A new allocation holds undefined bytes, and this slot is read once before its first
                // copy has ever landed. Zeroing makes that first read a real "no measurement yet",
                // rather than heap junk that sizes buffers off nonsense demand and trips the overflow
                // warnings on startup. Once per slot for the life of the scene.
                if (void* Host = RHI::ToHost(MeshletBoundReadback[Slot]))
                {
                    Memory::Memzero(Host, sizeof(uint32) * kTotalsSlots);
                }
            }
        }
    }

    // Picks up the GPU meshlet total this slot recorded on its previous turn. kFramesInFlight frames of
    // latency is exactly what makes the read safe without a stall, and the value only needs to be
    // approximately right -- the in-shader clamp covers the frames where it isn't.
    void FForwardRenderScene::UpdateMeshletBoundFeedback(uint8 Slot)
    {
        const RHI::GPUPtr Readback = MeshletBoundReadback[Slot];
        if (Readback == 0)
        {
            return;
        }
        // CPURead allocations are persistently mapped. Zeroed on allocation, so a slot that has not
        // completed a frame yet reads 0 everywhere -- which every consumer already treats as
        // "no measurement", falling back to its floor instead of a garbage demand.
        static_assert(FForwardRenderScene::kTotalsSlots >= 10, "Totals[9] is read below.");
        if (const uint32* Mapped = static_cast<const uint32*>(RHI::ToHost(Readback)))
        {
            LastMeshletBound         = Mapped[0];
            LastDrawListRequired     = Mapped[2];
            LastDrawListOverflowed   = Mapped[3];
            LastVisibleInstances     = Mapped[4];
            LastVisibleOverflowed    = Mapped[5];
            LastDeferRequested       = Mapped[6];
            LastDeferOverflowed      = Mapped[7];
            LastMeshletWorkRequested = Mapped[8];
            LastMeshletWorkClamped   = Mapped[9];
        }
    }

    /**
     * The GPU half of the scene: uploads whatever the retained set changed, then hands culling, LOD
     * selection and the draw-argument layout to the GPU.
     *
     * Nothing here is proportional to the scene. The uploads are proportional to what changed, and the
     * dispatches are fixed-cost from the CPU's side -- which is the whole point: moving the camera no
     * longer touches the CPU at all.
     */
    void FForwardRenderScene::DispatchGPUSceneCull(RHI::FCmdListH CL, const FFrameData& Frame)
    {
        static const FShaderEntry* const CullInstancesShader = FShaderLibrary::Get("CullInstances.slang");
        static const FShaderEntry* const DrawPrefixShader    = FShaderLibrary::Get("BuildDrawPrefix.slang");
        if (CullInstancesShader == nullptr || DrawPrefixShader == nullptr)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("GPU Scene Cull", tracy::Color::Magenta);
        SCENE_GPU_SCOPE(CL, "GPU Scene Cull");

        // Extract decided WHAT to send; the payload is read straight from ScenePrimitives' live arrays,
        // which nothing mutates between Extract and here. See FGeometry::FRetainedUpload.
        const FFrameData::FGeometry::FRetainedUpload& Upload = Frame.Geometry.RetainedUpload;
        const FInstanceCullEntry* SrcCullEntries = ScenePrimitives.GetRetainedCullEntries();
        const FTransform3x4*      SrcTransforms  = ScenePrimitives.GetRetainedTransforms();
        const FInstanceStatic*    SrcStatic      = ScenePrimitives.GetRetainedStatic();

        const uint8  Slot          = CurrentFrameSlot;
        const uint32 RetainedSlots = Upload.SlotCount;
        const uint32 NumBatches    = Math::Max(Frame.Views.NumDrawsPerView, 1u);
        const uint32 NumCullViews  = (uint32)Frame.Views.CullViews.size();
        const uint32 NumSkinned    = (uint32)Frame.Geometry.Instances.size();

        // First GPU work of the frame to touch Totals, so this is where the one-time zero belongs.
        // BuildDrawPrefix below overwrites [0..5] unconditionally, but [6]/[7] are not produced until
        // the view culls run later in the frame -- after the feedback copy at the bottom of this
        // function has already sampled them. Without this, that first copy feeds undefined bytes back
        // as a defer demand, which is what reported ~1.8 billion deferred meshlets on startup.
        if (!TotalsZeroed[Slot] && GetTotals())
        {
            RHI::CmdMemset(CL, GetTotals().Ptr, GetTotals().GetSize(), 0u);
            Barriers::TransferToAll(CL);
            TotalsZeroed[Slot] = true;
        }


        //~ Retained uploads. Ordered on the GPU timeline against every earlier frame's reads because
        //  they are CmdMemcpy from the transient ring, not CPU writes into memory the GPU is reading --
        //  which is what lets the retained buffers be single-instanced rather than ringed.
        {
            LUMINA_PROFILE_SECTION_COLORED("Retained Upload", tracy::Color::Magenta4);
            SCENE_GPU_SCOPE(CL, "Retained Upload");

            const SIZE_T CullBytes      = Math::Max<SIZE_T>(sizeof(FInstanceCullEntry), (SIZE_T)RetainedSlots * sizeof(FInstanceCullEntry));
            const SIZE_T TransformBytes = Math::Max<SIZE_T>(sizeof(FTransform3x4),      (SIZE_T)RetainedSlots * sizeof(FTransform3x4));
            const SIZE_T StaticBytes    = Math::Max<SIZE_T>(sizeof(FInstanceStatic),    (SIZE_T)RetainedSlots * sizeof(FInstanceStatic));

            // Shrink is gated on a full-snapshot frame: reclaiming the allocation drops the accumulated
            // contents, and only a frame carrying the whole array can put them back. Growth is always
            // accompanied by a full snapshot, because Extract sets bFull precisely when the slot
            // count outgrew the capacity published below.
            ResizeBufferIfNeeded(RetainedCullEntryBuffer, CullBytes,      1.5f, RetainedCullEntryLowUsage, Upload.bFull);
            ResizeBufferIfNeeded(RetainedTransformBuffer, TransformBytes, 1.5f, RetainedTransformLowUsage, Upload.bFull);
            ResizeBufferIfNeeded(RetainedStaticBuffer,    StaticBytes,    1.5f, RetainedStaticLowUsage,    Upload.bFull);

            if (RetainedCullEntryBuffer && RetainedTransformBuffer && RetainedStaticBuffer && RetainedSlots > 0)
            {
                if (Upload.bFull)
                {
                    // O(primitives). Expected only on frames that grow the allocation -- if this zone
                    // shows up every frame, the dirty channel is not being consumed or something is
                    // forcing a resync. See FGeometry::FRetainedUpload.
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
                    // One copy per CONTIGUOUS RUN of slots, not per slot. Every WriteBuffer is a transient
                    // allocation, and each allocation inserts into the RHI's memory-block vector -- so a
                    // copy per slot is quadratic in the dirty count and will hang the render phase on a
                    // large invalidation. The snapshot arrives sorted and deduped precisely so this can
                    // walk runs; in the steady state both lists are empty and neither loop executes.
                    //
                    // The two lists are walked separately because they cover different buffers: a frame
                    // where a crowd moved dirties the cull entries and transforms and leaves the static
                    // payload alone, so its buffer is not touched at all.
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
            // Publish the capacity Extract uses to decide whether the NEXT frame needs a full
            // re-send. Derived from the allocations we actually got, and from the smallest of the three so
            // a partial failure cannot advertise room that isn't there.
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
            ResizeBufferIfNeeded(SurfaceDescBuffer, DescBytes, 1.5f, SurfaceDescLowUsage, Upload.bSurfaceDescsChanged);

            // A replaced allocation holds undefined bytes until something writes it, so nothing in it may
            // be indexed yet. UploadedSurfaceDescs is what the cull is bounded by below, which makes this
            // assignment load-bearing rather than bookkeeping: without it a frame that resized the buffer
            // and then skipped the write (the count-drift path below) leaves the cull reading a table of
            // pure garbage, and a garbage LOD meshlet count is what the dispatch size is summed from.
            if (SurfaceDescBuffer.Ptr != PrevDescs)
            {
                UploadedSurfaceDescs = 0;
            }

            // Only writable when this frame actually carries the payload. A reallocation or a count change
            // on a frame that did NOT bring one cannot be serviced here, so ask for a re-send instead of
            // writing a short buffer -- forcing bFull next frame also re-copies the descriptors.
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

        //~ Per-frame outputs. VisibleInstanceRing is deliberately NOT sized here: the SceneRoot published
        //  before this call already baked in its device address, so a resize at this point would retire the
        //  allocation every shader is pointing at. CompileDrawCommands_Render sizes it up front and
        //  hands the capacity over in FrameVisibleInstanceCapacity -- one value, sampled once, so the
        //  buffer, the prefix allocation and the clamps below cannot disagree.
        const uint32 VisibleCapacity = FrameVisibleInstanceCapacity;
        const uint32 SeedViews       = Math::Max(NumCullViews, 1u);
        const SIZE_T ViewDrawEntries = (SIZE_T)SeedViews * (SIZE_T)NumBatches;
        ResizeBufferIfNeeded(BatchMeshletCountRing[Slot],
                             (SIZE_T)NumBatches * sizeof(uint32), 1.5f, BatchMeshletCountLowUsage[Slot]);
        ResizeBufferIfNeeded(ViewDrawCountRing[Slot],
                             ViewDrawEntries * sizeof(uint32), 1.5f, ViewDrawCountLowUsage[Slot]);
        ResizeBufferIfNeeded(ViewDrawOffsetRing[Slot],
                             ViewDrawEntries * sizeof(uint32), 1.5f, ViewDrawOffsetLowUsage[Slot]);

        // Skinned instances are produced on the CPU (their pose changes every frame with nothing to
        // observe it by) and land at the head of the buffer; the cull's append cursor starts past them.
        if (NumSkinned > 0 && VisibleInstanceRing[Slot])
        {
            WriteBuffer(CL, VisibleInstanceRing[Slot].GetAddress(),
                        Frame.Geometry.Instances.data(), (SIZE_T)NumSkinned * sizeof(FGPUInstance));
        }

        {
            const uint32 Counters[2] = { NumSkinned, 0u };   // {append cursor, overflow}
            WriteBuffer(CL, GetCullCounters().GetAddress(), Counters, sizeof(Counters));

            // Seeded with the skinned contribution so the prefix covers both producers.
            const SIZE_T SeedCount = Math::Min<SIZE_T>(NumBatches, Frame.Geometry.BatchMeshletSeed.size());
            WriteBuffer(CL, GetBatchMeshletCounts().GetAddress(),
                        Frame.Geometry.BatchMeshletSeed.data(), SeedCount * sizeof(uint32));

            // The per-(view, draw) counts take the same seed replicated into every view. Skinned instances
            // are CPU-produced and never pass through the instance cull, so which views see them is not
            // known here. Crediting every view over-reserves slightly (the skinned count is small) and,
            // unlike under-reserving, cannot drop a meshlet.
            ViewDrawSeedScratch.assign(ViewDrawEntries, 0u);
            for (uint32 v = 0; v < SeedViews; ++v)
            {
                for (SIZE_T d = 0; d < SeedCount; ++d)
                {
                    ViewDrawSeedScratch[(SIZE_T)v * NumBatches + d] = Frame.Geometry.BatchMeshletSeed[d];
                }
            }
            WriteBuffer(CL, GetViewDrawCounts().GetAddress(),
                        ViewDrawSeedScratch.data(), ViewDrawSeedScratch.size() * sizeof(uint32));
        }

        Barriers::TransferToAll(CL);

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
                // ShadowLODBias / ShadowCoarseLODDistSq moved to FCullData: the meshlet cull re-derives
                // the cascade shadow pick and has to read the same inputs this pass reserved from.
                uint32 bUseLODs;
                uint32 MaxVisibleInstances;
                uint32 NumSurfaceDescs;
                uint64 RetainedCullEntriesAddr;
                uint64 RetainedTransformsAddr;
                uint64 RetainedStaticAddr;
                uint64 SurfaceDescsAddr;
                uint64 OutInstancesAddr;
                uint64 OutInstanceCountAddr;
                uint64 OutBatchMeshletCountAddr;
                uint64 OutViewDrawCountAddr;
                uint64 OutOverflowFlagAddr;
            };
            static_assert(sizeof(FCullInstancesPC) == 96, "FCullInstancesPC must match CullInstances.slang.");

            FCullInstancesPC PC = {};
            PC.NumRetained              = RetainedSlots;
            PC.NumViews                 = NumCullViews;
            PC.NumBatches               = NumBatches;
            PC.bUseLODs                 = RenderSettings.bUseLODs ? 1u : 0u;
            PC.MaxVisibleInstances      = VisibleCapacity;
            // What the device buffer actually holds, not what the game thread has interned. A slot whose
            // binding is newer than the last upload reads past the table otherwise.
            PC.NumSurfaceDescs          = UploadedSurfaceDescs;
            PC.RetainedCullEntriesAddr  = RetainedCullEntryBuffer.GetAddress();
            PC.RetainedTransformsAddr   = RetainedTransformBuffer.GetAddress();
            PC.RetainedStaticAddr       = RetainedStaticBuffer.GetAddress();
            PC.SurfaceDescsAddr         = SurfaceDescBuffer.GetAddress();
            PC.OutInstancesAddr         = VisibleInstanceRing[Slot].GetAddress();
            PC.OutInstanceCountAddr     = GetCullCounters().GetAddress();
            PC.OutBatchMeshletCountAddr = GetBatchMeshletCounts().GetAddress();
            PC.OutViewDrawCountAddr     = GetViewDrawCounts().GetAddress();
            PC.OutOverflowFlagAddr      = GetCullCounters().GetAddress() + sizeof(uint32);

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullInstancesShader));
            constexpr uint32 MaxDispatchAxis = 65535u;
            const uint32 Groups   = (RetainedSlots + 63u) / 64u;
            const uint32 GroupsX  = Math::Min(Groups, MaxDispatchAxis);
            const uint32 GroupsY  = (Groups + MaxDispatchAxis - 1u) / MaxDispatchAxis;
            RHI::CmdDispatch(CL, MakeArgs(PC), GroupsX, GroupsY, 1u);

            Barriers::ComputeToAll(CL);
        }

        //~ Draw-argument layout, from counts the CPU never sees.
        {
            LUMINA_PROFILE_SECTION_COLORED("Build Draw Prefix", tracy::Color::Magenta3);
            SCENE_GPU_SCOPE(CL, "Build Draw Prefix");

            // Ceiling on the meshlet work domain. Using the GLOBAL max per-LOD meshlet count rather than
            // each slot's own desc is deliberate: it stays an upper bound across a rebind, which changes
            // which desc a slot points at without touching either count, so nothing has to invalidate it.
            //
            // Carried in FRetainedUpload rather than rescanned here. This used to walk SurfaceDescs from
            // the render phase and keep a running max in the scene -- correct, since extract and render are
            // phases of one thread, but it re-derived per frame what interning already knows, and a running
            // max that outlives the table it summarizes is a latch waiting for someone to get its reset
            // wrong. InternSurfaceDesc folds it in once, from the value it has just clamped.
            const uint64 MaxDescMeshlets = Math::Min<uint64>(Upload.MaxSurfaceDescMeshlets,
                                                             MAX_MESHLETS_PER_SURFACE_LOD);

            // The skinned contribution never passes through the instance cull, so it is not covered by
            // the retained-slot product and has to be added on top.
            uint64 SkinnedSeedTotal = 0;
            for (uint32 Seed : Frame.Geometry.BatchMeshletSeed)
            {
                SkinnedSeedTotal += Seed;
            }

            // Absolute backstop, independent of every input above. The product is a worst case -- every
            // retained slot surviving at the densest LOD any surface has -- so it is legitimately large on
            // a big scene, but past some size it stops bounding anything the GPU can finish inside a TDR
            // window, and a bad input makes it 2^32. Deliberately NOT a resource bound (the draw list, say):
            // the walk is what MEASURES demand, so clamping it to what currently fits would under-report,
            // the allocation would never grow, and the clamp would latch.
            //
            // Deliberately loose, and NOT reported: MaxDescMeshlets is the global max over the interned
            // table, so one dense surface (a dynamic mesh committed as a single section, say) becomes the
            // assumed per-slot count for every slot in the scene and inflates this by orders of magnitude.
            // Tripping the ceiling therefore says nothing about whether anything was dropped. BuildDrawPrefix
            // publishes the measured domain and a truncation flag in Totals[8]/[9]; the warning lives there.
            const uint64 RawMeshletWork   = (uint64)RetainedSlots * MaxDescMeshlets + SkinnedSeedTotal;
            const uint64 MaxMeshletWork64 = Math::Min<uint64>(RawMeshletWork, GMaxMeshletCullDomain);

            struct FBuildDrawPrefixPC
            {
                uint32 NumViews;
                uint32 NumDraws;
                uint32 MaxVisibleInstances;
                uint32 DrawListCapacityArg;
                uint32 MaxMeshletWork;
                uint32 Pad0;
                uint64 BatchMeshletCountsAddr;
                uint64 ViewDrawCountsAddr;
                uint64 InstanceCountAddr;
                uint64 OutViewDrawOffsetsAddr;
                uint64 OutTotalsAddr;
                uint64 OutScanDispatchArgsAddr;
            };
            static_assert(sizeof(FBuildDrawPrefixPC) == 72, "FBuildDrawPrefixPC must match BuildDrawPrefix.slang.");

            FBuildDrawPrefixPC PC = {};
            PC.NumViews                  = SeedViews;
            PC.NumDraws                  = NumBatches;
            PC.MaxVisibleInstances       = VisibleCapacity;
            PC.DrawListCapacityArg       = DrawListCapacity;
            PC.MaxMeshletWork            = (uint32)Math::Min<uint64>(MaxMeshletWork64, 0xFFFFFFFFull);
            PC.BatchMeshletCountsAddr    = GetBatchMeshletCounts().GetAddress();
            PC.ViewDrawCountsAddr        = GetViewDrawCounts().GetAddress();
            PC.InstanceCountAddr         = GetCullCounters().GetAddress();
            PC.OutViewDrawOffsetsAddr    = GetViewDrawOffsets().GetAddress();
            PC.OutTotalsAddr             = GetTotals().GetAddress();
            PC.OutScanDispatchArgsAddr   = GetScanDispatchArgs().GetAddress();

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(DrawPrefixShader));
            RHI::CmdDispatch(CL, MakeArgs(PC), 1u, 1u, 1u);
            Barriers::ComputeToAll(CL);
        }

        // Feedback for next time this slot comes around: copy the total the GPU just produced into the
        // CPU-visible buffer that sizes the meshlet draw list.
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

    void FForwardRenderScene::AllocateMSAAImages(FSceneView& View, const FUIntVector2& Extent)
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

    void FForwardRenderScene::SyncMSAAState()
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

        // MS scratch is per-view; reallocate (or drop) it for every view. Old images retire
        // on the deferred-release list so in-flight frames finish first.
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

    void FForwardRenderScene::ReleaseViewImages(FSceneView& View, bool bDeferRelease)
    {
        // Ownership-driven: release exactly what this view created and leave aliases to their owner.
        // This replaced a hand-maintained list of per-view ENamedImage slots, which silently leaked
        // the texture AND its bindless slot whenever a new per-view image was added to
        // InitViewImages without also being added to the list (VisBuffer was missing that way).
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
    }

    // Debug-utils name for each render target slot. A GPU crash report resolves a page fault back to
    // whatever resource owns the offending address, and an unnamed image resolves to nothing more
    // useful than its dimensions. Kept exhaustive rather than defaulted so a new ENamedImage entry
    // fails the switch warning instead of silently reporting as "Unknown".
    static const char* ENamedImageToString(FForwardRenderScene::ENamedImage Image)
    {
        using ENamedImage = FForwardRenderScene::ENamedImage;
        switch (Image)
        {
        case ENamedImage::HDR:                return "Scene.HDR";
        case ENamedImage::LDR:                return "Scene.LDR";
        case ENamedImage::PostProcessScratch: return "Scene.PostProcessScratch";
        case ENamedImage::SMAAEdges:          return "Scene.SMAAEdges";
        case ENamedImage::SMAABlend:          return "Scene.SMAABlend";
        case ENamedImage::SMAAArea:           return "Scene.SMAAArea";
        case ENamedImage::SMAASearch:         return "Scene.SMAASearch";
        case ENamedImage::SSAO:               return "Scene.SSAO";
        case ENamedImage::SSAODenoise:        return "Scene.SSAODenoise";
        case ENamedImage::SSAOBlur:           return "Scene.SSAOBlur";
        case ENamedImage::ShadowMask:         return "Scene.ShadowMask";
        case ENamedImage::Cascade:            return "Scene.Cascade";
        case ENamedImage::CascadePyramid:     return "Scene.CascadePyramid";
        case ENamedImage::DepthAttachment:    return "Scene.DepthAttachment";
        case ENamedImage::DepthPyramid:       return "Scene.DepthPyramid";
        case ENamedImage::Picker:             return "Scene.Picker";
        case ENamedImage::VisBuffer:          return "Scene.VisBuffer";
        case ENamedImage::MaterialDepth:      return "Scene.MaterialDepth";
        case ENamedImage::Accum:              return "Scene.Accum";
        case ENamedImage::Revealage:          return "Scene.Revealage";
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

    // Named after the fact rather than at each CreateSceneImage call, so a newly added image is
    // covered without touching its call site. Borrowed entries are skipped: they alias an image
    // someone else owns and named, and renaming through the copy would just fight the owner.
    void FForwardRenderScene::NameOwnedImages(TArray<FSceneImage, (int)ENamedImage::Num>& Images)
    {
        for (int i = 0; i < (int)ENamedImage::Num; ++i)
        {
            if (Images[i].bOwned && Images[i].IsValid())
            {
                RHI::SetDebugName(Images[i].Texture, ENamedImageToString((ENamedImage)i));
            }
        }
    }

    void FForwardRenderScene::InitViewImages(FSceneView& View, uint32 ReuseOutputSlot)
    {
        const FUIntVector2 Extent = View.Size;

        // Seed with the scene's shared images (BRDF LUT, sky cubes, SMAA LUTs, cascade atlas, icons) so
        // GetNamedImage() reads them uniformly through CurrentView; the per-view slots below override.
        View.Images = NamedImages;

        // Those seeded entries are BORROWED -- the scene (or the render manager) owns and releases
        // them. Clearing ownership on the copies is what stops ReleaseViewImages from double-freeing
        // the cascade atlas / sky cubes, which the scene releases itself.
        for (FSceneImage& Seeded : View.Images)
        {
            Seeded.bOwned = false;
        }

        RHI::FTextureDesc Desc;
        Desc.Type      = RHI::ETextureType::Tex2D;
        Desc.Dimension = FUIntVector3(Extent.x, Extent.y, 1);

        // Final display-referred target; the editor samples Output.GetResourceID(). Copy destination
        // (no-SMAA post-process chain blit) and copy source (thumbnail/screenshot readbacks).
        Desc.Format = EFormat::RGBA8_UNORM;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled |
                      RHI::EImageUsageFlags::TransferDst | RHI::EImageUsageFlags::TransferSrc;
        View.Output = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ false, ReuseOutputSlot);

        // HDR scene color; copy source for the water/underwater refraction snapshot.
        Desc.Format = EFormat::RGBA16_FLOAT;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferSrc;
        View.Images[(int)ENamedImage::HDR] = CreateSceneImage(Desc);

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

        // Single-channel AO chain: raw GTAO + plane-aware denoise at half res, then a joint
        // bilateral upsample into the full-res SSAOBlur the base pass samples (half-res AO
        // edges stair-step at creases under plain bilinear).
        Desc.Format    = EFormat::R8_UNORM;
        Desc.Dimension = FUIntVector3(Math::Max(Extent.x / 2, 1u), Math::Max(Extent.y / 2, 1u), 1);
        View.Images[(int)ENamedImage::SSAO]        = CreateSceneImage(Desc);
        View.Images[(int)ENamedImage::SSAODenoise] = CreateSceneImage(Desc);
        Desc.Dimension = FUIntVector3(Extent.x, Extent.y, 1);
        View.Images[(int)ENamedImage::SSAOBlur]    = CreateSceneImage(Desc);

        // Screen-space sun-shadow visibility. FULL res and R8, both load-bearing: the shading passes
        // read it with a Load() at their own SV_Position, so any other resolution would need a filtered
        // fetch and would soften contact shadows the PCSS deliberately keeps sharp (its filter floor is
        // 1.5 shadow texels). One channel because only the directional light is deferred -- spot/point
        // shadows stay inline, since a pixel can receive several of them and they will not pack.
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

        // VisBuffer: per-pixel {meshletSlot, triId} visibility ID; written by the VisBuffer geometry pass,
        // sampled by the deferred material pass.
        Desc.Format = EFormat::R32_UINT;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled;
        View.Images[(int)ENamedImage::VisBuffer] = CreateSceneImage(Desc);

        // MaterialDepth: each covered pixel's owning deferred material slot, encoded as a depth value by
        // MaterialDepthPass. Not scene depth -- it exists purely so the material draws can reject pixels they
        // do not own with a hardware DEPTH_EQUAL test instead of a per-pixel load and `discard`.
        Desc.Format = EFormat::D32;
        Desc.Usage  = RHI::EImageUsageFlags::DepthAttachment | RHI::EImageUsageFlags::Sampled;
        View.Images[(int)ENamedImage::MaterialDepth] = CreateSceneImage(Desc);

        AllocateMSAAImages(View, Extent);

        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled;

        Desc.Format = EFormat::RGBA16_FLOAT;
        View.Images[(int)ENamedImage::Accum] = CreateSceneImage(Desc);

        // WBOIT revealage = multiplicative product of (1-a_i); R16F is the reference format.
        Desc.Format = EFormat::R16_FLOAT;
        View.Images[(int)ENamedImage::Revealage] = CreateSceneImage(Desc);

        // Scene-color copy for the water + underwater passes (HDR is copied here, then sampled for
        // refraction / SSR / distortion so those passes never read the HDR target they also write).
        Desc.Format = EFormat::RGBA16_FLOAT;
        Desc.Usage  = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;
        View.Images[(int)ENamedImage::WaterRefraction] = CreateSceneImage(Desc);

        // DBuffer decal targets: BaseColor / WorldNormal / Roughness-Metallic-AO, each with transmittance
        // in alpha. RGBA8_UNORM; written by DecalPass, sampled by the base pass.
        Desc.Format = EFormat::RGBA8_UNORM;
        Desc.Usage  = RHI::EImageUsageFlags::ColorAttachment | RHI::EImageUsageFlags::Sampled;
        View.Images[(int)ENamedImage::DBufferA] = CreateSceneImage(Desc);
        View.Images[(int)ENamedImage::DBufferB] = CreateSceneImage(Desc);
        View.Images[(int)ENamedImage::DBufferC] = CreateSceneImage(Desc);

        {
            // Froxel fog volumes: fixed 3D grid (swapchain-independent). RGBA16F = (in-scatter, a) where a is
            // extinction (Scatter) or transmittance (Integrated). Storage for the UAVs, sampled to apply.
            // Resolution = base * CRendererSettings::FroxelResolutionScale, cached so the dispatches match.
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
            // Bloom mip chain (half-res, R11G11B10_FLOAT). SPD writes mips 0..N-1 from
            // HDR in one dispatch, then per-mip upsamples accumulate into mip[i-1].
            const uint32 BloomW = eastl::max<uint32>(Extent.x / 2u, 1u);
            const uint32 BloomH = eastl::max<uint32>(Extent.y / 2u, 1u);

            RHI::FTextureDesc BloomDesc;
            BloomDesc.Type      = RHI::ETextureType::Tex2D;
            BloomDesc.Dimension = FUIntVector3(BloomW, BloomH, 1);
            BloomDesc.Format    = EFormat::R11G11B10_FLOAT;
            // Clamped, not a flat 8: a window dragged small enough puts the half-res chain under 128px,
            // where 8 mips is more than the image can legally have.
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

    void FForwardRenderScene::BakeBRDFLUT()
    {
        constexpr uint32 BRDFLutSize = 256u;

        FSharedRenderResources& Shared = GRenderManager->GetSharedRenderResources();

        Shared.BRDFLut = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width    = BRDFLutSize,
            .Height   = BRDFLutSize,
            .Format   = EFormat::RG16_FLOAT,
            .bStorage = true
        });
        Shared.BRDFLutUAV = RHI::Textures::StorageSlot(Shared.BRDFLut, 0);

        static const FShaderEntry* const ComputeShader = FShaderLibrary::Get("BRDFIntegration.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::FPipelineH Pipeline = RHI::CreateComputePipeline(ComputeShader->Source());

        RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
        RHI::CmdSetPipeline(CL, Pipeline);

        // Mirrors FBRDFArgs in BRDFIntegration.slang: just the output UAV heap index.
        struct FBRDFArgs { uint32 OutUAV; uint32 _Pad0; uint32 _Pad1; uint32 _Pad2; };
        const FBRDFArgs Args{ Shared.BRDFLutUAV, 0, 0, 0 };
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

    void FForwardRenderScene::InitSkyCube(uint32 FaceSize)
    {
        // Face size drives the IBL source resolution and (in HDRI mode) the angular detail the
        // visible sky reflects. Bilinear filtering still supplies per-pixel sky detail, so the cube
        // need not match screen size. Set by the active environment's IBLQuality tier.
        RHI::FTextureDesc Desc;
        Desc.Type       = RHI::ETextureType::TexCube;
        Desc.Dimension  = FUIntVector3(FaceSize, FaceSize, 1);
        Desc.LayerCount = 6;
        Desc.Format     = EFormat::R11G11B10_FLOAT;
        Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage | RHI::EImageUsageFlags::TransferDst;

        NamedImages[(int)ENamedImage::SkyCube] = CreateSceneImage(Desc, true, /*bMipUAVs*/ true);
    }

    void FForwardRenderScene::InitIBLConvolutionTargets(const FIBLBakeResolution& Resolution)
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

        // Pre-filtered specular: roughness spread evenly across mips. Smallest mip = fully rough;
        // the roughness=1 GGX lobe is wide enough that a tiny face suffices.
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

    // Allocated on first use rather than at Init: a scene with no reflection probes is the common case
    // (and every thumbnail/preview world), and these two together run ~30 MB. Idempotent, so the bake
    // and the extract can both call it without coordinating.
    void FForwardRenderScene::InitReflectionProbeTargets()
    {
        if (NamedImages[(int)ENamedImage::ProbePrefiltered].IsValid())
        {
            return;
        }

        // The runtime array. One 6-layer slice per probe; R11G11B10 to match the sky prefilter it
        // blends against, which also keeps the array a third the size of an RGBA16F one.
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

        // Unbaked slices are undefined memory, which reads as garbage radiance until the bake fills
        // them. Clear once here so a probe that has been extracted but not yet baked contributes black
        // rather than noise while it waits its turn in the queue.
        RHI::FCmdListH CL = RHI::OpenCommandList();
        const float Black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Barriers::AllToTransfer(CL);
        RHI::CmdClearTexture(CL, NamedImages[(int)ENamedImage::ProbePrefiltered].Texture, Black);
        Barriers::TransferToAll(CL);
        RHI::SubmitAndWait(CL);

        NameOwnedImages(NamedImages);
    }

    // The capture cube must match the bake view's face size EXACTLY. It is a plain texture copy from the
    // view's 2D HDR target into a cube face, so a cube larger than the view would leave the rest of each
    // face unwritten -- and the prefilter samples the whole face directionally, with no way to restrict
    // itself to the written sub-rect. Recreation needs WaitDeviceIdle, so this runs in PrepareRender
    // (serial, before any scene records) rather than mid-RenderView, matching SyncIBLResolution.
    void FForwardRenderScene::SyncProbeCaptureCube(uint32 FaceSize)
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

    void FForwardRenderScene::SyncIBLResolution(const FIBLBakeResolution& Resolution)
    {
        if (Resolution == AppliedIBLResolution)
        {
            return;
        }

        // Rare (editor-driven quality change). Drain the GPU so no in-flight frame still reads the old
        // cubes through their heap slots, then recreate them. The bake passes read sizes dynamically
        // (GetSizeX / GetNumMips), so they adapt with no further changes.
        RHI::WaitDeviceIdle();

        ReleaseSceneImage(NamedImages[(int)ENamedImage::SkyCube]);
        ReleaseSceneImage(NamedImages[(int)ENamedImage::SkyIrradiance]);
        ReleaseSceneImage(NamedImages[(int)ENamedImage::SkyPrefilter]);

        InitSkyCube(Resolution.SkyCube);
        InitIBLConvolutionTargets(Resolution);

        // Views snapshot the shared images (InitViewImages: View.Images = NamedImages); repoint the three
        // IBL slots in every view so GetNamedImage / BuildViewSceneRoot pick up the new cubes.
        for (FSceneView& View : SceneViews)
        {
            // Borrowed: the scene owns these cubes and releases them itself, so the view's copies
            // must not be picked up by ReleaseViewImages' owned-image sweep.
            View.Images[(int)ENamedImage::SkyCube]       = BorrowSceneImage(NamedImages[(int)ENamedImage::SkyCube]);
            View.Images[(int)ENamedImage::SkyIrradiance] = BorrowSceneImage(NamedImages[(int)ENamedImage::SkyIrradiance]);
            View.Images[(int)ENamedImage::SkyPrefilter]  = BorrowSceneImage(NamedImages[(int)ENamedImage::SkyPrefilter]);
        }

        // The freshly-sized cubes have undefined contents, but Extract set bIBLDirty +
        // bIBLConvolutionDirty on this same resolution change (bResChanged), so the bake refills them
        // this frame. Don't touch bIBL*Valid here -- the bake later this frame owns them.
        AppliedIBLResolution = Resolution;
    }

    void FForwardRenderScene::InitFrameResources()
    {
        // Resize: rebuild the primary view's image chain at the new size. The per-view
        // cluster buffer is size-independent and persists across resize.
        FSceneView& Primary = SceneViews[0];

        // Output's heap slot survives the resize; only the texture behind it is replaced.
        const uint32 OutputSlot = DetachSampledSlot(Primary.Output);

        // Still DEFERRED, even though SwapchainResized already idled the device. The GPU is done, but the
        // command lists recorded before the resize have not been recycled yet and still name these textures;
        // destroying now would report them as in use. The slot's Core::BeginFrame resets its lists before
        // RenderView drains this queue, so by then nothing references them.
        ReleaseViewImages(Primary, /*bDeferRelease*/ true);
        InitViewImages(Primary, OutputSlot);
    }

    uint64 FForwardRenderScene::BuildViewSceneRoot(FSceneView& View, uint64 SceneDataAddr)
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

        // Probes are suppressed while a probe capture is rendering: a capture that sampled the array
        // would fold the previous bake's reflections into the new one, compounding on every rebuild.
        // Read from NamedImages rather than View.Images because the array is allocated lazily, after
        // InitViewImages has already snapshotted the shared slots into each view.
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

    RHI::FPipelineH FForwardRenderScene::GetOrCreatePipeline(const FGraphicsPipelineKey& Key)
    {
        // (ID, Generation) per shader: a recompile changes the hash, so stale pipelines
        // simply stop being found and the new bytecode gets a fresh pipeline.
        size_t Seed = 0;
        Hash::HashCombine(Seed, Key.VS ? Key.VS->PipelineHash() : 0ull);
        Hash::HashCombine(Seed, Key.PS ? Key.PS->PipelineHash() : 0ull);
        Hash::HashCombine(Seed, Key.MS ? Key.MS->PipelineHash() : 0ull);
        Hash::HashCombine(Seed, ((uint64)Key.Topology) | ((uint64)Key.bWireframe << 8) |
                                ((uint64)Key.bAlphaToCoverage << 9) | ((uint64)Key.SampleCount << 16) |
                                ((uint64)Key.DepthFormat << 24) | ((uint64)Key.PassVariant << 32) |
                                ((uint64)Key.ShadingFeatures << 40) | ((uint64)Key.bVisBufferMasked << 56) |
                                ((uint64)Key.SkinnedMode << 57));
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

        auto It = PipelineCache.find(Seed);
        if (It != PipelineCache.end())
        {
            return It->second;
        }

        RHI::FRasterDesc Desc;
        Desc.Topology         = Key.Topology;
        Desc.bWireframe       = Key.bWireframe;
        Desc.bAlphaToCoverage = Key.bAlphaToCoverage;
        Desc.SampleCount      = Key.SampleCount;
        Desc.DepthFormat      = Key.DepthFormat;
        Desc.ColorTargets     = TSpan(Key.ColorTargets.data(), Key.ColorTargets.size());

        const RHI::FShaderSource PSSource = Key.PS ? Key.PS->Source() : RHI::FShaderSource{};

        // Spec constants: id 0 = EPass (geometry pass inside the merged MeshletVertex.slang); ids 1-3 and 6
        // = ShadeSurface feature gates (DEBUG_VIEWS/DECALS/SSAO/SHADOW_MASK); id 4 = VISBUFFER_MASKED;
        // id 5 = SPEC_SKINNED. Shaders that don't declare an id ignore it, so the same set is fed
        // uniformly to every pipeline (matching the existing EPass-only behavior).
        auto MakeUInt = [](uint32 Id, uint32 Value) -> RHI::FSpecializationConstant
        {
            return RHI::FSpecializationConstant{ .ConstantID = Id, .AsInt = (uint64)Value, .Type = RHI::ESpecializationConstantType::UInt32 };
        };
        const RHI::FSpecializationConstant SpecConsts[] =
        {
            MakeUInt(0, (uint32)Key.PassVariant),
            MakeUInt(1, (Key.ShadingFeatures & SF_DebugViews) ? 1u : 0u),
            MakeUInt(2, (Key.ShadingFeatures & SF_Decals)     ? 1u : 0u),
            MakeUInt(3, (Key.ShadingFeatures & SF_SSAO)       ? 1u : 0u),
            MakeUInt(4, Key.bVisBufferMasked ? 1u : 0u),
            MakeUInt(5, (uint32)Key.SkinnedMode),   // SPEC_SKINNED: 0=static, 1=skinned, 2=dynamic
            MakeUInt(6, (Key.ShadingFeatures & SF_ShadowMask) ? 1u : 0u),
        };
        const TSpan<const RHI::FSpecializationConstant> Consts(SpecConsts, 7);
        RHI::FPipelineH Pipeline = Key.MS
            ? RHI::CreateMeshShaderPipeline(RHI::FShaderSource{}, Key.MS->Source(), PSSource, Desc, Consts)
            : RHI::CreateGraphicsPipeline(Key.VS->Source(), PSSource, Desc, Consts);
        PipelineCache.emplace(Seed, Pipeline);

#if USING(WITH_EDITOR)
        // Attach the driver's register count / occupancy to the pixel shader entry so the material
        // editor can show what this material actually costs the hardware. Only the FIRST pipeline built
        // from a given PS pays the query: later permutations of the same bytecode report the same
        // numbers, and this sits on the pipeline-cache miss path either way.
        //
        // Note the numbers describe THIS permutation. Spec constants change what survives dead-stripping,
        // so a debug-views pipeline and a shipping one can allocate differently from identical source --
        // which is exactly the kind of thing this is meant to make visible.
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

    RHI::FPipelineH FForwardRenderScene::GetOrCreateComputePipeline(const FShaderEntry* CS)
    {
        size_t Seed = 0;
        Hash::HashCombine(Seed, CS->PipelineHash());
        Hash::HashCombine(Seed, 0xC0C0C0C0ull);   // disambiguate from graphics keys

        auto It = PipelineCache.find(Seed);
        if (It != PipelineCache.end())
        {
            return It->second;
        }

        RHI::FPipelineH Pipeline = RHI::CreateComputePipeline(CS->Source());
        PipelineCache.emplace(Seed, Pipeline);
        return Pipeline;
    }

    RHI::FDepthStencilH FForwardRenderScene::GetOrCreateDepthState(const RHI::FDepthStencilDesc& Desc)
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

        auto It = DepthStateCache.find(Seed);
        if (It != DepthStateCache.end())
        {
            return It->second;
        }

        RHI::FDepthStencilH State = RHI::CreateDepthStencil(Desc);
        DepthStateCache.emplace(Seed, State);
        return State;
    }

    void FForwardRenderScene::SetViewportScissor(RHI::FCmdListH CL, const FUIntVector2& Extent)
    {
        const RHI::FRect Rect{ 0, (int)Extent.x, 0, (int)Extent.y };
        RHI::CmdSetViewport(CL, Rect);
        RHI::CmdSetScissor(CL, Rect);
    }

    void FForwardRenderScene::WriteBuffer(RHI::FCmdListH CL, RHI::GPUPtr Dst, const void* Data, uint64 Size)
    {
        RHI::FTransientAlloc Staging = RHI::Core::AllocTransient(Size);
        Memory::Memcpy(Staging.Cpu, Data, Size);
        RHI::CmdMemcpy(CL, Dst, Staging.Gpu, Size);
    }

    void FForwardRenderScene::ResizeBufferIfNeeded(FSceneBuffer& Buffer, uint64 NeededSize, float SlackFactor, uint32& LowUsageCounter,
                                                   bool bAllowShrink)
    {
        NeededSize = Math::Max<uint64>(NeededSize, 16ull);
        
        auto AlignUp16 = [](uint64 Size) { return (Size + 15ull) & ~15ull; };

        if (NeededSize > Buffer.Size)
        {
            if (Buffer)
            {
                DeferFree(Buffer.Ptr);
            }
            Buffer = CreateSceneBuffer(AlignUp16((uint64)((double)NeededSize * SlackFactor)));
            LowUsageCounter = 0;

            // Buffer.Size stays 0 on failure so the next frame retries rather than trusting a
            // capacity that was never backed. Passes read GetSize() and clamp accordingly.
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
                DeferFree(Buffer.Ptr);
                Buffer = CreateSceneBuffer(AlignUp16((uint64)((double)NeededSize * SlackFactor)));
                LowUsageCounter = 0;
            }
        }
        else
        {
            LowUsageCounter = 0;
        }
    }

    void FForwardRenderScene::DeferFree(RHI::GPUPtr Ptr)
    {
        if (Ptr != 0)
        {
            DeferredBufferFrees[CurrentFrameSlot].push_back(Ptr);
        }
    }

    void FForwardRenderScene::DeferRelease(FSceneImage& Image)
    {
        if (Image.IsValid())
        {
            DeferredImageReleases[CurrentFrameSlot].push_back(Image);
        }
        Image = {};
    }

    //~ End new-RHI helpers

    uint32 FForwardRenderScene::GetDisplayResourceID() const
    {
        if (SceneViews.empty())
        {
            return ~0u;
        }
        const int32 ID = SceneViews[0].Output.GetResourceID();
        return ID < 0 ? ~0u : (uint32)ID;
    }

    FUIntVector2 FForwardRenderScene::GetRenderExtent() const
    {
        return SceneViews.empty() ? FUIntVector2(0) : SceneViews[0].Size;
    }

    entt::entity FForwardRenderScene::GetEntityAtPixel(uint32 X, uint32 Y) const
    {
    #if USING(WITH_EDITOR)
        // Pick the newest complete slot whose window covers (X,Y). A slot is safe to map
        // without a semaphore once it's >= RHI::kFramesInFlight older than the latest issue.
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
            // No completed slot covers this pixel (startup/resize, early click, or fast
            // cursor motion); caller treats this as "no entity under cursor".
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
    void FForwardRenderScene::SetPickerCursor(uint32 X, uint32 Y, bool bOverViewport)
    {
        const uint64 Packed = (bOverViewport ? 1ull : 0ull)
                            | ((uint64(X) & 0x1FFFFF) << 1)
                            | ((uint64(Y) & 0x1FFFFF) << 22);
        PickerCursorPacked.store(Packed, std::memory_order_relaxed);
    }

    void FForwardRenderScene::IssuePickerReadback(RHI::FCmdListH CL)
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

        // Copy a small window around the cursor, not the whole RT; clamp it inside the
        // image so the region size is fixed and the cursor pixel stays inside.
        const uint32 RegionW = Math::Min(PickerRegionExtent, ImgW);
        const uint32 RegionH = Math::Min(PickerRegionExtent, ImgH);
        const uint32 OriginX = Math::Min(CursorX - Math::Min(CursorX, RegionW / 2), ImgW - RegionW);
        const uint32 OriginY = Math::Min(CursorY - Math::Min(CursorY, RegionH / 2), ImgH - RegionH);

        FPickerReadbackSlot& Slot = PickerReadbackRing[PickerReadbackWriteIndex];

        if (Slot.Readback == 0 || Slot.Width != RegionW || Slot.Height != RegionH)
        {
            // First use of this slot, or region size changed (post-resize). Allocate a host-readable
            // buffer sized to the region; bPending stays false until the copy below.
            if (Slot.Readback != 0)
            {
                DeferFree(Slot.Readback);
            }
            Slot.Readback = RHI::Malloc((uint64)RegionW * RegionH * sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
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
