#pragma once

#include "Lumina.h"
#include "World/ECS/Registry.h"


#include "Renderer/ShaderHandle.h"
#include "SceneBuffer.h"
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
#include "World/Scene/RenderScene/SceneUpscaler.h"
#include "World/Scene/RenderScene/SceneCullContext.h"
#include "World/Scene/RenderScene/ScenePrimitiveSet.h"
#include "World/Scene/RenderScene/TerrainRenderTypes.h"
#include "World/Scene/RenderScene/TexturePaintTypes.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "TaskSystem/FiberSync.h"
#include "World/Entity/Components/PostProcessSettings.h"
#include "World/Entity/Components/CloudComponent.h"

namespace Lumina
{
    class CMesh;
    enum class ESMAAMode : uint8;
    struct FLineBatcherComponent;
    struct FTriangleBatcherComponent;
    struct SDirectionalLightComponent;
    struct SSpotLightComponent;
    struct SPointLightComponent;
    struct SAreaLightComponent;
    struct SExponentialHeightFogComponent;
    struct SEnvironmentComponent;
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
    class CStaticMesh;
    class CMaterial;
    enum class EMaterialShaderStage : uint8;

    /** The engine's default scene renderer: GPU-driven meshlet culling into a visibility buffer, then
     *  per-material GBuffer resolve and a clustered lighting pass. RenderSceneFactory falls back to this
     *  whenever no override is installed. */
    class FDefaultSceneRenderer : public IRenderScene
    {
    public:

        FDefaultSceneRenderer(CWorld* InWorld);
        ~FDefaultSceneRenderer() override;
        LE_NO_COPYMOVE(FDefaultSceneRenderer);

        // A skinned slot past the frame data's current size, written by the merge once it has grown the array.
        struct FDeferredSkinnedSlot
        {
            uint32              InstanceSlot;
            FSkinnedFrameData   Data;
        };

        struct CACHE_ALIGN FThreadLocalDrawData
        {
            TVector<uint32>                     SkinnedSlots;
            TVector<FDeferredSkinnedSlot>       DeferredSlots;
            TVector<FUIntVector2>               BoneUploadRanges;

            FSceneRenderStats                   Stats = {};
            bool                                bTouched = false;

            FThreadLocalDrawData() = default;
            FThreadLocalDrawData(const FThreadLocalDrawData&) = delete;

            FThreadLocalDrawData(FThreadLocalDrawData&&) = default;
            FThreadLocalDrawData& operator=(FThreadLocalDrawData&&) noexcept = default;

            void ResetForFrame()
            {
                SkinnedSlots.clear();
                DeferredSlots.clear();
                BoneUploadRanges.clear();
                Stats = {};
                bTouched = false;
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
            // Point lights only, one bit per cube face that some view can see a receiver through.
            uint32      FaceMask = kAllCubeFaces;
        };

        // Gathered by the light tasks and resolved serially, since resolving touches the material.
        struct FLightFunctionRequest
        {
            CMaterialInterface* Material = nullptr;
            uint32      LightIndex = 0;
            ELightFlags Type = ELightFlags::None;
            FVector3    Position = FVector3(0.0f);
            // The way the light travels, so a spot's aim and the sun's opposite of its to-light direction.
            FVector3    Forward = FVector3(0.0f, 0.0f, -1.0f);
            FVector3    Right = FVector3(1.0f, 0.0f, 0.0f);
            FVector3    Up = FVector3(0.0f, 1.0f, 0.0f);
            float       ProjectionScale = 1.0f;
        };

        static constexpr uint32 kAllCubeFaces = 0x3Fu;
        // The cube faces of a point light whose region reaches any relevance view, since the rest shadow nothing on screen.
        uint32 VisibleCubeFaces(const FVector3& Position, float Radius) const;

        // This frame's gathered scene. Extract writes it, RenderView reads it, same frame.
        struct FFrameData
        {
            struct FDecalBatch
            {
                FRenderMaterialShaders Shaders;
                uint32      FirstInstance;
                uint32      Count;
            };

            // A run of sprite instances sharing one texture and render state; one instanced draw per batch.
            struct FSpriteBatch
            {
                uint32 TextureIndex  = 0;
                uint32 FirstInstance = 0;
                uint32 Count         = 0;
                bool   bDepthTest    = true;
                bool   bDoubleSided  = true;
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

            /** One species the scatter dispatches for, flattened from the material's GrassOutputs. */
            struct FGrassSpeciesExtract
            {
                CStaticMesh* Mesh      = nullptr;
                // Resolved during extraction, since worlds render in parallel and the cache is not safe to mutate there.
                uint32  ResolveHandle  = INVALID_MESH_RESOLVE_HANDLE;
                FName   TypeName;
                uint32  LayerIndex     = 0;
                float   CellSize       = 1.0f;   // world units between candidates, from density
                float   MinWeight      = 0.25f;
                float   ScaleMin       = 1.0f;
                float   ScaleMax       = 1.0f;
                float   ZOffset        = 0.0f;
                float   AlignToNormal  = 0.0f;
                float   MaxSlopeCos    = 0.0f;
                float   CullDistance   = 0.0f;
                uint32  Seed           = 0;
                bool    bRandomYaw     = true;
                bool    bReceiveShadow = true;
                bool    bContactShadows = false;
                float   FullDensityDistance = 0.0f;
            };

            struct FTerrainExtract
            {
                ECS::FEntity        Entity;
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
                bool                bMasked         = false;

                bool                bStructuralChange = false;

                /** Empty unless the terrain carries SGrassComponent and its material declares species. */
                TVector<FGrassSpeciesExtract> Grass;
                uint32              GrassMaxInstances = 0;

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
                ECS::FEntity            Entity;
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

                // Particle-domain material stages; both null leaves the emitter on the built-in sprite pair.
                FShaderH                MaterialVertexShader = {};
                FShaderH                MaterialPixelShader  = {};
                // Materials() slot the stages read, or -1 when this emitter has no material.
                int32                   MaterialIndex        = -1;
                // Authored on the material, so a Particle material draws the same way from every emitter.
                EBlendMode              MaterialBlendMode    = EBlendMode::Translucent;
                bool                    bMaterialWritesDepth = false;

                TVector<FVector4>       ModuleParamValues;

                // Floats per particle in the declared-attribute buffer; sizes the parallel allocation.
                uint32                  AttributeFloatCount = 1u;

                int32                   RenderAttrSlots[ParticleRenderAttribute::Count];

                // Meshlet header slots of the render and emission meshes, zero when absent or not resident.
                uint32                  MeshletHeaderSlot   = 0u;
                uint32                  MeshletCount        = 0u;
                uint32                  EmissionMeshSlot    = 0u;
                // The parent emitter in the same entity when this one is a sub-emitter, else -1.
                int32                   SourceEmitterIndex  = -1;
                // Event lists this emitter's particles raise because a sub-emitter consumes them or scripts read them.
                uint32                  RaisedEventMask     = 0u;
                bool                    bReportCollisions   = false;
                float                   RaisedEventRate     = 0.0f;
                TVector<FParticleEventGPU> ScriptEmits;
            };

            struct FCaptureViewData
            {
                FSceneGlobalData    SceneGlobalData = {};
                FViewVolume         ViewVolume      = {};
                uint32              CameraViewIndex = Constants::kIndexNoneU32;   // its cull-view index (frustum-only)
                int32               SceneViewIndex  = -1;    // index into FDefaultSceneRenderer::SceneViews
            };

            FViewVolume                      ViewVolume = {};
            FFrustum                         CameraFrustum = {};
            FSceneGlobalData                 SceneGlobalData = {};
            float                            CachedWorldDeltaTime = 0.0f;
            bool                             bExtractedThisFrame = false;
            FSceneRenderStats                FrameStats = {};

            struct FDeferredMaterialEntry
            {
                uint32              MaterialIndex;
                FShaderH DeferredShader;
                // Bit 0 when a static instance binds this material, bit 1 when a skinned one does.
                uint8               InstanceKinds = 0;
            };

            struct FGeometry
            {
                // CPU mirror, COMPACTED to this frame's gathered skeletons. 48B/bone (last row dropped).
                TVector<FBoneTransform>          BonesData;
                // Extent of the bone arena this frame; only the ranges below are actually uploaded.
                uint32                           BoneCount = 0;
                // Slices whose pose or base moved this frame. Everything else is already resident.
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
                    bool                        bFullStatic = false;
                    uint32                      SlotCount = 0;

                    TVector<uint32>             DirtySlots;
                    TVector<uint32>             DirtyStaticSlots;

                    bool                        bSurfaceDescsChanged = false;
                    uint32                      SurfaceDescCount = 0;

                    uint32                      MaxSurfaceDescMeshlets = 0;

                    uint32                      MeshletVisibilityWords = 0;
                } RetainedUpload;
            } Geometry;

