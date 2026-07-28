#pragma once
#include <condition_variable>
#include "Core/Delegates/Delegate.h"
#include "Memory/Allocators/Allocator.h"
#include "Memory/SmartPtr.h"
#include "Renderer/RHI.h"
#include "Renderer/RHICore.h"
#include "Core/Threading/Thread.h"
#include "Renderer/Vertex.h"
#include "TaskSystem/TaskGraph.h"
#include "World/Entity/Components/LineBatcherComponent.h"
#include "World/Scene/RenderScene/EnvironmentRenderTypes.h"
#include "World/Scene/RenderScene/MeshDrawCommand.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/SceneCullContext.h"
#include "World/Scene/RenderScene/TerrainRenderTypes.h"
#include "World/Scene/RenderScene/TexturePaintTypes.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "TaskSystem/FiberSync.h"
#include "World/Entity/Components/PostProcessSettings.h"
#include "World/Subsystems/WorldSettings.h"


namespace Lumina
{
    class CMesh;
    struct FLineBatcherComponent;
    struct FTriangleBatcherComponent;
    struct SDirectionalLightComponent;
    struct SSpotLightComponent;
    struct SPointLightComponent;
    struct SExponentialHeightFogComponent;
    class CWorld;
    struct SStaticMeshComponent;
    struct SDynamicMeshComponent;
    struct SFoliageComponent;
    struct SFoliageType;
    struct FFoliageBakedInstance;
    struct SSkeletalMeshComponent;
    struct STransformComponent;
    struct STerrainComponent;
    struct SDecalComponent;
    class CMaterialInterface;
    class CMaterial;

    /** Scene rendering via Clustered Forward Rendering. */
    class FForwardRenderScene : public IRenderScene
    {
    public:

        FForwardRenderScene(CWorld* InWorld);
        // Releases every GPU resource the scene owns (drains the device first). RAII: destroying the
        // scene IS the teardown -- there is no separate Shutdown() to forget to call.
        ~FForwardRenderScene() override;
        LE_NO_COPYMOVE(FForwardRenderScene);

        // Per-entity shared data. FProcessedDrawItem carries an EntityRecordIndex
        // into this table so Transform/Bounds aren't duplicated per surface.
        struct FEntityRecord
        {
            FMatrix4               Transform;
            FVector4               SphereBounds;
            uint64                  MeshletHeaderAddress;
            uint32                  CustomData;
            uint32                  EntityID;
            uint32                  LocalBoneOffset;            // ~0u for static meshes.
            // GPU pre-skinning: only the rendered-LOD meshlet span is skinned (0 for static).
            // SpanStart = first rendered vertex, SliceSize = extent; GlobalSkinnedBase resolved in merge.
            uint32                  SkinMeshletStart;
            uint32                  SkinMeshletCount;
            uint32                  SkinSpanStart;
            uint32                  SkinSliceSize;
            uint32                  GlobalSkinnedBase;
        };
        
        struct FProcessedDrawItem
        {
            uint32              EntityRecordIndex;
            uint32              SurfaceMeshletOffset;
            uint32              SurfaceMeshletCount;
            uint32              ShadowMeshletOffset;
            uint32              ShadowMeshletCount;
            EInstanceFlags      Flags;
            uint16              MaterialIndex;
            uint16              LocalBatchIndex;
            uint16              LocalDrawIndex;
            uint16              _Pad;
        };

        // Copied out of the shared FResolvedSurface when a local batch is first created.
        struct FCachedMaterialResolve
        {
            const FShaderEntry* VertexShader;
            const FShaderEntry* PixelShader;
            const FShaderEntry* MeshShader;
            const FShaderEntry* VisBufferMeshShader;
            const FShaderEntry* VisBufferVertexShader;
            const FShaderEntry* MaskedVisBufferPixelShader;
            const FShaderEntry* MaskedVisBufferPixelShaderPrim;
            const FShaderEntry* DeferredShader;
            uint64              MaterialID;
            uint16              MaterialIdx;
            bool                bTranslucent;
            bool                bMasked;
            bool                bAdditive;
            bool                bTwoSided;
            bool                bMaterialCastsShadows;
        };

        // One distinct GPU material slot a worker emitted; merged into Geometry::DeferredMaterials.
        struct FLocalDeferredMaterial
        {
            uint16              MaterialIndex;
            const FShaderEntry* DeferredShader;
        };

        struct CACHE_ALIGN FLocalBatchEntry
        {
            FDrawBatchKey                       Key;
            FCachedMaterialResolve              Resolve = {};
            TFrameVector<FDrawKey>              LocalDraws;
            TFrameVector<uint32>                LocalDrawCounts;
            TFrameVector<uint32>                LocalMeshletCounts;
            TFrameHashMap<FDrawKey, uint16>     DrawIndexByKey;
            uint32                              GlobalBatchIndex = ~0u;
            TFrameVector<uint32>                LocalToGlobalDraw;
            TFrameVector<uint32>                LocalDrawWriteBase;

            // Homogeneity of this batch's instances; merged into FMeshDrawCommand to pick the SPEC_SKINNED variant.
            bool                                bAnySkinned = false;
            bool                                bAnyStatic  = false;
            void AccumSkinned(EInstanceFlags F)
            {
                if (((uint32)F & (uint32)EInstanceFlags::Skinned) != 0u) { bAnySkinned = true; }
                else                                                     { bAnyStatic  = true; }
            }

            FLocalBatchEntry() = default;
            explicit FLocalBatchEntry(FFrameArenaAllocator A)
                : LocalDraws(A), LocalDrawCounts(A), LocalMeshletCounts(A)
                , DrawIndexByKey(A), LocalToGlobalDraw(A), LocalDrawWriteBase(A) {}
        };

