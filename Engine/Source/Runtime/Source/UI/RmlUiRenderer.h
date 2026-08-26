#pragma once

#include "Renderer/ShaderHandle.h"

// Rml::RenderInterface on the new RHI. Frame: BeginFrame -> Context::Render (defers draws) -> EndFrame (uploads + replay).
// Draws are deferred so texture uploads can run outside the render pass.

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Delegates/Delegate.h"
#include "Renderer/RHI.h"
#include "Renderer/RHITexture.h"

#include <atomic>

#include <RmlUi/Core/RenderInterface.h>

namespace Lumina
{
    class FRmlUiRenderer final : public Rml::RenderInterface
    {
    public:
        FRmlUiRenderer();
        ~FRmlUiRenderer() override;
        FRmlUiRenderer(const FRmlUiRenderer&) = delete;
        FRmlUiRenderer& operator = (const FRmlUiRenderer&) = delete;

        bool Initialize();
        void Shutdown();

        // LogicalSize=0 mirrors ViewportSize; nonzero decouples projection (layout pixels) from RT pixels.
        // ClearColor non-null folds the target clear into the pass load op instead of a separate transfer clear.
        void BeginFrame(RHI::FCmdListH CmdList, RHI::FTextureH Target, const FUIntVector2& ViewportSize,
                        const FUIntVector2& LogicalSize = FUIntVector2(0), const FVector4* ClearColor = nullptr);
        void EndFrame();

        // Content-change gating: PeekFrameHash hashes the draw list; IsTargetUpToDate is true when the target's
        // batch already holds it (persistent RTs skip the pass). AbortFrame discards pending draws without recording.
        uint64                      PeekFrameHash() const;
        bool                        IsTargetUpToDate(RHI::FTextureH Target, uint64 Hash) const;
        void                        AbortFrame();

        // Drop a target's cached batch buffers (called when a widget RT is destroyed).
        RUNTIME_API void            ReleaseTargetBatch(RHI::FTextureH Target);

        // Consecutive-stable-frame count (bStable increments, change resets) so the game thread can stop
        // ticking a settled widget; 0 for unknown targets.
        void                        NoteTargetStable(RHI::FTextureH Target, bool bStable);
        uint32                      GetTargetStableFrames(RHI::FTextureH Target);

        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices) override;
        void                        RenderGeometry(Rml::CompiledGeometryHandle Geometry, Rml::Vector2f Translation, Rml::TextureHandle Texture) override;
        void                        ReleaseGeometry(Rml::CompiledGeometryHandle Geometry) override;

        Rml::TextureHandle          LoadTexture(Rml::Vector2i& OutDimensions, const Rml::String& Source) override;
        Rml::TextureHandle          GenerateTexture(Rml::Span<const Rml::byte> Bytes, Rml::Vector2i Dimensions) override;
        void                        ReleaseTexture(Rml::TextureHandle Texture) override;

        void                        EnableScissorRegion(bool bEnable) override;
        void                        SetScissorRegion(Rml::Rectanglei Region) override;

        void                        EnableClipMask(bool bEnable) override;
        void                        RenderToClipMask(Rml::ClipMaskOperation Operation, Rml::CompiledGeometryHandle Geometry,
                                                     Rml::Vector2f Translation) override;

        void                        SetTransform(const Rml::Matrix4f* Transform) override;

        Rml::LayerHandle            PushLayer() override;
        void                        CompositeLayers(Rml::LayerHandle Source, Rml::LayerHandle Destination, Rml::BlendMode BlendMode,
                                                    Rml::Span<const Rml::CompiledFilterHandle> Filters) override;
        void                        PopLayer() override;

        Rml::TextureHandle          SaveLayerAsTexture() override;
        Rml::CompiledFilterHandle   SaveLayerAsMaskImage() override;

        Rml::CompiledFilterHandle   CompileFilter(const Rml::String& Name, const Rml::Dictionary& Parameters) override;
        void                        ReleaseFilter(Rml::CompiledFilterHandle Filter) override;

        Rml::CompiledShaderHandle   CompileShader(const Rml::String& Name, const Rml::Dictionary& Parameters) override;
        void                        RenderShader(Rml::CompiledShaderHandle Shader, Rml::CompiledGeometryHandle Geometry,
                                                 Rml::Vector2f Translation, Rml::TextureHandle Texture) override;
        void                        ReleaseShader(Rml::CompiledShaderHandle Shader) override;