            struct FViews
            {
                TVector<FCullView>               CullViews;
                // The primary camera's LOD inputs, ready before the skinned gather that reads them on workers.
                FCullView                        CameraLODView       = {};
                uint32                           NumDrawsPerView     = 0;
                uint32                           CascadeViewBase     = Constants::kIndexNoneU32;
                uint32                           NumCascadeViews     = 0;
                // The far cascade, when present, follows these and takes only far casters.
                uint32                           NumNearCascadeViews = 0;
                TVector<uint32>                  PointShadowCullViewBases;
                TVector<uint32>                  SpotShadowCullViewBases;
                TVector<FCaptureViewData>        CaptureViews;
            } Views;

            struct FLighting
            {
                FSceneLightData                  LightData = {};
                // Written densely from index 0, so the upload sends only the live prefix of each.
                TArray<FLight, MAX_LIGHTS>            Lights = {};
                TArray<FLightShadowData, MAX_SHADOWS> Shadows = {};
                TArray<TVector<FLightShadow>, (uint32)ELightType::Num> PackedShadows = {};
                TAtomic<uint32>                  ShadowDataCount = 0;
                // Camera, captures and the baking probe's faces. Rebuilt each extract before the light tasks.
                TVector<FFrustum>                RelevanceFrusta;
                TVector<FShadowRequest>          ShadowRequests;
                FMutex                           ShadowRequestMutex;

                struct FLightFunctionDraw
                {
                    FRenderMaterialShaders       Shaders;
                    FLightFunctionRequest        Request;
                    uint32                       MaterialIndex = 0;
                    uint32                       Slot = 0;
                };
                TVector<FLightFunctionRequest>   LightFunctionRequests;
                FMutex                           LightFunctionRequestMutex;
                TVector<FLightFunctionDraw>      LightFunctionDraws;
                TArray<FLightFunction, MAX_LIGHT_FUNCTIONS> LightFunctions = {};
                TVector<FShadowTile>             AtlasTiles;

                // What the sun's screen-space far shadow pass traces this frame.
                struct FSunFarShadow
                {
                    bool                         bTerrain = false;
                    FVector3                     ToSun = FVector3(0.0f, 1.0f, 0.0f);
                    float                        TanHalfAngle = 0.0f;
                    float                        TerrainDistance = 0.0f;

                    bool                         bDistanceField = false;
                    float                        DFNear = 0.0f;
                    float                        DFFar = 0.0f;
                    float                        DFMinRadius = 0.0f;
                    // The grid the casters are binned into, square in the plane facing the sun.
                    FVector3                     SunRight = FVector3(1.0f, 0.0f, 0.0f);
                    FVector3                     SunUp = FVector3(0.0f, 0.0f, 1.0f);
                    FVector2                     GridOrigin = FVector2(0.0f);
                    float                        GridCellSize = 1.0f;
                } SunFarShadow;
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

                TVector<FGPUSprite>              SpriteInstances;
                TVector<FSpriteBatch>            SpriteBatches;

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

                TVector<FGPUFogVolume>           FogVolumes;
                uint32                           FarShaftSteps        = 0;
                // Any local light this frame scatters through fog, which is what the froxel inject walks cluster lists for.
                bool                             bLocalVolumetricLights = false;
                float                            FarShaftDistance     = 4000.0f;
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
                TVector<ECS::FEntity>            LiveTerrainEntities;
                TVector<FParticleExtract>        ParticleExtracts;
                TVector<ECS::FEntity>            LiveParticleEntities;
                TVector<FParticleShapeGPU>       ParticleAttractors;
                TVector<FParticleShapeGPU>       ParticleColliders;
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
            SMAAEdgeMask,
            SMAAArea,
            SMAASearch,
            GTAOWorkingDepth,
            GTAOEdges,
            GTAO,
            GTAODenoise,
            GTAOBlur,
            SunFarShadowMask,
            SSRTrace,
            SSRPyramid,
            SSRSurface,
            SSRMipLevel,
            FogShaft,
            FogShaftDepth,
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
            MaterialSlot,
            Accum,
            Revealage,
            WaterRefraction,
            SceneDepthCopy,
            DBufferA,
            DBufferB,
            DBufferC,
            DBufferD,
            AdaptedLuminance,
            FroxelScatter,
            FroxelIntegrated,
            AerialInScatter,
            AerialTransmittance,
            CloudNoise,
            CloudScatter,
            CloudDepth,
            CloudShadow,
            BRDFLut,
            SkyCube,
            SkyIrradiance,
            SkyPrefilter,
            ProbeCaptureCube,
            ProbePrefiltered,
            Velocity,
            TemporalHistoryA,
            TemporalHistoryB,
            UpscaledHDR,

            #if USING(WITH_EDITOR)
            PointLightIcon,
            DirectionalLightIcon,
            SkyLightIcon,
            SpotLightIcon,
            CameraIcon,
            CharacterIcon,
            ParticleSystemIcon,
            AudioSourceIcon,
            AudioListenerIcon,
            #endif

            Num,
        };

        // Per-output-view rendering state.
        struct FSceneView
        {
            FSceneImage                                     Output;
            // The render resolution, which every scene pass and ScreenSize use.
            FUIntVector2                                    Size = FUIntVector2(0);
            // What Output and every pass after the upscale draw at.
            FUIntVector2                                    DisplaySize = FUIntVector2(0);
            // Resolved each extract, since a plugin can register or drop one between frames.
            IUpscaler*                                      Upscaler = nullptr;
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
            RHI::EQueueType                                 CloudNoiseQueue  = RHI::EQueueType::Graphics;
            RHI::FGPUAllocation                                    ClusterBuffer;
            RHI::FGPUAllocation                                    ClusterLightMaskBuffer;
            RHI::FGPUAllocation                                    ClusterRangeBuffer;
            FMatrix4                                        LastClusterInvProjection = FMatrix4(0.0f);
            FVector2                                        LastClusterNearFar       = FVector2(0.0f);
            FUIntVector2                                    LastClusterScreenSize    = FUIntVector2(0);
            bool                                            bClusterGridDirty        = true;
            // Drives both the jitter parity and the history ping-pong, so the two cannot disagree.
            uint32                                          TemporalFrameIndex       = 0;
            uint32                                          PendingTemporalFrameIndex = 0;
            bool                                            bTemporalHistoryValid    = false;
            FMatrix4                                        PrevViewProjection       = FMatrix4(1.0f);
            // Committed with the frame index, so a bailed extract cannot leave the two describing different frames.
            FMatrix4                                        PendingViewProjection    = FMatrix4(1.0f);
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

        RHI::FGPUAllocation GetPreSkinnedVerticesBuffer() const { return PreSkinnedVerticesBuffer; }
        const FSceneImage& GetNamedImage(ENamedImage Image) const { return CurrentView ? CurrentView->Images[(int)Image] : NamedImages[(int)Image]; }

        RHI::FGPUAllocation GetRenderBuckets()    const { return RenderBucketRing[CurrentFrameSlot]; }
        RHI::FGPUAllocation GetMeshletDrawList()  const { return MeshletDrawListRing[CurrentFrameSlot]; }
        RHI::FGPUAllocation GetMeshDrawArgs()     const { return MeshDrawArgsRing[CurrentFrameSlot]; }
        RHI::FGPUAllocation GetMeshletBlocks()    const { return MeshletBlockRing[CurrentFrameSlot]; }
        // Read by both meshlet-cull dispatches and written by neither: that is what makes them partition.
        RHI::FGPUAllocation GetInstanceVisibilityPrev()  const { return InstanceVisibilityBuffers[InstanceVisibilityWriteIndex ^ 1u]; }
        RHI::FGPUAllocation GetInstanceVisibilityWrite() const { return InstanceVisibilityBuffers[InstanceVisibilityWriteIndex]; }
        RHI::FGPUAllocation GetMeshletVisibility() const       { return MeshletVisibilityBuffer; }
        RHI::FGPUAllocation GetBlockDispatchArgs() const { return BlockDispatchArgsRing[CurrentFrameSlot]; }
        RHI::FGPUAllocation GetSkinDispatchArgs()  const { return SkinDispatchArgsRing[CurrentFrameSlot]; }
        /** One workgroup per written meshlet block, sized by BuildMeshletCullArgs. */
        RHI::FGPUAllocation GetMeshletCullDispatchArgs() const { return MeshletCullDispatchArgsRing[CurrentFrameSlot]; }
        /** (base, count) per (visible instance, view): the one place the per-view meshlet range is
            decided. CullInstances writes it when it reserves; BuildMeshletBlocks reads it to append. */
        RHI::FGPUAllocation GetInstanceViewRanges() const { return InstanceViewRangeRing[CurrentFrameSlot]; }
        // Every counter a pass accumulates into this frame lives in one scratch block, zeroed once in RenderView.
        RHI::FGPURange GetCullCounters()      const { return FrameScratchRange(kScratchCullCountersOffset, sizeof(uint32) * 4); }
        RHI::FGPURange GetSpdCounter(uint32 Index) const { return FrameScratchRange(kScratchSpdCountersOffset + Index * sizeof(uint32), sizeof(uint32)); }
        RHI::FGPURange GetLuminanceHistogram() const { return FrameScratchRange(kScratchHistogramOffset, sizeof(uint32) * kLuminanceHistogramBins); }
        RHI::FGPURange GetGrassCursor(uint32 Index) const { return FrameScratchRange(kScratchGrassCursorsOffset + Index * sizeof(uint32), sizeof(uint32)); }