        // Per-thread bone stream stored as fixed-size pages so no single arena allocation can exceed
        // the frame-arena block: a flat vector's capacity doubling overran the block (silent heap
        // corruption) once one gather thread accumulated a few MB of bones from a large visible
        // crowd. Entity bone offsets index the logical concatenation; MergeMeshDrawData flattens
        // the pages in order.
        struct FBonePageArray
        {
            static constexpr uint32 kPageBones = 16u * 1024u; // 768 KB/page, well under the arena block

            TFrameVector<TFrameVector<FBoneTransform>> Pages;
            uint32 Count = 0;
            FFrameArenaAllocator Allocator;

            FBonePageArray() = default;
            explicit FBonePageArray(FFrameArenaAllocator A)
                : Pages(A), Allocator(A) {}

            uint32 Size() const { return Count; }
            bool IsEmpty() const { return Count == 0; }

            void Append(const FBoneTransform* Src, uint32 Num)
            {
                while (Num > 0)
                {
                    TFrameVector<FBoneTransform>& Page = EnsureSpace();
                    const uint32 Take = Math::Min(Num, kPageBones - (uint32)Page.size());
                    Page.insert(Page.end(), Src, Src + Take);
                    Src   += Take;
                    Num   -= Take;
                    Count += Take;
                }
            }

            void AppendIdentity(uint32 Num)
            {
                static constexpr FBoneTransform IdentityBone{ FVector4(1,0,0,0), FVector4(0,1,0,0), FVector4(0,0,1,0) };
                while (Num > 0)
                {
                    TFrameVector<FBoneTransform>& Page = EnsureSpace();
                    const uint32 Take = Math::Min(Num, kPageBones - (uint32)Page.size());
                    Page.resize(Page.size() + Take, IdentityBone);
                    Num   -= Take;
                    Count += Take;
                }
            }

        private:

            TFrameVector<FBoneTransform>& EnsureSpace()
            {
                if (Pages.empty() || (uint32)Pages.back().size() == kPageBones)
                {
                    TFrameVector<FBoneTransform>& Page = Pages.emplace_back(TFrameVector<FBoneTransform>(Allocator));
                    Page.reserve(kPageBones);
                    return Page;
                }
                return Pages.back();
            }
        };

        struct alignas(64) FThreadLocalDrawData
        {
            TFrameVector<FProcessedDrawItem>    Items;
            TFrameVector<FEntityRecord>         EntityRecords;
            TFrameVector<FLocalBatchEntry>      LocalBatches;
            FBonePageArray                      BonesData;
            TFrameVector<FLocalDeferredMaterial> DeferredMaterials;
            // Batch key hash -> LocalBatches index, so batch lookup stays O(1) as material count grows.
            TFrameHashMap<uint64, uint16>       BatchIndexByKey;
            FFrameArenaAllocator                Arena;
            FSceneRenderStats                   Stats = {};

            // Surfaces of one mesh usually share a material; check the last hit before the map.
            uint16                              LastBatchIndex = 0xFFFFu;
            uint64                              LastBatchHash  = 0u;

            FThreadLocalDrawData() = default;
            explicit FThreadLocalDrawData(FFrameArenaAllocator A)
                : Items(A), EntityRecords(A), LocalBatches(A), BonesData(A)
                , DeferredMaterials(A), BatchIndexByKey(A), Arena(A) {}

            FThreadLocalDrawData(FThreadLocalDrawData&&) = default;
            FThreadLocalDrawData& operator=(FThreadLocalDrawData&&) = default;
            
            void ResetForFrame(FFrameArenaAllocator A)
            {
                new (&Items)         TFrameVector<FProcessedDrawItem>(A);
                new (&EntityRecords) TFrameVector<FEntityRecord>(A);
                new (&LocalBatches)  TFrameVector<FLocalBatchEntry>(A);
                new (&BonesData)     FBonePageArray(A);
                new (&DeferredMaterials) TFrameVector<FLocalDeferredMaterial>(A);
                new (&BatchIndexByKey)   TFrameHashMap<uint64, uint16>(A);
                Arena = A;
                Stats = {};
                LastBatchIndex = 0xFFFFu;
                LastBatchHash  = 0u;
            }
            
            ~FThreadLocalDrawData()
            {
                ResetForFrame(FFrameArenaAllocator());
            }
        };

        // Shadow tile request captured during parallel light processing;
        // resolved + shrunk-to-fit by AllocateShadowTiles.
        struct FShadowRequest
        {
            uint32      LightIndex;
            ELightType  Type;
            uint32      DesiredPixels;
            float       DistanceToCamera;
            FVector3    Position;
            FVector3    Direction;      // Spot only
            FVector3    Up;             // Spot only
            float       Attenuation;
            float       OuterFOVDegrees;
        };

        // Per-frame extracted snapshot. RHI::kFramesInFlight instances in FrameRing;
        // game thread writes one in Extract, render thread reads it in RenderView.
        struct FFrameData
        {
            struct FDecalBatch
            {
                // Ref-held shaders (not the live CMaterial*) so a deleted decal asset can't dangle
                // the render thread; the refcount keeps the bytecode alive past the material's death.
                FRenderMaterialShaders Shaders;
                uint32      FirstInstance;
                uint32      Count;
            };

            // A run of glyph instances sharing one font atlas; one instanced draw per batch.
            struct FTextBatch
            {
                uint32      AtlasIndex   = 0;   // global-heap ResourceID of the font atlas
                uint32      AtlasWidth   = 0;
                uint32      AtlasHeight  = 0;
                float       DistanceRange = 0.0f; // px range baked into the MSDF (drives shader AA)
                uint32      FirstInstance = 0;
                uint32      Count         = 0;
                bool        bDepthTest    = false; // occluded by + writes scene depth, vs. always-on-top
            };

            // Per-frame snapshot of one terrain; render passes read ONLY this. GPU resources live in
            // TerrainGPUStates keyed by Entity, so the render thread never dereferences the component.
            struct FTerrainExtract
            {
                entt::entity        Entity;
                FMatrix4            WorldMatrix;