    private:
        // Count in the low 8 bits, first PendingClipMasks index above them. Zero means unclipped.
        static constexpr uint32 kMaxClipMasksPerDraw = 8;
        static constexpr uint32 kNoLayer    = 0xFFFFFFFFu;

        // RmlUi compiles geometry once per element; we cache the CPU bytes and concatenate them at EndFrame
        // into the target's resident VB/IB (rebuilt only on draw-list change).
        struct FGeometry
        {
            TVector<uint8> VertexData;
            TVector<uint8> IndexData;
            uint32         IndexCount = 0;
        };

        struct FTexture
        {
            RHI::FManagedTexture       Managed;                    // owned (generated textures + brush RTs)
            uint32                     ResourceID = RHI::kInvalidHeapSlot; // global-heap sampled slot
            class CTexture*            AssetKeepalive = nullptr;   // rooted while held; released on ReleaseTexture
            class CMaterialInterface*  BrushMaterial  = nullptr;   // UI-material brush; rooted while held, rendered each frame
            FUIntVector2               BrushSize = {0, 0};
            FString                    BrushSourcePath;            // resolved asset path; re-validated so a rename/delete breaks the brush
            bool                       bBrushStale = false;        // source path no longer resolves -> cleared + not rendered (material stays rooted so a rename-back can resume)
            bool                       bBrushCleared = false;      // RT has defined contents; until then a not-yet-ready material would leave the UI sampling garbage
        };

        struct FPendingTexture
        {
            Rml::TextureHandle Handle = 0;
            int                Width = 0;
            int                Height = 0;
            TVector<uint8>     Bytes;
        };

        struct FDrawCall
        {
            Rml::CompiledGeometryHandle Geometry = 0;
            Rml::TextureHandle          Texture = 0;
            Rml::CompiledShaderHandle   Shader = 0;   // gradient decorator; 0 = plain textured draw
            uint32                      ClipMaskRange = 0;
            FVector2                    Translation = {0.0f, 0.0f};
            FMatrix4                    MVP = FMatrix4(1.0f);
            bool                        bScissorEnabled = false;
            Rml::Rectanglei             Scissor;
        };

        // Flattened FDrawCall for the dormancy hash, padding-free so the hash never reads indeterminate bytes.
        struct FDrawKey
        {
            uint64 Geometry;
            uint64 Texture;
            uint64 Shader;
            float  Translation[2];
            float  MVP[16];
            int32  Scissor[4];
            uint32 bScissorEnabled;
            uint32 ClipMaskRange;
        };
        static_assert(sizeof(FDrawKey) == 120, "FDrawKey must stay padding-free");

        // GPU vertex for the batched path (pos/uv/color + per-draw index); matches RmlUiCommon.slang (stride 24).
        // The two float2s stay adjacent so neither straddles a 16-byte boundary under BDA layout rules.
        struct FUiVertex
        {
            float  Position[2];
            float  UV[2];
            uint32 Color;        // premultiplied RGBA8
            uint32 DrawIndex;
        };

        // Per-draw data read in-shader via device address (std430). Matches
        // RmlUiCommon.slang::FUiDraw.
        struct FUiDraw
        {
            FMatrix4 MVP;
            FVector4 ClipRect;     // minX, minY, maxX, maxY (framebuffer pixels)
            uint32   TextureID;
            uint32   SamplerIndex;
            uint32   ShaderType;
            uint32   StopOffset;
            FVector4 ShaderParams;
            uint32   StopCount;
            uint32   bRepeating;
            float    ShaderScale;
            uint32   ClipMaskRange;
        };


        // Matches RmlUiCommon.slang::FUiClipMask.
        struct FUiClipMask
        {
            FVector4 Rect;    // center.xy, half-extent.zw in document space
            FVector4 Radii;   // top-left, top-right, bottom-right, bottom-left
            FVector4 Params;  // .x nonzero keeps the area outside the shape instead of inside
        };

        // Matches RmlUiCommon.slang::FUiColorStop.
        struct FUiColorStop
        {
            FVector4 Color;
            FVector4 Position;
        };

