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
#include "Containers/Optional.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/LineBatcherComponent.h"
#include "World/Entity/Components/TextComponent.h"
#include "World/Scene/RenderScene/EnvironmentRenderTypes.h"
#include "World/Scene/RenderScene/MeshDrawCommand.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/SceneUpscaler.h"
#include "World/Scene/RenderScene/SceneCullContext.h"
#if USING(WITH_EDITOR)
#include "World/Scene/RenderScene/EditorGridRequest.h"
#endif
#include "World/Scene/RenderScene/ScenePrimitiveSet.h"
#include "World/Scene/RenderScene/TerrainRenderTypes.h"
#include "World/Scene/RenderScene/TexturePaintTypes.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Threading/Sync.h"
#include "World/Entity/Components/PostProcessSettings.h"
#include "World/Entity/Components/CloudComponent.h"
#include "World/Entity/Systems/WeatherState.h"

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
    struct SSplineComponent;
    struct SReflectionProbeComponent;
    struct FSplineSample;
    class CMaterialInterface;
    class CStaticMesh;
    class CGrassType;
    class CMaterial;
    enum class EMaterialShaderStage : uint8;

    // RenderSceneFactory falls back to this whenever no override is installed.
    class FDefaultSceneRenderer : public IRenderScene
    {
    public:

        FDefaultSceneRenderer(CWorld* InWorld);
        ~FDefaultSceneRenderer() override;
        LE_NO_COPYMOVE(FDefaultSceneRenderer);

        // A skinned index past the frame data's current size, written by the merge once it has grown the array.
        struct FDeferredSkinnedSlot
        {
            uint32              SkinnedIndex;
            FSkinnedFrameData   Data;
        };

        struct CACHE_ALIGN FThreadLocalDrawData
        {
            TVector<uint32>                     SkinnedIndices;
            TVector<FDeferredSkinnedSlot>       DeferredSlots;
            TVector<FUIntVector2>               BoneUploadRanges;

            FSceneRenderStats                   Stats = {};
            bool                                bTouched = false;

            FThreadLocalDrawData() = default;
            ~FThreadLocalDrawData() = default;
            FThreadLocalDrawData(const FThreadLocalDrawData&) = delete;
            FThreadLocalDrawData& operator=(const FThreadLocalDrawData&) = delete;

            FThreadLocalDrawData(FThreadLocalDrawData&&) = default;
            FThreadLocalDrawData& operator=(FThreadLocalDrawData&&) noexcept = default;

            void ResetForFrame()
            {
                SkinnedIndices.Reset();
                DeferredSlots.Reset();
                BoneUploadRanges.Reset();
                Stats = {};
                bTouched = false;
            }
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

            struct FGrassSpeciesExtract
            {
                const CGrassType* Type = nullptr;
                // Which repeat of this type and layer in the material's list, so duplicates keep separate blocks.
                uint32 Occurrence      = 0;
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
                float   TerrainNormalDistance = 0.0f;
                float   RootDarkening  = 0.0f;
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

                // Empty unless the terrain carries SGrassComponent and its material declares species.
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
                bool                    bOnDrawnLayer       = true;

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

                // Zero channels when the emitter has no grid, in which case the fields after it are unset.
                uint32                  GridChannels        = 0u;
                FUIntVector2            GridResolution      = FUIntVector2(0u);
                FVector2                GridWorldSize       = FVector2(0.0f);
                bool                    bGridWraps          = false;
                TVector<int32>          GridStageIterations;
                uint32                  GridTargetSlot      = RHI::kInvalidHeapSlot;
                FUIntVector2            GridTargetExtent    = FUIntVector2(0u);
                bool                    bReadBackGrid       = false;

                // Every array the emitter declares packed back to back, with an offset and count per array.
                TVector<FVector4>       ArrayData;
                TVector<FUIntVector2>   ArrayRanges;

                // Zero cells when no module looks particles up by their neighbors.
                uint32                  NeighborResolution  = 0u;
                uint32                  NeighborsPerCell    = 0u;
                float                   NeighborCellSize    = 0.0f;
            };

            struct FCaptureViewData
            {
                FSceneGlobalData    SceneGlobalData = {};
                FViewVolume         ViewVolume      = {};
                uint32              CameraViewIndex = Constants::kIndexNoneU32;   // its cull-view index (frustum-only)
                int32               SceneViewIndex  = -1;    // index into FDefaultSceneRenderer::SceneViews
            };

            FViewVolume                      ViewVolume = {};
            FViewLayerSettings               ViewLayers = {};
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
                // Indexed by skinned index, only gathered entries are written, and stale ones fail their frame tag.
                TVector<FSkinnedFrameData>       SkinnedFrameData;
                TVector<uint32>                  SkinnedIndices;
                TVector<FMeshDrawCommand>        DrawCommands;
                TVector<uint32>                  OpaqueDrawList;
                TVector<uint32>                  TranslucentDrawList;
                bool                             bHasBlendedTranslucency   = false;   // drawn by the WBOIT pass and its resolve
                bool                             bHasUnorderedTranslucency = false;   // additive or modulate
                TVector<FDeferredMaterialEntry>  DeferredMaterials;   // distinct opaque slots for the deferred pass
                FSceneCullContext                SceneCullContext;

                struct FRetainedUpload
                {
                    bool                        bFull = false;
                    bool                        bFullStatic = false;
                    uint32                      SlotCount = 0;

                    TVector<uint32>             DirtySlots;
                    TVector<uint32>             DirtyStaticSlots;
                    TVector<uint32>             DeactivatedSlots;

                    bool                        bSurfaceDescsChanged = false;
                    uint32                      SurfaceDescCount = 0;

                    uint32                      MaxSurfaceDescMeshlets = 0;

                    uint32                      MeshletVisibilityWords = 0;
                    uint64                      LargestLODMeshlets = 0;
                    FScenePrimitiveSet::FMeshletDemand GrassMeshlets;
                    uint32                      GrassSlotCount = 0;

                    // Moved instances the extract wrote straight to GPU-visible memory, scattered after the uploads above.
                    RHI::FGPURange              StreamRecords;
                    RHI::FGPURange              StreamSlots;
                    uint32                      StreamCount = 0;
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
                // The camera again, holding only custom depth instances, and kIndexNoneU32 while none exist.
                uint32                           CustomDepthCullView = Constants::kIndexNoneU32;
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

                struct FLightFunctionDraw
                {
                    FRenderMaterialShaders       Shaders;
                    FLightFunctionRequest        Request;
                    uint32                       MaterialIndex = 0;
                    uint32                       Slot = 0;
                };
                TVector<FLightFunctionRequest>   LightFunctionRequests;
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

                    // Zero steps leaves the contact march off.
                    uint32                       ContactSteps = 0u;
                    float                        ContactLength = 0.0f;
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
                // Weather wind drift added on top of the cloud component's own.
                FVector2                         CloudWindOffset      = FVector2(0.0f);

                TVector<FGPUFogVolume>           FogVolumes;
                uint32                           FarShaftSteps        = 0;
                // Any local light this frame scatters through fog, which is what the froxel inject walks cluster lists for.
                bool                             bLocalVolumetricLights = false;
                float                            FarShaftDistance     = 4000.0f;
            } Volumetrics;

            // Inactive unless a weather component is playing, which leaves every weather pass and allocation idle.
            struct FWeather
            {
                bool                             bActive                  = false;
                bool                             bDrops                   = false;
                FWeatherState                    State                    = {};
                int32                            OcclusionShadowDataIndex = Constants::kIndexNone;
                uint32                           OcclusionCullView        = Constants::kIndexNoneU32;
                uint32                           OcclusionResolution      = 0;
                FMatrix4                         OcclusionViewProjection  = FMatrix4(1.0f);
                float                            OcclusionDepthRange      = 1.0f;
                float                            OcclusionTexelWorld      = 0.1f;
                FVector3                         FallDirection            = FVector3(0.0f, -1.0f, 0.0f);
                uint32                           NumDrops                 = 0;
                uint32                           NumSplashes              = 0;
                uint32                           NumFlakes                = 0;
            } Weather;

            // What Weather Interactors did to the world, cleared rather than reassigned so its arrays keep their capacity.
            struct FWeatherInteraction
            {
                // The field simulates while contacts disturb it and for a while after, so ripples settle rather than freeze.
                bool                             bField                   = false;
                // The field was idle, so its images hold stale ripples and are cleared before this frame's step.
                bool                             bReset                   = false;
                // The first of this frame's passes also advances the waves, rather than only taking this frame's contacts.
                bool                             bFirstPassSteps          = false;
                // Snow lies on the ground, so prints fade over minutes rather than seconds.
                bool                             bSnow                    = false;
                uint32                           Passes                   = 0;
                // Images the first pass reads and the last one writes.
                uint32                           InputImage               = 0;
                uint32                           OutputImage              = 0;
                uint32                           Resolution               = 0;
                float                            TexelWorld               = 0.0f;
                FIntVector2                      OriginTexel              = FIntVector2(0);
                // Texels the window moved since the last frame, which the first pass reads its input through.
                FIntVector2                      Shift                    = FIntVector2(0);
                uint32                           Serial                   = 0;
                TVector<FWeatherContactGPU>      Contacts;
                TVector<FWeatherImpactGPU>       Impacts;
                TVector<FUIntVector2>            InteractorTable;
                TVector<FVector4>                InteractorRecords;
                TVector<FWeatherWaterBodyGPU>    WaterBodies;
            } WeatherInteraction;

            // World space throughout: SSplineComponent authors in entity-local space and the extract bakes the transform in.
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
                FEditorGridRequest               Grid;
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
            ScreenSpaceGITrace,
            ScreenSpaceGIBlur,
            ScreenSpaceGI,
            ScreenSpaceGIHistory,
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
            PickerFoliage,
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
            CloudWeather,
            CloudScatter,
            CloudDepth,
            CloudScatterHistory,
            CloudDepthHistory,
            CloudMarch,
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
            CustomDepth,
            CustomDepthVis,
            CustomStencil,

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
            /// Tick each on-demand image was last needed; only the optional entries are read (see EnsureOptionalViewImages).
            TArray<uint64, (int)ENamedImage::Num>           ImageLastUsedTick = {};
            FSceneImage                                     BloomChainImage;
            // CloudNoise is per-view, so a renderer-wide flag leaves view two sampling an unbaked volume.
            bool                                            bCloudNoiseBaked = false;
            // Picks which texel of each 2x2 block the clouds march this frame, the rest keeping their last march.
            uint32                                          CloudFrame = 0;
            bool                                            bCloudHistoryValid = false;
            FVector2                                        CloudHistoryWindOffset = FVector2(0.0f);
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
            // A debug view skips the capture, so the history reprojects through the camera it was taken with.
            bool                                            bScreenSpaceGIHistoryValid = false;
            FMatrix4                                        ScreenSpaceGIHistoryViewProjection = FMatrix4(1.0f);
        };

        void Init() override;

        void Extract(const FViewVolume& ViewVolume, const SPostProcessSettings* PostProcess, const FViewLayerSettings& Layers) override;
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
        RHI::FGPUAllocation GetMeshletCullDispatchArgs() const { return MeshletCullDispatchArgsRing[CurrentFrameSlot]; }
        // (base, count) per (visible instance, view); CullInstances writes it on reserve, BuildMeshletBlocks reads it to append.
        RHI::FGPUAllocation GetInstanceViewRanges() const { return InstanceViewRangeRing[CurrentFrameSlot]; }
        // Every counter a pass accumulates into this frame lives in one scratch block, zeroed once in RenderView.
        RHI::FGPURange GetCullCounters()      const { return FrameScratchRange(kScratchCullCountersOffset, sizeof(uint32) * 4); }
        RHI::FGPURange GetSpdCounter(uint32 Index) const { return FrameScratchRange(kScratchSpdCountersOffset + Index * sizeof(uint32), sizeof(uint32)); }
        RHI::FGPURange GetLuminanceHistogram() const { return FrameScratchRange(kScratchHistogramOffset, sizeof(uint32) * kLuminanceHistogramBins); }
        RHI::FGPURange GetGrassCursor(uint32 Index) const { return FrameScratchRange(kScratchGrassCursorsOffset + Index * sizeof(uint32), sizeof(uint32)); }

        RHI::FGPUAllocation GetMaterialClassify()  const { return MaterialClassifyRing[CurrentFrameSlot]; }
        RHI::FGPUAllocation GetMaterialPairList() const { return MaterialPairList; }

        uint32 GetDisplayResourceID() const override;
        bool HasCompositedFrame() const override { return FramesComposited > 0; }

        RUNTIME_API void SettleResolveWork(int32 MaxIterations = 8);

        // Editor-only tripwire: warns about, and queues, any mesh component a resolve pass left stale.
        void ValidateNoStaleResolves(ECS::FRegistry& Registry);
        RHI::FTextureH GetDisplayTexture() const override { return SceneViews[0].Output.Texture; }
        const FSceneImage& GetDisplayImage() const { return SceneViews[0].Output; }
        const FSceneImage& GetPrimaryNamedImage(ENamedImage Image) const { return SceneViews[0].Images[(int)Image]; }
        FUIntVector2 GetRenderExtent() const override;
        ECS::FEntity GetEntityAtPixel(uint32 X, uint32 Y) const override;
        bool GetSurfaceAtPixel(uint32 X, uint32 Y, TSpan<const ECS::FEntity> Ignored, FVector3& OutPosition, FVector3& OutNormal) const override;
        #if USING(WITH_EDITOR)
        void RequestSurfaceSnapshot() override;
        void ReleaseSurfaceSnapshot() override;
        bool IsSurfaceSnapshotReady() const override;
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

        // Unconditional: callers decide whether the new size is worth the WaitDeviceIdle this costs.
        RUNTIME_API void ResizePrimaryView(const FUIntVector2& NewSize);

        // Applies a deferred SetPrimaryViewSize. Called from Extract, which only runs for a ticking world.
        void ApplyPendingPrimarySize();

        // The primary view's render size for its display size, from the screen percentage and the upscaler.
        static FUIntVector2 ComputeRenderSize(const FUIntVector2& DisplaySize, const IUpscaler* Upscaler);

        // Resolves the primary view's upscaler and reallocates when the screen percentage or upscaler changed it.
        void RefreshPrimaryRenderSize();

        // Editor clicks arrive in display pixels and the picker is drawn at render resolution.
        FUIntVector2 DisplayToPickerTexel(uint32 X, uint32 Y) const;

        // Cleared the first time something sizes the primary view explicitly (an editor tool panel).
        bool bPrimaryTracksSwapchain = true;

        FSceneView& AddSceneView(const FUIntVector2& Size, bool bPrimary);
        
        void RenderCaptureView(RHI::FCmdListH CL);

        //~ Render callbacks, which run on the primary view only.

        FRenderContext MakeRenderContext(RHI::FCmdListH CL, ERenderStage Stage);
        void RunRenderCallbacks(RHI::FCmdListH CL, ERenderStage Stage);
        void CallbackDepthPass(RHI::FCmdListH CL);
        void RunShadowRenderCallbacks(RHI::FCmdListH CL, EShadowViewType Type, uint32 Index, const FMatrix4& ViewProjection,
                                      const RHI::FRect& Tile, EFormat DepthFormat);
        bool HasRenderCallbacks(ERenderStage Stage) const;
        void PrepareSceneColorForCallbacks(RHI::FCmdListH CL);

        // Set when a scene with nothing else writing HDR had it cleared for callbacks, so later passes load it.
        bool bSceneColorClearedForCallbacks = false;
        void RestoreAfterRenderCallbacks(RHI::FCmdListH CL);

        FSceneGlobalData MakeSecondaryViewGlobals(const FSceneGlobalData& ViewGlobals);

        // Every rendered view passes through here, so no pass can see a view with a missing optional target.
        void PointAtView(FSceneView& View);

        // On-demand (~230 MB/view at 1080p); an absent FSceneImage reports id -1, which shaders already test for as 0xFFFFFFFF.
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
        bool BuildDepthPyramid(RHI::FCmdListH CL, const FSceneImage& Source, const FSceneImage& Pyramid, bool bReduceMax,
                               uint32 SpdCounterIndex, uint32 SourceScale = 2);

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

        // Custom depth instances drawn again from the camera with nothing occluding them, then their stencil values resolved.
        void CustomDepthStencilPass(RHI::FCmdListH CL);
        void DrawVisBufferBatches(RHI::FCmdListH CL, uint32 ViewIndex, EMeshletSlice Slice, const FUIntVector2& Extent, EFormat VisFormat);
        static bool IsTwoPhaseView(const FFrameData& Frame, uint32 ViewIndex);
        bool HasFarProxies() const;
        // Early sizes the dispatch from the listed proxies; Late compacts the ones its pyramid leaves visible into the second half of the list and draw args.
        void OcclusionCullFarProxies(RHI::FCmdListH CL, ECullPhase::Type Phase);
        // Splats the far proxies the cull listed this frame into the open VisBuffer pass, once per frame after the meshlets.
        void DrawFarProxies(RHI::FCmdListH CL, const FUIntVector2& Extent, EFormat VisFormat, bool bOcclusionCulled);
        static FShaderH GetVisBufferPixelShader();
        const char* FindMissingMeshletDrawBuffer() const;
        void VisBufferClassifyPass(RHI::FCmdListH CL);
        void MaterialGBufferPass(RHI::FCmdListH CL);
        void DeferredLightingPass(RHI::FCmdListH CL);
        void LightFunctionPass(RHI::FCmdListH CL);
        #if USING(WITH_EDITOR)
        void PickerResolvePass(RHI::FCmdListH CL);
        void EditorGridPass(RHI::FCmdListH CL);
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
                if (Views.IsEmpty())
                {
                    FirstCullView = CullView;
                }
                bContiguous = bContiguous && CullView == FirstCullView + (uint32)Views.Num();
                Views.Add({ ShadowDataIndex, ViewIndex, { (float)(Tile.MaxX - Tile.MinX), (float)(Tile.MaxY - Tile.MinY) } });
                Tiles.Add(Tile);
            }

            void Reset()
            {
                Views.Reset();
                Tiles.Reset();
                FirstCullView = Constants::kIndexNoneU32;
                bContiguous   = true;
            }
        };
        FShadowViewRun ShadowViewRunScratch;

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
        const FSceneImage& GetWidgetDepth(RHI::FCmdListH CL, const FUIntVector2& Extent);
        FSceneImage WidgetDepth;

        // Clear for the first overlay pass to write this view's HDR and Load after; bAfterParticles counts particles as a writer.
        RHI::ELoadOp TakeHDRLoadOp(bool bAfterParticles);
        uint64 HDRWrittenTick = 0;
        const FSceneView* HDRWrittenView = nullptr;
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
        void PruneTerrainStates();
        void TerrainCullPass(RHI::FCmdListH CL);

        // Every terrain pass asks the same question, so a terrain skipped by the cull is never drawn with last frame's count.
        bool IsTerrainInView(const FFrameData::FTerrainExtract& Terrain) const;

        void GrassScatterPass(RHI::FCmdListH CL);
        void RetireGrassBlades(RHI::FCmdListH CL);
        // Returns whether it cleared the VisBuffer and depth, which it does when bClear and any terrain drew.
        bool TerrainDepthPrePass(RHI::FCmdListH CL, bool bClear);
        void TerrainRenderPass(RHI::FCmdListH CL);
        // The prefilter reads scene depth into GTAO's own working depth, and the trace reads only that.
        enum class EGTAOStage : uint8 { Prefilter, Trace };
        bool GTAOPass(RHI::FCmdListH CL, EGTAOStage Stage);
        struct FGTAOParams
        {
            uint32 Width                    = 0;
            uint32 Height                   = 0;
            bool   bHalfTrace               = false;
            int32  DepthSlot                = -1;
            float  EffectRadius             = 0.0f;
            float  FalloffRange             = 0.615f;
            float  SampleDistributionPower  = 2.0f;
            float  ThinOccluderCompensation = 0.0f;
            float  FinalValuePower          = 2.2f;
            float  DepthMipSamplingOffset   = 3.3f;
            float  Intensity                = 1.0f;
            int32  QualityLevel             = 3;
            int32  DenoisePasses            = 1;
        };
        bool MakeGTAOParams(FGTAOParams& P) const;
        bool GTAOPrefilterDepth(RHI::FCmdListH CL, const FGTAOParams& P, FShaderH PrefilterCS);
        bool GTAOMain(RHI::FCmdListH CL, const FGTAOParams& P, FShaderH MainCS);
        bool GTAODenoise(RHI::FCmdListH CL, const FGTAOParams& P, FShaderH DenoiseCS);
        // Upsamples a half size Source into Output against GTAO's working depth, which is DepthScale times Source's size.
        bool GTAOUpsample(RHI::FCmdListH CL, const FGTAOParams& P, FShaderH UpsampleCS,
            const FSceneImage& Source, const FSceneImage& Output, uint32 DepthScale);
        // Marches GTAO's working depth, so it needs GTAO on, and only the primary view keeps the color history it gathers.
        bool WantsScreenSpaceGI(const FSceneView& View) const;
        bool IsScreenSpaceGITraced() const;
        void ScreenSpaceGIPass(RHI::FCmdListH CL);
        void CaptureScreenSpaceGIHistory(RHI::FCmdListH CL);
        // The mask exists for either writer, the far trace or the contact march.
        bool WantsSunFarShadowMask() const;
        bool WantsSunFarShadowTrace() const;
        bool SunFarShadowPass(RHI::FCmdListH CL);
        void SunContactShadowPass(RHI::FCmdListH CL);
        bool SunDFShadowCullPass(RHI::FCmdListH CL);
        void TransparentPass(RHI::FCmdListH CL);
        void OITResolvePass(RHI::FCmdListH CL);
        void UnorderedTranslucentPass(RHI::FCmdListH CL);
        // Single source of truth for every fog consumer, including the translucent material pass.
        void PublishFogGlobals(FSceneGlobalData& Globals) const;
        // The extract-time globals of the view being recorded, before its fog and weather terms were published.
        const FSceneGlobalData& GetCurrentViewGlobals() const;
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
        struct FSSRParams
        {
            uint32 ScreenW        = 0;
            uint32 ScreenH        = 0;
            uint32 TraceW         = 0;
            uint32 TraceH         = 0;
            uint32 Scale          = 1;
            uint32 MipCount       = 1;
            uint32 HDRUAV         = 0;
            float  DepthTolerance = 0.01f;
            float  FadeIn         = 0.0f;
            float  FadeOut        = 0.0f;
            float  MaxRoughness   = 0.0f;
            float  Intensity      = 0.0f;
        };
        void SSRDownsample(RHI::FCmdListH CL, const FSSRParams& P, FShaderH DownsampleCS);
        void SSRBuildChain(RHI::FCmdListH CL, const FSSRParams& P, FShaderH LevelCS, const FSceneImage& ChainImage);
        void SSRTrace(RHI::FCmdListH CL, const FSSRParams& P, FShaderH TraceCS);
        void SSRResolve(RHI::FCmdListH CL, const FSSRParams& P, FShaderH ResolveCS);
        void WaterPass(RHI::FCmdListH CL);
        void UnderwaterPass(RHI::FCmdListH CL);
        void EnvironmentPass(RHI::FCmdListH CL);
        void SkyCubeCapturePass(RHI::FCmdListH CL);
        void IrradianceConvolutionPass(RHI::FCmdListH CL);
        void PrefilterEnvMapPass(RHI::FCmdListH CL);
        void DispatchPrefilterMips(RHI::FCmdListH CL, const FSceneImage& Source, const FSceneImage& Target, uint32 DstLayerOffset);
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
        // Only a temporal upscaler averages enough frames to hide rotating noise, and T2x's two-frame blend makes it crawl.
        static uint32 GetScreenSpaceNoiseIndex(const FSceneView& View);
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

        // Extract-phase half: retained sync, then the draw and emit graphs, then the serial lighting tail.
        void CompileDrawCommands_Extract();
        void PreparePostProcess(FFrameData& Frame, const SPostProcessSettings* PostProcess);
        void PreparePrimaryGlobals(FFrameData& Frame, const FViewVolume& ViewVolume, const FViewLayerSettings& Layers);
        bool IsDefaultMaterialReady();
        void PrepareRetainedScene(ECS::FRegistry& Registry);
        void LaunchExtracts(ECS::FRegistry& Registry, FFrameData& Frame);
        void FinalizeLighting(ECS::FRegistry& Registry, FFrameData& Frame);

        void PrepareGatherScratch(FFrameData& Frame);
        void ScheduleSkinnedGather(ECS::FRegistry& Registry);
        void ScheduleLineBatching(ECS::FRegistry& Registry);

        // Each Schedule* builds its views on the main thread and spawns the tasks that use them; the templates take those views.
        void SchedulePrimitiveExtracts(ECS::FRegistry& Registry, FFrameData& Frame);
        void ScheduleBillboardExtract(ECS::FRegistry& Registry, FFrameData& Frame);
        void ScheduleTerrainExtract(ECS::FRegistry& Registry, FFrameData& Frame);
        void ScheduleParticleExtract(ECS::FRegistry& Registry, FFrameData& Frame);
        void ScheduleFogExtract(ECS::FRegistry& Registry, FFrameData& Frame);

        void ExtractBatchedTriangles(const auto& TriangleBatcherView);
        void ExtractWidgets(FFrameData& Frame, const auto& WidgetView, const auto& Transforms);
        void ExtractText(FFrameData& Frame, const auto& TextView, const auto& Transforms, bool bTextMayHaveChanged);
        void ExtractSprites(FFrameData& Frame, const auto& SpriteView, const auto& Transforms);
        void ExtractBillboards(FFrameData& Frame, const auto& BillboardView, const auto& Transforms);
        void ExtractEditorIcons(FFrameData& Frame, const auto& Transforms, const auto& CameraView, const auto& CharacterView,
                                const auto& SkyLightView, const auto& ParticleView, const auto& AudioSourceView, const auto& AudioListenerView);
        void ExtractSelection(FFrameData& Frame, const auto& SelectedView);
        void ExtractTerrain(FFrameData& Frame, const auto& TerrainView, const auto& Transforms);
        void ExtractParticleFields(FFrameData& Frame, const auto& AttractorView, const auto& ColliderView, const auto& Transforms);
        void ExtractParticles(FFrameData& Frame, const auto& ParticleView, const auto& Transforms);
        void ExtractDecals(FFrameData& Frame, const auto& DecalView, const auto& Transforms);
        void ExtractWater(FFrameData& Frame, const auto& WaterView, const auto& Transforms);
        void ExtractFog(FFrameData& Frame, const auto& CloudView, const auto& FogView, const auto& FogVolumeView, const auto& Transforms);
        void ExtractDebugText(FFrameData& Frame);

        // Serial after the light tasks; the skylight reads the sun the directional pass resolved.
        const SEnvironmentComponent* ExtractEnvironment(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractSkyLight(ECS::FRegistry& Registry, FFrameData& Frame, const SEnvironmentComponent* ActiveEnv);
        void ExtractWeather(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractWeatherInteraction(ECS::FRegistry& Registry, FFrameData& Frame);
        void ApplyWeatherToClouds(FFrameData& Frame);

        // Render-phase half: buffer resize + upload commands; reads extract-phase state.
        void CompileDrawCommands_Render(RHI::FCmdListH CL);
        void PlanCullCapacities(RHI::FCmdListH CL, const FFrameData& Frame);
        bool TrackCameraCut(const FFrameData& Frame);
        bool TrackSceneGrowth(const FFrameData& Frame);
        void ReservePreSkinBuffers(RHI::FCmdListH CL, uint32 PreSkinWanted);
        void ReserveMeshletDrawBuffers(RHI::FCmdListH CL, const FFrameData& Frame, uint32 DrawListWanted);
        void ReserveVisibleInstanceBuffers(RHI::FCmdListH CL, const FFrameData& Frame, bool bAfterCameraCut);
        void ReserveMeshletBlockList(RHI::FCmdListH CL, bool bAfterCameraCut, uint32 SceneListFloor);
        void LogCullOverflow();
        void UploadLightData(FFrameData& Frame);
        void PublishSceneRoot(RHI::FCmdListH CL, FFrameData& Frame);
        void PublishViewImageIndices(FFrameData& Frame);
        uint32 CullOverflowLogCounter = 0;

        // Serial pre-pass so the parallel gather is pure reads; O(queued components).
        void ResolveDirtyMeshComponents();

        void ResolveDynamicMeshMaterials(ECS::FRegistry& Registry, FRenderDirtyTracker& Tracker);

        // Reused by the resolve pass for one source's queued entities and one component's watch keys; keep capacity.
        TVector<ECS::FEntity> ResolveDirtyScratch;
        TVector<uint64> ResolveKeyScratch;

        // Dirty-slot runs for the partial retained upload; keeps capacity.
        TVector<FUIntVector2> RetainedRunScratch;

        // Reused to flatten a component's material overrides; keeps capacity.
        TVector<CMaterialInterface*> ResolveOverrideScratch;

        // Routes this frame's transform + component changes into the primitive table. O(changed).
        void SyncScenePrimitives();

        // Skeletal primitives only; statics flow through the retained batch registry, not a per-frame gather.
        void CullSkinnedPrimitives(const Jobs::FParallelRange& Range, FThreadLocalDrawData& Local);
        void LayoutSkinnedBoneSlices(TVector<FThreadLocalDrawData>& ThreadLocal, const auto& FollowerStorage, const auto& SkeletalStorage);
        void EmitSkinnedPrimitives(const Jobs::FParallelRange& Range, FThreadLocalDrawData& Local, const auto& SkeletalStorage);

        void BuildSceneCullContext(const SDirectionalLightComponent* Sun);
        void MergeMeshDrawData(TVector<FThreadLocalDrawData>& ThreadLocal);

        FThreadLocalDrawData& AcquireThreadLocalDrawData(uint32 Slot);

        // Per-primitive base in the bone arena; kNoBoneSlice for anything not gathered this frame.
        TVector<uint32>                        BoneSliceByPrimitive;
        // Primitive indices the cull kept, dense, filled straight from the parallel cull via a cursor.
        TVector<uint32>                        SkinnedCandidates;
        TVector<uint32>                        SkinnedCandidateBones;
        TVector<uint32>                        PendingSliceAllocs;
        // Candidates that may skin from their leader's slice, settled once every own slice is laid out.
        TVector<uint32>                        SharedPoseCandidates;
        TAtomic<uint32>                        SharedPoseCursor{0};
        // Per primitive, set when this frame's slice is the leader's, so the emit leaves it to the leader.
        TVector<uint8>                         BoneSliceBorrowed;
        // Ranges the layout packed for ungathered leaders, kept apart because other gather tasks own the thread locals then.
        TVector<FUIntVector2>                  LeaderPoseUploadRanges;
        TAtomic<uint32>                        SkinnedCandidateCursor{0};
        TAtomic<uint32>                        PendingSliceCursor{0};
        uint32                                 SkinnedCandidateCount = 0;
        // Snapshot of the primitive set's dense skeletal list, captured before the parallel cull.
        const uint32*                          SkeletalPrimitiveIndices = nullptr;
        // Frames a slice survives ungathered, so a mesh flicking through the cull does not thrash its base.
        static constexpr uint32                kBoneSliceGraceFrames = 300;
        uint32                                 BoneSliceFrameNumber = 0;
        TVector<uint64>                        BoneDirtyChunks;
        // Leaders the layout packs itself because nothing else gathered them this frame.
        TVector<uint32>                        UngatheredLeaderScratch;

        // An editor icon the light pass records while it already holds the light and its position.
        struct FLightIcon
        {
            ECS::FEntity   Entity;
            FVector3       Position;
            FVector3       Color;
        };

        // One fixed chunk of dense light slots, so a chunk never overflows and the commit order never depends on scheduling.
        struct FLightBatch
        {
            static constexpr uint32 Capacity = 32;
            FLight         Lights[Capacity];
            // LightIndex holds the slot within Lights until the batch is committed.
            FShadowRequest Shadows[Capacity];
            FLightFunctionRequest LightFunctions[Capacity];
            FSceneCullContext::FLightSphere ShadowLights[Capacity];
            FLightIcon     Icons[Capacity];
            uint32         NumLights  = 0;
            uint32         NumShadows = 0;
            uint32         NumLightFunctions = 0;
            uint32         NumShadowLights = 0;
            uint32         NumIcons = 0;
        };

        // Point chunks first, then spot, then area, each in dense order.
        TVector<FLightBatch>                   LightBatches;
        uint32                                 NumPointLightChunks = 0;
        uint32                                 NumSpotLightChunks  = 0;
        uint32                                 NumAreaLightChunks  = 0;
        TVector<FLightIcon>                    DirectionalLightIcons;
        bool                                   bRecordLightIcons = false;

        void ScheduleLightExtract(ECS::FRegistry& Registry);
        const SDirectionalLightComponent* SelectActiveSun(const auto& DirectionalView, const auto& Transforms);
        void ProcessLightChunk(uint32 Chunk, const auto& PointView, const auto& SpotView, const auto& AreaView, const auto& Transforms);
        void CommitLightBatches(uint32 FirstLocalLight);
        void ProcessPointLight(ECS::FEntity Entity, const SPointLightComponent& PointLight, const STransformComponent& TransformComponent, FLightBatch& Batch);
        void ProcessSpotLight(ECS::FEntity Entity, const SSpotLightComponent& SpotLight, const STransformComponent& TransformComponent, FLightBatch& Batch);
        void ProcessAreaLight(ECS::FEntity Entity, const SAreaLightComponent& AreaLight, const STransformComponent& TransformComponent, FLightBatch& Batch);
        void ProcessDirectionalLight(const SDirectionalLightComponent& DirectionalLight);

        void AllocateShadowTiles();
        // Hands each resolvable light function an atlas tile and flags its light, serially after the light tasks.
        void ResolveLightFunctions();
        
        void BuildCullViews(const FViewVolume& ViewVolume);
        bool HasCustomDepthPrimitives() const { return ScenePrimitives.GetCustomDepthPrimitiveCount() > 0u; }
        
        uint32 PrepareBatchedLines(FLineBatcherComponent& Batcher);
        void   BatchLineChunks(const Jobs::FParallelRange& Range);
        void   FinalizeBatchedLines(FLineBatcherComponent& Batcher);
        void   ProcessBatchedTriangles(FTriangleBatcherComponent& Batcher);

        void NotifyMaxLightsHit();

        void BuildLightRelevanceVolumes();

        // Every volume that shades from the shared light buffer, so a light rejected here reaches nothing; bInCamera skips the camera's own test.
        bool IsLightRelevant(const FVector3& LightPosition, float LightRadius, bool bInCamera) const;
        
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
            TInlineVector<RHI::FColorTarget, 4> ColorTargets;
        };

        // Mirrors TRI_CULL_* in Includes/MeshletGeometry.slang.
        enum ETriCullFlags : uint8
        {
            TriCull_None      = 0,
            TriCull_Backface  = 1 << 0,   // only when the pipeline also sets ECullMode::Back
            TriCull_SmallPrim = 1 << 1,   // single-sample, non-wireframe pipelines only
        };

        RHI::FPipelineH      GetOrCreatePipeline(const FGraphicsPipelineKey& Key);
        void BeginHDROverlayPass(RHI::FCmdListH CL, RHI::ELoadOp HDRLoadOp, bool bWithPicker, const RHI::FBlendDesc& HDRBlend, FGraphicsPipelineKey& Key);

        // Starts a background build on a miss and returns null until it lands, for pipelines a frame can draw without.
        RHI::FPipelineH      FindPipeline(const FGraphicsPipelineKey& Key);
        static uint64        HashGraphicsKey(const FGraphicsPipelineKey& Key);

        // The default material's stage, which stands in for a material whose pipeline is still compiling.
        static FShaderH      DefaultMaterialStage(EMaterialShaderStage Stage);
        static TMoveOnlyFunction<RHI::FPipelineH()> MakeGraphicsBuild(const FGraphicsPipelineKey& Key);
        static TMoveOnlyFunction<RHI::FPipelineH()> MakeComputeBuild(FShaderH CS, TSpan<const RHI::FSpecializationConstant> Constants);

        // Everything here is per PASS, not per batch.
        struct FMeshletPassContext
        {
            uint32            CullViewIndex     = 0;
            int32             ShadowDataIndex   = -1;
            int32             ShadowViewIndex   = 0;
            float             ViewportW         = 0.0f;
            float             ViewportH         = 0.0f;
            // Non-empty for a draw over consecutive shadow cull views starting at CullViewIndex.
            RHI::TGPUSpan<FShadowRasterViewGPU> ShadowViews;
            // The two VisBuffer phases each take their own slice; single-phase passes take All, which is final by the time it runs.
            EMeshletSlice     Slice             = EMeshletSlice::All;
        };

        // The sole authority: nothing rasterizes a meshlet this did not keep.
        void MeshletCullPass(RHI::FCmdListH CL, EMeshletSlice Slice, bool bCascadePhase = false);
        void CopyCullTotalsForReadback(RHI::FCmdListH CL);

        void DrawMeshletBatch(RHI::FCmdListH CL, const FMeshDrawCommand& Batch, const FMeshletPassContext& Ctx);

        // Survivors one sub-draw covers, the mesh workgroup limit or one unsplit instanced draw on the vertex path.
        static uint32 MeshletsPerSubDraw()
        {
            return RHI::SupportsMeshShaders() ? Math::Max(RHI::GetMaxMeshWorkGroupCount(), 1u) : kVertexPathMeshletsPerDraw;
        }

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

        // Stage all of one buffer's runs before starting the next, or nothing groups.
        void StageWrite(RHI::GPUPtr Dst, const void* Data, uint64 Size, bool bFillBeforeSubmit = false);
        // Launched once the retained slots are synced, so the bounds build on workers while the rest of the frame records.
        void LaunchInstanceBlockBounds();
        void JoinInstanceBlockBounds();
        void FinishInstanceBlockBounds(RHI::FCmdListH CL);
        void DispatchMeshletBlockBuild(RHI::FCmdListH CL, bool bLatePass, bool bCascadePhase = false);
        bool bSplitMeshletPhasesThisFrame = false;
        void FlushStagedWrites(RHI::FCmdListH CL);

        // The GPU reads staging only once the list is submitted, so these PCIe-bound fills overlap the rest of the recording.
        void LaunchDeferredStageFills();
        void WaitDeferredStageFills();
        void RunDeferredStageFills(uint32 NumFills);

        // Scattered (Start, Count) runs of one array, into a single ring block and one copy command.
        void WriteBufferRuns(RHI::FCmdListH CL, RHI::GPUPtr Dst, const void* Src, uint64 Stride,
                             const TVector<FUIntVector2>& Runs, bool bFillBeforeSubmit = false);

        // Clears the active bit of each listed slot in the retained cull entries on the GPU.
        void DeactivateRetainedSlots(RHI::FCmdListH CL, const TVector<uint32>& Slots);

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
            uint32             CustomDepthCullView = Constants::kIndexNoneU32;
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
        uint32                                             StaleValidationCursor = 0;
        uint32                                             StaleValidationSurfaces = 0;
        uint32                                             StaleValidationEntities = 0;
        uint32                                             StaleValidationMissed = 0;
        uint8                                              StaleValidationSource = 0;
        TArray<FSceneImage, (int)ENamedImage::Num>          NamedImages = {};

        
        static constexpr uint32                 BLOOM_MIP_COUNT = 8;
        
        static constexpr uint32                 MaxSceneViews = 16;
        TVector<FSceneView>                     SceneViews;
        FSceneView*                             CurrentView = nullptr;
        
        uint32                                  CurrentCameraEarlyView = 0;

        FDelegateHandle                         SwapchainResizedHandle;

        FUIntVector3                            FroxelGridSize = FUIntVector3(160, 90, 128);

        // What an IBL bake last consumed, so a frame rebakes only when one of these moved.
        struct FIBLBakeSource
        {
            FEnvironmentParams                  Params       = {};
            int32                               MapID        = -1;
            FVector3                            SunDirection = FVector3(0.0f);
            bool                                bHasSun      = false;
            bool                                bValid       = false;
        };
        FIBLBakeSource                          SkyCubeBakeSource;
        FIBLBakeSource                          ConvolutionBakeSource;
        bool TakeIBLSourceChange(FIBLBakeSource& Source, const FFrameData& Frame, float SunCosThreshold, bool bResolutionChanged);
        void UpdateIBLDirty(FFrameData& Frame);

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

        // Splines are extracted only for components with bSendToGPU, so a world that authors splines purely as data uploads nothing.
        RHI::TGPUSpan<FGPUSpline>               SplineBufferSpan;
        RHI::TGPUSpan<FGPUSplinePoint>          SplinePointBufferSpan;
        RHI::TGPUSpan<FGPUSplineSample>         SplineSampleBufferSpan;
        uint32                                  NumActiveSplines       = 0;

        // The pool write epoch an incremental extract last acted on; see PoolMayHaveChanged.
        struct FPoolWatch
        {
            uint64 Epoch = ~0ull;
        };

        // A spline's upload as of the inputs hashed into Key, rebuilt only when that key moves.
        struct FSplineExtractCache
        {
            uint64                   Key = 0;
            uint64                   DataKey = 0;
            uint32                   SeenSerial = 0;
            bool                     bValid = false;
            FGPUSpline               Header = {};
            TVector<FGPUSplinePoint> Points;
            TVector<FGPUSplineSample> Samples;
        };

        struct FProbeSortEntry
        {
            int32                   Priority = 0;
            FGPUReflectionProbe     Gpu = {};
            FReflectionProbeCapture Capture = {};
        };

        struct FProbeExtractCache
        {
            uint64          Key = 0;
            uint32          SeenSerial = 0;
            bool            bValid = false;
            FProbeSortEntry Entry;
        };

        ECS::TEntityMap<FSplineExtractCache>    SplineCache;
        TVector<FSplineSample>                  SplineSampleScratch;
        FPoolWatch                              SplinePoolWatch;
        ECS::TEntityMap<FProbeExtractCache>     ProbeCache;
        TVector<FProbeSortEntry>                ProbeSortScratch;
        // Last extract's probe walk, so an unchanged set skips the sort and the bake diff.
        TVector<ECS::FEntity>                   ProbeOrder;

        void ScheduleSplineExtract(ECS::FRegistry& Registry, FFrameData& Frame);
        void ExtractSplines(FFrameData& Frame, const auto& SplineView, bool bDataMayHaveChanged);
        void BuildSplineCache(FSplineExtractCache& Cache, ECS::FEntity Entity, const SSplineComponent& Spline, const FMatrix4& LocalToWorld);

        void InitReflectionProbeTargets();
        void SyncProbeCaptureCube(uint32 FaceSize);
        void SyncCascadeAtlas(int32 CascadeCount);
        void ExtractReflectionProbes(ECS::FRegistry& Registry, FFrameData& Frame);
        bool GatherReflectionProbes(ECS::FRegistry& Registry);
        static FProbeSortEntry MakeProbeSortEntry(const SReflectionProbeComponent& Probe, const FMatrix4& WorldMatrix);
        void DiffReflectionProbesAgainstBake(FFrameData& Frame);
        void ScheduleReflectionProbeBake(FFrameData& Frame);
        // Renders the six faces, copies each into the scratch cube, prefilters into the probe's slice.
        void ReflectionProbeBakePass(RHI::FCmdListH CL);
        
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
        Jobs::FTaskEvent                                                DeferredStageFillsDone;
        bool                                                            bDeferredStageFillsRunning = false;
        std::atomic<uint32>                                             DeferredFillCursor{0};
        bool                                                            bDeferredUploadsQueued = false;
        bool                                                            bDeferredGatherQueued  = false;
        TVector<RHI::FBufferCopy>                                       UploadCopyScratch;
        TVector<uint64>                                                 UploadCursorScratch;
        uint64                                                          CurrentSceneRootAddr = 0;
        // Builds the per-view FSceneRoot transient (shared addrs + view camera/clusters/IBL) -> address.
        uint64 BuildViewSceneRoot(FSceneView& View);
        void   EnsureClusterLightMaskCapacity(FSceneView& View);

        // OR-accumulated by the material lanes over STREAMING_FEEDBACK_WINDOW frames, then copied to a readback slot; read kFramesInFlight later.
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
        TArray<FSceneBuffer, RHI::kFramesInFlight> SkinWorkEntryRing = MakeSceneRing<RHI::kFramesInFlight>("Skinning.WorkEntry", 1.5f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> SkinDispatchArgsRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.SkinDispatchArgs", 1.0f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> MeshletCullDispatchArgsRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.MeshletCullDispatchArgs", 1.0f);
        // One slice more than frames in flight, since movers fill the next frame's slice before that frame waits on its fence.
        static constexpr uint32 kTransformStreamSlices = RHI::kFramesInFlight + 1;
        // CPU-written per frame, the moved transforms the movers and the extract stream for the GPU to scatter.
        TArray<FSceneBuffer, kTransformStreamSlices> TransformStreamRing = MakeSceneRing<kTransformStreamSlices>("Scene.TransformStream", 1.25f);
        uint32                                       TransformStreamSlot = 0;
        // Counts extracts, which picks each frame's slice so a slice is reused only after its frame's fence.
        uint64                                       TransformStreamSerial = 0;
        TArray<FSceneBuffer, RHI::kFramesInFlight> InstanceViewRangeRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.InstanceViewRanges", 1.25f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> TotalsRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.Totals", 1.0f);
        TArray<bool, RHI::kFramesInFlight>                                  TotalsZeroed = {};

        RHI::FGPUAllocation GetTotals() const { return TotalsRing[CurrentFrameSlot]; }

        FSceneBuffer RetainedCullEntryBuffer { "Retained.CullEntries", 1.5f, EBufferInit::Zeroed };
        FSceneBuffer InstanceBlockBoundsBuffer { "Retained.InstanceBlockBounds", 1.5f, EBufferInit::Zeroed };

        // The sun's distance-field casters for this frame and the grid they are binned into.
        FSceneBuffer SunDFObjectBuffer    { "SunDF.Objects",    kSceneBufferGrowth, EBufferInit::Undefined, false };
        FSceneBuffer SunDFCellCountBuffer { "SunDF.CellCounts", 1.0f, EBufferInit::Zeroed,    false };
        FSceneBuffer SunDFCellItemBuffer  { "SunDF.CellItems",  kSceneBufferGrowth, EBufferInit::Undefined, false };
        FSceneBuffer SunDFCounterBuffer   { "SunDF.Counters",   1.0f, EBufferInit::Zeroed,    false };
        FSceneBuffer SunDFBlockListBuffer { "SunDF.Blocks",     1.5f, EBufferInit::Undefined };
        FSceneBuffer SunDFLargeObjectBuffer { "SunDF.LargeObjects", 1.0f, EBufferInit::Undefined, false };
        // The cell items and casters each frame's binning asked for, read back to grow the item and object buffers to the demand.
        TArray<FSceneBuffer, RHI::kFramesInFlight> SunDFDemandReadback = MakeSceneRing<RHI::kFramesInFlight>("Readback.SunDFDemand", 1.0f);
        static constexpr uint32 kSunDFGridDimension = 128u;
        static constexpr uint32 kSunDFCellCapacity  = 32u;
        static constexpr uint32 kSunDFMinObjects    = 16384u;
        static constexpr uint32 kTransformStreamGroupSize = 64u;
        // A caster past this count bins in the cull pass itself, slower but never dropped.
        static constexpr uint32 kSunDFMaxLargeCasters = 256u;
        bool                    bSunDFGridReady     = false;
        TVector<FInstanceBlockBounds> InstanceBlockBounds;
        TVector<uint32>               DirtyInstanceBlocks;
        TVector<uint32>               BlockMergeScratch;
        // Per block, whether it holds a distance-field caster, and the compact list the sun's distance-field cull walks.
        TVector<uint8>                InstanceBlockHasDF;
        TVector<uint32>               DistanceFieldBlocks;
        bool                          bDistanceFieldBlocksDirty = true;
        TVector<FUIntVector2>         GPUWrittenSlotRanges;
        bool                          bInstanceBlockBoundsValid = false;
        Jobs::FTaskEvent              InstanceBlockBoundsDone;
        bool                          bBlockBoundsRunning = false;
        std::atomic<bool>             bBlockDFChanged{ false };
        bool                          bBlockBoundsLaunched   = false;
        bool                          bBlockBoundsRebuildAll = false;
        uint32                        BlockBoundsNumBlocks   = 0;
        FSceneBuffer RetainedTransformBuffer { "Retained.Transforms", 1.5f, EBufferInit::Zeroed };
        FSceneBuffer RetainedStaticBuffer { "Retained.Static", 1.5f, EBufferInit::Zeroed };
        // Parallel to the static buffer, the splat each slot draws as once it is too small for meshlets.
        FSceneBuffer RetainedProxyBuffer { "Retained.Proxies", 1.5f, EBufferInit::Zeroed };
        FSceneBuffer SurfaceDescBuffer { "Retained.SurfaceDescs", 1.5f, EBufferInit::Zeroed };
        // Slots CullInstances turned into proxies this frame and the indirect draw that splats them, each followed by the late occlusion cull's copy.
        TArray<FSceneBuffer, RHI::kFramesInFlight> ProxyListRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.ProxyList", 1.5f);
        TArray<FSceneBuffer, RHI::kFramesInFlight> ProxyDrawArgsRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.ProxyDrawArgs", 1.0f);
        uint32 ProxyListCapacity = 0;
        // One arena per frame slot, since the next frame's upload can run on compute while this frame's passes still read poses.
        TArray<FSceneBuffer, RHI::kFramesInFlight> BoneArenaRing = MakeSceneRing<RHI::kFramesInFlight>("Skinning.BoneArena", 1.5f);
        // The upload serial each slot's arena last matched the mirror at, so it knows which dirty ranges it missed.
        TArray<uint64, RHI::kFramesInFlight>       BoneArenaSyncedSerial = {};
        uint64                                     BoneUploadSerial = 0;
        TVector<FUIntVector2>                      LastBoneUploadRanges;
        RHI::FGPUAllocation GetBoneArena() const { return BoneArenaRing[CurrentFrameSlot]; }
        // Snapshots taken before each frame's incremental uploads land, so motion vectors have a past.
        RHI::FGPUAllocation                                        PrevRetainedTransformBuffer;
        bool                                                bPrevMotionStateValid = false;
        TVector<FUIntVector2>                               MotionGrassRangeScratch;
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

        // Every gathered slot's surface meshlets, which bounds the skinning dispatch and sizes its work-entry map.
        uint32                                              SkinWorkCapacity = 0;
        // Slots the retained static buffer actually holds, which bounds the header lookup in that pass.
        uint32                                              RetainedStaticCapacity = 0;
        uint32                                              UploadedSurfaceDescs = 0;

        TArray<FSceneBuffer, RHI::kFramesInFlight> VisibleInstanceRing = MakeSceneRing<RHI::kFramesInFlight>("Cull.VisibleInstances", kSceneBufferGrowth);


        // CPU-fed head of the visible buffer, which CullInstances never claims and so never writes a range for.

        RHI::FGPUAllocation GetVisibleInstances()  const { return VisibleInstanceRing[CurrentFrameSlot]; }


        // Must run before anything reads Bones(): the skinning dispatch and the in-draw skinning fallback both do.
        void UploadBoneArena(RHI::FCmdListH CL, const FFrameData& Frame);

        // Must run before CullInstances and before the skinning dispatch.
        void UploadSkinnedFrameData(RHI::FCmdListH CL, FFrameData& Frame);
        void SkinnedMeshletBoundsPass(RHI::FCmdListH CL, const FFrameData& Frame);
        // Slots whose list and frame data reached the GPU this frame, zero when either buffer failed to grow.
        uint32 SkinnedSlotsUploaded = 0;

        void DispatchGPUSceneCull(RHI::FCmdListH CL, const FFrameData& Frame);
        bool UploadRetainedInstances(RHI::FCmdListH CL, const FFrameData::FGeometry::FRetainedUpload& Upload);
        void WriteRetainedInstances(RHI::FCmdListH CL, const FFrameData::FGeometry::FRetainedUpload& Upload);
        void UploadSurfaceDescs(RHI::FCmdListH CL, const FFrameData::FGeometry::FRetainedUpload& Upload);
        void DispatchCullInstances(RHI::FCmdListH CL, const FFrameData& Frame, FShaderH CullInstancesShader);
        void DispatchDrawPrefix(RHI::FCmdListH CL, const FFrameData& Frame, FShaderH DrawPrefixShader);
        // False when the bucket list could not grow to every view and batch, which skips the frame's meshlet draws.
        bool bCullBucketsValid = false;

        void PublishRetainedUpload();

        // Room for this many moved records in this frame slot's stream, or an empty target when it cannot be had.
        FTransformStreamTarget ReserveTransformStream(uint64 Serial, uint32 NumMoved);
        // Opens the next frame's slice to the movers, when this frame moved enough to be worth it.
        void ArmTransformStream(ECS::FRegistry& Registry);
        void ScatterTransformStream(RHI::FCmdListH CL, const FFrameData::FGeometry::FRetainedUpload& Upload);

        std::atomic<uint32>                                 RetainedDeviceCapacity{0};

        std::atomic<bool>                                   bDepthPyramidValid{false};

        std::atomic<bool>                                   bCascadePyramidValid{false};

        static constexpr uint32                             kReceiverMaskDim = 128;
        FSceneBuffer                                        ReceiverMaskBuffer { "Shadow.ReceiverMask", 1.0f };
        bool                                                bShadowReceiverCullThisFrame = false;
        bool                                                bReceiverMaskValid = false;
        RHI::TGPUSpan<uint32>                               ReceiverMaskFinal;
        bool WantsShadowReceiverCull(const FFrameData& Frame) const;
        void ShadowReceiverMaskPass(RHI::FCmdListH CL);

        // The graphics value the next frame's compute cull waits on, the last submission that reads what the cull rewrites.
        uint64                                              FrameCullGateValue = 0;
        // Set when the cascade pyramid was released to compute for the next frame's cull, and names the image released.
        RHI::FTextureH                                      CascadePyramidOnCompute = {};
        bool ShouldCullOnCompute(const FFrameData& Frame) const;
        static bool HasGrassScatter(const FFrameData& Frame);

        FSceneImage                                         RainOcclusionImage;
        FSceneBuffer                                        RainVisibleBuffer  { "Weather.RainVisible", 1.25f };
        FSceneBuffer                                        RainArgsBuffer     { "Weather.RainArgs", 1.0f };
        FSceneBuffer                                        RainSplashBuffer   { "Weather.Splashes", 1.0f, EBufferInit::Zeroed };
        uint32                                              RainSplashCursor   = 0;
        uint32                                              WeatherIdleFrames  = 0;
        FSceneBuffer                                        SnowVisibleBuffer  { "Weather.SnowVisible", 1.25f };
        FSceneBuffer                                        SnowArgsBuffer     { "Weather.SnowArgs", 1.0f };

        // Ping-pong ripple, foam and trail field the interaction pass steps around the camera.
        FSceneImage                                         WeatherFieldImages[2];
        FSceneBuffer                                        WeatherSplashBuffer { "Weather.InteractionSplashes", 1.0f, EBufferInit::Zeroed };
        uint32                                              WeatherSplashCursor        = 0;
        // Frames since the last splash spawned, so the draw stops once every splash has run its course.
        uint32                                              WeatherSplashQuietFrames   = ~0u;
        uint32                                              WeatherFieldSimulatedSerial = 0;

        // Kept by the extract, which decides each frame's passes, images and window ahead of the render.
        uint32                                              WeatherFieldParity         = 0;
        uint32                                              WeatherFieldQuietFrames    = ~0u;
        uint32                                              WeatherFieldSerial         = 0;
        float                                               WeatherFieldAccumulator    = 0.0f;
        FIntVector2                                         WeatherFieldOriginTexel    = FIntVector2(0);
        uint32                                              WeatherFieldResolution     = 0;
        float                                               WeatherFieldTexelWorld     = 0.0f;

        static constexpr uint32 kWeatherRainSplashCapacity   = 8192u;
        static constexpr uint32 kWeatherImpactSplashCapacity = 512u;

        void PublishWeatherGlobals(FSceneGlobalData& Globals);
        void WeatherOcclusionPass(RHI::FCmdListH CL);
        void WeatherInteractionPass(RHI::FCmdListH CL);
        // Rain streaks and splashes, snow flakes and interaction splashes, all in one pass over the HDR target.
        void WeatherDrawPass(RHI::FCmdListH CL);
        void ReleaseIdleWeatherResources();

        FMatrix4                                            CascadeHZBViewProjection[NumCascades] = {};
        FVector4                                            CascadeHZBNdcScale[NumCascades] = {};
        bool                                                bCascadeHZBTransformsValid = false;

        float                                               CascadeMinTexels = 1.0f;
        float                                               CasterMinScreenRadius = 0.0f;
        int32                                               ActiveCascadeCount = NumCascades;
        int32                                               NearCascadeCount = NumCascades;
        // Cascades the atlas and pyramid are sized for; PrepareRender reallocates when ActiveCascadeCount moves.
        int32                                               CascadeAtlasCount = 0;

        static constexpr uint32                             kTotalsSlots = 9;

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
        uint32                                              VisiblePeak = 0;
        FCullView                                           LastCameraCullView = {};
        bool                                                bHasLastCameraCullView = false;
        uint32                                              CameraCutFramesLeft = 0;
        // A load appends before its readback lands, so the lists hold the whole scene for the frames in between.
        uint32                                              SceneFloorFramesLeft = 0;
        uint32                                              LastSceneVisibilityWords = 0;
        uint32                                              LastSceneGrassSlots = 0;
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

        // One scatter cursor per (terrain, species position); each records the block it last scattered into, so a reshuffle retires stale blades.
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
        FName                                           LastMissingMeshletDrawBuffer;
        
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
        THashMap<ECS::FEntity, TVector<SParticleCollision>> ParticleCollisionResults;
        void ReadParticleCollisions();
        void CopyParticleCollisions(RHI::FCmdListH CL);

        // One frame slot's copies of the grids of emitters that read theirs back, laid out one after another in Buffer.
        struct FParticleGridReadback
        {
            RHI::FGPUAllocation             Buffer = {};
            TVector<FParticleGridSnapshot>  Layouts;
            TVector<RHI::GPUPtr>            Grids;
            TVector<ECS::FEntity>           Owners;
        };
        TArray<FParticleGridReadback, RHI::kFramesInFlight> ParticleGridReadback = {};
        THashMap<ECS::FEntity, TVector<FParticleGridSnapshot>> ParticleGridResults;
        void ReadParticleGrids();
        void CopyParticleGrids(RHI::FCmdListH CL);
        THashMap<uint64, TVector<RHI::FGPUAllocation>>    ParticleBufferPool;
        uint64                                            ParticlePoolBytes = 0;
        // Set once a pooled buffer is handed out this frame, since its previous frame's reads need a barrier before the clear.
        bool                                              bParticlePoolReuseBarrierIssued = false;
        // Emitters given buffers this frame, held under a cap so a mass spawn ramps in over a few frames.
        uint32                                            ParticleSetupsThisFrame = 0;

        // One simulation dispatch, planned before any is recorded so independent emitters can share a barrier.
        struct FParticleSimStep
        {
            RHI::FPipelineH Pipeline;
            // Where this step's FParticleSimArgs sit in ParticleSimArgsScratch.
            uint32          ArgsOffset = 0;
            // Zero for a batched step, whose counters sit in ParticleCounterArena and are cleared together.
            RHI::GPUPtr     SpawnCounter = 0;
            RHI::GPUPtr     EventCounts = 0;
            uint32          Groups = 0;
            // Where Groups starts within the emitter, for work split across dispatches by the group limit.
            uint32          FirstGroup = 0;
            // Zeroed before the dispatch, the way the neighbor counts are before particles are bucketed again.
            RHI::GPUPtr     ClearAddress = 0;
            uint64          ClearBytes = 0;
            bool            bBarrierAfter = false;
        };
        // Spawn counters for every batched emitter, so one fill clears them all.
        RHI::FGPUAllocation                               ParticleCounterArena = {};
        uint32                                            ParticleCounterArenaSlots = 0;
        uint32                                            ParticleCounterSlotsUsed = 0;
        // Per-emitter sort arguments gathered for the single compact sort dispatch. Reused each frame.
        TVector<uint8>                                    ParticleSortScratch;
        // Planned simulation arguments, and the emitter and group tables of the dispatch being built. Reused each frame.
        TVector<uint8>                                    ParticleSimArgsScratch;
        TVector<uint8>                                    ParticleSimBatchEmitters;
        TVector<FUIntVector2>                             ParticleSimBatchGroups;
        // One planned particle draw, so consecutive ones on a pipeline go out as a single multi-draw.
        struct FParticleDraw
        {
            RHI::FPipelineH Pipeline;
            bool            bWriteDepth = false;
        };
        // Planned draws with their FParticlePushConstants and FParticleDrawArgsSource bytes. Reused each frame.
        TVector<FParticleDraw>                            ParticleDraws;
        TVector<uint8>                                    ParticleDrawConstants;
        TVector<uint8>                                    ParticleDrawArgSources;
        TVector<uint32>                                   ParticleShadowVertexCounts;
        // Every particle draw's indirect arguments back to back, gathered on the GPU each frame.
        RHI::FGPUAllocation                               ParticleDrawArgsArena = {};
        uint32                                            ParticleDrawArgsArenaSlots = 0;
        // Single-step emitters with no parent run back to back; the rest keep a barrier around every step. Reused each frame.
        TVector<FParticleSimStep>                         ParticleBatchedSteps;
        TVector<FParticleSimStep>                         ParticleSerialSteps;
        void RecordParticleSimSteps(RHI::FCmdListH CL);
        struct FParticleSimFrame;
        struct FParticleEmitterSim;
        void PruneParticleStates();
        void PrepareParticleSimFrame(FParticleSimFrame& Sim);
        FParticleGPUState* AcquireParticleEmitterState(RHI::FCmdListH CL, const FFrameData::FParticleExtract& Item, FParticleSimFrame& Sim);
        RHI::FPipelineH ResolveParticleSimPipeline(const FFrameData::FParticleExtract& Item, FParticleSimFrame& Sim);
        void SimulateParticleEmitter(RHI::FCmdListH CL, const FFrameData::FParticleExtract& Item, FParticleSimFrame& Sim);
        void PlanParticleSimStep(FParticleEmitterSim& E, FParticleSimFrame& Sim, uint32 StepIndex);
        void QueueParticleEmitterOutputs(FParticleEmitterSim& E, FParticleSimFrame& Sim);
        THashSet<ECS::FEntity>                            ParticleLiveScratch;

        FScenePrimitiveSet                      ScenePrimitives;
        TVector<ECS::FEntity>                   MovedTransformScratch;
        uint32                                  MovedTransformConsumer = Constants::kIndexNoneU32;
        TVector<uint64>                         DirtySlotBits;

        // Merge scratch, sized by draw slots (not by entities). Members so capacity survives frames.

        TVector<FShaderH>            BinnedDeferredSlotShaders;
        TVector<uint8>               BinnedDeferredSlotKinds;
        // Bit per EMaterialShadingModel among the binned materials, which specializes the lighting dispatch.
        uint32                       BinnedShadingModelMask = 0u;
        TVector<uint32>                         BinnedDeferredSlotByMaterial;
        // Shader handle -> its dense bin, so binning stays linear in the visible material count.
        THashMap<uint64, uint32>                BinnedDeferredSlotLookup;
        bool                                    bWarnedDeferredBinExtent = false;
        bool                                    bWarnedDeferredSlotCap   = false;
        double                                  LastDeferredSlotCapToast = -1.0e9;

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
        // Zeroed at the top of the classify pass before any early return, so a bail-out frame cannot leave later passes on a stale slot table.
        FMaterialClassifyLayout                 MaterialClassifyLayout;

        bool BuildDeferredMaterialBinning(RHI::FCmdListH CL);

        TVector<uint32>                         ShadowSizeScratch;
        TVector<uint32>                         ShadowSortedScratch;
        // Only the over-budget view trim uses these; persistent so that path allocates nothing.
        TVector<uint32>                         ShadowDropOrderScratch;
        TVector<bool>                           ShadowDropScratch;

        struct FDecalSortEntry { CMaterial* ShaderOwner; int32 SortOrder; FGPUDecal Gpu; };
        TVector<FDecalSortEntry>                DecalSortScratch;

        struct FDecalMaterialState
        {
            CMaterial* ShaderOwner   = nullptr;
            int32      MaterialIndex = -1;
            bool       bUsable       = false;
        };
        // Reset every extract, so a material resolves once per frame however many decals share it.
        TVector<FDecalMaterialState>            DecalMaterialStates;
        THashMap<CMaterialInterface*, int32>    DecalMaterialLookup;

        struct FDecalExtractCache
        {
            uint64   Key = 0;
            uint32   SeenSerial = 0;
            bool     bValid = false;
            FMatrix4 DecalToWorld = FMatrix4(1.0f);
            FMatrix4 WorldToDecal = FMatrix4(1.0f);
        };
        ECS::TEntityMap<FDecalExtractCache>     DecalCache;
        FPoolWatch                              TextPoolWatch;
        ECS::TEntityMap<FTextRenderCache>       TextCache;

        FDecalMaterialState FindOrResolveDecalMaterial(CMaterialInterface* Material);

        // What one emitter of an asset contributes whatever the instance, worked out once per asset per extract.
        struct FParticleEmitterStatics
        {
            CParticleEmitter* Emitter            = nullptr;
            int32             EmitterIndex       = 0;
            int32             SourceEmitterIndex = -1;
            uint32            RaisedEventMask    = 0u;
            float             RaisedEventRate    = 0.0f;
            uint32            MeshletHeaderSlot  = 0u;
            uint32            MeshletCount       = 0u;
            uint32            EmissionMeshSlot   = 0u;
            uint32            TextureIndex       = 0u;
            uint32            GridTargetSlot     = RHI::kInvalidHeapSlot;
            FUIntVector2      GridTargetExtent   = FUIntVector2(0u);
            bool              bReady             = false;
        };

        // A run of enabled emitters in ParticleEmitterStatics, and the asset's full emitter count.
        struct FParticleAssetStatics
        {
            int32 FirstEmitter = 0;
            int32 NumEmitters  = 0;
            int32 EmitterCount = 0;
        };

        struct FParticleMaterialStatics
        {
            FShaderH   VertexShader = {};
            FShaderH   PixelShader  = {};
            int32      MaterialIndex = -1;
            EBlendMode BlendMode     = EBlendMode::Translucent;
            bool       bWritesDepth  = false;
            bool       bResolved     = false;
        };

        // One system this extract fills, owning the items from FirstItem for its asset's enabled emitters.
        struct FParticleSystemWork
        {
            ECS::FEntity              Entity;
            SParticleSystemComponent* Component = nullptr;
            int32                     Asset     = -1;
            int32                     FirstItem = 0;
        };

        TVector<FParticleEmitterStatics>        ParticleEmitterStatics;
        TVector<FParticleAssetStatics>          ParticleAssetStatics;
        THashMap<const CParticleSystem*, int32> ParticleAssetLookup;
        TVector<FParticleMaterialStatics>       ParticleMaterialStatics;
        THashMap<CMaterialInterface*, int32>    ParticleMaterialLookup;
        // Per particle item, its slot in ParticleMaterialStatics or -1.
        TVector<int32>                          ParticleItemMaterials;
        TVector<FParticleSystemWork>            ParticleSystemWork;

        int32 PrepareParticleSystems(const auto& ParticleView);
        int32 FindOrBuildParticleAssetStatics(CParticleSystem* PS);
        int32 FindOrResolveParticleMaterial(CMaterialInterface* Candidate);
        void  FillParticleSystemExtracts(FFrameData& Frame, const FParticleSystemWork& Work, const FMatrix4& WorldMatrix);
        void  FillParticleExtract(FFrameData::FParticleExtract& Item, const FParticleEmitterStatics& Statics, const SParticleSystemComponent& Component, int32 MaterialSlot) const;
        void  WriteBackParticleComponents();

        // The dense entities the live lists were last built from, so an unchanged pool rebuilds nothing.
        TVector<ECS::FEntity>                   TerrainDenseSnapshot;
        TVector<ECS::FEntity>                   ParticleDenseSnapshot;
        void RefreshLiveEntities(TSpan<const ECS::FEntity> DenseSlots, TVector<ECS::FEntity>& Live, TVector<ECS::FEntity>& DenseSnapshot);

        // Stamped on cache entries each extract, so the sweep finds the ones nobody visited.
        uint32                                  ExtractSerial = 0;
        static bool PoolMayHaveChanged(const ECS::FRegistry& Registry, ECS::FComponentTypeID TypeID, FPoolWatch& Watch);

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

        Jobs::FLatch                            ExtractLatch{0};
        Jobs::FTaskEvent                        LightsReady;

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
        // One past the frames in flight plus one, so the newest landed copy is never the one this frame rewrites.
        static constexpr uint32                 PickerReadbackRingSize = RHI::kFramesInFlight + 2;
        static constexpr uint32                 PickerRegionExtent = 64;
        struct FPickerReadbackSlot
        {
            RHI::FGPUAllocation Readback = {};       // CPURead buffer, Width*Height*4 bytes
            RHI::FGPUAllocation DepthReadback = {};  // the same window of scene depth, one float per texel
            RHI::FGPUAllocation FoliageReadback = {}; // one byte per texel, set where the front surface is foliage
            FMatrix4            InverseViewProjection = FMatrix4(1.0f);
            // Unjittered, so two copies taken from a camera that never moved compare equal.
            FMatrix4            InverseView = FMatrix4(1.0f);
            FVector3            CameraLocation = FVector3(0.0f);
            uint32              ImageWidth = 0;     // the picker image the window came from
            uint32              ImageHeight = 0;
            uint32              OriginX = 0;        // top-left of the copied region, in picker texels
            uint32              OriginY = 0;
            uint32              Width = 0;          // region dimensions
            uint32              Height = 0;
            uint64              SubmittedFrame = 0;
            bool                bPending = false;
            bool                bHasDepth = false;
        };
        mutable TArray<FPickerReadbackSlot,     PickerReadbackRingSize> PickerReadbackRing;
        uint64                                  PickerReadbackFrame = 0;
        uint32                                  PickerReadbackWriteIndex = 0;

        TAtomic<uint64>                         PickerCursorPacked = 0;
        uint64                                  PickerCursorThisFrame = 0;

        // Requests bump the serial on the game thread, and the render thread copies once per serial it has not served.
        FPickerReadbackSlot                     SurfaceSnapshot;
        TAtomic<uint32>                         SurfaceSnapshotRequest = 0;
        TAtomic<bool>                           bSurfaceSnapshotWanted = false;
        uint32                                  SurfaceSnapshotServed = 0;
        // Latched by the picker resolve, so the resolve that runs unclipped and the copy that follows agree on one request.
        bool                                    bSurfaceSnapshotThisFrame = false;

        void IssuePickerReadback(RHI::FCmdListH CL);

        // Copies Width by Height texels at Origin of the picker, foliage and depth images into Slot, stamped with this frame's camera.
        void CopyPickerRegion(RHI::FCmdListH CL, FPickerReadbackSlot& Slot, uint32 OriginX, uint32 OriginY, uint32 Width, uint32 Height);

        static void ReleasePickerSlot(FPickerReadbackSlot& Slot);

        // The newest finished slot whose window covers the picker texel, or null.
        const FPickerReadbackSlot* FindPickerReadbackSlot(FUIntVector2 Texel) const;
#endif

    };
}