        /** Per-material counts, starts, scatter cursors, dispatch args and the frame pixel total. */
        RHI::FGPUAllocation GetMaterialClassify()  const { return MaterialClassifyRing[CurrentFrameSlot]; }
        /** One packed screen position per classified pixel, grouped into one contiguous run per material. */
        RHI::FGPUAllocation GetMaterialPairList() const { return MaterialPairList; }

        uint32 GetDisplayResourceID() const override;
        bool HasCompositedFrame() const override { return FramesComposited > 0; }

        RUNTIME_API void SettleResolveWork(int32 MaxIterations = 8);

        /** Editor-only tripwire: warns if any dynamic-mesh surface is STILL resolved against superseded
            shaders after a full resolve pass, i.e. a resolve gate is missing an input. */
        void ValidateNoStaleResolves(ECS::FRegistry& Registry);
        RHI::FTextureH GetDisplayTexture() const override { return SceneViews[0].Output.Texture; }
        const FSceneImage& GetDisplayImage() const { return SceneViews[0].Output; }
        const FSceneImage& GetPrimaryNamedImage(ENamedImage Image) const { return SceneViews[0].Images[(int)Image]; }
        FUIntVector2 GetRenderExtent() const override;
        ECS::FEntity GetEntityAtPixel(uint32 X, uint32 Y) const override;
        #if USING(WITH_EDITOR)
        void SetPickerCursor(uint32 X, uint32 Y, bool bOverViewport) override;
        #endif
        const FShadowAtlas* GetShadowAtlas() const override { return &ShadowAtlas; }

        RHI::FPipelineH GetCallbackPipeline(const FRenderPipelineDesc& Desc, TSpan<const RHI::FColorTarget> ColorTargets, EFormat DepthFormat) override;
        RHI::FPipelineH GetCallbackComputePipeline(FShaderH ComputeShader) override;

    private:
        
        void InitBuffers();
        void InitViewImages(FSceneView& View);

        void NameOwnedImages(TArray<FSceneImage, (int)ENamedImage::Num>& Images);
        void ReleaseViewImages(FSceneView& View);
        void InitFrameResources();

        // Shared body of SwapchainResized / SetPrimaryViewSize. Unconditional: callers decide whether the
        // new size is worth the WaitDeviceIdle this costs.
        RUNTIME_API void ResizePrimaryView(const FUIntVector2& NewSize);

        // Applies a deferred SetPrimaryViewSize. Called from Extract, which only runs for a ticking world.
        void ApplyPendingPrimarySize();

        // The primary view's render size for its display size, from the screen percentage and the upscaler.
        static FUIntVector2 ComputeRenderSize(const FUIntVector2& DisplaySize, const IUpscaler* Upscaler);

        // Resolves the primary view's upscaler and reallocates when the screen percentage or upscaler changed it.
        void RefreshPrimaryRenderSize();

        // Editor clicks arrive in display pixels and the picker is drawn at render resolution.
        FUIntVector2 DisplayToPickerTexel(uint32 X, uint32 Y) const;

        /** Cleared the first time something sizes the primary view explicitly (an editor tool panel). */
        bool bPrimaryTracksSwapchain = true;

        FSceneView& AddSceneView(const FUIntVector2& Size, bool bPrimary);
        
        void RenderCaptureView(RHI::FCmdListH CL);

        //~ Render callbacks, which run on the primary view only.

        FRenderContext MakeRenderContext(RHI::FCmdListH CL, ERenderStage Stage);
        void RunRenderCallbacks(RHI::FCmdListH CL, ERenderStage Stage);
        void CustomDepthPass(RHI::FCmdListH CL);
        void RunShadowRenderCallbacks(RHI::FCmdListH CL, EShadowViewType Type, uint32 Index, const FMatrix4& ViewProjection,
                                      const RHI::FRect& Tile, EFormat DepthFormat);
        bool HasRenderCallbacks(ERenderStage Stage) const;
        void PrepareSceneColorForCallbacks(RHI::FCmdListH CL);

        // Set when a scene with nothing else writing HDR had it cleared for callbacks, so later passes load it.
        bool bSceneColorClearedForCallbacks = false;
        void RestoreAfterRenderCallbacks(RHI::FCmdListH CL);

        FSceneGlobalData MakeSecondaryViewGlobals(const FSceneGlobalData& ViewGlobals);

        /** Makes View current AND brings its on-demand targets in line with what this frame actually
         *  draws. Out of line for that second half -- it is the one place every rendered view passes
         *  through, so a view can never reach a pass with an optional target missing. */
        void PointAtView(FSceneView& View);

