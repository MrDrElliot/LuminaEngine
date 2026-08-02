#pragma once

// Rml::RenderInterface on the new RHI. Frame: BeginFrame -> Context::Render (defers draws) -> EndFrame (uploads + replay).
// Draws are deferred so texture uploads can run outside the render pass.

#include "Containers/Array.h"
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
        void BeginFrame(RHI::FCmdListH CmdList, RHI::FTextureH Target, const FUIntVector2& ViewportSize, const FUIntVector2& LogicalSize = FUIntVector2(0));
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
        uint32                      GetTargetStableFrames(RHI::FTextureH Target) const;

        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices) override;
        void                        RenderGeometry(Rml::CompiledGeometryHandle Geometry, Rml::Vector2f Translation, Rml::TextureHandle Texture) override;
        void                        ReleaseGeometry(Rml::CompiledGeometryHandle Geometry) override;

        Rml::TextureHandle          LoadTexture(Rml::Vector2i& OutDimensions, const Rml::String& Source) override;
        Rml::TextureHandle          GenerateTexture(Rml::Span<const Rml::byte> Bytes, Rml::Vector2i Dimensions) override;
        void                        ReleaseTexture(Rml::TextureHandle Texture) override;

        void                        EnableScissorRegion(bool bEnable) override;
        void                        SetScissorRegion(Rml::Rectanglei Region) override;

        void                        SetTransform(const Rml::Matrix4f* Transform) override;

    private:
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
            FVector2                    Translation = {0.0f, 0.0f};
            FMatrix4                    MVP = FMatrix4(1.0f);
            bool                        bScissorEnabled = false;
            Rml::Rectanglei             Scissor;
        };

        // GPU vertex for the batched path (pos/uv/color + per-draw index); matches RmlUiCommon.slang (stride 24).
        // The two float2s stay adjacent so neither straddles a 16-byte boundary under BDA layout rules.
        struct FUiVertex
        {
            float  Position[2];
            float  UV[2];
            uint32 Colour;        // premultiplied RGBA8
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
            uint32   Pad0;
            uint32   Pad1;
        };

        // Persistent per-target geometry: the VB/IB batch lives in grown device buffers (not the transient ring,
        // which UI churn thrashes), re-uploaded only on draw-list change. Keyed by render-target handle.
        struct FTargetBatch
        {
            RHI::GPUPtr       VertexBuffer = 0;
            RHI::GPUPtr       IndexBuffer = 0;
            uint64            VertexCapacity = 0;
            uint64            IndexCapacity = 0;
            TVector<FUiDraw>  Draws;            // cached per-draw data; re-uploaded to transient each draw
            uint32            IndexCount = 0;
            uint64            LastHash = 0;
            uint32            StableFrames = 0;  // consecutive frames the draw list was unchanged (drives dormancy)
            bool              bValid = false;
        };

        RHI::FPipelineH             GetPipelineForFormat(EFormat Format);
        RHI::FPipelineH             GetBrushPipeline(const struct FShaderEntry* VS, const struct FShaderEntry* PS);
        uint64                      ComputeDrawCallHash() const;
        void                        EnsureBatchBuffers(FTargetBatch& Batch, uint64 VertexBytes, uint64 IndexBytes);
        void                        ResetFrameState();   // clears the pending draw list + current frame target/cmdlist
        Rml::TextureHandle          RegisterTexturePending(TVector<uint8>&& Bytes, int Width, int Height);
        Rml::TextureHandle          LoadMaterialBrush(Rml::Vector2i& OutDimensions, class CMaterialInterface* Material, const FStringView& SourcePath, uint32 Width, uint32 Height);
        Rml::TextureHandle          LoadTextureAsset(Rml::Vector2i& OutDimensions, class CTexture* Texture);
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
        THashMap<uint64, FTargetBatch>                      TargetBatches;   // key = FTextureH.Handle
        Rml::CompiledGeometryHandle                         NextGeometryHandle = 1;
        Rml::TextureHandle                                  NextTextureHandle = 1;

        // Bumped each BeginFrame; salts the draw-call hash when a UI-material brush is referenced
        // so animated brushes never get gated away as "unchanged".
        uint64                      FrameCounter = 0;

        TVector<FPendingTexture>    PendingTextureUploads;
        TVector<FDrawCall>          DrawCalls;

        TVector<FUiVertex>          BatchVertices;
        TVector<uint32>             BatchIndices;
        TVector<FUiDraw>            BatchDrawData;

        RHI::FCmdListH              CurrentCmdList = {};
        RHI::FTextureH              CurrentTarget = {};
        FUIntVector2                CurrentSize = {0, 0};
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