                int32               Resolution      = 0;
                int32               ChunkResolution = 0;
                float               TileWorldSize   = 0.0f;
                float               MaxHeight       = 0.0f;
                int32               LayerCount       = 0;
                FRenderMaterialShaders Shaders;
                uint32              MaterialIndex   = 0;
                bool                bCastShadow     = true;
                bool                bReceiveShadow  = true;

                // Dimensions changed this frame -> render thread (re)creates GPU textures
                // and the Full upload payloads below re-seed every slice.
                bool                bStructuralChange = false;

                // Height upload: 0 none, 1 full map, 2 packed dirty rect.
                uint8               HeightUpload    = 0;
                FIntVector2         HeightRectMin   = FIntVector2(0);
                FIntVector2         HeightRectMax   = FIntVector2(0);
                TVector<float>      HeightBytes;     // full map, or tightly-packed rect rows

                // Weight upload: 0 none, 1 all slices, 2 selected dirty slices.
                uint8               WeightUpload    = 0;
                uint32              WeightSliceMask = 0u;         // bit L set => slice L present
                TVector<uint8>      WeightBytes;     // dirty slices packed back-to-back

                // Chunk/meshlet metadata rebuilt this frame; copied so next frame's rebuild can't race the upload.
                bool                         bGeometryRebuilt = false;
                TVector<FTerrainChunkInfo>   Chunks;
                TVector<FTerrainMeshletInfo> Meshlets;
            };

            // Per-frame snapshot of one emitter; render passes read ONLY this. GPU + sim state lives in
            // ParticleGPUStates keyed by Entity, so the render thread never dereferences the component.
            struct FParticleExtract
            {
                entt::entity            Entity;
                FMatrix4                WorldMatrix;

                FVector3                EmitterOffset       = FVector3(0.0f);
                float                   TimeScale           = 1.0f;
                float                   SpawnRateMultiplier = 1.0f;
                bool                    bEmit               = true;
                bool                    bBurstOnSpawn       = true;

                // Game-thread Activate()/Deactivate() intents, applied once then cleared.
                bool                    bForceBurst         = false;
                bool                    bForceReset         = false;

                // Asset+override params resolved on the game thread.
                FResolvedParticleParams Resolved;

                bool                    bReady              = false;  // asset ready to simulate
                bool                    bUsesCustomShader   = false;
                const FShaderEntry*     CustomComputeShader = nullptr;  // set iff bUsesCustomShader
                uint32                  TextureIndex        = 0u;     // heap ResourceID, resolved game-side
            };

            // Per-frame snapshot of enabled capture views, in registration order. The shared cull fills
            // each view's slice; RenderView shades each into SceneViews[SceneViewIndex] after the primary.
            struct FCaptureViewData
            {
                FSceneGlobalData    SceneGlobalData = {};
                FViewVolume         ViewVolume      = {};
                uint32              CameraViewIndex = ~0u;   // its cull-view index (frustum-only)
                int32               SceneViewIndex  = -1;    // index into FForwardRenderScene::SceneViews
            };

            FViewVolume                      ViewVolume = {};
            // Camera frustum snapshot (CPU form of CullData.Frustum) so per-light / per-task
            // consumers don't each rebuild it from the GPU representation.
            FFrustum                         CameraFrustum = {};
            FSceneGlobalData                 SceneGlobalData = {};
            SDefaultWorldSettings            CachedWorldSettings = {};
            float                            CachedWorldDeltaTime = 0.0f;
            bool                             bExtractedThisFrame = false;
            FSceneRenderStats                FrameStats = {};

            // One distinct opaque GPU material slot (a material instance) + the master DeferredShader that
            // shades it. The deferred pass groups these by DeferredShader so instances of one master share a
            // single shading draw (they batch in geometry too), while each keeps its own MaterialIndex/uniforms.
            struct FDeferredMaterialEntry
            {
                uint32              MaterialIndex;
                const FShaderEntry* DeferredShader;
            };

            struct FGeometry
            {
                TVector<FGPUInstance>            Instances;
                TVector<FBoneTransform>          BonesData;   // 48B/bone (last row dropped)
                TVector<FSkinDescriptor>         SkinDescriptors;
                uint32                           TotalPreSkinnedVertices = 0;
                TVector<FMeshDrawCommand>        DrawCommands;
                TVector<uint32>                  OpaqueDrawList;
                TVector<uint32>                  TranslucentDrawList;
                TVector<FDeferredMaterialEntry>  DeferredMaterials;   // distinct opaque slots for the deferred pass
                FSceneCullContext                SceneCullContext;
                TVector<uint32>                  DrawMeshletStartOffsets;
                // Per-instance meshlet prefix is GPU-built (BuildInstancePrefix.slang) into
                // InstancePrefixRing; no CPU-side array exists anymore.
            } Geometry;

            struct FViews
            {
                // Indirect-arg + mesh-task-arg seeds are GPU-generated (SeedIndirectArgs.slang) from
                // Geometry.DrawMeshletStartOffsets; no CPU-side V*D arrays exist anymore.
                TVector<FCullView>               CullViews;
                uint32                           TotalMeshletBound   = 0;
                uint32                           NumDrawsPerView     = 0;
                uint32                           CameraLateViewIndex = ~0u;
                uint32                           CascadeViewBase     = ~0u;
                TVector<uint32>                  PointShadowCullViewBases;
                TVector<uint32>                  SpotShadowCullViewBases;
                TVector<FCaptureViewData>        CaptureViews;
            } Views;

            struct FLighting
            {
                FSceneLightData                  LightData = {};
                TArray<TVector<FLightShadow>, (uint32)ELightType::Num> PackedShadows = {};
                TAtomic<uint32>                  ShadowDataCount = 0;
                TVector<FShadowRequest>          ShadowRequests;
                FMutex                           ShadowRequestMutex;
                TVector<FShadowTile>             AtlasTiles;
            } Lighting;