        /** Targets for features a scene may contain none of: OIT translucency (Accum, Revealage),
         *  decals (DBufferA/B/C) and water (WaterRefraction). Sized into every view up front
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
        // Set once the sky targets hold the no-environment black, so later frames without one skip the clears.
        bool bSkyTargetsBlack = false;

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
        void BuildDepthPyramid(RHI::FCmdListH CL, const FSceneImage& Source, const FSceneImage& Pyramid, bool bReduceMax,
                               uint32 SpdCounterIndex);

        // Sizes and zeroes this frame's scratch counters, the one fill and barrier the counters share.
        void BeginFrameScratch(RHI::FCmdListH CL, const FFrameData& Frame);
        RHI::FGPURange FrameScratchRange(uint64 Offset, uint64 Size) const
        {
            return { FrameScratchRing[CurrentFrameSlot].Gpu + Offset, Size };
        }
        void ClusterBuildPass(RHI::FCmdListH CL);
        void LightCullPass(RHI::FCmdListH CL);
        void PointShadowPass(RHI::FCmdListH CL);
        void SpotShadowPass(RHI::FCmdListH CL);
        void CascadedShowPass(RHI::FCmdListH CL, uint32 CascadeViewBase);
        void DecalPass(RHI::FCmdListH CL);
        void VisBufferPass(RHI::FCmdListH CL, uint32 ViewIndex, bool bClear,
                           ECullPhase::Type Phase = ECullPhase::Early);
        void VisBufferClassifyPass(RHI::FCmdListH CL);
        void MaterialGBufferPass(RHI::FCmdListH CL);
        void DeferredLightingPass(RHI::FCmdListH CL);
        void LightFunctionPass(RHI::FCmdListH CL);
        #if USING(WITH_EDITOR)
        void PickerResolvePass(RHI::FCmdListH CL);
        void SelectionOutlinePass(RHI::FCmdListH CL);
        FSceneBuffer SelectionTileStampBuffer { "Editor.SelectionOutlineTiles", 1.5f, EBufferInit::Zeroed };
        FSceneBuffer SelectionTileListBuffer { "Editor.SelectionOutlineTileList", 1.5f };
        FSceneBuffer SelectionOutlineDrawArgsBuffer { "Editor.SelectionOutlineDrawArgs", 1.0f, EBufferInit::Zeroed };
        uint32       SelectionTileStamp = 0;
        #endif
        #if !defined(LE_SHIPPING)
        void SceneDebugViewPass(RHI::FCmdListH CL);
        #endif
        bool BindShadowBatchPipeline(RHI::FCmdListH CL, const FMeshDrawCommand& Batch,
                                    FShaderH PixelShader);

        void DrawShadowBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, bool bUseMesh,
                             uint32 CullViewIndex, int32 ShadowDataIndex, int32 ShadowViewIndex,
                             const FUIntVector2& ViewportExtent);

        // The Vulkan minimum for maxViewports under multiViewport, and the size of the RHI's fixed viewport array.
        static constexpr uint32 kMaxShadowViewportsPerDraw = 16;

        struct FShadowViewRun
        {
            TVector<FShadowRasterViewGPU> Views;
            TVector<RHI::FRect>           Tiles;
            uint32                        FirstCullView = Constants::kIndexNoneU32;
            bool                          bContiguous   = true;

            void Add(uint32 CullView, int32 ShadowDataIndex, int32 ViewIndex, const RHI::FRect& Tile)
            {
                if (Views.empty())
                {
                    FirstCullView = CullView;
                }
                bContiguous = bContiguous && CullView == FirstCullView + (uint32)Views.size();
                Views.push_back({ ShadowDataIndex, ViewIndex, { (float)(Tile.MaxX - Tile.MinX), (float)(Tile.MaxY - Tile.MinY) } });
                Tiles.push_back(Tile);
            }
        };

        // One indirect draw per batch for each group of up to 16 views, or false when the run cannot be drawn that way.
        bool DrawShadowViewsTogether(RHI::FCmdListH CL, const FShadowViewRun& Run, FShaderH PixelShader);
        

        void ApplyCullFreeze(FFrameData& Frame);
        void DrawFrozenCullFrustum(const FFrameData& Frame);
        void DropStaleFrozenOcclusion(FFrameData& Frame) const;
        bool CaptureCascadeShadowFit(const FFrameData& Frame);
        void RestoreCascadeShadowFit(FFrameData& Frame) const;

        void BillboardPass(RHI::FCmdListH CL);
        void WidgetPass(RHI::FCmdListH CL);
        void SpritePass(RHI::FCmdListH CL);
        void TextPass(RHI::FCmdListH CL);
        void DebugTextPass(RHI::FCmdListH CL);
        void WidgetPickerPass(RHI::FCmdListH CL);
        void ParticleSimulatePass(RHI::FCmdListH CL);
        void ParticleSortPass(RHI::FCmdListH CL);
        void ParticleRenderPass(RHI::FCmdListH CL);
        void ParticleShadowCasters(RHI::FCmdListH CL, int32 SunShadowDataIndex);

        // Exact-size free lists, so effects that come and go reuse their buffers instead of reallocating.
        RHI::FGPUAllocation AcquireParticleBuffer(uint64 Size, const char* DebugName);
        void ReleaseParticleBuffer(RHI::FGPUAllocation& Allocation, uint64 Size);
        void ReleaseParticleState(FParticleGPUState& State);
        bool EnsureParticleBuffers(RHI::FCmdListH CL, const FFrameData::FParticleExtract& Item, FParticleGPUState& State);
        bool IsParticleEmitterCulled(const FFrameData::FParticleExtract& Item, bool bForDraw) const;
        void SortParticlesGlobal(RHI::FCmdListH CL, FParticleGPUState& State, uint32 SortMode, uint32 VertsPerParticle);
        void TerrainUpdatePass(RHI::FCmdListH CL);
        void TerrainCullPass(RHI::FCmdListH CL);

        // Every terrain pass asks the same question, so a terrain skipped by the cull is never drawn with last frame's count.
        bool IsTerrainInView(const FFrameData::FTerrainExtract& Terrain) const;

        /** Generates grass instances on the GPU for every species the visible terrains declare. */
        void GrassScatterPass(RHI::FCmdListH CL);
        // Returns whether it cleared the VisBuffer and depth, which it does when bClear and any terrain drew.
        bool TerrainDepthPrePass(RHI::FCmdListH CL, bool bClear);
        void TerrainRenderPass(RHI::FCmdListH CL);
        // The prefilter reads scene depth into GTAO's own working depth, and the trace reads only that.
        enum class EGTAOStage : uint8 { Prefilter, Trace };
        bool GTAOPass(RHI::FCmdListH CL, EGTAOStage Stage);
        bool WantsSunFarShadowMask() const;
        bool SunFarShadowPass(RHI::FCmdListH CL);
        bool SunDFShadowCullPass(RHI::FCmdListH CL);
        void TransparentPass(RHI::FCmdListH CL);
        void OITResolvePass(RHI::FCmdListH CL);
        void UnorderedTranslucentPass(RHI::FCmdListH CL);
        // Single source of truth for every fog consumer, including the translucent material pass.
        void PublishFogGlobals(FSceneGlobalData& Globals) const;
        bool CloudShadowMapPass(RHI::FCmdListH CL);
        bool BakeCloudNoiseIfNeeded(RHI::FCmdListH CL);
        void FroxelInjectPass(RHI::FCmdListH CL);
        void FroxelIntegratePass(RHI::FCmdListH CL);
        bool AerialPerspectivePass(RHI::FCmdListH CL);
        bool VolumetricCloudPass(RHI::FCmdListH CL);
        // One HDR read-modify-write for every term the three passes above produced a volume for.
        void FogShaftPass(RHI::FCmdListH CL);
        void AtmosphereCompositePass(RHI::FCmdListH CL);
        void ScreenSpaceReflectionsPass(RHI::FCmdListH CL);
        void WaterPass(RHI::FCmdListH CL);
        void UnderwaterPass(RHI::FCmdListH CL);
        void EnvironmentPass(RHI::FCmdListH CL);
        void SkyCubeCapturePass(RHI::FCmdListH CL);
        void IrradianceConvolutionPass(RHI::FCmdListH CL);
        void PrefilterEnvMapPass(RHI::FCmdListH CL);
        void BatchedLineDraw(RHI::FCmdListH CL);
        void BatchedTriangleDraw(RHI::FCmdListH CL);
        void DepthOfFieldPass(RHI::FCmdListH CL);
        void BloomPass(RHI::FCmdListH CL);
        void AutoExposurePass(RHI::FCmdListH CL);
        void UpscalePass(RHI::FCmdListH CL);
        void SpatialUpscalePass(RHI::FCmdListH CL);
        void ToneMappingPass(RHI::FCmdListH CL);
        void PostProcessMaterialPass(RHI::FCmdListH CL);
        void SMAAEdgeDetectionPass(RHI::FCmdListH CL);
        void SMAABlendWeightPass(RHI::FCmdListH CL);
        void SMAANeighborhoodBlendPass(RHI::FCmdListH CL);
        void VelocityPass(RHI::FCmdListH CL);
        void TemporalResolvePass(RHI::FCmdListH CL);
        #if !defined(LE_SHIPPING)
        void VelocityDebugPass(RHI::FCmdListH CL);
        #endif
        //~ End Render Passes

        // SMAA steps aside for a temporal upscaler, and T2x drops to 1x under a spatial one, whose jitter it was not built for.
        static ESMAAMode GetViewSMAAMode(const FSceneView& View);
        static bool IsTemporalAAEnabledFor(const FSceneView& View);
        static bool IsTemporalUpscalerFor(const FSceneView& View);
        static bool IsUpscalingView(const FSceneView& View);
        // What tonemapping and post-process materials read, the upscaled image when the view has one.
        const FSceneImage& GetPostInputImage() const;
        bool IsTemporalAAEnabled() const;
        bool IsTemporalResolveReady() const;
        bool IsVelocityDebugActive() const;
        bool IsVelocityWanted() const;
        // Slot the neighborhood blend writes this frame; the resolve reads the other one as history.
        ENamedImage GetTemporalCurrentImage() const;
        ENamedImage GetTemporalHistoryImage() const;
        static FVector2 GetTemporalJitterNDC(const FSceneView& View, uint32 FrameIndex);
        // How many jitter positions one cycle visits, two for T2x and the upscaler's count for a temporal upscaler.
        static uint32 GetTemporalPhaseCount(const FSceneView& View);
        // AreaTex slice the blend-weight pass resolves against, matched to the jitter this frame rendered.
        FVector4 GetSMAASubsampleIndices() const;

        // Extract-phase half: ECS reads + parallel Process* tasks + cull/shadow setup.
        void CompileDrawCommands_Extract();

        void PrepareGatherScratch(FFrameData& Frame);
        void ScheduleSkinnedGather(FTaskGraph& Graph);
        void ScheduleLineBatching(FTaskGraph& EmitGraph, ECS::FRegistry& Registry);

