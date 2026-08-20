#pragma once

#include "Renderer/ShaderHandle.h"
#include "Core/Delegates/Delegate.h"
#include "Memory/Allocators/Allocator.h"
#include "Memory/SmartPtr.h"
#include "Renderer/ImmediateLineRenderer.h"
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
#include "World/Entity/Components/CloudComponent.h"
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

    /** The engine's default scene renderer: GPU-driven meshlet culling into a visibility buffer, then
     *  per-material GBuffer resolve and a clustered lighting pass. RenderSceneFactory falls back to this
     *  whenever no override is installed. */
    class FDefaultSceneRenderer : public IRenderScene
    {
    public:

        FDefaultSceneRenderer(CWorld* InWorld);
        ~FDefaultSceneRenderer() override;
        LE_NO_COPYMOVE(FDefaultSceneRenderer);

        struct FEntityRecord
        {
            FMatrix4               Transform;
            FVector4               SphereBounds;
            uint32                  MeshletHeaderSlot;
            uint32                  CustomData;
            uint32                  EntityID;
            uint32                  BoneArenaBase;
            uint32                  BoneArenaCount;
        };

        struct FProcessedDrawItem
        {
            uint32              EntityRecordIndex;
            uint32              BatchIndex;
            uint32              InstanceSlot;
            uint32              SurfaceMeshletOffset;
            uint32              SurfaceMeshletCount;
            uint32              ShadowMeshletOffset;
            uint32              ShadowMeshletCount;
            uint32              MeshletTotalCount;
            EInstanceFlags      Flags;
            uint16              MaterialIndex;
            uint16              _Pad;
        };

        struct CACHE_ALIGN FThreadLocalDrawData
        {
            TFrameVector<FProcessedDrawItem>    Items;
            TFrameVector<FEntityRecord>         EntityRecords;

            TVector<uint32>                     DrawInstanceCounts;
            TVector<uint8>                      BatchSkinFlags;
            TVector<uint32>                     TouchedSlots;

            FSceneRenderStats                   Stats = {};
            bool                                bTouched = false;

            FThreadLocalDrawData() = default;
            FThreadLocalDrawData(const FThreadLocalDrawData&) = delete;

            FThreadLocalDrawData(FThreadLocalDrawData&&) = default;
            FThreadLocalDrawData& operator=(FThreadLocalDrawData&&) noexcept = default;

            void ResetForFrame()
            {
                Items.clear();
                EntityRecords.clear();
                Stats = {};
                bTouched = false;
            }

            // Zeroes only what the last gather dirtied, then grows to the current batch count.
            void PrepareCounters(uint32 NumBatches)
            {
                for (uint32 Slot : TouchedSlots)
                {
                    DrawInstanceCounts[Slot] = 0u;
                }
                TouchedSlots.clear();

                if ((uint32)DrawInstanceCounts.size() < NumBatches)
                {
                    DrawInstanceCounts.resize(NumBatches, 0u);
                }
                if ((uint32)BatchSkinFlags.size() < NumBatches)
                {
                    BatchSkinFlags.resize(NumBatches, 0u);
                }
                Memory::Memzero(BatchSkinFlags.data(), BatchSkinFlags.size());
            }

            ~FThreadLocalDrawData()
            {
                ResetForFrame();
            }
        };

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

            struct FParticleExtract
            {
                entt::entity            Entity;
                int32                   EmitterIndex        = 0;
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
                FShaderH     CustomComputeShader = {};  // set iff bUsesCustomShader
                uint32                  TextureIndex        = 0u;     // heap ResourceID, resolved game-side

                TVector<FVector4>       ModuleParamValues;

                // Floats per particle in the declared-attribute buffer; sizes the parallel allocation.
                uint32                  AttributeFloatCount = 1u;

                int32                   RenderAttrSlots[ParticleRenderAttribute::Count];
            };

            struct FCaptureViewData
            {
                FSceneGlobalData    SceneGlobalData = {};
                FViewVolume         ViewVolume      = {};
                uint32              CameraViewIndex = ~0u;   // its cull-view index (frustum-only)
                int32               SceneViewIndex  = -1;    // index into FDefaultSceneRenderer::SceneViews
            };

            FViewVolume                      ViewVolume = {};
            FFrustum                         CameraFrustum = {};
            FSceneGlobalData                 SceneGlobalData = {};
            SDefaultWorldSettings            CachedWorldSettings = {};
            float                            CachedWorldDeltaTime = 0.0f;
            bool                             bExtractedThisFrame = false;
            FSceneRenderStats                FrameStats = {};

            struct FDeferredMaterialEntry
            {
                uint32              MaterialIndex;
                FShaderH DeferredShader;
            };

            struct FGeometry
            {
                // CPU mirror of the bone arena, indexed by FScenePrimitive::BoneArenaBase -- NOT a packed
                // concatenation. Only the slices of skeletons gathered this frame are written and uploaded;
                // the rest holds don't-care data, because an entity that was not gathered is not drawn.
                // 48B/bone (last row dropped).
                TVector<FBoneTransform>          BonesData;
                // (base, count) per gathered skeleton, coalesced into the upload. Derived in the merge from
                // the entity records, so workers need no shared list.
                TVector<FUIntVector2>            BoneUploadRanges;
                // Per-frame skinned data indexed by RETAINED slot; only gathered slots are written, and
                // SkinnedSlots lists which. Stale entries are rejected by their frame tag, not cleared.
                TVector<FSkinnedFrameData>       SkinnedFrameData;
                TVector<uint32>                  SkinnedSlots;
                TVector<FMeshDrawCommand>        DrawCommands;
                TVector<uint32>                  OpaqueDrawList;
                TVector<uint32>                  TranslucentDrawList;
                TVector<FDeferredMaterialEntry>  DeferredMaterials;   // distinct opaque slots for the deferred pass
                FSceneCullContext                SceneCullContext;

                struct FRetainedUpload
                {
                    bool                        bFull = false;
                    uint32                      SlotCount = 0;

                    TVector<uint32>             DirtySlots;
                    TVector<uint32>             DirtyStaticSlots;

                    bool                        bSurfaceDescsChanged = false;
                    uint32                      SurfaceDescCount = 0;

                    uint32                      MaxSurfaceDescMeshlets = 0;
                } RetainedUpload;
            } Geometry;

            struct FViews
            {
                TVector<FCullView>               CullViews;
                uint32                           NumDrawsPerView     = 0;
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

                FImmediateLineRenderer::FDrawRange ImmediateLines[FImmediateLineRenderer::NumChannels];

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
                bool                             bAerialPerspective   = false;
                float                            AerialRange          = 8000.0f;
                float                            AerialIntensity      = 1.0f;

                bool                             bClouds              = false;
                SCloudComponent                  Clouds               = {};
            } Volumetrics;

            // Splines uploaded this frame. Headers index into the two shared arrays; everything is world
            // space (SSplineComponent authors in entity-local space and the extract bakes the transform in).
            struct FSplines
            {
                TVector<FGPUSpline>              Splines;
                TVector<FGPUSplinePoint>         Points;
                TVector<FGPUSplineSample>        Samples;
            } Splines;

            struct FReflectionProbes
            {
                TVector<FGPUReflectionProbe>     Probes;
                // Per-probe capture parameters, parallel to Probes. Only the bake reads these.
                TVector<FReflectionProbeCapture> Captures;
                bool                             bNeedsRebake = false;
                bool                             bLayoutChanged = false;

                int32                            BakingProbe    = -1;
                int32                            BakeViewIndex  = -1;      // FSceneView the faces render through
                uint32                           BakeFaceSize   = 0;
                TArray<uint32, 6>                FaceCullViews  = {};      // per-face cull-view index
                TArray<FViewVolume, 6>           FaceVolumes    = {};
                TArray<FSceneGlobalData, 6>      FaceGlobals    = {};
            } ReflectionProbes;

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

                #if USING(WITH_EDITOR)
                TVector<uint32>                  SelectionBits;
                #endif
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
            GTAO,
            GTAODenoise,
            GTAOBlur,
            ShadowMask,
            Cascade,
            CascadePyramid,
            DepthAttachment,
            DepthPyramid,
            Picker,
            VisBuffer,
            GBufferA,
            GBufferB,
            GBufferC,
            GBufferD,
            Accum,
            MomentZeroth,
            Moments,
            WaterRefraction,
            DBufferA,
            DBufferB,
            DBufferC,
            AdaptedLuminance,
            FroxelScatter,
            FroxelIntegrated,
            AerialInScatter,
            AerialTransmittance,
            CloudNoise,
            CloudScatter,
            BRDFLut,
            SkyCube,
            SkyIrradiance,
            SkyPrefilter,
            ProbeCaptureCube,
            ProbePrefiltered,

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
            bool                                            bReservedForProbeBake = false;
            TArray<FSceneImage, (int)ENamedImage::Num>      Images = {};
            /// Tick each on-demand image was last needed on. Only the optional entries are ever read;
            /// see EnsureOptionalViewImages.
            TArray<uint64, (int)ENamedImage::Num>           ImageLastUsedTick = {};
            FSceneImage                                     BloomChainImage;
            // CloudNoise is per-view, so a renderer-wide flag leaves view two sampling an unbaked volume.
            bool                                            bCloudNoiseBaked = false;
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
        void Resize(const FUIntVector2& NewSize) override { ResizePrimaryView(NewSize); }
        void SetPrimaryViewSize(const FUIntVector2& SizePixels) override;

        int32 RegisterCaptureView(const FUIntVector2& Size) override;
        bool  SetCaptureView(int32 Handle, const FViewVolume& View, bool bEnabled) override;
        int32 GetCaptureDisplayResourceID(int32 Handle) const override;

        void DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale) override;
        void DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness, bool bDepthTest, float Duration) override { }

        FImmediateLineRenderer* GetImmediateLines() override { return &ImmediateLines; }
        void BeginImmediateLines() override { ImmediateLines.BeginFrame(); }

        FSceneBuffer GetPreSkinnedVerticesBuffer() const { return PreSkinnedVerticesBuffer; }
        const FSceneImage& GetNamedImage(ENamedImage Image) const { return CurrentView ? CurrentView->Images[(int)Image] : NamedImages[(int)Image]; }

        FSceneBuffer GetRenderBuckets()    const { return RenderBucketRing[CurrentFrameSlot]; }
        FSceneBuffer GetMeshletDrawList()  const { return MeshletDrawListRing[CurrentFrameSlot]; }
        FSceneBuffer GetMeshDrawArgs()     const { return MeshDrawArgsRing[CurrentFrameSlot]; }
        FSceneBuffer GetMeshletBlocks()    const { return MeshletBlockRing[CurrentFrameSlot]; }
        // Read by both meshlet-cull dispatches and written by neither: that is what makes them partition.
        FSceneBuffer GetInstanceVisibilityPrev()  const { return InstanceVisibilityBuffers[InstanceVisibilityWriteIndex ^ 1u]; }
        FSceneBuffer GetInstanceVisibilityWrite() const { return InstanceVisibilityBuffers[InstanceVisibilityWriteIndex]; }
        FSceneBuffer GetBlockDispatchArgs() const { return BlockDispatchArgsRing[CurrentFrameSlot]; }
        FSceneBuffer GetSkinDispatchArgs()  const { return SkinDispatchArgsRing[CurrentFrameSlot]; }
        /** One workgroup per written meshlet block, sized by BuildMeshletCullArgs. */
        FSceneBuffer GetMeshletCullDispatchArgs() const { return MeshletCullDispatchArgsRing[CurrentFrameSlot]; }
        /** (base, count) per (visible instance, view): the one place the per-view meshlet range is
            decided. CullInstances writes it when it reserves; BuildMeshletBlocks reads it to append. */
        FSceneBuffer GetInstanceViewRanges() const { return InstanceViewRangeRing[CurrentFrameSlot]; }
        FSceneBuffer GetSpdCounter()       const { return SpdCounterRing[CurrentFrameSlot]; }
        FSceneBuffer GetLuminanceHistogram() const { return LuminanceHistogramRing[CurrentFrameSlot]; }

        /** Per-material counts, starts, scatter cursors, dispatch args and the frame pixel total. */
        FSceneBuffer GetMaterialClassify()  const { return MaterialClassifyRing[CurrentFrameSlot]; }
        /** One packed screen position per classified pixel, grouped into one contiguous run per material. */
        FSceneBuffer GetMaterialPixelList() const { return MaterialPixelListRing[CurrentFrameSlot]; }

        uint32 GetDisplayResourceID() const override;

        RUNTIME_API void SettleResolveWork(int32 MaxIterations = 8);

        /** Editor-only tripwire: warns if any dynamic-mesh surface is STILL resolved against superseded
            shaders after a full resolve pass, i.e. a resolve gate is missing an input. */
        void ValidateNoStaleResolves(FEntityRegistry& Registry);
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
        void InitViewImages(FSceneView& View, uint32 ReuseOutputSlot = RHI::kInvalidHeapSlot);

        void NameOwnedImages(TArray<FSceneImage, (int)ENamedImage::Num>& Images);
        void ReleaseViewImages(FSceneView& View, bool bDeferRelease);
        void InitFrameResources();

        // Shared body of SwapchainResized / SetPrimaryViewSize. Unconditional: callers decide whether the
        // new size is worth the WaitDeviceIdle this costs.
        RUNTIME_API void ResizePrimaryView(const FUIntVector2& NewSize);

        /** Cleared the first time something sizes the primary view explicitly (an editor tool panel). */
        bool bPrimaryTracksSwapchain = true;

        FSceneView& AddSceneView(const FUIntVector2& Size, bool bPrimary);
        
        void RenderCaptureView(RHI::FCmdListH CL);

        FSceneGlobalData MakeSecondaryViewGlobals(const FSceneGlobalData& ViewGlobals);

        /** Makes View current AND brings its on-demand targets in line with what this frame actually
         *  draws. Out of line for that second half -- it is the one place every rendered view passes
         *  through, so a view can never reach a pass with an optional target missing. */
        void PointAtView(FSceneView& View);

        /** Targets for features a scene may contain none of: MBOIT translucency (Accum, MomentZeroth,
         *  Moments), decals (DBufferA/B/C) and water (WaterRefraction). Sized into every view up front
         *  they are ~230 MB at 1080p -- per view, resident for the session, in a scene that may have no
         *  translucency, no decals and no water at all.
         *
         *  Created on the first frame the feature actually draws, released once it has been idle for a
         *  while. "Not allocated" is a legal state rather than a crash because an absent FSceneImage
         *  reports resource id -1, which reaches the shaders as the 0xFFFFFFFF sentinel they already
         *  test for (DBuffer.slang, BasePixelPass.slang). */
        static bool IsOptionalNamedImage(ENamedImage Image);
        static bool MakeOptionalImageDesc(ENamedImage Image, const FUIntVector2& Extent, RHI::FTextureDesc& OutDesc);
        void        EnsureOptionalViewImages(FSceneView& View);
        void        ReleaseIdleOptionalImages(FSceneView& View);

        /// Advances once per RenderView; the clock ImageLastUsedTick is stamped against.
        uint64      OptionalImageTick = 0;

        void InitSharedResources();

        void BakeBRDFLUT();

        void InitSkyCube(uint32 FaceSize);

        void InitIBLConvolutionTargets(const FIBLBakeResolution& Resolution);
        
        void SyncIBLResolution(const FIBLBakeResolution& Resolution);

        //~ Begin Render Passes
        void ResetPass_Extract();

        void ResetGeometry_Extract();

        void ResetPass_Render(RHI::FCmdListH CL);
        void SkinningPass(RHI::FCmdListH CL);
        void TexturePaintPass(RHI::FCmdListH CL);
        void DepthPyramidPass(RHI::FCmdListH CL);
        void CascadePyramidPass(RHI::FCmdListH CL);
        void BuildDepthPyramid(RHI::FCmdListH CL, const FSceneImage& Source, const FSceneImage& Pyramid, bool bReduceMax);
        void ClusterBuildPass(RHI::FCmdListH CL);
        void LightCullPass(RHI::FCmdListH CL);
        void PointShadowPass(RHI::FCmdListH CL);
        void SpotShadowPass(RHI::FCmdListH CL);
        void CascadedShowPass(RHI::FCmdListH CL, uint32 CascadeViewBase);
        void DecalPass(RHI::FCmdListH CL);
        /** Classic two-phase: Early replays the set that was visible last frame, with no Hi-Z, to build
            the pyramid; Late tests every instance against that pyramid and draws the ones Early did not.
            A view without ECullViewFlags::MeshletHiZ is single-phase and drawn entirely by Early. */
        void VisBufferPass(RHI::FCmdListH CL, uint32 ViewIndex, bool bClear,
                           ECullPhase::Type Phase = ECullPhase::Early);
        /** Count pixels per material, prefix-sum the counts, scatter every pixel's position into its
            material's run of the list, and build the indirect args the two passes below dispatch on. */
        void VisBufferClassifyPass(RHI::FCmdListH CL);
        /** One indirect compute dispatch per material over its own pixel run; writes the GBuffer. */
        void MaterialGBufferPass(RHI::FCmdListH CL);
        /** One indirect compute dispatch over every classified pixel; writes lit HDR. */
        void DeferredLightingPass(RHI::FCmdListH CL);
        #if USING(WITH_EDITOR)
        void PickerResolvePass(RHI::FCmdListH CL);
        // Edge-detects the Picker RT, so billboards, widgets and world text outline as well as meshes.
        void SelectionOutlinePass(RHI::FCmdListH CL);
        #endif
        #if !defined(LE_SHIPPING)
        void SceneDebugViewPass(RHI::FCmdListH CL);
        #endif
        bool BindShadowBatchPipeline(RHI::FCmdListH CL, const FMeshDrawCommand& Batch,
                                    FShaderH PixelShader);

        void DrawShadowBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, bool bUseMesh,
                             uint32 CullViewIndex, int32 ShadowDataIndex, int32 ShadowViewIndex,
                             const FUIntVector2& ViewportExtent);
        

        void ApplyCullFreeze(FFrameData& Frame);
        void DrawFrozenCullFrustum(const FFrameData& Frame);

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
        void GTAOPass(RHI::FCmdListH CL);
        void GTAOBlurPass(RHI::FCmdListH CL);
        void ShadowMaskPass(RHI::FCmdListH CL);
        /** MBOIT pass 1: accumulate absorbance moments over the translucent draw list, opacity only. */
        void MomentGenerationPass(RHI::FCmdListH CL);
        void TransparentPass(RHI::FCmdListH CL);
        void OITResolvePass(RHI::FCmdListH CL);
        void AdditiveTranslucentPass(RHI::FCmdListH CL);
        void FroxelInjectPass(RHI::FCmdListH CL);
        void FroxelIntegratePass(RHI::FCmdListH CL);
        void FroxelApplyPass(RHI::FCmdListH CL);
        void AerialPerspectivePass(RHI::FCmdListH CL);
        void VolumetricCloudPass(RHI::FCmdListH CL);
        void ScreenSpaceReflectionsPass(RHI::FCmdListH CL);
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

        void ResolveDynamicMeshMaterials(FEntityRegistry& Registry, FRenderDirtyTracker& Tracker);

        uint32 LastResolvedPendingGeneration = 0;

        // Reused to flatten a component's material overrides; keeps capacity.
        TVector<CMaterialInterface*> ResolveOverrideScratch;

        // Routes this frame's transform + component changes into the primitive table. O(changed).
        void SyncScenePrimitives();

        // Skeletal primitives only; statics flow through the retained batch registry, not a per-frame gather.
        void CullAndEmitSkinnedPrimitives(const Task::FParallelRange& Range, FThreadLocalDrawData& Local);

        void BuildSceneCullContext();
        void MergeMeshDrawData(TVector<FThreadLocalDrawData>& ThreadLocal);
        void DumpSkinnedPrimitiveState() const;

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
        
        enum EShadingFeature : uint32
        {
            SF_DebugViews = 1u << 0,
            SF_Decals     = 1u << 1,
            SF_GTAO       = 1u << 2,
            SF_ShadowMask = 1u << 3,
            SF_All        = SF_DebugViews | SF_Decals | SF_GTAO,
        };

        struct FGraphicsPipelineKey
        {
            FShaderH VS = {};
            FShaderH PS = {};    // null = depth-only
            FShaderH MS = {};    // mesh shader; when set, a mesh pipeline is built (VS ignored)
            RHI::ETopology  Topology = RHI::ETopology::TriangleList;
            bool            bWireframe = false;
            bool            bAlphaToCoverage = false;
            uint8           SampleCount = 1;
            EFormat         DepthFormat = EFormat::UNKNOWN;
            uint32          ShadingFeatures = SF_All;        // ShadeSurface spec constants (ids 1-3)
            bool            bVisBufferMasked = false;        // VISBUFFER_MASKED spec constant (id 4): VisBuffer geometry emits interpolants
            uint8           SkinnedMode = 2;                 // SPEC_SKINNED spec constant (id 5): 0=static, 1=skinned, 2=dynamic (runtime branch)
            uint8           TriCullMode = 0;
            TFixedVector<RHI::FColorTarget, 4> ColorTargets;
        };

        // Mirrors TRI_CULL_* in Includes/MeshletGeometry.slang.
        enum ETriCullFlags : uint8
        {
            TriCull_None      = 0,
            TriCull_Backface  = 1 << 0,   // only when the pipeline also sets ECullMode::Back
            TriCull_SmallPrim = 1 << 1,   // single-sample, non-wireframe pipelines only
        };

        RHI::FPipelineH      GetOrCreatePipeline(const FGraphicsPipelineKey& Key);

        /** Where a meshlet pass draws, as opposed to what. Everything here is per PASS, not per batch. */
        struct FMeshletPassContext
        {
            uint32            CullViewIndex     = 0;
            int32             ShadowDataIndex   = -1;
            int32             ShadowViewIndex   = 0;
            float             ViewportW         = 0.0f;
            float             ViewportH         = 0.0f;
            /** Which part of the bucket's draw region to rasterize. The two VisBuffer phases each take
                their own slice; every single-phase pass takes All, which is final by the time it runs. */
            EMeshletSlice     Slice             = EMeshletSlice::All;
        };

        /** Culls every view's meshlets into the draw list and publishes the slice each pass draws.
            The sole authority: nothing rasterizes a meshlet this did not keep. */
        void MeshletCullPass(RHI::FCmdListH CL, EMeshletSlice Slice);

        void DrawMeshletBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, const FMeshletPassContext& Ctx);

        template<typename TSetup, typename TBound>
        void ForEachMeshletBatch(RHI::FCmdListH CL, const TVector<uint32>& DrawList,
                                 const FMeshletPassContext& Ctx, TSetup&& Setup, TBound&& Bound)
        {
            const auto& DrawCommands = RenderFrame->Geometry.DrawCommands;

            for (uint32 Idx : DrawList)
            {
                const FMeshDrawCommand& Batch = DrawCommands[Idx];

                FGraphicsPipelineKey Key;
                Key.SkinnedMode = (Batch.bAnySkinned && Batch.bAnyStatic) ? 2u : (Batch.bAnySkinned ? 1u : 0u);

                if (!Setup(Key, Batch))
                {
                    continue;
                }

                RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));
                Bound(Batch);

                DrawMeshletBatch(CL, Batch, Ctx);
            }
        }

        /** Overload for passes with no per-batch dynamic state. */
        template<typename TSetup>
        void ForEachMeshletBatch(RHI::FCmdListH CL, const TVector<uint32>& DrawList,
                                 const FMeshletPassContext& Ctx, TSetup&& Setup)
        {
            ForEachMeshletBatch(CL, DrawList, Ctx, static_cast<TSetup&&>(Setup),
                                [](const FMeshDrawCommand&) {});
        }

        RHI::FPipelineH      GetOrCreateComputePipeline(FShaderH CS,
                                 TSpan<const RHI::FSpecializationConstant> Constants = {});
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

        /** What a freshly (re)allocated scene buffer holds. Undefined is the honest description of what
         *  RHI::Malloc returns -- a recycling pool hands back the previous tenant's bytes. Only pick it for
         *  a buffer that is provably rewritten in full before anything reads it. */
        enum class EBufferInit : uint8
        {
            Undefined,
            Zeroed,
        };

        // DebugName rides every reallocation, so a fault in one of these resolves to a name in the
        // device-lost report rather than a bare address. Nothing else reads it.
        void ResizeBufferIfNeeded(RHI::FCmdListH CL, FSceneBuffer& Buffer, uint64 NeededSize, float SlackFactor,
                                  uint32& LowUsageCounter, bool bAllowShrink = true,
                                  EBufferInit Init = EBufferInit::Zeroed, const char* DebugName = nullptr);

        // Freed when this slot's previous GPU work has completed.
        void DeferFree(RHI::GPUPtr Ptr);
        void DeferRelease(FSceneImage& Image);
    
    private:
        
        struct FFrozenCull
        {
            TVector<FCullView> Views;
            FVector4           CameraPosition   = {};
            FMatrix4           CameraView       = {};
            FMatrix4           CameraProjection = {};
            FGPUFrustum        Frustum          = {};
            FGPUFrustum        ShadowFrustum    = {};
            FGPUFrustum        CascadeFrustum[NumCascades] = {};
            uint32             CascadeViewBase  = ~0u;
            bool               bValid           = false;
        };
        FFrozenCull FrozenCull;
        
        FSceneBuffer                                        PreSkinnedVerticesBuffer;
        uint32                                              PreSkinnedVerticesLowUsage = 0;

        TArray<uint32,       RHI::kFramesInFlight>          InstanceBufferLowUsage = {};
        TArray<uint64,       RHI::kFramesInFlight>          InstanceBufferSerial = {};
        TArray<uint32, RHI::kFramesInFlight>                RenderBucketRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MeshletDrawListRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MeshDrawArgsRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                InstanceViewRangeRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MeshletBlockRingLowUsage = {};
        // Sharing one buffer would clear the flags the late dispatch still needs to read.
        TArray<FSceneBuffer, 2>                            InstanceVisibilityBuffers = {};
        TArray<uint32, 2>                                  InstanceVisibilityLowUsage = {};
        uint32                                             InstanceVisibilityCapacity = 0;
        uint8                                              InstanceVisibilityWriteIndex = 0;
        uint32                                             LastStaleValidationGeneration = 0;
        TArray<uint32, RHI::kFramesInFlight>                MaterialClassifyRingLowUsage = {};
        TArray<uint32, RHI::kFramesInFlight>                MaterialPixelListRingLowUsage = {};
        TArray<FSceneImage, (int)ENamedImage::Num>          NamedImages = {};

        /** Reconcile cached sample count with the world setting; reallocates every view's MS images when it changes. */
        
        static constexpr uint32                 BLOOM_MIP_COUNT = 8;
        
        static constexpr uint32                 MaxSceneViews = 16;
        TVector<FSceneView>                     SceneViews;
        FSceneView*                             CurrentView = nullptr;
        
        uint32                                  CurrentCameraEarlyView = 0;

        FDelegateHandle                         SwapchainResizedHandle;

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

        static constexpr uint32                 MaxReflectionProbes    = 32;
        static constexpr uint32                 ProbePrefilterBaseSize = 128;
        static constexpr uint32                 ProbePrefilterMips     = 5;

        // Address of this frame's uploaded probe array, and how many entries it holds.
        uint64                                  ProbeBufferAddr  = 0;
        uint32                                  NumActiveProbes  = 0;

        TVector<uint32>                         PendingProbeBakes;
        uint32                                  BakedProbeMask   = 0;
        // Face size the scratch capture cube is currently allocated at; 0 = not yet created.
        uint32                                  ProbeCaptureCubeSize = 0;
        // The reserved FSceneView the six faces render through, held across bakes. -1 = none yet.
        int32                                   ProbeBakeViewIndex = -1;
        uint32                                  ProbeBakeViewSize  = 0;
        TAtomic<uint32>                         CompletedProbeBakes{0};
        bool                                    bCapturingProbe       = false;

        TVector<FGPUReflectionProbe>            LastExtractedProbes;
        TVector<FReflectionProbeCapture>        LastExtractedCaptures;
        // Last global rebake-request counter this scene acted on. See RequestReflectionProbeRebake.
        uint32                                  LastSeenRebakeRequest = 0;
        uint32                                  AlwaysProbeCursor = 0;

        // Addresses of this frame's uploaded spline arrays. Splines are extracted only for components with
        // bSendToGPU, so a world that authors splines purely as data uploads nothing.
        uint64                                  SplineBufferAddr       = 0;
        uint64                                  SplinePointBufferAddr  = 0;
        uint64                                  SplineSampleBufferAddr = 0;
        uint32                                  NumActiveSplines       = 0;

        void ExtractSplines(FEntityRegistry& Registry, FFrameData& Frame);

        void InitReflectionProbeTargets();
        void SyncProbeCaptureCube(uint32 FaceSize);
        void ExtractReflectionProbes(FEntityRegistry& Registry, FFrameData& Frame);
        void ScheduleReflectionProbeBake(FFrameData& Frame);
        // Renders the six faces, copies each into the scratch cube, prefilters into the probe's slice.
        void ReflectionProbeBakePass(RHI::FCmdListH CL);
        
        FEnvironmentParams                      LastUploadedEnvironmentParams = {};
        bool                                    bEnvironmentParamsUploaded    = false;
        
        mutable FSharedMutex                    PipelineCacheMutex;
        THashMap<uint64, RHI::FPipelineH>       PipelineCache;
        THashMap<uint64, RHI::FDepthStencilH>   DepthStateCache;

        
        TVector<CMaterialInterface*>            PendingPostProcessMaterials;
        
        FSceneRoot                                                      SceneRootShared = {};
        uint64                                                          CurrentSceneRootAddr = 0;
        // Builds the per-view FSceneRoot transient (shared addrs + view camera/clusters/IBL) -> address.
        uint64 BuildViewSceneRoot(FSceneView& View, uint64 SceneDataAddr);

        /** Texture-streaming feedback (see RequestTextureResolution in SceneGlobals.slang). One uint per
         *  bindless slot, OR-accumulated by the material lanes, copied to a readback slot and zeroed each
         *  frame. Read kFramesInFlight later, which is when the copy is guaranteed complete. */
        void EnsureStreamingFeedbackBuffer();
        void CollectStreamingFeedback(RHI::FCmdListH CL);
        void PublishStreamingFeedback();

        FSceneBuffer                                        StreamingFeedbackBuffer;
        TArray<FSceneBuffer, RHI::kFramesInFlight>          StreamingFeedbackReadback = {};
        /// Frame the slot was written on, so a slot is only read once its copy has certainly landed.
        TArray<uint64, RHI::kFramesInFlight>                StreamingFeedbackStamp = {};
        uint32                                              StreamingFeedbackSlots = 0;
        uint64                                              StreamingFeedbackFrame = 0;
        
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          RenderBucketRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MeshletDrawListRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MeshDrawArgsRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          SpdCounterRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          LuminanceHistogramRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MeshletBlockRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          BlockDispatchArgsRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          SkinWorkBaseRing = {};
        TArray<uint32,       RHI::kFramesInFlight>                          SkinWorkBaseLowUsage = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          SkinDispatchArgsRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MeshletCullDispatchArgsRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          InstanceViewRangeRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          TotalsRing = {};
        TArray<bool, RHI::kFramesInFlight>                                  TotalsZeroed = {};

        FSceneBuffer GetTotals() const { return TotalsRing[CurrentFrameSlot]; }

        FSceneBuffer                                        RetainedCullEntryBuffer;
        FSceneBuffer                                        RetainedTransformBuffer;
        FSceneBuffer                                        RetainedStaticBuffer;
        FSceneBuffer                                        SurfaceDescBuffer;
        FSceneBuffer                                        BoneArenaBuffer;
        uint32                                              BoneArenaLowUsage = 0;
        FSceneBuffer                                        SkinnedFrameDataBuffer;
        uint32                                              SkinnedFrameDataLowUsage = 0;
        FSceneBuffer                                        SkinnedSlotListBuffer;
        uint32                                              SkinnedSlotListLowUsage = 0;
        TVector<FUIntVector2>                               SkinnedUploadScratch;
        uint32                                              CurrentSkinnedFrameTag = 0;
        TVector<FUIntVector2>                               BoneUploadScratch;
        uint32                                              RetainedCullEntryLowUsage = 0;
        uint32                                              RetainedTransformLowUsage = 0;
        uint32                                              RetainedStaticLowUsage = 0;
        uint32                                              SurfaceDescLowUsage = 0;
        uint32                                              UploadedSurfaceDescs = 0;

        TArray<FSceneBuffer, RHI::kFramesInFlight>          VisibleInstanceRing = {};
        TArray<uint32,       RHI::kFramesInFlight>          VisibleInstanceLowUsage = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>          CullCounterRing = {};

        // Skinned contribution replicated across views, staged once per frame for the V*D upload.
        TVector<FRenderBucketGPU>                           BucketSeedScratch;

        // (base, count) per (skinned instance, view) for the CPU-fed head of the visible buffer, which
        // CullInstances never claims and therefore never writes a range for.

        FSceneBuffer GetVisibleInstances()  const { return VisibleInstanceRing[CurrentFrameSlot]; }
        FSceneBuffer GetCullCounters()      const { return CullCounterRing[CurrentFrameSlot]; }

        // Sends only the arena slices this frame's gather wrote, coalesced. Must run before anything reads
        // Bones() -- the skinning dispatch and the in-draw skinning fallback both do.
        void UploadBoneArena(RHI::FCmdListH CL, const FFrameData& Frame);

        // Ships the per-frame half of every gathered skinned slot's payload, plus the compact slot list the
        // skinning passes iterate. Must run before CullInstances and before the skinning dispatch.
        void UploadSkinnedFrameData(RHI::FCmdListH CL, FFrameData& Frame);

        void DispatchGPUSceneCull(RHI::FCmdListH CL, const FFrameData& Frame);

        void PublishRetainedUpload();

        std::atomic<uint32>                                 RetainedDeviceCapacity{0};

        std::atomic<bool>                                   bDepthPyramidValid{false};

        std::atomic<bool>                                   bCascadePyramidValid{false};

        FMatrix4                                            CascadeHZBViewProjection[NumCascades] = {};
        FVector4                                            CascadeHZBNdcScale[NumCascades] = {};
        bool                                                bCascadeHZBTransformsValid = false;

        float                                               CascadeMinTexels = 1.0f;

        static constexpr uint32                             kTotalsSlots = 8;

        // Must match HISTOGRAM_BINS in LuminanceHistogram.slang and its TILE_DIM^2 thread count.
        static constexpr uint32                             kLuminanceHistogramBins = 256;

        static constexpr uint32                             kAerialLUTSize   = 32;
        static constexpr uint32                             kAerialLUTSlices = 32;

        static constexpr uint32                             kCloudNoiseSize = 128;

        TArray<RHI::GPUPtr, RHI::kFramesInFlight>           MeshletBoundReadback = {};
        uint32                                              LastDrawListRequired = 0;
        uint32                                              LastDrawListOverflowed = 0;
        uint32                                              LastVisibleInstances = 0;
        uint32                                              LastVisibleOverflowed = 0;
        uint32                                              DrawListCapacity = 0;
        uint32                                              BlockListCapacity = 0;
        // A plain running max never decays, pinning the allocation at the session's peak forever.
        struct FDemandWindow
        {
            uint32 Observe(uint32 Demand)
            {
                Current = Math::Max(Current, Demand);
                if (++Frames >= kWindowFrames)
                {
                    Previous = Current;
                    Current  = Demand;
                    Frames   = 0;
                }
                return Math::Max(Current, Previous);
            }

            static constexpr uint32 kWindowFrames = 120;

            uint32 Current  = 0;
            uint32 Previous = 0;
            uint32 Frames   = 0;
        };

        FDemandWindow                                       BlockListDemand;
        FDemandWindow                                       PreSkinDemand;
        uint32                                              PreSkinnedVertexCapacity = 0;
        uint32                                              MeshSubDrawsPerSlice = 1;
        uint32                                              LastBlocksRequested = 0;
        uint32                                              LastPreSkinRequested = 0;
        uint32                                              LastPreSkinOverflowed = 0;
        uint32                                              LastBlocksOverflowed = 0;
        uint32                                              MeshletDrawTagCounter = 0;
        uint32                                              FrameVisibleInstanceCapacity = 0;
        void   UpdateMeshletBoundFeedback(uint8 Slot);
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MaterialClassifyRing = {};
        TArray<FSceneBuffer, RHI::kFramesInFlight>                          MaterialPixelListRing = {};
        
        uint8                                                           CurrentFrameSlot = 0;

        FShadowAtlas                            ShadowAtlas;
        
        THashMap<entt::entity, FTerrainGPUState> TerrainGPUStates;
        
        THashMap<entt::entity, TVector<FParticleGPUState>> ParticleGPUStates;

        FScenePrimitiveSet                      ScenePrimitives;
        TVector<entt::entity>                   MovedTransformScratch;

        // Merge scratch, sized by draw slots (not by entities). Members so capacity survives frames.

        TVector<FShaderH>            BinnedDeferredSlotShaders;
        TVector<uint32>                         BinnedDeferredSlotByMaterial;

        struct FMaterialClassifyLayout
        {
            uint32 NumSlots      = 0;
            uint32 ScreenW       = 0;
            uint32 ScreenH       = 0;
            uint32 PixelCapacity = 0;   // entries the pixel list holds, taken from the allocation
        };
        // Derived once by VisBufferClassifyPass and read by the material and lighting passes. Zeroed at
        // the top of the classify pass before any early return, so a bail-out frame cannot leave the
        // later passes dispatching against a stale slot table.
        FMaterialClassifyLayout                 MaterialClassifyLayout;

        bool BuildDeferredMaterialBinning(RHI::FCmdListH CL);

        TVector<uint32>                         ShadowSizeScratch;
        TVector<uint32>                         ShadowSortedScratch;

        struct FDecalSortEntry { CMaterial* ShaderOwner; int32 SortOrder; FGPUDecal Gpu; };
        TVector<FDecalSortEntry>                DecalSortScratch;
        THashMap<CMaterial*, int32>             DecalGroupMinSort;
        
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

        FImmediateLineRenderer                  ImmediateLines;

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

        TAtomic<uint64>                         PickerCursorPacked = 0;

        void IssuePickerReadback(RHI::FCmdListH CL);
#endif

    };
}