            struct FPrimitives
            {
                TVector<FSimpleElementVertex>    SimpleVertices;
                TVector<FLineBatch>              LineBatches;
                TVector<FSimpleElementVertex>    SolidVertices;
                TVector<FSolidBatch>             SolidBatches;
                TVector<FBillboardInstance>      BillboardInstances;
                TVector<FGPUDecal>               DecalExtracts;
                TVector<FDecalBatch>             DecalBatches;
                TVector<FWidgetInstance>         WidgetInstances;
                TVector<FGPUGlyph>               GlyphInstances;
                TVector<FTextBatch>              TextBatches;

                TVector<FGPUGlyph>               DebugTextGlyphs;
                FTextBatch                       DebugTextBatch;
            } Primitives;

            struct FVolumetrics
            {
                FEnvironmentParams               EnvironmentParams = {};
                // HDRI env map (heap ResourceID + width for the equirect LOD); -1 = none.
                int32                            EnvironmentMapID    = -1;
                uint32                           EnvironmentMapWidth = 0;
                FExponentialHeightFogParams      FogParams           = {};
                bool                             bHasFog             = false;
                bool                             bVolumetricFog      = false;
                bool                             bIBLDirty            = false;
                bool                             bIBLConvolutionDirty = false;
                FIBLBakeResolution               IBLResolution        = {};
            } Volumetrics;

            // Post-process material resolved + ref-held on the game thread; the render thread reads
            // these instead of dereferencing a (possibly deleted) CMaterial.
            struct FPostProcessMaterial
            {
                FRenderMaterialShaders Shaders;
                uint32                 MaterialIndex = 0;
            };

            struct FPostProcess
            {
                SPostProcessSettings             ActivePostProcessStorage = {};
                bool                             bHasActivePostProcess = false;
                TVector<FPostProcessMaterial>    ActivePostProcessMaterials;
            } PostProcess;

            struct FExtracts
            {
                TVector<FTerrainExtract>         TerrainExtracts;
                TVector<entt::entity>            LiveTerrainEntities;
                TVector<FParticleExtract>        ParticleExtracts;
                TVector<entt::entity>            LiveParticleEntities;
                TVector<FTexturePaintOp>         PaintOps;
            } Extracts;

            struct FWater
            {
                TVector<FGPUWater>               Surfaces;
                bool                             bUnderwaterActive = false;
                FWaterUnderwaterParams           Underwater = {};
            } Water;
        };

        enum class ENamedImage : uint8
        {
            HDR,
            LDR,
            PostProcessScratch,
            SMAAEdges,
            SMAABlend,
            SMAAArea,
            SMAASearch,
            SSAO,
            SSAODenoise,
            SSAOBlur,
            Cascade,
            DepthAttachment,
            DepthPyramid,
            Picker,
            VisBuffer,
            Accum,
            Revealage,
            WaterRefraction,
            DBufferA,
            DBufferB,
            DBufferC,
            AdaptedLuminance,
            FroxelScatter,
            FroxelIntegrated,
            HDR_MS,
            Depth_MS,
            Picker_MS,
            BRDFLut,
            SkyCube,
            SkyIrradiance,
            SkyPrefilter,

            #if USING(WITH_EDITOR)
            PointLightIcon,
            DirectionalLightIcon,
            SkyLightIcon,
            SpotLightIcon,
            CameraIcon,
            CharacterIcon,
            ParticleSystemIcon,
            #endif

            Num,
        };

        // Per-output-view rendering state.
        struct FSceneView
        {
            FSceneImage                                     Output;
            FUIntVector2                                    Size = FUIntVector2(0);
            bool                                            bIsPrimary = false;
            FViewVolume                                     PendingViewVolume;
            bool                                            bEnabled = false;
            TArray<FSceneImage, (int)ENamedImage::Num>      Images = {};
            FSceneImage                                     BloomChainImage;
            FSceneBuffer                                    ClusterBuffer;
            FMatrix4                                        LastClusterInvProjection = FMatrix4(0.0f);
            FVector2                                        LastClusterNearFar       = FVector2(0.0f);
            FUIntVector2                                    LastClusterScreenSize    = FUIntVector2(0);
            bool                                            bClusterGridDirty        = true;
        };

        void Init() override;

        void Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess) override;
        void PrepareRender(uint8 FrameIndex) override;
        void RenderView(uint8 FrameIndex) override;
        void SignalFrameConsumed(uint8 FrameIndex) override;
        void SetActivePostProcessMaterials(const TVector<CMaterialInterface*>& Materials) override { PendingPostProcessMaterials = Materials; }
        void SwapchainResized(FVector2 NewSize);
        void Resize(const FUIntVector2& NewSize) override { SwapchainResized(FVector2(NewSize)); }

        int32 RegisterCaptureView(const FUIntVector2& Size) override;
        bool  SetCaptureView(int32 Handle, const FViewVolume& View, bool bEnabled) override;
        int32 GetCaptureDisplayResourceID(int32 Handle) const override;