        // A compiled RmlUi gradient decorator. RmlUi recompiles one whenever the element box changes,
        // so the handle doubles as the change token the frame hash keys off.
        struct FShader
        {
            uint32                 Type = 0;
            FVector4               Params = FVector4(0.0f);
            float                  Scale = 1.0f;
            bool                   bRepeating = false;
            TVector<FUiColorStop>  Stops;
        };

        // Persistent per-target geometry: the VB/IB batch lives in grown device buffers (not the transient ring,
        // which UI churn thrashes), re-uploaded only on draw-list change. Keyed by render-target handle.
        struct FTargetBatch
        {
            RHI::FGPUAllocation VertexBuffer = {};
            RHI::FGPUAllocation IndexBuffer = {};
            uint64            VertexCapacity = 0;
            uint64            IndexCapacity = 0;
            TVector<FUiDraw>      Draws;        // cached per-draw data; re-uploaded to transient each draw
            TVector<FUiColorStop> Stops;        // cached gradient stops, indexed by FUiDraw::StopOffset
            TVector<FUiClipMask>  ClipMasks;    // cached rounded-rect clips, ranged by FUiDraw::ClipMaskRange
            TVector<uint32>       DrawFirstIndex;  // per draw call, so layer ops can split the batch
            TVector<uint32>       DrawIndexCount;

            // RmlUi handles rather than heap slots. A slot baked into a cached batch goes stale the
            // moment its texture is released, and the heap answers a dead slot with magenta.
            TVector<Rml::TextureHandle> DrawTextures;
            uint32            IndexCount = 0;
            uint64            LastHash = 0;
            uint32            StableFrames = 0;  // consecutive frames the draw list was unchanged (drives dormancy)
            uint64            LastUsedFrame = 0; // eviction stamp; targets die without notice (resize) and their handles get recycled
            bool              bValid = false;
        };

        // A pooled full-size render target. Layer 0 is the frame's own target and is never pooled.
        struct FLayer
        {
            RHI::FManagedTexture Color;
            uint32               SampledSlot = RHI::kInvalidHeapSlot;
            FUIntVector2         Size = {0, 0};
            uint64               LastUsedFrame = 0;
        };

        enum class EFilterKind : uint32
        {
            None = 0,
            ColorMatrix,   // brightness, contrast, invert, grayscale, sepia, hue-rotate, saturate
            Opacity,       // scales the whole premultiplied color
            Blur,
            DropShadow,
            MaskImage,
        };

        struct FFilter
        {
            EFilterKind Kind = EFilterKind::None;
            FMatrix4    ColorMatrix = FMatrix4(1.0f);
            float       Opacity = 1.0f;
            float       Sigma = 0.0f;
            FVector2    Offset = {0.0f, 0.0f};
            FVector4    Color = FVector4(0.0f);
            uint32      MaskLayer = kNoLayer;   // MaskImage only, an index into OwnedLayers
        };

        enum class ELayerOp : uint8
        {
            Push,
            Composite,
            Pop,
            SaveTexture,
            SaveMaskImage,
        };

        // Replayed at EndFrame, so the op records how many draws preceded it and the batch splits there.
        struct FLayerCommand
        {
            ELayerOp           Op = ELayerOp::Push;
            uint32             DrawsBefore = 0;
            uint32             Source = 0;
            uint32             Destination = 0;
            bool               bReplace = false;      // Composite with BlendMode::Replace
            uint32             FilterOffset = 0;
            uint32             FilterCount = 0;
            uint32             TargetLayer = kNoLayer;
            bool               bScissorEnabled = false;
            Rml::Rectanglei    Scissor;
            uint32             ClipMaskRange = 0;
            RHI::FTextureH     DestTexture = {};
            FUIntVector2       DestSize = {0, 0};
            FVector4           SourceRect = FVector4(1.0f, 1.0f, 0.0f, 0.0f);
            Rml::TextureHandle SavedTexture = 0;
        };

        RHI::FPipelineH             GetPipelineForFormat(EFormat Format);
        RHI::FPipelineH             GetFilterPipeline(EFormat Format, bool bBlend);
        RHI::FPipelineH             GetBrushPipeline(FShaderH VS, FShaderH PS);

