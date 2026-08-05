#pragma once
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
#include "World/Scene/RenderScene/ScenePrimitiveSet.h"
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
            // GPU pre-skinning. SkinSliceSize is the sum of the entity's distinct rendered meshlet
            // blocks (per surface, per LOD) rather than one span across them, so unrendered surfaces
            // and intermediate LODs cost nothing. Merge grants GlobalSkinnedBase for the whole
            // entity (kNoPreSkinBase when it lost the budget); SkinCursor sub-allocates the blocks.
            uint32                  SkinSliceSize;
            uint32                  GlobalSkinnedBase;
            uint32                  SkinCursor;
            uint32                  SkinBoneOffset;             // global; resolved during merge.
        };

        // One skinned entity competing for the per-frame pre-skin budget. Ranked by a screen-size
        // proxy so the biggest on-screen meshes keep the compute path when the budget is short.
        struct FSkinCandidate
        {
            FEntityRecord*          Record;
            float                   Priority;
        };

        struct FProcessedDrawItem
        {
            uint32              EntityRecordIndex;
            // Batch this surface draws with, read straight out of the primitive's surface binding. One
            // batch is one draw, so this doubles as the DrawID the GPU cull uses; there is no separate
            // flat (batch, draw) slot space any more.
            uint32              BatchIndex;
            uint32              SurfaceMeshletOffset;
            uint32              SurfaceMeshletCount;
            uint32              ShadowMeshletOffset;
            uint32              ShadowMeshletCount;
            // Vertex extent of the two meshlet blocks this item draws (skinned only; 0 otherwise).
            // Merge overwrites the *Offset fields in place with the resolved pre-skin bases, which is
            // what FGPUInstance carries -- the offsets are only needed to derive them.
            uint32              SurfaceVertexOffset;
            uint32              SurfaceVertexCount;
            uint32              ShadowVertexOffset;
            uint32              ShadowVertexCount;
            EInstanceFlags      Flags;
            uint16              MaterialIndex;
            uint16              _Pad;
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
            FBonePageArray                      BonesData;

            // Per-draw-slot counters, indexed by FProcessedDrawItem::GlobalDrawSlot.
            //
            // These are HEAP-persistent, not frame-arena, and deliberately so. The slot space is sized by
            // the scene's distinct (material, geometry range) pairs, which in a chunked world runs to tens
            // of thousands -- and a frame-arena vector value-initializes on resize, so rebuilding them per
            // frame would memset slots x threads x 12 B every frame regardless of how few slots a worker
            // actually touched. Instead they are allocated once, and each frame clears only the entries
            // named by the previous frame's TouchedSlots. Cost is O(touched), not O(slot space).
            TVector<uint32>                     DrawInstanceCounts;
            TVector<uint32>                     DrawMeshletCounts;
            // Filled by the merge: where this thread's instances for each slot start.
            TVector<uint32>                     DrawWriteBase;
            // Per-batch instance homogeneity (bit 0 skinned, bit 1 static); merged into FMeshDrawCommand
            // to pick the SPEC_SKINNED variant.
            TVector<uint8>                      BatchSkinFlags;
            // Slots this thread emitted into, appended on each slot's 0->1 transition. Drives both the
            // merge's walk and the next frame's clear, so it outlives the frame arena too.
            TVector<uint32>                     TouchedSlots;

            FFrameArenaAllocator                Arena;
            FSceneRenderStats                   Stats = {};
            bool                                bTouched = false;

            FThreadLocalDrawData() = default;
            explicit FThreadLocalDrawData(FFrameArenaAllocator A)
                : Items(A), EntityRecords(A), BonesData(A), Arena(A) {}

            FThreadLocalDrawData(FThreadLocalDrawData&&) = default;
            FThreadLocalDrawData& operator=(FThreadLocalDrawData&&) = default;

            void ResetForFrame(FFrameArenaAllocator A)
            {
                new (&Items)              TFrameVector<FProcessedDrawItem>(A);
                new (&EntityRecords)      TFrameVector<FEntityRecord>(A);
                new (&BonesData)          FBonePageArray(A);
                Arena = A;
                Stats = {};
                bTouched = false;
            }

            // Zeroes only what the last gather dirtied, then grows to the current batch count.
            void PrepareCounters(uint32 NumBatches)
            {
                for (uint32 Slot : TouchedSlots)
                {
                    DrawInstanceCounts[Slot] = 0u;
                    DrawMeshletCounts[Slot]  = 0u;
                }
                TouchedSlots.clear();

                if ((uint32)DrawInstanceCounts.size() < NumBatches)
                {
                    DrawInstanceCounts.resize(NumBatches, 0u);
                    DrawMeshletCounts.resize(NumBatches, 0u);
                    DrawWriteBase.resize(NumBatches, 0u);
                }
                if ((uint32)BatchSkinFlags.size() < NumBatches)
                {
                    BatchSkinFlags.resize(NumBatches, 0u);
                }
                // Batch flags are OR-accumulated, so they need clearing too; batches are orders of
                // magnitude fewer than slots, so a flat clear is cheaper than tracking which were hit.
                Memory::Memzero(BatchSkinFlags.data(), BatchSkinFlags.size());
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

        // This frame's gathered scene. Extract writes it, RenderView reads it, same frame.
        struct FFrameData
        {
            struct FDecalBatch
            {
                // Ref-held shaders (not the live CMaterial*) so a deleted decal asset can't dangle
                // the render phase; the refcount keeps the bytecode alive past the material's death.
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
            // TerrainGPUStates keyed by Entity, so they outlive the component and no pass dereferences it.
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

                // Dimensions changed this frame -> render phase (re)creates GPU textures
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
            // ParticleGPUStates keyed by Entity, so it outlives the component and no pass dereferences it.
            // One of these per (entity, emitter): a system with four emitters extracts four items, each
            // dispatching and drawing independently.
            struct FParticleExtract
            {
                entt::entity            Entity;
                // Index into the system's Emitters, and the component's slot in that entity's GPU state
                // vector. Stays valid for the frame because extract snapshots everything the passes read.
                int32                   EmitterIndex        = 0;
                // Emitters on the system this frame, including disabled ones. Lets the sim pass size the
                // entity's state vector exactly, so deleting an emitter frees its buffers instead of
                // leaving them stranded until the entity dies.
                int32                   EmitterCount        = 1;
                FMatrix4                WorldMatrix;

                FVector3                EmitterOffset       = FVector3(0.0f);
                float                   TimeScale           = 1.0f;
                float                   SpawnRateMultiplier = 1.0f;
                bool                    bEmit               = true;
                bool                    bBurstOnSpawn       = true;

                // Extract-phase Activate()/Deactivate() intents, applied once then cleared.
                bool                    bForceBurst         = false;
                bool                    bForceReset         = false;

                // Asset+override params resolved on Extract.
                FResolvedParticleParams Resolved;

                bool                    bReady              = false;  // asset ready to simulate
                bool                    bUsesCustomShader   = false;
                const FShaderEntry*     CustomComputeShader = nullptr;  // set iff bUsesCustomShader
                uint32                  TextureIndex        = 0u;     // heap ResourceID, resolved game-side

                // Module-stack parameter slots, copied on Extract rather than read from the asset on the
                // render thread. Small (one float4 per module input) and uploaded through the transient
                // ring, so no persistent buffer or dirty tracking is needed.
                TVector<FVector4>       ModuleParamValues;

                // Floats per particle in the declared-attribute buffer; sizes the parallel allocation.
                uint32                  AttributeFloatCount = 1u;

                // Slot per ParticleRenderAttribute::Type, -1 when undeclared. Copied on extract so the
                // render thread never reads the asset.
                int32                   RenderAttrSlots[ParticleRenderAttribute::Count];
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
                // Per-batch meshlet totals contributed by the CPU-produced skinned instances. Seeds the
                // GPU counter CullInstances then accumulates the rigid contribution into.
                TVector<uint32>                  BatchMeshletSeed;
                FSceneCullContext                SceneCullContext;
                // Per-instance meshlet prefix is GPU-built (ScanPrefix* passes) into
                // InstancePrefixRing; no CPU-side array exists anymore.

                // WHAT changed in the retained scene this frame, decided once by Extract. Carries no
                // payload: the upload reads ScenePrimitives' live arrays, which nothing mutates between
                // Extract and the render phase.
                //
                // The incremental path is the point: in the steady state DirtySlots is empty and the whole
                // retained upload costs nothing.
                struct FRetainedUpload
                {
                    // The device buffer cannot be patched and must be rebuilt from scratch. Set when the
                    // primitive set asks for it, or when SlotCount outgrew the capacity the render phase
                    // published -- i.e. exactly the frames on which the device allocation is replaced.
                    bool                        bFull = false;
                    uint32                      SlotCount = 0;

                    // Changed slots, sorted + deduped so the upload can coalesce adjacent runs. Empty
                    // when bFull.
                    //
                    // Split the same way the retained arrays are: DirtySlots covers the cull entry and
                    // the transform (what a MOVE writes), DirtyStaticSlots the static payload (what a
                    // RE-BIND writes). A crowd moving leaves the second list empty, so its buffer is not
                    // re-sent -- and each run is a transient RHI allocation, so the count matters too.
                    TVector<uint32>             DirtySlots;
                    TVector<uint32>             DirtyStaticSlots;

                    // SurfaceDescCount is the LIVE count every frame; bSurfaceDescsChanged says whether
                    // the payload moved. They are separate because the count also gates the cull
                    // dispatch -- deriving it from the change flag would skip culling entirely on every
                    // frame the descriptors happened not to move.
                    bool                        bSurfaceDescsChanged = false;
                    uint32                      SurfaceDescCount = 0;
                } RetainedUpload;
            } Geometry;

            struct FViews
            {
                // Indirect-arg + mesh-task-arg seeds are GPU-generated (SeedIndirectArgs.slang) from
                // the GPU-built per-draw meshlet prefix; no CPU-side V*D arrays exist anymore.
                TVector<FCullView>               CullViews;
                // No CPU-side meshlet total lives here. BuildDrawPrefix produces it on the GPU and it is
                // never read back on the critical path; anything that needs an index bound uses the
                // allocation capacity instead (FForwardRenderScene::DrawListCapacity). A CPU mirror of it
                // would only ever be stale-or-zero, which is exactly how the VisBuffer resolve once ended
                // up rejecting every shaded pixel.
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

            // Post-process material resolved + ref-held on Extract; the render phase reads
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
            CascadePyramid,
            DepthAttachment,
            DepthPyramid,
            Picker,
            VisBuffer,
            MaterialDepth,
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

        // Deferred material-binning scratch (device-address only). MaterialDepth produces the per-tile slot
        // bitmask; BuildMaterialTileList compacts it into the per-slot tile list and its indirect draw args.
        FSceneBuffer GetMaterialBinTileBits() const { return MaterialBinTileBitsRing[CurrentFrameSlot]; }
        FSceneBuffer GetMaterialTileList()    const { return MaterialTileListRing[CurrentFrameSlot]; }
        FSceneBuffer GetMaterialTileArgs()    const { return MaterialTileArgsRing[CurrentFrameSlot]; }

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

        // Runs the resolve pre-pass repeatedly until it stops re-marking itself pending, or the budget
        // runs out. Only for callers that render exactly ONE frame and then read the result back --
        // the thumbnail capture. The normal frame loop needs none of this: a component that is not
        // fully resolvable yet re-arms and lands on the next frame, which is invisible at 60fps. A
        // capture has no next frame, so anything deferred is simply absent from the image, which is
        // what an "empty world" thumbnail is.
        // RUNTIME_API: the class is not exported wholesale, and this one is called from the editor.
        RUNTIME_API void SettleResolveWork(int32 MaxIterations = 8);
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
        // ReuseOutputSlot adopts the heap slot detached from the previous Output image, keeping the
        // view's published ResourceID stable across a resize. See InitFrameResources.
        void InitViewImages(FSceneView& View, uint32 ReuseOutputSlot = RHI::kInvalidHeapSlot);

        // Attach debug-utils names to every image the array owns, so a GPU crash report can resolve
        // a faulting address to a render target by name. Idempotent; safe to re-run after a rebuild.
        void NameOwnedImages(TArray<FSceneImage, (int)ENamedImage::Num>& Images);
        // bDeferRelease routes the images through this slot's deferred-free list instead of freeing them
        // outright, so in-flight GPU work finishes first. Pass false only at shutdown, where nothing will
        // ever process the deferred list.
        void ReleaseViewImages(FSceneView& View, bool bDeferRelease);
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
        void ResetPass_Extract();

        // The geometry half of the frame reset. Kept separate from ResetPass_Extract because it has to
        // run after SyncScenePrimitives (which reads the previous frame's state) and immediately before the
        // gather that refills these arrays, not at the top of Extract with everything else.
        void ResetGeometry_Extract();

        // Hashes everything this frame's geometry would be derived from: the primitive table's structure
        // generation, the batch layout, the camera, the CPU cull volumes and the LOD settings. Equal
        // serials mean equal geometry. Returns 0 for scenes that can never be reused frame to frame
        // (any skinned mesh -- its pose changes with nothing to observe it by).
        void ResetPass_Render(RHI::FCmdListH CL);
        void CullPassEarly(RHI::FCmdListH CL);
        void CullPassLate(RHI::FCmdListH CL);
        void SkinningPass(RHI::FCmdListH CL);
        void TexturePaintPass(RHI::FCmdListH CL);
        void DepthPyramidPass(RHI::FCmdListH CL);
        void CascadePyramidPass(RHI::FCmdListH CL);
        // Shared SPD driver behind both pyramids. bReduceMax picks the reduction that matches the source's
        // depth convention: min for the camera's reverse-Z, max for the cascade atlas's standard Z.
        void BuildDepthPyramid(RHI::FCmdListH CL, const FSceneImage& Source, const FSceneImage& Pyramid, bool bReduceMax);
        void ClusterBuildPass(RHI::FCmdListH CL);
        void LightCullPass(RHI::FCmdListH CL);
        void PointShadowPass(RHI::FCmdListH CL);
        void SpotShadowPass(RHI::FCmdListH CL);
        void CascadedShowPass(RHI::FCmdListH CL);
        void DecalPass(RHI::FCmdListH CL);
        void VisBufferPass(RHI::FCmdListH CL, uint32 ViewIndex, bool bClear);
        void MaterialDepthPass(RHI::FCmdListH CL);
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

        // Extract-phase half: ECS reads + parallel Process* tasks + cull/shadow setup.
        void CompileDrawCommands_Extract();

        // Render-phase half: buffer resize + upload commands; reads extract-phase state.
        void CompileDrawCommands_Render(RHI::FCmdListH CL);

        // Serial pre-pass so the parallel gather is pure reads; skipped when nothing changed.
        void ResolveDirtyMeshComponents();

        // Dynamic meshes bypass FMeshResolveCache, so their material state is polled by content every frame
        // rather than gated on the cache generation. Runs before ResolveDirtyMeshComponents.
        void ResolveDynamicMeshMaterials(FEntityRegistry& Registry, FRenderDirtyTracker& Tracker);

        // Last FMeshResolveCache pending generation this scene resolved against. Per scene because the
        // cache is shared across every world but each world resolves only its own components.
        uint32 LastResolvedPendingGeneration = 0;

        // Reused to flatten a component's material overrides; keeps capacity.
        TVector<CMaterialInterface*> ResolveOverrideScratch;

        // Routes this frame's transform + component changes into the primitive table. O(changed).
        void SyncScenePrimitives();

        // Recomputes the (batch, batch-local draw) -> flat slot mapping. Only when the batch registry's
        // layout generation moves, i.e. when a new material or geometry range entered the scene.

        // Parallel per-frame visible-set build over the dense primitive arrays. Camera-dependent work
        // only: cull, LOD pick, item emit. Everything else it reads was derived incrementally.
        //
        // Skinned survivors additionally read their live component here (pose bones change every frame
        // by definition, so there is nothing to cache) -- but only after they pass the cull.
        void CullAndEmitPrimitives(const Task::FParallelRange& Range, FThreadLocalDrawData& Local);

        void BuildSceneCullContext();
        void MergeMeshDrawData(TVector<FThreadLocalDrawData>& ThreadLocal);
        void AssignPreSkinSlices(FFrameData& Frame,
                                 TVector<FSkinCandidate>& Candidates,
                                 TVector<FThreadLocalDrawData>& ThreadLocal);

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
        // bAllowShrink=false keeps a sustained-low-usage reclaim from replacing the allocation. Pass false
        // for any buffer whose CONTENTS must survive across frames (the retained scene): a shrink drops
        // them, and only a frame carrying a full snapshot can refill it.
        void ResizeBufferIfNeeded(FSceneBuffer& Buffer, uint64 NeededSize, float SlackFactor, uint32& LowUsageCounter,
                                  bool bAllowShrink = true);

        // Freed when this slot's previous GPU work has completed.
        void DeferFree(RHI::GPUPtr Ptr);
        void DeferRelease(FSceneImage& Image);
    
    private:
        
        FSceneBuffer                                        PreSkinnedVerticesBuffer;
        uint32                                              PreSkinnedVerticesLowUsage = 0;

        // Per-slot instance storage, written only when that slot's compiled geometry actually changed.
        // Per slot rather than shared because a slot's buffer is still being read by in-flight GPU work
        // for kFramesInFlight frames; the serial records which compile the buffer currently holds.
        TArray<FSceneBuffer, RHI::kFramesInFlight>          InstanceBufferRing = {};
        TArray<uint32,       RHI::kFramesInFlight>          InstanceBufferLowUsage = {};
        TArray<uint64,       RHI::kFramesInFlight>          InstanceBufferSerial = {};
        TArray<uint32, RHI::kFramesInFlight>                IndirectArgsRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MeshletDrawListRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MeshDrawArgsRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MeshletDeferListRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                InstancePrefixRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MaterialBinTileBitsRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MaterialTileListRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MaterialTileArgsRingLowUsage = {};
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
        // {TotalMeshletBound, InstanceCount}. The meshlet cull's dispatch domain and its binary-search
        // upper bound, in GPU memory rather than push constants, so BuildDrawPrefix can produce them
        // from counts the CPU never sees.
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          TotalsRing = {};
        // Whether this slot's Totals block has been zeroed on the GPU yet. Totals[6]/[7] are written by
        // BuildCullDispatchArgs during view rendering, which is AFTER the feedback copy that captures
        // them, so on a slot's first trip the copy reads bytes nothing has written. Zeroing once makes
        // that read a real 0 instead of whatever the allocation came with.
        TArray<bool, RHI::kFramesInFlight>                                  TotalsZeroed = {};

        // Same one-time-zero contract as TotalsZeroed, for the early cull's indirect dispatch args.
        // Undefined bytes in a group count are a GPU hang, not a wrong number.
        TArray<bool, RHI::kFramesInFlight>                                  EarlyCullArgsZeroed = {};
        FSceneBuffer GetTotals() const { return TotalsRing[CurrentFrameSlot]; }

        //~ GPU-driven scene ---------------------------------------------------------------------
        //
        // The retained side is NOT ringed: uploads are CmdMemcpy from the transient ring, so they are
        // ordered on the GPU timeline against every earlier frame's reads on the same queue. The
        // per-frame outputs ARE ringed, because the next frame's cull would otherwise overwrite results
        // the current frame is still rasterizing.
        //
        // Three retained buffers, split by access rate rather than held as one interleaved struct. See
        // FInstanceCullEntry: the cull entries are streamed in full every frame and the other two are
        // read only for survivors, so keeping them apart is what stops the cull dragging ~5x the
        // bandwidth it consumes through the memory system.
        FSceneBuffer                                        RetainedCullEntryBuffer;
        FSceneBuffer                                        RetainedTransformBuffer;
        FSceneBuffer                                        RetainedStaticBuffer;
        FSceneBuffer                                        SurfaceDescBuffer;
        uint32                                              RetainedCullEntryLowUsage = 0;
        uint32                                              RetainedTransformLowUsage = 0;
        uint32                                              RetainedStaticLowUsage = 0;
        uint32                                              SurfaceDescLowUsage = 0;
        // Whether the device buffers need a full re-send is decided during Extract from
        // RetainedDeviceCapacity, so no second slot mirror is needed.
        uint32                                              UploadedSurfaceDescs = 0;

        TArray<FSceneBuffer, RHI::kFramesInFlight>          VisibleInstanceRing = {};
        TArray<uint32,       RHI::kFramesInFlight>          VisibleInstanceLowUsage = {};
        // {AppendCursor, OverflowFlag}: seeded past the CPU-written skinned head, then atomically
        // advanced by CullInstances.
        TArray<FSceneBuffer, RHI::kFramesInFlight>          CullCounterRing = {};
        // Per-batch VIEW-INDEPENDENT meshlet work (NumDraws entries), seeded with the skinned contribution
        // then accumulated by the cull. This is the meshlet cull's dispatch domain.
        TArray<FSceneBuffer, RHI::kFramesInFlight>          BatchMeshletCountRing = {};
        TArray<uint32,       RHI::kFramesInFlight>          BatchMeshletCountLowUsage = {};
        // Per-(view, draw) emit bound and its exclusive prefix -- NumCullViews * NumDraws entries each.
        // These are what replaced slicing the draw list into equal per-view chunks: every region is sized
        // to exactly what that pair can emit, so a heavily-culled cascade costs almost no space.
        TArray<FSceneBuffer, RHI::kFramesInFlight>          ViewDrawCountRing = {};
        TArray<uint32,       RHI::kFramesInFlight>          ViewDrawCountLowUsage = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>          ViewDrawOffsetRing = {};
        TArray<uint32,       RHI::kFramesInFlight>          ViewDrawOffsetLowUsage = {};
        // Skinned contribution replicated across views, staged once per frame for the V*D upload.
        TVector<uint32>                                     ViewDrawSeedScratch;
        // {GroupCountX,Y,Z} for the EARLY cull, written by BuildDrawPrefix from the GPU-side total.
        TArray<FSceneBuffer, RHI::kFramesInFlight>          EarlyCullDispatchArgsRing = {};
        // Per-block sums for the hierarchical instance-prefix scan, scanned in place by pass 2.
        // ceil(capacity / 256) + 1 uints; the last entry is the grand total.
        TArray<FSceneBuffer, RHI::kFramesInFlight>          ScanBlockSumRing = {};
        TArray<uint32,       RHI::kFramesInFlight>          ScanBlockSumLowUsage = {};
        // {GroupCountX,Y,Z} shared by scan passes 1 and 3 (identical domain), also from BuildDrawPrefix.
        TArray<FSceneBuffer, RHI::kFramesInFlight>          ScanDispatchArgsRing = {};

        FSceneBuffer GetVisibleInstances()  const { return VisibleInstanceRing[CurrentFrameSlot]; }
        FSceneBuffer GetCullCounters()      const { return CullCounterRing[CurrentFrameSlot]; }
        FSceneBuffer GetBatchMeshletCounts()const { return BatchMeshletCountRing[CurrentFrameSlot]; }
        FSceneBuffer GetViewDrawCounts()    const { return ViewDrawCountRing[CurrentFrameSlot]; }
        FSceneBuffer GetViewDrawOffsets()   const { return ViewDrawOffsetRing[CurrentFrameSlot]; }
        FSceneBuffer GetEarlyCullDispatchArgs() const { return EarlyCullDispatchArgsRing[CurrentFrameSlot]; }
        FSceneBuffer GetScanBlockSums()      const { return ScanBlockSumRing[CurrentFrameSlot]; }
        FSceneBuffer GetScanDispatchArgs()   const { return ScanDispatchArgsRing[CurrentFrameSlot]; }

        // Uploads the retained scene's dirty ranges and dispatches CullInstances -> BuildDrawPrefix.
        // Everything the CPU used to decide per frame -- membership, LOD, the draw-argument layout --
        // is produced here instead.
        void DispatchGPUSceneCull(RHI::FCmdListH CL, const FFrameData& Frame);

        // Collects the retained scene's changes into ExtractFrame and consumes the dirty channel.
        // Must run after ScenePrimitives.Sync and before DispatchGPUSceneCull reads them.
        void PublishRetainedUpload();

        // Trims each (view, draw) indirect count to the entries the cull actually wrote. MUST run after
        // every cull phase and before that phase's raster, or the raster reads unwritten draw-list slots.
        void ClampDrawArgs(RHI::FCmdListH CL);


        // Element capacity of the retained device buffers, written during the render phase after each
        // resize and read by the next frame's Extract to decide whether it needs a full re-send.
        // Capacity only ever grows (a shrink is gated on a frame that already carries a full snapshot).
        std::atomic<uint32>                                 RetainedDeviceCapacity{0};

        // Whether the depth pyramid holds the PREVIOUS rendered frame's depth, which is the only thing
        // two-phase occlusion culling may test against. Cleared whenever a frame does not render or the
        // view is resized; the camera view then drops its occlusion test for one frame rather than
        // deferring the whole scene against a pyramid that means nothing.
        std::atomic<bool>                                   bDepthPyramidValid{false};

        // Same contract as bDepthPyramidValid, for the cascade pyramid: set once a frame has actually
        // rastered cascades and pyramided them, cleared whenever the sun stops casting or a frame is skipped.
        std::atomic<bool>                                   bCascadePyramidValid{false};

        // The cascade transforms that produced the cascade pyramid's current contents. Extract publishes
        // these (not this frame's) into CullData, because the pyramid is built after the shadow raster and
        // so always describes the previous frame. Written and read only during Extract.
        FMatrix4                                            CascadeHZBViewProjection[NumCascades] = {};
        FVector4                                            CascadeHZBNdcScale[NumCascades] = {};
        bool                                                bCascadeHZBTransformsValid = false;

        // Slots in the Totals block BuildDrawPrefix writes. The TotalsRing allocation, the feedback copy
        // and the CPU-side readback all derive their size from this: they were three separate literals,
        // and the readback kept the 2 it was born with while Totals grew to 8, so every value past
        // Totals[1] was read out of bounds.
        static constexpr uint32                             kTotalsSlots = 8;

        // TotalMeshletBound is GPU-only now, but the meshlet draw list is a CPU allocation sized by it.
        // Resolved by feedback: each frame copies the GPU total into a CPURead buffer, the CPU picks it
        // up kFramesInFlight frames later, and the next allocation is sized from it plus slack. The
        // in-shader capacity clamp in EmitMeshlet is what makes a mis-prediction a dropped meshlet
        // rather than a write past the allocation.
        TArray<RHI::GPUPtr, RHI::kFramesInFlight>           MeshletBoundReadback = {};
        // Totals[0]: view-independent meshlet work. Sizes the defer list.
        uint32                                              LastMeshletBound = 0;
        // Totals[2]: draw-list entries the frame actually required, summed over every (view, draw) region.
        // This is what sizes the draw list now. It is the real requirement rather than
        // NumViews * worst-case, which is why the allocation collapsed.
        uint32                                              LastDrawListRequired = 0;
        // Totals[3]: the GPU saw the requirement exceed the allocation and dropped meshlets.
        uint32                                              LastDrawListOverflowed = 0;
        // Totals[4]: visible instances the frame actually demanded, uncapped. Sizes the visible-instance
        // buffer. Before this existed the CPU had to assume every retained slot survives.
        uint32                                              LastVisibleInstances = 0;
        // Totals[5]: demand exceeded capacity and whole instances were dropped. CullInstances always wrote
        // this flag; nothing read it, so the documented backstop did not actually exist.
        uint32                                              LastVisibleOverflowed = 0;
        uint32                                              DrawListCapacity = 0;
        // Entries the defer list holds. The early cull bounds its append with this; without it DeferCount
        // was an unbounded atomic used straight as a write index.
        uint32                                              DeferListCapacity = 0;
        // Totals[6]/[7]: deferred meshlets demanded, and whether that exceeded the allocation.
        uint32                                              LastDeferRequested = 0;
        uint32                                              LastDeferOverflowed = 0;
        // Monotonic per-frame counter behind CullData::MeshletDrawTag. Must not be derived from the frame
        // slot: stale entries live in the slot they were written to, so a slot-derived tag would match them.
        uint32                                              MeshletDrawTagCounter = 0;
        // This frame's visible-instance buffer capacity, snapshotted ONCE per frame.
        //
        // It derives from ScenePrimitives.GetRetainedSlotCount(), which is live extract-phase state: calling
        // deriving it twice in one frame could return two different numbers if the game
        // thread adds primitives in between. Every consumer -- the VisibleInstanceRing allocation, the
        // InstancePrefixRing allocation, CullInstances' MaxVisibleInstances, BuildDrawPrefix's clamp and
        // CullData::InstanceNum -- must agree on one value, or the GPU indexes one buffer with another
        // buffer's bound. Sampled in CompileDrawCommands_Render before the SceneRoot is published.
        uint32                                              FrameVisibleInstanceCapacity = 0;
        // Ceiling BuildDrawPrefix clamps the meshlet work domain to, so a corrupt GPU-side counter cannot
        // become an unbounded indirect dispatch. Surface descs are interned and append-only, so the max
        // only grows and only the entries added since ScannedSurfaceDescCount need looking at.
        uint32                                              MaxSurfaceDescMeshlets = 0;
        uint32                                              ScannedSurfaceDescCount = 0;
        void   UpdateMeshletBoundFeedback(uint8 Slot);
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MaterialBinTileBitsRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MaterialTileListRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MaterialTileArgsRing = {};
        
        uint8                                                           CurrentFrameSlot = 0;

        FShadowAtlas                            ShadowAtlas;
        
        THashMap<entt::entity, FTerrainGPUState> TerrainGPUStates;
        
        // Per entity, one state per emitter, indexed by FParticleExtract::EmitterIndex. Grouped by entity
        // rather than keyed by a packed (entity, emitter) pair so the dead-entity sweep still drops an
        // entity's entire footprint in a single erase.
        THashMap<entt::entity, TVector<FParticleGPUState>> ParticleGPUStates;

        // Persistent scene state. Membership and every camera-independent property are maintained from
        // the dirty channel, so a frame in which nothing changed does no scene work at all.
        FScenePrimitiveSet                      ScenePrimitives;
        TVector<entt::entity>                   MovedTransformScratch;

        // Flat (batch, batch-local draw) slot space. Rebuilt only when the batch registry's layout
        // generation moves; the per-frame path indexes it and never rebuilds it.

        // Merge scratch, sized by draw slots (not by entities). Members so capacity survives frames.
        TVector<uint32>                         MergeDrawInstanceCounts;
        TVector<uint32>                         MergeMeshletCountsPerDraw;
        TVector<uint32>                         MergeDrawInstanceOffsets;
        TVector<uint32>                         MergeThreadBoneBase;
        TVector<FSkinCandidate>                 MergeSkinCandidates;
        uint32                                  LastPreSkinDeferredCount = 0;

        // Deferred material-binning scratch (rebuilt each MaterialDepthPass; capacity reused):
        // dense slot -> master DeferredShader, and global MaterialIndex -> dense slot.
        TVector<const FShaderEntry*>            BinnedDeferredSlotShaders;
        TVector<uint32>                         BinnedDeferredSlotByMaterial;

        // Tile geometry for the current frame's material binning. MaterialDepthPass derives it and stamps
        // the per-pixel slot depths against it; DeferredMaterialPass then issues one indirect draw per slot
        // off the same numbers. Both must read ONE copy: the tile grid is what maps a pixel to a bitmask
        // word, so two passes deriving it independently is how a binning pass silently starts disagreeing
        // with the draws that consume it.
        struct FMaterialBinLayout
        {
            uint32 NumSlots      = 0;
            uint32 TileCountX    = 0;
            uint32 TotalTiles    = 0;
            uint32 TileWordCount = 0;
            uint32 ScreenW       = 0;
            uint32 ScreenH       = 0;
        };
        FMaterialBinLayout                      MaterialBinLayout;

        // Builds BinnedDeferredSlot* + MaterialBinLayout from this frame's deferred materials and sizes the
        // binning rings. False when there is nothing to shade (no slots, or a zero-area view).
        bool BuildDeferredMaterialBinning();

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

        FFrameData                              FrameData;

        FFrameData*                             ExtractFrame = nullptr;  // non-null while Extract runs
        FFrameData*                             RenderFrame  = nullptr;  // non-null while RenderView runs

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