        void DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale) override;
        void DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness, bool bDepthTest, float Duration) override { }

        FSceneBuffer GetPreSkinnedVerticesBuffer() const { return PreSkinnedVerticesBuffer; }
        const FSceneImage& GetNamedImage(ENamedImage Image) const { return CurrentView ? CurrentView->Images[(int)Image] : NamedImages[(int)Image]; }

        // Ringed accessors for the cull-pass scratch (see IndirectArgsRing).
        FSceneBuffer GetIndirectArgs()     const { return IndirectArgsRing[CurrentFrameSlot]; }
        FSceneBuffer GetMeshletDrawList()  const { return MeshletDrawListRing[CurrentFrameSlot]; }
        FSceneBuffer GetMeshDrawArgs()     const { return MeshDrawArgsRing[CurrentFrameSlot]; }
        FSceneBuffer GetMeshletDeferList() const { return MeshletDeferListRing[CurrentFrameSlot]; }
        FSceneBuffer GetDeferCount()       const { return DeferCountRing[CurrentFrameSlot]; }
        FSceneBuffer GetCullDispatchArgs() const { return CullDispatchArgsRing[CurrentFrameSlot]; }
        FSceneBuffer GetSpdCounter()       const { return SpdCounterRing[CurrentFrameSlot]; }
        FSceneBuffer GetInstancePrefix()   const { return InstancePrefixRing[CurrentFrameSlot]; }

        // Deferred material-binning scratch (device-address only): per-tile material bitmask + per-pixel
        // owning MaterialIndex, both produced by ClassifyMaterialTiles and consumed by the deferred pass.
        FSceneBuffer GetMaterialBinTileBits() const { return MaterialBinTileBitsRing[CurrentFrameSlot]; }
        FSceneBuffer GetMaterialBinPixelId()  const { return MaterialBinPixelIdRing[CurrentFrameSlot]; }

        // MSAA scratch RT when enabled, else the 1x image; use for the render-target
        // binding on geometry passes that participate in MSAA. 1x image is the resolve target.
        const FSceneImage& GetSceneColorRT() const { return MSAASampleCount > 1 ? GetNamedImage(ENamedImage::HDR_MS) : GetNamedImage(ENamedImage::HDR); }
        const FSceneImage& GetSceneDepthRT() const { return MSAASampleCount > 1 ? GetNamedImage(ENamedImage::Depth_MS) : GetNamedImage(ENamedImage::DepthAttachment); }
        const FSceneImage& GetPickerRT()     const { return MSAASampleCount > 1 ? GetNamedImage(ENamedImage::Picker_MS) : GetNamedImage(ENamedImage::Picker); }

        /** Resolve target, invalid handle when MSAA off (no resolve needed). */
        RHI::FTextureH GetSceneColorResolve() const { return MSAASampleCount > 1 ? GetNamedImage(ENamedImage::HDR).Texture : RHI::FTextureH{}; }
        RHI::FTextureH GetSceneDepthResolve() const { return MSAASampleCount > 1 ? GetNamedImage(ENamedImage::DepthAttachment).Texture : RHI::FTextureH{}; }
        RHI::FTextureH GetPickerResolve()     const { return MSAASampleCount > 1 ? GetNamedImage(ENamedImage::Picker).Texture : RHI::FTextureH{}; }

        uint8 GetMSAASampleCount() const { return MSAASampleCount; }

        uint32 GetDisplayResourceID() const override;
        RHI::FTextureH GetDisplayTexture() const override { return SceneViews[0].Output.Texture; }
        const FSceneImage& GetDisplayImage() const { return SceneViews[0].Output; }
        const FSceneImage& GetPrimaryNamedImage(ENamedImage Image) const { return SceneViews[0].Images[(int)Image]; }
        FUIntVector2 GetRenderExtent() const override;
        entt::entity GetEntityAtPixel(uint32 X, uint32 Y) const override;
        #if USING(WITH_EDITOR)
        void SetPickerCursor(uint32 X, uint32 Y, bool bOverViewport) override;
        #endif
        const FShadowAtlas* GetShadowAtlas() const override { return &ShadowAtlas; }


    private:
        
        void InitBuffers();
        void InitViewImages(FSceneView& View);
        void ReleaseViewImages(FSceneView& View);
        void InitFrameResources();

        FSceneView& AddSceneView(const FUIntVector2& Size, bool bPrimary);
        
        void RenderCaptureView(RHI::FCmdListH CL);

        void PointAtView(FSceneView& View)
        {
            CurrentView = &View;
        }
        
        void InitSharedResources();

        void BakeBRDFLUT();

        void InitSkyCube(uint32 FaceSize);

        void InitIBLConvolutionTargets(const FIBLBakeResolution& Resolution);
        
        void SyncIBLResolution(const FIBLBakeResolution& Resolution);

        //~ Begin Render Passes
        void ResetPass_GameThread();
        void ResetPass_RenderThread(RHI::FCmdListH CL);
        void CullPassEarly(RHI::FCmdListH CL);
        void CullPassLate(RHI::FCmdListH CL);
        void SkinningPass(RHI::FCmdListH CL);
        void TexturePaintPass(RHI::FCmdListH CL);
        void DepthPyramidPass(RHI::FCmdListH CL);
        void ClusterBuildPass(RHI::FCmdListH CL);
        void LightCullPass(RHI::FCmdListH CL);
        void PointShadowPass(RHI::FCmdListH CL);
        void SpotShadowPass(RHI::FCmdListH CL);
        void CascadedShowPass(RHI::FCmdListH CL);
        void DecalPass(RHI::FCmdListH CL);
        void VisBufferPass(RHI::FCmdListH CL, uint32 ViewIndex, bool bClear);
        void DeferredMaterialPass(RHI::FCmdListH CL);
        bool BindShadowBatchPipeline(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, const FShaderEntry* PixelShader);
        void DrawShadowBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, bool bUseMesh,
                             uint32 CullViewIndex, int32 ShadowDataIndex, int32 ShadowViewIndex);
        void BillboardPass(RHI::FCmdListH CL);
        void WidgetPass(RHI::FCmdListH CL);
        void TextPass(RHI::FCmdListH CL);
        void DebugTextPass(RHI::FCmdListH CL);
        void WidgetPickerPass(RHI::FCmdListH CL);
        void ParticleSimulatePass(RHI::FCmdListH CL);
        void ParticleRenderPass(RHI::FCmdListH CL);
        void TerrainUpdatePass(RHI::FCmdListH CL);
        void TerrainCullPass(RHI::FCmdListH CL);
        void TerrainDepthPrePass(RHI::FCmdListH CL);
        void TerrainRenderPass(RHI::FCmdListH CL);
        void SSAOPass(RHI::FCmdListH CL);
        void SSAOBlurPass(RHI::FCmdListH CL);
        void TransparentPass(RHI::FCmdListH CL);
        void OITResolvePass(RHI::FCmdListH CL);
        void AdditiveTranslucentPass(RHI::FCmdListH CL);
        void FroxelInjectPass(RHI::FCmdListH CL);
        void FroxelIntegratePass(RHI::FCmdListH CL);
        void FroxelApplyPass(RHI::FCmdListH CL);
        void WaterPass(RHI::FCmdListH CL);
        void UnderwaterPass(RHI::FCmdListH CL);
        void EnvironmentPass(RHI::FCmdListH CL);
        void SkyCubeCapturePass(RHI::FCmdListH CL);
        void IrradianceConvolutionPass(RHI::FCmdListH CL);
        void PrefilterEnvMapPass(RHI::FCmdListH CL);
        void BatchedLineDraw(RHI::FCmdListH CL);
        void BatchedTriangleDraw(RHI::FCmdListH CL);
        void BloomPass(RHI::FCmdListH CL);
        void AutoExposurePass(RHI::FCmdListH CL);
        void ToneMappingPass(RHI::FCmdListH CL);
        void PostProcessMaterialPass(RHI::FCmdListH CL);
        void SMAAEdgeDetectionPass(RHI::FCmdListH CL);
        void SMAABlendWeightPass(RHI::FCmdListH CL);
        void SMAANeighborhoodBlendPass(RHI::FCmdListH CL);
        //~ End Render Passes

        // Game-thread half: ECS reads + parallel Process* tasks + cull/shadow setup.
        void CompileDrawCommands_GameThread();

        // Render-thread half: buffer resize + upload commands; reads game-thread state.
        void CompileDrawCommands_RenderThread(RHI::FCmdListH CL);

        // Serial pre-pass so the parallel gather is pure reads; skipped when nothing changed.
        void ResolveDirtyMeshComponents();

        // Reused to flatten a component's material overrides; keeps capacity.
        TVector<CMaterialInterface*> ResolveOverrideScratch;

        void ProcessStaticMeshEntityInternal(entt::entity Entity, const SStaticMeshComponent& MeshComponent, const STransformComponent& TransformComponent, FThreadLocalDrawData& Local);
        void ProcessDynamicMeshEntityInternal(entt::entity Entity, const SDynamicMeshComponent& MeshComponent, const STransformComponent& TransformComponent, FThreadLocalDrawData& Local);
        void ProcessFoliageBakedInstance(const SFoliageType& Type, const FFoliageBakedInstance& Baked, uint32 OwnerEntityID, FThreadLocalDrawData& Local);
        void ProcessSkeletalMeshEntityInternal(entt::entity Entity, SSkeletalMeshComponent& MeshComponent, const STransformComponent& TransformComponent, FThreadLocalDrawData& Local);
        
        void BuildSceneCullContext();
        void MergeMeshDrawData(TVector<FThreadLocalDrawData>& ThreadLocal);

        // Bind this worker's draw-data slot to its thread frame arena on the first touch of a gather pass,
        // then accumulate. Must be called from inside a parallel-for body (Slot == Range.Thread); the arena
        // is thread-local, so Slot's OS thread and the arena it allocates from are one and the same.
        FThreadLocalDrawData& AcquireThreadLocalDrawData(uint32 Slot);

        void ProcessPointLight(const SPointLightComponent& PointLight, const STransformComponent& TransformComponent, TAtomic<uint32>& LightCount);
        void ProcessSpotLight(const SSpotLightComponent& SpotLight, const STransformComponent& TransformComponent, TAtomic<uint32>& LightCount);
        void ProcessDirectionalLight(const SDirectionalLightComponent& DirectionalLight, TAtomic<uint32>& LightCount);

        void AllocateShadowTiles();
        
        void BuildCullViews(const FViewVolume& ViewVolume);
        
        uint32 PrepareBatchedLines(FLineBatcherComponent& Batcher);
        void   BatchLineChunks(const Task::FParallelRange& Range);
        void   FinalizeBatchedLines(FLineBatcherComponent& Batcher);
        void   ProcessBatchedTriangles(FTriangleBatcherComponent& Batcher);

        void NotifyMaxLightsHit();
        
        bool ShouldRequestShadow(const FVector3& LightPosition, float LightRadius) const;
        
        // Mesh vertex-emulation pass selector; drives the MeshletVertex.slang EPass spec constant (id 0).
        enum class EMeshPass : uint8 { Base = 0, Depth = 1, Shadow = 2 };

        // ShadeSurface feature gates, fed as spec constants ids 1-3 (see SurfaceShading.slang). Default =
        // all on, identical to the un-specialized shader; the terrain pass clears Decals|SSAO (it binds
        // neither the DBuffer overlay nor an SSAO input, so those blocks dead-strip).
        enum EShadingFeature : uint32
        {
            SF_DebugViews = 1u << 0,
            SF_Decals     = 1u << 1,
            SF_SSAO       = 1u << 2,
            SF_All        = SF_DebugViews | SF_Decals | SF_SSAO,
        };

        struct FGraphicsPipelineKey
        {
            const FShaderEntry* VS = nullptr;
            const FShaderEntry* PS = nullptr;    // null = depth-only
            const FShaderEntry* MS = nullptr;    // mesh shader; when set, a mesh pipeline is built (VS ignored)
            RHI::ETopology  Topology = RHI::ETopology::TriangleList;
            bool            bWireframe = false;
            bool            bAlphaToCoverage = false;
            uint8           SampleCount = 1;
            EFormat         DepthFormat = EFormat::UNKNOWN;
            EMeshPass       PassVariant = EMeshPass::Base;   // EPass spec constant for the merged VS
            uint32          ShadingFeatures = SF_All;        // ShadeSurface spec constants (ids 1-3)
            bool            bVisBufferMasked = false;        // VISBUFFER_MASKED spec constant (id 4): VisBuffer geometry emits interpolants
            uint8           SkinnedMode = 2;                 // SPEC_SKINNED spec constant (id 5): 0=static, 1=skinned, 2=dynamic (runtime branch)
            TFixedVector<RHI::FColorTarget, 4> ColorTargets;
        };

        RHI::FPipelineH      GetOrCreatePipeline(const FGraphicsPipelineKey& Key);
        RHI::FPipelineH      GetOrCreateComputePipeline(const FShaderEntry* CS);
        RHI::FDepthStencilH  GetOrCreateDepthState(const RHI::FDepthStencilDesc& Desc);

        // Engine-wide per-draw args.
        template<typename T>
        RHI::GPUPtr MakeArgs(const T& PassData)
        {
            DEBUG_ASSERT(CurrentSceneRootAddr != 0);   // null root = GPU page fault at first scene-buffer read
            return RHI::Core::CopyTransient(FRootConstants{ CurrentSceneRootAddr, RHI::Core::CopyTransient(PassData) });
        }
        
        RHI::GPUPtr MakeArgs()
        {
            DEBUG_ASSERT(CurrentSceneRootAddr != 0);
            return RHI::Core::CopyTransient(FRootConstants{ CurrentSceneRootAddr, 0 });
        }

        // Sets the full-extent viewport + scissor for the current render area.
        static void SetViewportScissor(RHI::FCmdListH CL, const FUIntVector2& Extent);

        static void WriteBuffer(RHI::FCmdListH CL, RHI::GPUPtr Dst, const void* Data, uint64 Size);

        // Grow/shrink-with-hysteresis for the persistent GPU buffers.
        void ResizeBufferIfNeeded(FSceneBuffer& Buffer, uint64 NeededSize, float SlackFactor, uint32& LowUsageCounter);

        // Freed when this slot's previous GPU work has completed.
        void DeferFree(RHI::GPUPtr Ptr);
        void DeferRelease(FSceneImage& Image);
    
    private:
        
        FSceneBuffer                                        PreSkinnedVerticesBuffer;
        uint32                                              PreSkinnedVerticesLowUsage = 0;
        TArray<uint32, RHI::kFramesInFlight>                IndirectArgsRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MeshletDrawListRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MeshDrawArgsRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MeshletDeferListRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                InstancePrefixRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MaterialBinTileBitsRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MaterialBinPixelIdRingLowUsage = {};
        TArray<FSceneImage, (int)ENamedImage::Num>          NamedImages = {};

        /** MSAA sample count cached from world settings. 1 == disabled (no overhead). */
        uint8                                           MSAASampleCount = 1;

        /** Allocate a view's MS-only scratch images (HDR_MS, Depth_MS, Picker_MS). No-op when MSAA is off. */
        void AllocateMSAAImages(FSceneView& View, const FUIntVector2& Extent);

        /** Reconcile cached sample count with the world setting; reallocates every view's MS images when it changes. */
        void SyncMSAAState();
        
        static constexpr uint32                 BLOOM_MIP_COUNT = 8;
        
        static constexpr uint32                 MaxSceneViews = 16;
        TVector<FSceneView>                     SceneViews;
        FSceneView*                             CurrentView = nullptr;
        
        uint32                                  CurrentCameraEarlyView = 0;
        uint32                                  CurrentCameraLateView  = ~0u;

        FDelegateHandle                         SwapchainResizedHandle;

        // Froxel volume dimensions; set from CRendererSettings::FroxelResolutionScale at image creation
        // and reused by the inject/integrate/apply dispatches so they always match the allocated textures.
        FUIntVector3                            FroxelGridSize = FUIntVector3(160, 90, 128);

        FEnvironmentParams                      LastIBLEnvironmentParams = {};
        int32                                   LastIBLEnvironmentMapID  = -1;
        FVector3                               LastIBLSunDirection      = FVector3(0.0f);
        bool                                    bLastIBLHasSun           = false;
        bool                                    bIBLValid                = false;

        FEnvironmentParams                      LastConvolvedEnvironmentParams = {};
        int32                                   LastConvolvedEnvironmentMapID  = -1;
        FVector3                               LastConvolvedSunDirection      = FVector3(0.0f);
        bool                                    bLastConvolvedHasSun           = false;
        bool                                    bIBLConvolutionValid           = false;

        FIBLBakeResolution                      AppliedIBLResolution           = {};
        FIBLBakeResolution                      LastExtractedIBLResolution     = {};
        
        FEnvironmentParams                      LastUploadedEnvironmentParams = {};
        bool                                    bEnvironmentParamsUploaded    = false;
        
        THashMap<uint64, RHI::FPipelineH>       PipelineCache;
        THashMap<uint64, RHI::FDepthStencilH>   DepthStateCache;

        TArray<TVector<RHI::GPUPtr>,  RHI::kFramesInFlight> DeferredBufferFrees;
        TArray<TVector<FSceneImage>,  RHI::kFramesInFlight> DeferredImageReleases;
        
        TVector<CMaterialInterface*>            PendingPostProcessMaterials;
        
        FSceneRoot                                                      SceneRootShared = {};
        uint64                                                          CurrentSceneRootAddr = 0;
        // Builds the per-view FSceneRoot transient (shared addrs + view camera/clusters/IBL) -> address.
        uint64 BuildViewSceneRoot(FSceneView& View, uint64 SceneDataAddr);

        TArray<FSceneBuffer, RHI::kFramesInFlight>                          IndirectArgsRing = {};
        
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MeshletDrawListRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MeshDrawArgsRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MeshletDeferListRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          DeferCountRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          CullDispatchArgsRing = {};   // {GroupCountX,Y,Z} for the late-cull indirect dispatch
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          SpdCounterRing = {};
        // GPU-built exclusive prefix over per-instance meshlet counts (N+1 uints); the cull binary-searches it.
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          InstancePrefixRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MaterialBinTileBitsRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MaterialBinPixelIdRing = {};
        
        uint8                                                           CurrentFrameSlot = 0;

        FShadowAtlas                            ShadowAtlas;
        
        THashMap<entt::entity, FTerrainGPUState> TerrainGPUStates;
        
        THashMap<entt::entity, FParticleGPUState> ParticleGPUStates;

        TVector<FDrawBatchKey>                  MergeGlobalBatchKeys;
        // Batch key hash -> global batch index; keeps the merge linear in total batches, not quadratic.
        THashMap<uint64, uint32>                MergeGlobalBatchIndexByKey;
        TVector<TVector<FLocalBatchEntry*>>     MergeBatchToLocals;
        TVector<TVector<FDrawKey>>              MergeGlobalDrawsPerBatch;
        TVector<uint32>                         MergeBatchDrawArgBase;
        TVector<uint32>                         MergeDrawInstanceCounts;
        TVector<uint32>                         MergeMeshletCountsPerDraw;
        TVector<uint32>                         MergeDrawInstanceOffsets;
        TVector<uint32>                         MergeDrawCursor;
        TVector<uint32>                         MergeThreadBoneBase;

        // Deferred material-binning scratch (rebuilt each DeferredMaterialPass; capacity reused):
        // dense slot -> master DeferredShader, and global MaterialIndex -> dense slot.
        TVector<const FShaderEntry*>            BinnedDeferredSlotShaders;
        TVector<uint32>                         BinnedDeferredSlotByMaterial;

        TVector<uint32>                         ShadowSizeScratch;
        TVector<uint32>                         ShadowSortedScratch;

        struct FDecalSortEntry { CMaterial* ShaderOwner; int32 SortOrder; FGPUDecal Gpu; };
        TVector<FDecalSortEntry>                DecalSortScratch;
        THashMap<CMaterial*, int32>             DecalGroupMinSort;
        
        // Per-worker draw-data slots, persistent across frames. Each slot's arena-backed vectors are
        // (re)bound to the owning worker's thread frame arena lazily, on that worker's first touch of a
        // gather pass (see AcquireThreadLocalDrawData); a null arena marks a slot not yet bound this frame.
        TVector<FThreadLocalDrawData>           ThreadLocalStorage;
        uint32                                  CurrentReservePerThread = 0;

        struct alignas(64) FLineBatchScratch
        {
            static constexpr uint32 kMaxBuckets = 16;
            float    BucketThickness[kMaxBuckets];
            uint8    BucketDepthTest[kMaxBuckets];
            uint32   GlobalBucket[kMaxBuckets];     // local -> global index, filled at merge
            uint32   WriteCursor[kMaxBuckets];      // vertex write offset, filled at merge
            uint32   NumBuckets = 0;
            TVector<FSimpleElementVertex>                  BucketVerts[kMaxBuckets];
            TVector<FLineBatcherComponent::FLineInstance>  Survivors;
        };
        TVector<FLineBatchScratch>              LineBatchScratch;
        TVector<FLineBatcherComponent::FLineInstance> LineCompactScratch;

        // Fixed-size views over the batcher's per-worker buffers + persistent list, built each frame as the
        // balanced work units for the parallel line batch (no drain). Reused so it doesn't churn the heap.
        struct FLineChunk { const FLineBatcherComponent::FLineInstance* Data; uint32 Count; };
        TVector<FLineChunk>                     LineChunkScratch;

        FTaskGraph                              DrawTaskGraph;   // mesh gather critical path; dispatched first
        FTaskGraph                              EmitTaskGraph;   // lights/primitives/extract emitters; built while DrawTaskGraph runs
        FTaskGraph                              DedupTaskGraph;  // nested inside MergeMeshDrawData

        TArray<FFrameData,      RHI::kFramesInFlight>   FrameRing;
        TArray<TAtomic<uint64>, RHI::kFramesInFlight>   SlotConsumedCount;
        TArray<uint64,          RHI::kFramesInFlight>   SlotProducedCount = {};
        TArray<TAtomic<bool>,   RHI::kFramesInFlight>   SlotHasPendingConsume = {};
        FMutex                                          SlotMutex;
        std::condition_variable                         SlotCV;

        FFrameData*                             ExtractFrame = nullptr;  // game thread
        FFrameData*                             RenderFrame  = nullptr;  // render thread

        void WaitForSlotConsumed(uint8 Slot, uint64 Target);
        void SignalSlotConsumed(uint8 Slot);

#if USING(WITH_EDITOR)
        static constexpr uint32                 PickerReadbackRingSize = RHI::kFramesInFlight + 1;
        static constexpr uint32                 PickerRegionExtent = 64;
        struct FPickerReadbackSlot
        {
            RHI::GPUPtr         Readback = 0;       // CPURead buffer, Width*Height*4 bytes
            uint32              OriginX = 0;        // top-left of the copied region, in picker texels
            uint32              OriginY = 0;
            uint32              Width = 0;          // region dimensions
            uint32              Height = 0;
            uint64              SubmittedFrame = 0;
            bool                bPending = false;
        };
        mutable TArray<FPickerReadbackSlot,     PickerReadbackRingSize> PickerReadbackRing;
        uint64                                  PickerReadbackFrame = 0;
        uint32                                  PickerReadbackWriteIndex = 0;

        // Pick-cursor published by editor, read by readback. Packed into one atomic to
        // avoid torn reads: bit 0 = over-viewport, bits 1..21 = X, bits 22..42 = Y.
        TAtomic<uint64>                         PickerCursorPacked = 0;

        void IssuePickerReadback(RHI::FCmdListH CL);
#endif

    };
}