        uint32                      AcquireLayer();
        void                        ReleaseLayerToPool(uint32 LayerIndex);
        void                        ReleaseAllLayers();
        void                        EvictStaleLayers();
        void                        OpenPass(RHI::FCmdListH CL, RHI::FTextureH Target, bool bClear);
        void                        OpenPassSized(RHI::FCmdListH CL, RHI::FTextureH Target, bool bClear, const FUIntVector2& Extent);
        void                        ClearLayer(RHI::FCmdListH CL, uint32 LayerIndex);
        void                        RunFilterPass(RHI::FCmdListH CL, uint32 SourceLayer, uint32 DestLayer, struct FUiFilterArgs Args, bool bBlend);
        void                        CopyLayer(RHI::FCmdListH CL, uint32 SourceLayer, uint32 DestLayer, bool bBlend);
        void                        BlitTargetToLayer(RHI::FCmdListH CL, uint32 DestLayer);
        void                        CopyLayerToTexture(RHI::FCmdListH CL, uint32 SourceLayer, RHI::FTextureH Dest,
                                                       const FUIntVector2& DestSize, const FVector4& SourceRect);
        void                        ApplyBlur(RHI::FCmdListH CL, uint32 InOut, uint32 Scratch, float Sigma);
        void                        ApplyFilters(RHI::FCmdListH CL, uint32& Primary, uint32 Scratch, uint32 FilterOffset, uint32 FilterCount);
        void                        ReplayFrame(RHI::FCmdListH CL, const FTargetBatch& Batch, RHI::FPipelineH TargetPipeline, RHI::GPUPtr ArgsPtr);

        uint32                      ResolveLayerHandle(Rml::LayerHandle Handle) const;
        uint32                      CurrentLayerIndex() const;
        RHI::FTextureH              LayerTexture(uint32 LayerIndex) const;
        uint32                      LayerSlot(uint32 LayerIndex) const;
        uint64                      ComputeDrawCallHash() const;
        void                        ResolveBatchTextures(FTargetBatch& Batch) const;
        static bool                 InferRoundedRect(const FGeometry& Geom, FVector2 Translation, FUiClipMask& Out);
        void                        EnsureBatchBuffers(FTargetBatch& Batch, uint64 VertexBytes, uint64 IndexBytes);
        void                        EvictStaleBatches();
        void                        ResetFrameState();   // clears the pending draw list + current frame target/cmdlist
        Rml::TextureHandle          RegisterTexturePending(TVector<uint8>&& Bytes, int Width, int Height);
        Rml::TextureHandle          LoadMaterialBrush(Rml::Vector2i& OutDimensions, class CMaterialInterface* Material, const FStringView& SourcePath, uint32 Width, uint32 Height);
        Rml::TextureHandle          LoadTextureAsset(Rml::Vector2i& OutDimensions, class CTexture* Texture);
        void                        ReleaseTextureNow(Rml::TextureHandle Texture);
        void                        UploadPendingTextures();
        void                        RenderMaterialBrushes();
        void                        RevalidateBrushes(RHI::FCmdListH CmdList);

        // Pipelines keyed by target format (widget/brush RTs and the world display image can differ).
        THashMap<uint64, RHI::FPipelineH>   PipelineByFormat;
        // Brush pipelines keyed by material shader-object pointers (recompile -> new pointers -> new entry).
        THashMap<uint64, RHI::FPipelineH>   BrushPipelines;
        RHI::FDepthStencilH                 DepthState;

        RHI::FManagedTexture                DefaultWhite;

        std::atomic<bool>           bBrushRevalidatePending{false};
        FDelegateHandle             AssetRegistryUpdateHandle;

        THashMap<Rml::CompiledGeometryHandle, FGeometry>    Geometries;
        THashMap<Rml::TextureHandle, FTexture>              Textures;
        THashMap<Rml::CompiledShaderHandle, FShader>        Shaders;
        THashMap<uint64, FTargetBatch>                      TargetBatches;   // key = FTextureH.Handle
        Rml::CompiledGeometryHandle                         NextGeometryHandle = 1;
        Rml::TextureHandle                                  NextTextureHandle = 1;
        Rml::CompiledShaderHandle                           NextShaderHandle = 1;

        // Bumped each BeginFrame; salts the draw-call hash when a UI-material brush is referenced
        // so animated brushes never get gated away as "unchanged".
        uint64                      FrameCounter = 0;
        uint64                      LastEvictFrame = 0;