        // One emit task per primitive family, all joined before the serial tail below runs.
        void ExtractBatchedTriangles(ECS::FRegistry& Registry);
        void ExtractWidgets(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractText(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractSprites(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractBillboards(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractSelection(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractTerrain(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractParticles(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractDecals(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractWater(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractDebugText(FFrameData& Frame);

        // Serial after the light tasks; the skylight reads the sun the directional pass resolved.
        const SEnvironmentComponent* ExtractEnvironment(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractSkyLight(ECS::FRegistry& Registry, FFrameData& Frame, const SEnvironmentComponent* ActiveEnv);
        void ExtractFog(ECS::FRegistry& Registry, FFrameData& Frame);

        // Render-phase half: buffer resize + upload commands; reads extract-phase state.
        void CompileDrawCommands_Render(RHI::FCmdListH CL);

        // Serial pre-pass so the parallel gather is pure reads; skipped when nothing changed.
        void ResolveDirtyMeshComponents();

        void ResolveDynamicMeshMaterials(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker);

        uint32 LastResolvedPendingGeneration = 0;
        uint32 LastDynamicResolveGeneration = 0;

        // Dirty-slot runs for the partial retained upload; keeps capacity.
        TVector<FUIntVector2> RetainedRunScratch;

        // Reused to flatten a component's material overrides; keeps capacity.
        TVector<CMaterialInterface*> ResolveOverrideScratch;

        // Routes this frame's transform + component changes into the primitive table. O(changed).
        void SyncScenePrimitives();

        // Skeletal primitives only; statics flow through the retained batch registry, not a per-frame gather.
        void CullSkinnedPrimitives(const Task::FParallelRange& Range, FThreadLocalDrawData& Local);
        void LayoutSkinnedBoneSlices(TVector<FThreadLocalDrawData>& ThreadLocal);
        void EmitSkinnedPrimitives(const Task::FParallelRange& Range, FThreadLocalDrawData& Local);

        void BuildSceneCullContext();
        void MergeMeshDrawData(TVector<FThreadLocalDrawData>& ThreadLocal);

        FThreadLocalDrawData& AcquireThreadLocalDrawData(uint32 Slot);

        // Per-primitive base in the bone arena; kNoBoneSlice for anything not gathered this frame.
        TVector<uint32>                        BoneSliceByPrimitive;
        // Primitive indices the cull kept, dense, filled straight from the parallel cull via a cursor.
        TVector<uint32>                        SkinnedCandidates;
        TVector<uint32>                        SkinnedCandidateBones;
        TVector<uint32>                        PendingSliceAllocs;
        TAtomic<uint32>                        SkinnedCandidateCursor{0};
        TAtomic<uint32>                        PendingSliceCursor{0};
        uint32                                 SkinnedCandidateCount = 0;
        // Snapshot of the primitive set's dense skeletal list, captured before the parallel cull.
        const uint32*                          SkeletalPrimitiveIndices = nullptr;
        // Frames a slice survives ungathered, so a mesh flicking through the cull does not thrash its base.
        static constexpr uint32                kBoneSliceGraceFrames = 300;
        uint32                                 BoneSliceFrameNumber = 0;
        TVector<FUIntVector2>                  BoneUploadScratch;

        // Lights one parallel range builds, committed with one reservation so the ranges never contend per light.
        struct FLightBatch
        {
            static constexpr uint32 Capacity = 32;
            FLight         Lights[Capacity];
            // LightIndex holds the slot within Lights until the batch is committed.
            FShadowRequest Shadows[Capacity];
            FLightFunctionRequest LightFunctions[Capacity];
            uint32         NumLights  = 0;
            uint32         NumShadows = 0;
            uint32         NumLightFunctions = 0;
        };

        void ProcessPointLight(const SPointLightComponent& PointLight, const STransformComponent& TransformComponent, FLightBatch& Batch, TAtomic<uint32>& LightCount);
        void ProcessSpotLight(const SSpotLightComponent& SpotLight, const STransformComponent& TransformComponent, FLightBatch& Batch, TAtomic<uint32>& LightCount);
        void ProcessAreaLight(const SAreaLightComponent& AreaLight, const STransformComponent& TransformComponent, FLightBatch& Batch, TAtomic<uint32>& LightCount);
        void FlushLightBatch(FLightBatch& Batch, TAtomic<uint32>& LightCount);
        void ProcessDirectionalLight(const SDirectionalLightComponent& DirectionalLight);

        void AllocateShadowTiles();
        // Hands each resolvable light function an atlas tile and flags its light, serially after the light tasks.
        void ResolveLightFunctions();
        
        void BuildCullViews(const FViewVolume& ViewVolume);
        
        uint32 PrepareBatchedLines(FLineBatcherComponent& Batcher);
        void   BatchLineChunks(const Task::FParallelRange& Range);
        void   FinalizeBatchedLines(FLineBatcherComponent& Batcher);
        void   ProcessBatchedTriangles(FTriangleBatcherComponent& Batcher);

        void NotifyMaxLightsHit();
        
        bool ShouldRequestShadow(const FVector3& LightPosition, float LightRadius) const;

        void BuildLightRelevanceVolumes();

        // Every volume that shades from the shared light buffer, so a light rejected here reaches nothing.
        bool IsLightRelevant(const FVector3& LightPosition, float LightRadius) const;
        
        enum EShadingFeature : uint32
        {
            SF_DebugViews = 1u << 0,
            SF_Decals     = 1u << 1,
            SF_GTAO       = 1u << 2,
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
            uint8           SkyMode = (uint8)GSkyMode_Runtime;   // SPEC_SKY_MODE spec constant (id 8)
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

        // Starts a background build on a miss and returns null until it lands, for pipelines a frame can draw without.
        RHI::FPipelineH      FindPipeline(const FGraphicsPipelineKey& Key);
        static uint64        HashGraphicsKey(const FGraphicsPipelineKey& Key);

        // The default material's stage, which stands in for a material whose pipeline is still compiling.
        static FShaderH      DefaultMaterialStage(EMaterialShaderStage Stage);
        static TMoveOnlyFunction<RHI::FPipelineH()> MakeGraphicsBuild(const FGraphicsPipelineKey& Key);
        static TMoveOnlyFunction<RHI::FPipelineH()> MakeComputeBuild(FShaderH CS, TSpan<const RHI::FSpecializationConstant> Constants);

        /** Where a meshlet pass draws, as opposed to what. Everything here is per PASS, not per batch. */
        struct FMeshletPassContext
        {
            uint32            CullViewIndex     = 0;
            int32             ShadowDataIndex   = -1;
            int32             ShadowViewIndex   = 0;
            float             ViewportW         = 0.0f;
            float             ViewportH         = 0.0f;
            // Non-empty for a draw over consecutive shadow cull views starting at CullViewIndex.
            RHI::TGPUSpan<FShadowRasterViewGPU> ShadowViews;
            /** Which part of the bucket's draw region to rasterize. The two VisBuffer phases each take
                their own slice; every single-phase pass takes All, which is final by the time it runs. */
            EMeshletSlice     Slice             = EMeshletSlice::All;
        };

        /** Culls every view's meshlets into the draw list and publishes the slice each pass draws.
            The sole authority: nothing rasterizes a meshlet this did not keep. */
        void MeshletCullPass(RHI::FCmdListH CL, EMeshletSlice Slice);

        void DrawMeshletBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, const FMeshletPassContext& Ctx);

        // Fallback rewrites the key to one that can stand in while the material's own pipeline compiles, or returns false to skip the batch.
        template<typename TSetup, typename TBound, typename TFallback>
        void ForEachMeshletBatch(RHI::FCmdListH CL, const TVector<uint32>& DrawList,
                                 const FMeshletPassContext& Ctx, TSetup&& Setup, TBound&& Bound, TFallback&& Fallback)
        {
            const auto& DrawCommands = RenderFrame->Geometry.DrawCommands;

            for (uint32 Idx : DrawList)
            {
                const FMeshDrawCommand& Batch = DrawCommands[Idx];

                FGraphicsPipelineKey Key;
                Key.SkinnedMode = SelectSkinnedMode(Batch);

                if (!Setup(Key, Batch))
                {
                    continue;
                }

                RHI::FPipelineH Pipeline = FindPipeline(Key);
                if (!Pipeline)
                {
                    if (!Fallback(Key, Batch))
                    {
                        continue;
                    }
                    Pipeline = GetOrCreatePipeline(Key);
                    if (!Pipeline)
                    {
                        continue;
                    }
                }

                RHI::CmdSetPipeline(CL, Pipeline);
                Bound(Batch);

                DrawMeshletBatch(CL, Batch, Ctx);
            }
        }

        template<typename TSetup, typename TBound>
        void ForEachMeshletBatch(RHI::FCmdListH CL, const TVector<uint32>& DrawList,
                                 const FMeshletPassContext& Ctx, TSetup&& Setup, TBound&& Bound)
        {
            ForEachMeshletBatch(CL, DrawList, Ctx, static_cast<TSetup&&>(Setup), static_cast<TBound&&>(Bound),
                                [](FGraphicsPipelineKey&, const FMeshDrawCommand&) { return false; });
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
        RHI::FPipelineH      FindComputePipeline(FShaderH CS,
                                 TSpan<const RHI::FSpecializationConstant> Constants = {});

        // Build and publish together, or the next view draws against the previous view's bindings.
        void SetSceneRoot(RHI::FCmdListH CL, FSceneView& View, uint64 SceneDataAddr)
        {
            SceneBindings.Root      = BuildViewSceneRoot(View);
            SceneBindings.SceneData = SceneDataAddr;
            CurrentSceneRootAddr    = SceneBindings.Root;
            RHI::CmdSetSceneRoot(CL, SceneBindings);
        }

        // Engine-wide per-draw args.
        template<typename T>
        RHI::GPUPtr MakeArgs(const T& PassData)
        {
            DEBUG_ASSERT(CurrentSceneRootAddr != 0);   // null root = GPU page fault at first scene-buffer read
            return RHI::CopyTransient(PassData);
        }

        // Zero leaves the args slot alone, which is safe only because no caller's shader reads a pass block.
        RHI::GPUPtr MakeArgs()
        {
            DEBUG_ASSERT(CurrentSceneRootAddr != 0);
            return 0;
        }

        template<typename T>
        void DispatchCompute(RHI::FCmdListH CL, FShaderH Shader, const T& PassData, uint32 GroupsX, uint32 GroupsY = 1u, uint32 GroupsZ = 1u)
        {
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(Shader));
            RHI::CmdDispatch(CL, MakeArgs(PassData), GroupsX, GroupsY, GroupsZ);
        }

        template<typename T>
        void DispatchComputeIndirect(RHI::FCmdListH CL, FShaderH Shader, const T& PassData, RHI::FGPURange Arguments)
        {
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(Shader));
            RHI::CmdDispatchIndirect(CL, MakeArgs(PassData), Arguments);
        }

        // Sets the full-extent viewport + scissor for the current render area.
        static void SetViewportScissor(RHI::FCmdListH CL, const FUIntVector2& Extent);

        static void WriteBuffer(RHI::FCmdListH CL, RHI::GPUPtr Dst, const void* Data, uint64 Size);

        // Same staging as WriteBuffer, deferred so writes sharing a destination collapse into one copy
        // command. Stage all of one buffer's runs before starting the next, or nothing groups.
        void StageWrite(RHI::GPUPtr Dst, const void* Data, uint64 Size, bool bFillBeforeSubmit = false);
        // Launched once the retained slots are synced, so the bounds build on workers while the rest of the frame records.
        void LaunchInstanceBlockBounds();
        void JoinInstanceBlockBounds();
        void FinishInstanceBlockBounds(RHI::FCmdListH CL);
        void DispatchMeshletBlockBuild(RHI::FCmdListH CL, bool bLatePass);
        bool bSplitMeshletPhasesThisFrame = false;
        void FlushStagedWrites(RHI::FCmdListH CL);

        // The GPU reads staging only once the list is submitted, so these PCIe-bound fills overlap the rest of the recording.
        void LaunchDeferredStageFills();
        void WaitDeferredStageFills();
        void RunDeferredStageFills(uint32 NumFills);

        // Scattered (Start, Count) runs of one array, into a single ring block and one copy command.
        void WriteBufferRuns(RHI::FCmdListH CL, RHI::GPUPtr Dst, const void* Src, uint64 Stride,
                             const TVector<FUIntVector2>& Runs, bool bFillBeforeSubmit = false);

        // Stages each listed element compactly and scatters it to its slot on the GPU, for a fragmented dirty set.
        void WriteBufferScatter(RHI::FCmdListH CL, RHI::GPUPtr Dst, uint64 DstBytes, const void* Src, uint64 Stride,
                                RHI::FGPURange Slots, const TVector<uint32>& SlotList, bool bFillBeforeSubmit = false);

        // The fill skips the first RewrittenBytes, which the upload ring rewrites ahead of this list and a later fill would erase.
        void ReserveBuffer(RHI::FCmdListH CL, FSceneBuffer& Buffer, uint64 NeededBytes, bool bAllowShrink = true,
                           bool bPreserveContents = false, uint64 RewrittenBytes = 0);

        // Freed when this slot's previous GPU work has completed.
        void DeferFree(const RHI::FGPUAllocation& Allocation);
    
    private:
        
        struct FFrozenCull
        {
            TVector<FCullView> Views;
            FVector4           CameraPosition   = {};
            FMatrix4           CameraView       = {};
            FMatrix4           CameraProjection = {};
            float              NearPlane        = 0.0f;
            float              FarPlane         = 0.0f;
            FGPUFrustum        Frustum          = {};
            FGPUFrustum        ShadowFrustum    = {};
            FGPUFrustum        CascadeFrustum[NumCascades] = {};

            // The cascade pyramid stops rebuilding under freeze, so the transforms that tap it must stop too.
            FMatrix4           CascadeHZBViewProjection[NumCascades]    = {};
            FVector4           CascadeHZBNdcScale[NumCascades]          = {};
            FMatrix4           CascadeHZBViewProjectionMid[NumCascades] = {};
            FVector4           CascadeHZBNdcScaleMid[NumCascades]       = {};

            // The cascade fit itself, so the shadow raster covers the region the frozen cull selected for.
            FMatrix4           CascadeShadowViewProjection[NumCascades] = {};
            FVector4           CascadeRadii       = {};
            FVector4           CascadeDepthRanges = {};
            bool               bHasCascadeShadow  = false;

            uint32             CascadeViewBase  = Constants::kIndexNoneU32;
            uint32             NumCascadeViews  = 0;
            uint32             NumNearCascadeViews = 0;
            bool               bValid           = false;
        };
        FFrozenCull FrozenCull;
        
        FSceneBuffer PreSkinnedVerticesBuffer { "Cull.PreSkinnedVertices", 1.2f };
        FSceneBuffer PreSkinnedPrevPositionsBuffer { "Cull.PreSkinnedPrevPositions", 1.2f };

        // Sharing one buffer would clear the flags the late dispatch still needs to read.
        TArray<FSceneBuffer, 2> InstanceVisibilityBuffers = MakeSceneRing<2>("Retained.InstanceVisibility", 1.5f, EBufferInit::Zeroed);
        uint32                                             InstanceVisibilityCapacity = 0;
        // One word per block of each instance's camera LOD, read and written in place since the late phase reads a word before replacing it.
        FSceneBuffer                                       MeshletVisibilityBuffer { "Retained.MeshletVisibility", 1.5f, EBufferInit::Zeroed };
        uint32                                             MeshletVisibilityCapacity = 0;
        uint8                                              InstanceVisibilityWriteIndex = 0;
        // Stamped by the late cull; starts past zero so a never-written slot can never read as visible.
        uint32                                             InstanceVisibilityTag = 1;
        uint32                                             LastStaleValidationGeneration = 0;
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
        RHI::TGPUSpan<FGPUReflectionProbe>      ProbeBufferSpan;
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

        // The first shadow pass to open each atlas this frame clears it through LoadOp; the rest load.
        bool                                    bLocalAtlasClearedThisFrame = false;
        bool                                    bCascadeClearedThisFrame    = false;

        // Clear on the first call per frame, Load after; ResetPass_Render rearms both.
        RHI::ELoadOp TakeLocalAtlasLoadOp();
        RHI::ELoadOp TakeCascadeLoadOp();

        TVector<FGPUReflectionProbe>            LastExtractedProbes;
        TVector<FReflectionProbeCapture>        LastExtractedCaptures;
        // Last global rebake-request counter this scene acted on. See RequestReflectionProbeRebake.
        uint32                                  LastSeenRebakeRequest = 0;
        uint32                                  AlwaysProbeCursor = 0;

        // Addresses of this frame's uploaded spline arrays. Splines are extracted only for components with
        // bSendToGPU, so a world that authors splines purely as data uploads nothing.
        RHI::TGPUSpan<FGPUSpline>               SplineBufferSpan;
        RHI::TGPUSpan<FGPUSplinePoint>          SplinePointBufferSpan;
        RHI::TGPUSpan<FGPUSplineSample>         SplineSampleBufferSpan;
        uint32                                  NumActiveSplines       = 0;

        void ExtractSplines(ECS::FRegistry& Registry, FFrameData& Frame);

        void InitReflectionProbeTargets();
        void SyncProbeCaptureCube(uint32 FaceSize);
        void ExtractReflectionProbes(ECS::FRegistry& Registry, FFrameData& Frame);
        void ScheduleReflectionProbeBake(FFrameData& Frame);
        // Renders the six faces, copies each into the scratch cube, prefilters into the probe's slice.
        void ReflectionProbeBakePass(RHI::FCmdListH CL);
        
        FEnvironmentParams                      LastUploadedEnvironmentParams = {};
        bool                                    bEnvironmentParamsUploaded    = false;

        // Off unless the pass filling the volume dispatched, so the composite cannot read a stale one.
        struct FAtmosphereTerms
        {
            uint32 AerialInScatterIndex     = Constants::kIndexNoneU32;
            uint32 AerialTransmittanceIndex = Constants::kIndexNoneU32;
            float  AerialRange              = 0.0f;
            float  AerialIntensity          = 0.0f;
            uint32 CloudScatterIndex        = Constants::kIndexNoneU32;
            uint32 CloudDepthIndex          = Constants::kIndexNoneU32;
            uint32 FogShaftIndex            = Constants::kIndexNoneU32;
            uint32 FogShaftDepthIndex       = Constants::kIndexNoneU32;
        };
        FAtmosphereTerms                        AtmosphereTerms = {};
        

        
        TVector<CMaterialInterface*>            PendingPostProcessMaterials;
        
        FSceneRoot                                                      SceneRootShared = {};
        RHI::FSceneBindings                                             SceneBindings = {};
        TVector<RHI::FBufferCopy>                                       StagedWrites;

        // Dest is a mapped staging pointer, or null when the bytes go through the upload ring to UploadDest.
        struct FDeferredStageFill
        {
            uint8*          Dest;
            const uint8*    Source;
            uint64          Bytes;
            RHI::GPUPtr     UploadDest = 0;
            // Set for a gather, which copies Bytes of elements of GatherWords 16-byte words from Source at these slots.
            const uint32*   GatherSlots = nullptr;
            uint32          GatherWords = 0;
        };
        TVector<FDeferredStageFill>                                     DeferredStageFills;
        FTaskHandle                                                     DeferredStageFillTask;
        std::atomic<uint32>                                             DeferredFillCursor{0};
        bool                                                            bDeferredUploadsQueued = false;
        bool                                                            bDeferredGatherQueued  = false;
        TVector<RHI::FBufferCopy>                                       UploadCopyScratch;
        TVector<uint64>                                                 UploadCursorScratch;
        uint64                                                          CurrentSceneRootAddr = 0;
        // Builds the per-view FSceneRoot transient (shared addrs + view camera/clusters/IBL) -> address.
        uint64 BuildViewSceneRoot(FSceneView& View);
        void   EnsureClusterLightMaskCapacity(FSceneView& View);

        /** Texture-streaming feedback (see RequestTextureResolution in SceneGlobals.slang). One uint per
         *  bindless slot, OR-accumulated by the material lanes over STREAMING_FEEDBACK_WINDOW frames, then
         *  copied to a readback slot and zeroed. Read kFramesInFlight later, once the copy has landed. */
        void EnsureStreamingFeedbackBuffer();
        void CollectStreamingFeedback(RHI::FCmdListH CL);
        void PublishStreamingFeedback();

        RHI::FGPUAllocation                                        StreamingFeedbackBuffer;
        TArray<RHI::FGPUAllocation, RHI::kFramesInFlight>          StreamingFeedbackReadback = {};
        /// Frame the slot was written on, so a slot is only read once its copy has certainly landed.
        TArray<uint64, RHI::kFramesInFlight>                StreamingFeedbackStamp = {};
        uint32                                              StreamingFeedbackSlots = 0;

        // Live prefix of Frame.Lighting.Lights / .Shadows; the GPU side reads these off the spans.
        uint32                                              NumLiveLights  = 0;
        uint32                                              NumLiveShadows = 0;
        // Created on the first frame any light carries a light function, then kept.
        FSceneImage                                         LightFunctionAtlas;
        uint64                                              StreamingFeedbackFrame = 0;
        // Frames counted toward the current feedback window, and the newest window already handed to streaming.
        uint64                                              StreamingFeedbackTick = 0;
        uint64                                              StreamingFeedbackPublished = 0;
        
        TArray<FSceneBuffer, RHI::kFramesInFlight> RenderBucketRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.RenderBuckets", 1.5f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> MeshletDrawListRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.MeshletDrawList", kSceneBufferGrowth);
        TArray<FSceneBuffer, RHI::kFramesInFlight> MeshDrawArgsRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.MeshDrawArgs", 1.2f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> FrameScratchRing = MakeSceneRing<RHI::kFramesInFlight>("Frame.Scratch", 1.5f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> MeshletBlockRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.MeshletBlocks", 1.2f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> BlockDispatchArgsRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.BlockDispatchArgs", 1.0f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> SkinWorkBaseRing = MakeSceneRing<RHI::kFramesInFlight>("Skinning.WorkBase", 1.5f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> SkinDispatchArgsRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.SkinDispatchArgs", 1.0f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> MeshletCullDispatchArgsRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.MeshletCullDispatchArgs", 1.0f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> InstanceViewRangeRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.InstanceViewRanges", 1.25f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> TotalsRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.Totals", 1.0f);
        TArray<bool, RHI::kFramesInFlight>                                  TotalsZeroed = {};

        RHI::FGPUAllocation GetTotals() const { return TotalsRing[CurrentFrameSlot]; }

        FSceneBuffer RetainedCullEntryBuffer { "Retained.CullEntries", 1.5f, EBufferInit::Zeroed };
        FSceneBuffer InstanceBlockBoundsBuffer { "Retained.InstanceBlockBounds", 1.5f, EBufferInit::Zeroed };

        // The sun's distance-field casters for this frame and the grid they are binned into.
        FSceneBuffer SunDFObjectBuffer    { "SunDF.Objects",    1.0f, EBufferInit::Undefined, false };
        FSceneBuffer SunDFCellCountBuffer { "SunDF.CellCounts", 1.0f, EBufferInit::Zeroed,    false };
        FSceneBuffer SunDFCellItemBuffer  { "SunDF.CellItems",  1.0f, EBufferInit::Undefined, false };
        FSceneBuffer SunDFCounterBuffer   { "SunDF.Counters",   1.0f, EBufferInit::Zeroed,    false };
        FSceneBuffer SunDFBlockListBuffer { "SunDF.Blocks",     1.5f, EBufferInit::Undefined };
        static constexpr uint32 kSunDFGridDimension = 128u;
        static constexpr uint32 kSunDFCellCapacity  = 32u;
        static constexpr uint32 kSunDFMaxObjects    = 16384u;
        bool                    bSunDFGridReady     = false;
        TVector<FInstanceBlockBounds> InstanceBlockBounds;
        TVector<uint32>               DirtyInstanceBlocks;
        // Per block, whether it holds a distance-field caster, and the compact list the sun's distance-field cull walks.
        TVector<uint8>                InstanceBlockHasDF;
        TVector<uint32>               DistanceFieldBlocks;
        bool                          bDistanceFieldBlocksDirty = true;
        TVector<FUIntVector2>         GPUWrittenSlotRanges;
        bool                          bInstanceBlockBoundsValid = false;
        FTaskHandle                   InstanceBlockBoundsTask;
        std::atomic<bool>             bBlockDFChanged{ false };
        bool                          bBlockBoundsLaunched   = false;
        bool                          bBlockBoundsRebuildAll = false;
        uint32                        BlockBoundsNumBlocks   = 0;
        FSceneBuffer RetainedTransformBuffer { "Retained.Transforms", 1.5f, EBufferInit::Zeroed };
        FSceneBuffer RetainedStaticBuffer { "Retained.Static", 1.5f, EBufferInit::Zeroed };
        FSceneBuffer SurfaceDescBuffer { "Retained.SurfaceDescs", 1.5f, EBufferInit::Zeroed };
        FSceneBuffer BoneArenaBuffer { "Skinning.BoneArena", 1.5f };
        // Snapshots taken before each frame's incremental uploads land, so motion vectors have a past.
        RHI::FGPUAllocation                                        PrevBoneArenaBuffer;
        RHI::FGPUAllocation                                        PrevRetainedTransformBuffer;
        bool                                                bPrevMotionStateValid = false;
        void SnapshotMotionState(RHI::FCmdListH CL);
        FSceneBuffer SkinnedFrameDataBuffer { "Skinning.FrameData", 1.25f };
        FSceneBuffer SkinnedSlotListBuffer { "Skinning.SlotList", 1.5f };
        TVector<uint64>                                     SkinnedSlotBits;
        TVector<FUIntVector2>                               SkinnedRunScratch;
        uint32                                              CurrentSkinnedFrameTag = 0;
        // Per-frame posed meshlet spheres, so the cull can reject skinned geometry per meshlet.
        FSceneBuffer SkinnedMeshletBoundsBuffer { "Skinning.MeshletBounds", 1.25f };
        // Same index space and same base as the bounds arena, so the two are sized together.
        FSceneBuffer SkinnedMeshletConeBuffer { "Skinning.MeshletCones", 1.25f };
        uint32                                              SkinnedMeshletBoundsCapacity = 0;
        // Longest per-slot meshlet range, which is the x extent of the bounds dispatch.
        uint32                                              SkinnedBoundsMaxRange = 0;
        // Slots the retained static buffer actually holds, which bounds the header lookup in that pass.
        uint32                                              RetainedStaticCapacity = 0;
        uint32                                              UploadedSurfaceDescs = 0;

        TArray<FSceneBuffer, RHI::kFramesInFlight> VisibleInstanceRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.VisibleInstances", kSceneBufferGrowth);


        // (base, count) per (skinned instance, view) for the CPU-fed head of the visible buffer, which
        // CullInstances never claims and therefore never writes a range for.

        RHI::FGPUAllocation GetVisibleInstances()  const { return VisibleInstanceRing[CurrentFrameSlot]; }


        // Sends only the arena slices this frame's gather wrote, coalesced. Must run before anything reads
        // Bones() -- the skinning dispatch and the in-draw skinning fallback both do.
        void UploadBoneArena(RHI::FCmdListH CL, const FFrameData& Frame);

        // Ships the per-frame half of every gathered skinned slot's payload, plus the compact slot list the
        // skinning passes iterate. Must run before CullInstances and before the skinning dispatch.
        void UploadSkinnedFrameData(RHI::FCmdListH CL, FFrameData& Frame);
        void SkinnedMeshletBoundsPass(RHI::FCmdListH CL, const FFrameData& Frame);

        void DispatchGPUSceneCull(RHI::FCmdListH CL, const FFrameData& Frame);

        void PublishRetainedUpload();

        std::atomic<uint32>                                 RetainedDeviceCapacity{0};

        std::atomic<bool>                                   bDepthPyramidValid{false};

        std::atomic<bool>                                   bCascadePyramidValid{false};

        FMatrix4                                            CascadeHZBViewProjection[NumCascades] = {};
        FVector4                                            CascadeHZBNdcScale[NumCascades] = {};
        bool                                                bCascadeHZBTransformsValid = false;

        float                                               CascadeMinTexels = 1.0f;
        float                                               CasterMinScreenRadius = 0.0f;
        int32                                               ActiveCascadeCount = NumCascades;
        int32                                               NearCascadeCount = NumCascades;

        static constexpr uint32                             kTotalsSlots = 8;

        // Must match HISTOGRAM_BINS in LuminanceHistogram.slang and its TILE_DIM^2 thread count.
        static constexpr uint32                             kLuminanceHistogramBins = 256;

        // Frame scratch layout, in bytes. Grass cursors are last because their count is per frame.
        static constexpr uint64                             kScratchCullCountersOffset = 0;
        static constexpr uint64                             kScratchSpdCountersOffset  = 16;
        static constexpr uint32                             kScratchSpdCounterCount    = 2;
        static constexpr uint64                             kScratchHistogramOffset    = 32;
        static constexpr uint64                             kScratchGrassCursorsOffset = 32 + sizeof(uint32) * 256;

        static constexpr uint32                             kAerialLUTSize   = 32;
        static constexpr uint32                             kAerialLUTSlices = 32;

        static constexpr uint32                             kCloudNoiseSize = 128;

        TArray<RHI::FGPUAllocation, RHI::kFramesInFlight>   MeshletBoundReadback = {};
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
        // Highest demand seen this session, which sizes the lists for the frames after a camera cut.
        uint32                                              BlockListPeak = 0;
        uint32                                              DrawListPeak = 0;
        FVector3                                            LastCullOrigin = FVector3(0.0f);
        bool                                                bHasLastCullOrigin = false;
        uint32                                              CameraCutFramesLeft = 0;
        FDemandWindow                                       PreSkinDemand;
        FDemandWindow                                       VisibleInstanceDemand;
        FDemandWindow                                       DrawListDemand;
        uint32                                              PreSkinnedVertexCapacity = 0;
        uint32                                              PreSkinnedPrevCapacity = 0;
        uint32                                              MeshSubDrawsPerSlice = 1;
        // Logs only a new peak, since the value alternates with the frame ring.
        uint32                                              LoggedSubDrawPeak = 0;
        uint32                                              LastBlocksRequested = 0;
        uint32                                              LastPreSkinRequested = 0;
        uint32                                              LastPreSkinOverflowed = 0;
        uint32                                              LastBlocksOverflowed = 0;
        uint32                                              MeshletDrawTagCounter = 0;
        uint32                                              FrameVisibleInstanceCapacity = 0;

        // Instance slots over the last few frames, since anything added after the readback it lags could be visible.
        uint32                                              RecentSlotCounts[RHI::kFramesInFlight + 1] = {};
        uint32                                              RecentSlotCursor = 0;
        void   UpdateMeshletBoundFeedback(uint8 Slot);
        TArray<FSceneBuffer, RHI::kFramesInFlight> MaterialClassifyRing = MakeSceneRing<RHI::kFramesInFlight>("Material.ClassifyBlock", 1.0f, EBufferInit::Undefined, /*bAllowShrink*/ false);
        // GPU-only scratch ordered by the graphics queue, so one copy serves every frame in flight.
        FSceneBuffer MaterialPairList { "Material.PairList", 1.2f };
        
        uint8                                                           CurrentFrameSlot = 0;

        FShadowAtlas                            ShadowAtlas;
        
        THashMap<ECS::FEntity, FTerrainGPUState> TerrainGPUStates;

        // One scatter target per (terrain, species slot). Keyed by entity; the vector is indexed by the
        // species' position in the extract, which is stable for a given compiled material.
        THashMap<ECS::FEntity, TVector<FGrassGPUState>> GrassGPUStates;

        // One per species scattered this frame, drained by the batched retire that follows the scatters.
        struct FGrassRetireItem
        {
            uint32              SlotBase = 0;
            uint32              Capacity = 0;
            RHI::FGPURange      Cursor;
            RHI::FGPUAllocation PrevCursor;
            RHI::FGPUAllocation CursorReadback;
        };
        TVector<FGrassRetireItem>                       GrassRetireScratch;
        uint32                                          GrassCursorCursor = 0;
        
        THashMap<ECS::FEntity, TVector<FParticleGPUState>> ParticleGPUStates;

        struct FParticleCollisionSource
        {
            ECS::FEntity Entity;
            int32        EmitterIndex = 0;
            RHI::GPUPtr  EventBuffer = 0;
        };
        // One frame slot's copies of the collision lists of emitters that report them, read once the slot comes round again.
        struct FParticleCollisionReadback
        {
            RHI::FGPUAllocation               Buffer = {};
            TVector<FParticleCollisionSource> Sources;
        };
        TArray<FParticleCollisionReadback, RHI::kFramesInFlight> ParticleCollisionReadback = {};
        // Read back but not yet handed to the components, which the next extract does.
        THashMap<ECS::FEntity, TVector<FParticleCollision>> ParticleCollisionResults;
        void ReadParticleCollisions();
        void CopyParticleCollisions(RHI::FCmdListH CL);
        THashMap<uint64, TVector<RHI::FGPUAllocation>>    ParticleBufferPool;
        uint64                                            ParticlePoolBytes = 0;
        // Set once a pooled buffer is handed out this frame, since its previous frame's reads need a barrier before the clear.
        bool                                              bParticlePoolReuseBarrierIssued = false;

        FScenePrimitiveSet                      ScenePrimitives;
        TVector<ECS::FEntity>                   MovedTransformScratch;
        TVector<uint64>                         DirtySlotBits;

        // Merge scratch, sized by draw slots (not by entities). Members so capacity survives frames.

        TVector<FShaderH>            BinnedDeferredSlotShaders;
        TVector<uint8>               BinnedDeferredSlotKinds;
        // Bit per EMaterialShadingModel among the binned materials, which specializes the lighting dispatch.
        uint32                       BinnedShadingModelMask = 0u;
        TVector<uint32>                         BinnedDeferredSlotByMaterial;
        // Shader handle -> its dense bin, so binning stays linear in the visible material count.
        THashMap<uint64, uint32>                BinnedDeferredSlotLookup;

        struct FMaterialClassifyLayout
        {
            uint32 NumSlots      = 0;
            uint32 ScreenW       = 0;
            uint32 ScreenH       = 0;
            uint32 PairCapacity  = 0;   // (tile, slot) entries the pair list holds, taken from the allocation
            uint32 OverflowCountOffset = 0;
            uint32 TilesX        = 0;
            uint32 NumTiles      = 0;
            uint32 OverflowCapacity = 0;
            uint64 TileSlotsOffset  = 0;    // byte offsets into the pair list allocation
            uint64 OverflowOffset   = 0;
            uint64 PixelListOffset  = 0;
            uint32 PixelCapacity    = 0;

            // Byte offsets, packed to the live slot count; the shaders take each region by its own address.
            uint32 CountsOffset       = 0;
            uint32 OffsetsOffset      = 0;
            uint32 CursorsOffset      = 0;
            uint32 MaterialArgsOffset = 0;
            uint32 ScatterArgsOffset  = 0;
            uint32 BlockSize          = 0;
        };
        // Derived once by VisBufferClassifyPass and read by the material and lighting passes. Zeroed at
        // the top of the classify pass before any early return, so a bail-out frame cannot leave the
        // later passes dispatching against a stale slot table.
        FMaterialClassifyLayout                 MaterialClassifyLayout;

        bool BuildDeferredMaterialBinning(RHI::FCmdListH CL);

        TVector<uint32>                         ShadowSizeScratch;
        TVector<uint32>                         ShadowSortedScratch;
        // Only the over-budget view trim uses these; persistent so that path allocates nothing.
        TVector<uint32>                         ShadowDropOrderScratch;
        TVector<bool>                           ShadowDropScratch;

        struct FDecalSortEntry { CMaterial* ShaderOwner; int32 SortOrder; FGPUDecal Gpu; };
        TVector<FDecalSortEntry>                DecalSortScratch;

        // Sorted before batching so alpha blending composites back to front.
        struct FSpriteSortEntry
        {
            uint32     TextureIndex;
            int32      SortOrder;
            float      ViewDepthSq;
            bool       bDepthTest;
            bool       bDoubleSided;
            FGPUSprite Gpu;
        };
        TVector<FSpriteSortEntry>               SpriteSortScratch;
        
        TVector<FThreadLocalDrawData>           ThreadLocalStorage;

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

        // Frames that reached Submit in RenderView, so Output holds a real image rather than undefined memory.
        uint64                                  FramesComposited = 0;
        bool                                    bWarnedNoComposite = false;
        bool                                    bWarnedFallbackMaterial = false;

        // A resize destroys Output, and only a ticking world can repaint it. Held until Extract runs.
        FUIntVector2                            PendingPrimarySize = FUIntVector2(0);
        bool                                    bHasPendingPrimarySize = false;
        FUIntVector2                            LastRequestedPrimarySize = FUIntVector2(0);
        uint32                                  PrimarySizeStableFrames = 0;

#if USING(WITH_EDITOR)
        static constexpr uint32                 PickerReadbackRingSize = RHI::kFramesInFlight + 1;
        static constexpr uint32                 PickerRegionExtent = 64;
        struct FPickerReadbackSlot
        {
            RHI::FGPUAllocation Readback = {};       // CPURead buffer, Width*Height*4 bytes
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