        // FrameCounter counts BeginFrame calls, one per UI target, so several land in one engine frame.
        // Resource lifetimes have to be measured against engine frames, which is what the GPU retires.
        uint64                      EngineFrameCounter = 0;
        uint32                      LastRenderFrameSlot = 0xFFFFFFFFu;

        TVector<FPendingTexture>    PendingTextureUploads;
        TVector<FDrawCall>          DrawCalls;

        // Resources RmlUi frees mid-frame, held until the deferred draws referencing them are built.
        TVector<Rml::CompiledGeometryHandle> DeferredGeometryReleases;
        TVector<Rml::CompiledFilterHandle>   DeferredFilterReleases;
        TVector<Rml::CompiledShaderHandle>   DeferredShaderReleases;
        // Releasing a texture unbinds its heap slot immediately, so a handle has to outlive every
        // frame still in flight that might sample it, not just the frame that dropped it.
        struct FRetiredTexture
        {
            Rml::TextureHandle Handle = 0;
            uint64             Frame = 0;
        };
        TVector<FRetiredTexture>             DeferredTextureReleases;

        // Reused across frames so the dormancy hash does not allocate on an idle widget.
        mutable TVector<FDrawKey>   HashKeyScratch;

        // Lets the hash skip a per-draw map lookup entirely when no material brush is live.
        uint32                      LiveBrushCount = 0;

        TVector<FUiVertex>          BatchVertices;
        TVector<uint32>             BatchIndices;
        TVector<FUiDraw>            BatchDrawData;
        TVector<FUiColorStop>       BatchStops;
        TVector<Rml::TextureHandle> BatchDrawTextures;

        // Rounded-rect clip masks recorded during Context::Render; FDrawCall::ClipMaskRange spans this.
        TVector<FUiClipMask>        PendingClipMasks;
        uint32                      ActiveClipOffset = 0;
        uint32                      ActiveClipCount = 0;

        // Layers owned this frame; kNoLayer stands for the frame's own target, RmlUi's layer zero.
        TVector<FLayer>             OwnedLayers;
        TVector<uint32>             LayerStack;
        TVector<uint32>             FreeLayers;      // pooled across frames, matched to a target by extent


        TVector<FLayerCommand>      LayerCommands;
        TVector<uint32>             PendingFilterRefs;   // flattened filter lists, indexed by FLayerCommand
        THashMap<Rml::CompiledFilterHandle, FFilter>  Filters;
        Rml::CompiledFilterHandle   NextFilterHandle = 1;

        // Index range each recorded draw occupies in the batch, so a layer op can split the draw.
        TVector<uint32>             DrawIndexOffsets;
        TVector<uint32>             DrawIndexCounts;

        THashMap<uint64, RHI::FPipelineH> FilterPipelines;

        // Composites honor the clip region RmlUi set when the op was recorded, not the frame extent.
        RHI::FRect                  PassScissor = {};
        bool                        bPassScissorSet = false;

        uint32                      PassClipMaskRange = 0;
        FVector2                    PassClipScale = {1.0f, 1.0f};
        RHI::GPUPtr                 PassClipMasksPtr = 0;

        RHI::FCmdListH              CurrentCmdList = {};
        RHI::FTextureH              CurrentTarget = {};
        FUIntVector2                CurrentSize = {0, 0};        // render-target pixels
        FUIntVector2                CurrentLogicalSize = {0, 0}; // layout pixels RmlUi reports scissor rects in
        FVector4                    CurrentClearColor = FVector4(0.0f);
        bool                        bClearTarget = false;
        bool                        bInitialized = false;

        // PeekFrameHash (caller-side dormancy check) and EndFrame both need the draw-list hash;
        // cache it across the pair so we hash once per frame. Invalidated each BeginFrame.
        mutable uint64              CachedFrameHash = 0;
        mutable bool                bCachedFrameHashValid = false;

        FMatrix4                    ProjectionMatrix = FMatrix4(1.0f);
        FMatrix4                    UserTransform = FMatrix4(1.0f);
        FMatrix4                    CachedMVP = FMatrix4(1.0f);
        bool                        bScissorEnabled = false;
        Rml::Rectanglei             CurrentScissor;
    };
}
