#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include "RmlUiRenderer.h"

#include "Core/Engine/Engine.h"
#include "Log/Log.h"
#include "Renderer/Format.h"
#include "Renderer/RHICore.h"
#include "Renderer/ShaderLibrary.h"
#include "Renderer/RenderResource.h"

#include "Core/Math/Math.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Vertex.h>

#include "Assets/AssetTypes/Textures/Texture.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "FileSystem/FileSystem.h"
#include "Renderer/RenderManager.h"
#include "Renderer/MaterialManager.h"


namespace Lumina
{
    // Mirrors RmlUiCommon.slang::FRmlUiArgs.
    struct FRmlUiArgs
    {
        RHI::GPUPtr Draws;      // per-draw FUiDraw array (transient)
        RHI::GPUPtr Vertices;   // resident batch vertex buffer (vertex pulling)
        RHI::GPUPtr Stops;      // gradient color stops (transient)
        RHI::GPUPtr ClipMasks;  // rounded-rect clip masks (transient)
    };

    // RmlUi's own decorators cap out at 16; anything past that is dropped rather than overrunning.
    static constexpr uint32 GMaxColorStops = 16;

    // ScreenSize must stay at offset 16, since relaxed block layout forbids a straddling vector.
    struct FUIMaterialBrushArgs
    {
        RHI::GPUPtr Materials;
        uint32      MaterialIndex;
        float       Time;
        uint32      ScreenSize[4];   // .xy = brush resolution
    };

    static_assert(sizeof(FUIMaterialBrushArgs) == 32, "FUIMaterialArgs layout must match UIMaterialGlobals.slang");
    static_assert(offsetof(FUIMaterialBrushArgs, ScreenSize) == 16, "ScreenSize must not straddle a 16-byte boundary");

    // Mirrors UIFilter.slang::FUiFilterArgs.
    struct FUiFilterArgs
    {
        FMatrix4 ColorMatrix;
        FVector4 Color;
        FVector4 TexelStep;
        FVector4 SourceRect = FVector4(1.0f, 1.0f, 0.0f, 0.0f);
        uint32   Kind;
        uint32   SourceID;
        uint32   MaskID;
        uint32   SamplerIndex;
        float    Opacity;
        uint32      TapCount;
        float       Sigma;
        uint32      ClipMaskRange;
        RHI::GPUPtr ClipMasks;
        FVector2    ClipScale;
        FVector2    Pad2;
        FVector2    Pad3;
    };

    static_assert(sizeof(FUiFilterArgs) == 176, "FUiFilterArgs must match UIFilter.slang::FUiFilterArgs.");

    enum class EUiFilterPass : uint32
    {
        Copy = 0,
        ColorMatrix = 1,
        Opacity = 2,
        Blur = 3,
        DropShadow = 4,
        MaskImage = 5,
    };

    // Steady state is roughly the per-frame peak times GFramesInFlight, since a layer waits for its readers.
    static constexpr uint32 GMaxLiveLayers = 48;

    // BeginFrame fences the slot it reuses, so a resource this many engine frames old has already retired.
    static constexpr uint64 GFramesInFlight = RHI::kFramesInFlight;

    // A dropped draw is silent and the hardest failure to chase here, so every distinct skip reports once.
    static void WarnDroppedDrawOnce(const char* Reason)
    {
        static const char* Reported[8] = {};
        for (const char*& Slot : Reported)
        {
            if (Slot == Reason)
            {
                return;
            }
            if (Slot == nullptr)
            {
                Slot = Reason;
                LOG_WARN("[RmlUi] Dropping a draw from the batch, {}. It renders nothing.", Reason);
                return;
            }
        }
    }

    // Count in the low 8 bits so one draw carries a whole intersection without growing FUiDraw.
    static constexpr uint32 PackClipRange(uint32 Offset, uint32 Count)
    {
        return (Count == 0) ? 0u : ((Offset << 8) | Count);
    }

    // Beyond this a wider Gaussian is downsampled rather than tapped, matching RmlUi's own cutoff.
    static constexpr uint32 GMaxBlurTaps = 12;

    // RmlUi wants bilinear + clamp; stock sampler heap slot 1.
    static constexpr uint32 GRmlUiSamplerIndex = (uint32)RHI::EStockSampler::LinearClamp;

    // Vertex layout is pos(8) color(4) uv(8).
    static_assert(sizeof(Rml::Vertex) == 20, "Rml::Vertex layout drifted; renderer vertex conversion must be updated.");

    FRmlUiRenderer::FRmlUiRenderer() = default;
    FRmlUiRenderer::~FRmlUiRenderer() { Shutdown(); }

    bool FRmlUiRenderer::Initialize()
    {
        if (bInitialized)
        {
            return true;
        }

        static_assert(sizeof(FUiVertex) == 24, "FUiVertex must match RmlUiCommon.slang::FUiVertex (stride 24).");
        static_assert(sizeof(FUiDraw) == 128,  "FUiDraw must match RmlUiCommon.slang::FUiDraw (std430).");
        static_assert(offsetof(FUiDraw, ShaderParams) == 96, "ShaderParams must not straddle a 16-byte boundary.");
        static_assert(sizeof(FUiColorStop) == 32, "FUiColorStop must match RmlUiCommon.slang::FUiColorStop.");
        static_assert(sizeof(FUiClipMask) == 48, "FUiClipMask must match RmlUiCommon.slang::FUiClipMask.");

        DepthState = RHI::CreateDepthStencil(RHI::FDepthStencilDesc{});

        // 1x1 white fallback for untextured geometry.
        DefaultWhite = RHI::Textures::Create(RHI::FTexture2DDesc{ .Width = 1, .Height = 1, .Format = EFormat::RGBA8_UNORM,
                                                                  .DebugName = "RmlUi.DefaultWhite" });
        constexpr uint8 WhitePixel[4] = {255, 255, 255, 255};
        RHI::Textures::Upload(DefaultWhite, 0, WhitePixel, sizeof(WhitePixel), 1);

        AssetRegistryUpdateHandle = FAssetRegistry::Get().GetOnAssetRegistryUpdated().AddLambda([this]()
        {
            bBrushRevalidatePending.store(true, std::memory_order_release);
        });

        bInitialized = true;
        return true;
    }

    void FRmlUiRenderer::Shutdown()
    {
        if (!bInitialized)
        {
            return;
        }

        FAssetRegistry::Get().GetOnAssetRegistryUpdated().Remove(AssetRegistryUpdateHandle);

        RHI::WaitDeviceIdle();

        DrawCalls.clear();
        PendingTextureUploads.clear();
        Geometries.clear();
        Shaders.clear();
        for (auto& KV : Textures)
        {
            if (KV.second.AssetKeepalive != nullptr)
            {
                KV.second.AssetKeepalive->RemoveFromRoot();
                KV.second.AssetKeepalive = nullptr;
            }
            if (KV.second.BrushMaterial != nullptr)
            {
                KV.second.BrushMaterial->RemoveFromRoot();
                KV.second.BrushMaterial = nullptr;
            }
            if (KV.second.Managed.IsValid())
            {
                RHI::Textures::Release(KV.second.Managed);
            }
        }
        Textures.clear();

        for (auto& KV : TargetBatches)
        {
            RHI::Free(KV.second.VertexBuffer);
            RHI::Free(KV.second.IndexBuffer);
        }
        TargetBatches.clear();

        Filters.clear();
        ReleaseAllLayers();

        RHI::Textures::Release(DefaultWhite);

        for (auto& KV : PipelineByFormat)
        {
            RHI::FreeH(KV.second);
        }
        PipelineByFormat.clear();
        for (auto& KV : BrushPipelines)
        {
            RHI::FreeH(KV.second);
        }
        BrushPipelines.clear();
        for (auto& KV : FilterPipelines)
        {
            RHI::FreeH(KV.second);
        }
        FilterPipelines.clear();
        RHI::FreeH(DepthState);
        DepthState = {};

        bInitialized = false;
    }

    RHI::FPipelineH FRmlUiRenderer::GetPipelineForFormat(EFormat Format)
    {
        auto It = PipelineByFormat.find((uint64)Format);
        if (It != PipelineByFormat.end())
        {
            return It->second;
        }

        // Premultiplied alpha, since RmlUi pre-multiplies vertex colors.
        RHI::FBlendDesc Blend;
        Blend.bBlendEnable   = true;
        Blend.ColorOp        = RHI::EBlend::Add;
        Blend.SrcColorFactor = RHI::EFactor::One;
        Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
        Blend.AlphaOp        = RHI::EBlend::Add;
        Blend.SrcAlphaFactor = RHI::EFactor::One;
        Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        const RHI::FColorTarget ColorTarget { .Format = Format, .Blend = Blend };
        RHI::FRasterDesc RasterDesc;
        RasterDesc.Topology     = RHI::ETopology::TriangleList;
        RasterDesc.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

        RHI::FPipelineH Pipeline = RHI::Core::CreateGraphicsPipeline("RmlUiVert.slang", "RmlUiPixel.slang", RasterDesc);
        if (RHI::IsValid(Pipeline))
        {
            PipelineByFormat.emplace((uint64)Format, Pipeline);
        }
        return Pipeline;
    }

    RHI::FPipelineH FRmlUiRenderer::GetFilterPipeline(EFormat Format, bool bBlend)
    {
        const uint64 Key = (uint64)Format | (bBlend ? (1ull << 32) : 0ull);
        auto It = FilterPipelines.find(Key);
        if (It != FilterPipelines.end())
        {
            return It->second;
        }

        // A filter pass fully writes its destination; only the composite step blends.
        RHI::FBlendDesc Blend;
        if (bBlend)
        {
            Blend.bBlendEnable   = true;
            Blend.ColorOp        = RHI::EBlend::Add;
            Blend.SrcColorFactor = RHI::EFactor::One;
            Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
            Blend.AlphaOp        = RHI::EBlend::Add;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;
        }
        const RHI::FColorTarget ColorTarget { .Format = Format, .Blend = Blend };
        RHI::FRasterDesc RasterDesc;
        RasterDesc.Topology     = RHI::ETopology::TriangleList;
        RasterDesc.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

        RHI::FPipelineH Pipeline = RHI::Core::CreateGraphicsPipeline("FullscreenQuad.slang", "UIFilter.slang", RasterDesc);
        if (RHI::IsValid(Pipeline))
        {
            FilterPipelines.emplace(Key, Pipeline);
        }
        return Pipeline;
    }

    uint32 FRmlUiRenderer::AcquireLayer()
    {
        for (size_t i = 0; i < FreeLayers.size(); ++i)
        {
            const uint32 Index = FreeLayers[i];
            FLayer& Candidate = OwnedLayers[Index];
            if (Candidate.Size.x != CurrentSize.x || Candidate.Size.y != CurrentSize.y)
            {
                continue;
            }

            // Reuse within a frame is command-list ordered and safe; across frames the GPU may still be reading it.
            const bool bSameFrame = (Candidate.LastUsedFrame == EngineFrameCounter);
            if (!bSameFrame && Candidate.LastUsedFrame + GFramesInFlight > EngineFrameCounter)
            {
                continue;
            }

            FreeLayers[i] = FreeLayers.back();
            FreeLayers.pop_back();
            Candidate.LastUsedFrame = EngineFrameCounter;
            return Index;
        }

        // Stale-size layers are dropped, since a resize drag otherwise builds hundreds, but only once their readers retire.
        for (size_t i = 0; i < FreeLayers.size();)
        {
            FLayer& Stale = OwnedLayers[FreeLayers[i]];
            const bool bMatchesExtent = (Stale.Size.x == CurrentSize.x && Stale.Size.y == CurrentSize.y);
            const bool bInFlight      = (Stale.LastUsedFrame + GFramesInFlight > EngineFrameCounter);
            if (bMatchesExtent || bInFlight)
            {
                ++i;
                continue;
            }

            if (Stale.Color.IsValid())
            {
                RHI::Textures::Release(Stale.Color);
            }
            Stale.SampledSlot = RHI::kInvalidHeapSlot;
            Stale.Size = FUIntVector2(0, 0);
            FreeLayers[i] = FreeLayers.back();
            FreeLayers.pop_back();
        }

        uint32 LiveLayers = 0;
        for (const FLayer& Existing : OwnedLayers)
        {
            LiveLayers += Existing.Color.IsValid() ? 1u : 0u;
        }
        if (LiveLayers >= GMaxLiveLayers)
        {
            LOG_WARN("[RmlUi] Layer budget of {} reached; effects on this frame are skipped.", GMaxLiveLayers);
            return kNoLayer;
        }

        // A retired entry is reused so OwnedLayers does not grow without bound across resizes.
        uint32 Index = kNoLayer;
        for (uint32 i = 0; i < uint32(OwnedLayers.size()); ++i)
        {
            if (!OwnedLayers[i].Color.IsValid())
            {
                Index = i;
                break;
            }
        }

        FLayer Layer;
        Layer.Color = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = CurrentSize.x,
            .Height = CurrentSize.y,
            .Format = EFormat::RGBA8_UNORM,
            .bRenderTarget = true,
            .DebugName = "RmlUi.Layer",
        });
        if (!Layer.Color.IsValid() || Layer.Color.SampledSlot == RHI::kInvalidHeapSlot)
        {
            LOG_WARN("[RmlUi] Failed to create a layer render target; effects on this frame are skipped.");
            return kNoLayer;
        }
        Layer.SampledSlot   = Layer.Color.SampledSlot;
        Layer.Size          = CurrentSize;
        Layer.LastUsedFrame = EngineFrameCounter;

        if (Index != kNoLayer)
        {
            OwnedLayers[Index] = Layer;
            return Index;
        }

        OwnedLayers.push_back(Layer);
        return uint32(OwnedLayers.size() - 1);
    }

    void FRmlUiRenderer::ReleaseLayerToPool(uint32 LayerIndex)
    {
        if (LayerIndex != kNoLayer && LayerIndex < OwnedLayers.size())
        {
            FreeLayers.push_back(LayerIndex);
        }
    }

    void FRmlUiRenderer::ReleaseAllLayers()
    {
        for (FLayer& Layer : OwnedLayers)
        {
            if (Layer.Color.IsValid())
            {
                RHI::Textures::Release(Layer.Color);
            }
        }
        OwnedLayers.clear();
        FreeLayers.clear();
        LayerStack.clear();
    }

    uint32 FRmlUiRenderer::ResolveLayerHandle(Rml::LayerHandle Handle) const
    {
        const size_t Depth = size_t(Handle);
        if (Depth == 0 || Depth > LayerStack.size())
        {
            return kNoLayer;
        }
        return LayerStack[Depth - 1];
    }

    uint32 FRmlUiRenderer::CurrentLayerIndex() const
    {
        return LayerStack.empty() ? kNoLayer : LayerStack.back();
    }

    RHI::FTextureH FRmlUiRenderer::LayerTexture(uint32 LayerIndex) const
    {
        if (LayerIndex == kNoLayer || LayerIndex >= OwnedLayers.size())
        {
            return CurrentTarget;
        }
        return OwnedLayers[LayerIndex].Color.Texture;
    }

    uint32 FRmlUiRenderer::LayerSlot(uint32 LayerIndex) const
    {
        if (LayerIndex == kNoLayer || LayerIndex >= OwnedLayers.size())
        {
            return RHI::kInvalidHeapSlot;
        }
        return OwnedLayers[LayerIndex].SampledSlot;
    }

    RHI::FPipelineH FRmlUiRenderer::GetBrushPipeline(FShaderH VS, FShaderH PS)
    {
        // The handle is the identity, so a recompiled brush shader keys a new pipeline for free.
        uint64 Key = 0;
        Hash::HashCombine(Key, VS.Handle);
        Hash::HashCombine(Key, PS.Handle);

        auto It = BrushPipelines.find(Key);
        if (It != BrushPipelines.end())
        {
            return It->second;
        }

        const RHI::FColorTarget ColorTarget { .Format = EFormat::RGBA8_UNORM, .Blend = {} };
        RHI::FRasterDesc RasterDesc;
        RasterDesc.Topology     = RHI::ETopology::TriangleList;
        RasterDesc.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);

        const FShaderEntry* VSEntry = FShaderLibrary::Resolve(VS);
        const FShaderEntry* PSEntry = FShaderLibrary::Resolve(PS);
        if (VSEntry == nullptr || PSEntry == nullptr)
        {
            return {};
        }

        RHI::FPipelineH Pipeline = RHI::CreateGraphicsPipeline(VSEntry->Source(), PSEntry->Source(), RasterDesc);
        if (RHI::IsValid(Pipeline))
        {
            BrushPipelines.emplace(Key, Pipeline);
        }
        return Pipeline;
    }

    void FRmlUiRenderer::EvictStaleLayers()
    {
        constexpr uint64 IdleFrames = 240;
        if (EngineFrameCounter < IdleFrames)
        {
            return;
        }

        for (size_t i = 0; i < FreeLayers.size();)
        {
            FLayer& Layer = OwnedLayers[FreeLayers[i]];
            if (Layer.Color.IsValid() && Layer.LastUsedFrame + IdleFrames < EngineFrameCounter)
            {
                RHI::Textures::Release(Layer.Color);
                Layer.SampledSlot = RHI::kInvalidHeapSlot;
                Layer.Size = FUIntVector2(0, 0);
                FreeLayers[i] = FreeLayers.back();
                FreeLayers.pop_back();
                continue;
            }
            ++i;
        }
    }

    void FRmlUiRenderer::BeginFrame(RHI::FCmdListH CmdList, RHI::FTextureH Target, const FUIntVector2& ViewportSize,
                                    const FUIntVector2& LogicalSize, const FVector4* ClearColor)
    {
        CurrentCmdList   = CmdList;
        CurrentTarget    = Target;
        CurrentSize      = ViewportSize;
        bClearTarget     = (ClearColor != nullptr);
        CurrentClearColor = bClearTarget ? *ClearColor : FVector4(0.0f);
        bCachedFrameHashValid = false;
        ++FrameCounter;

        // The render manager's slot cycles once per engine frame, which is the tick lifetimes need.
        if (const FRenderManager* RenderManager = TryRender())
        {
            const uint32 Slot = RenderManager->GetCurrentFrameIndex();
            if (Slot != LastRenderFrameSlot)
            {
                LastRenderFrameSlot = Slot;
                ++EngineFrameCounter;
            }
        }

        EvictStaleLayers();

        DrawCalls.clear();
        EvictStaleBatches();

        const FUIntVector2 ProjSize = (LogicalSize.x > 0 && LogicalSize.y > 0) ? LogicalSize : ViewportSize;
        CurrentLogicalSize = ProjSize;

        // pixel -> NDC ortho; no Y-flip since Vulkan viewport is +Y-down.
        const float W = ProjSize.x > 0 ? float(ProjSize.x) : 1.0f;
        const float H = ProjSize.y > 0 ? float(ProjSize.y) : 1.0f;
        ProjectionMatrix = FMatrix4(
            2.0f / W,  0.0f,       0.0f,  0.0f,
            0.0f,      2.0f / H,   0.0f,  0.0f,
            0.0f,      0.0f,       1.0f,  0.0f,
           -1.0f,     -1.0f,       0.0f,  1.0f);

        UserTransform = FMatrix4(1.0f);
        CachedMVP     = ProjectionMatrix;
        bScissorEnabled = false;
        // Reset the leftover clip rect too, or a stale region clips this context's first draws.
        CurrentScissor  = Rml::Rectanglei();
    }

    uint64 FRmlUiRenderer::ComputeDrawCallHash() const
    {
        LUMINA_PROFILE_SCOPE();

        HashKeyScratch.clear();
        HashKeyScratch.reserve(DrawCalls.size());

        // Only worth a per-draw map lookup when a brush exists at all, and most UIs have none.
        bool bHasBrush = false;

        for (const FDrawCall& Draw : DrawCalls)
        {
            FDrawKey Key;
            Key.Geometry        = Draw.Geometry;
            Key.Texture         = Draw.Texture;
            Key.Shader          = Draw.Shader;
            Key.Translation[0]  = Draw.Translation.x;
            Key.Translation[1]  = Draw.Translation.y;
            Memory::Memcpy(Key.MVP, &Draw.MVP, sizeof(Key.MVP));
            Key.Scissor[0]      = Draw.Scissor.Position().x;
            Key.Scissor[1]      = Draw.Scissor.Position().y;
            Key.Scissor[2]      = Draw.Scissor.Width();
            Key.Scissor[3]      = Draw.Scissor.Height();
            Key.bScissorEnabled = Draw.bScissorEnabled ? 1u : 0u;
            Key.ClipMaskRange   = Draw.ClipMaskRange;
            HashKeyScratch.push_back(Key);

            if (!bHasBrush && LiveBrushCount > 0 && Draw.Texture != 0)
            {
                auto It = Textures.find(Draw.Texture);
                bHasBrush = (It != Textures.end() && It->second.BrushMaterial != nullptr);
            }
        }

        uint64 Hash = Hash::XXHash::GetHash64(HashKeyScratch.data(), HashKeyScratch.size() * sizeof(FDrawKey));
        Hash = Hash::XXHash::GetHash64(PendingClipMasks.data(), PendingClipMasks.size() * sizeof(FUiClipMask), Hash);

        // Layer and filter work is invisible to the draw list, so a filter-only change must still reach the hash.
        for (const FLayerCommand& Cmd : LayerCommands)
        {
            const uint64 OpKey[4] = { uint64(Cmd.Op), uint64(Cmd.DrawsBefore),
                                      (uint64(Cmd.Source) << 32) | uint64(Cmd.Destination),
                                      (uint64(Cmd.FilterCount) << 32) | uint64(Cmd.bReplace ? 1u : 0u) };
            Hash = Hash::XXHash::GetHash64(OpKey, sizeof(OpKey), Hash);

            for (uint32 i = 0; i < Cmd.FilterCount; ++i)
            {
                auto It = Filters.find(Rml::CompiledFilterHandle(PendingFilterRefs[Cmd.FilterOffset + i]));
                if (It != Filters.end())
                {
                    Hash = Hash::XXHash::GetHash64(&It->second, sizeof(FFilter), Hash);
                }
            }
        }

        struct FFrameKey
        {
            FUIntVector2 Size;
            FUIntVector2 LogicalSize;
            uint64       Count;
            uint64       BrushSalt;
        };

        // Without per-frame salt an animated material brush would freeze (draw list unchanged).
        const FFrameKey FrameKey { CurrentSize, CurrentLogicalSize, DrawCalls.size(), bHasBrush ? FrameCounter : 0ull };
        return Hash::XXHash::GetHash64(&FrameKey, sizeof(FrameKey), Hash);
    }

    void FRmlUiRenderer::ResolveBatchTextures(FTargetBatch& Batch) const
    {
        const size_t Count = Math::Min(Batch.Draws.size(), Batch.DrawTextures.size());
        for (size_t i = 0; i < Count; ++i)
        {
            uint32 Slot = DefaultWhite.SampledSlot;

            const Rml::TextureHandle Handle = Batch.DrawTextures[i];
            if (Handle != 0)
            {
                auto It = Textures.find(Handle);
                if (It != Textures.end() && It->second.ResourceID != RHI::kInvalidHeapSlot)
                {
                    Slot = It->second.ResourceID;
                }
                else
                {
                    // Once per session, because the silent version of this cost a long hunt.
                    static bool bWarned = false;
                    if (!bWarned)
                    {
                        bWarned = true;
                        LOG_WARN("[RmlUi] A cached draw references texture {} that is no longer live. It falls "
                                 "back to the default rather than a dead heap slot, but something released a "
                                 "texture the batch still names.", uint64(Handle));
                    }
                }
            }

            Batch.Draws[i].TextureID = Slot;
        }
    }

    uint64 FRmlUiRenderer::PeekFrameHash() const
    {
        CachedFrameHash = ComputeDrawCallHash();
        bCachedFrameHashValid = true;
        return CachedFrameHash;
    }

    bool FRmlUiRenderer::IsTargetUpToDate(RHI::FTextureH Target, uint64 Hash) const
    {
        if (!RHI::IsValid(Target))
        {
            return false;
        }
        auto It = TargetBatches.find(Target.Handle);
        return It != TargetBatches.end() && It->second.bValid && It->second.LastHash == Hash;
    }

    void FRmlUiRenderer::AbortFrame()
    {
        // Flush glyph uploads even on abort so a later render finds the atlas complete.
        UploadPendingTextures();
        ResetFrameState();
    }

    void FRmlUiRenderer::ResetFrameState()
    {
        for (const Rml::CompiledGeometryHandle Handle : DeferredGeometryReleases)
        {
            Geometries.erase(Handle);
        }
        DeferredGeometryReleases.clear();

        for (const Rml::CompiledFilterHandle Handle : DeferredFilterReleases)
        {
            Filters.erase(Handle);
        }
        DeferredFilterReleases.clear();

        for (const Rml::CompiledShaderHandle Handle : DeferredShaderReleases)
        {
            Shaders.erase(Handle);
        }
        DeferredShaderReleases.clear();

        // Unbinding inside the frames-in-flight window repoints a slot a recorded draw still samples, showing magenta.
        for (size_t i = 0; i < DeferredTextureReleases.size();)
        {
            const FRetiredTexture& Retired = DeferredTextureReleases[i];
            if (Retired.Frame + GFramesInFlight <= EngineFrameCounter)
            {
                ReleaseTextureNow(Retired.Handle);
                DeferredTextureReleases[i] = DeferredTextureReleases.back();
                DeferredTextureReleases.pop_back();
                continue;
            }
            ++i;
        }

        DrawCalls.clear();
        PendingClipMasks.clear();
        ActiveClipOffset = 0;
        ActiveClipCount  = 0;

        FreeLayers.clear();
        for (uint32 Index = 0; Index < uint32(OwnedLayers.size()); ++Index)
        {
            if (OwnedLayers[Index].Color.IsValid())
            {
                FreeLayers.push_back(Index);
            }
        }
        LayerStack.clear();
        LayerCommands.clear();
        PendingFilterRefs.clear();
        CurrentCmdList = {};
        CurrentTarget  = {};
        bClearTarget   = false;
    }

    void FRmlUiRenderer::ReleaseTargetBatch(RHI::FTextureH Target)
    {
        auto It = TargetBatches.find(Target.Handle);
        if (It != TargetBatches.end())
        {
            RHI::Core::Retire(It->second.VertexBuffer);
            RHI::Core::Retire(It->second.IndexBuffer);
            TargetBatches.erase(It);
        }
    }

    void FRmlUiRenderer::NoteTargetStable(RHI::FTextureH Target, bool bStable)
    {
        auto It = TargetBatches.find(Target.Handle);
        if (It == TargetBatches.end())
        {
            return;
        }
        It->second.LastUsedFrame = FrameCounter;
        if (bStable)
        {
            ++It->second.StableFrames;
        }
        else
        {
            It->second.StableFrames = 0;
        }
    }

    uint32 FRmlUiRenderer::GetTargetStableFrames(RHI::FTextureH Target)
    {
        auto It = TargetBatches.find(Target.Handle);
        if (It == TargetBatches.end())
        {
            return 0;
        }
        // A dormant widget stops rendering entirely, so this poll is the only thing keeping its batch alive.
        It->second.LastUsedFrame = FrameCounter;
        return It->second.StableFrames;
    }

    void FRmlUiRenderer::EnsureBatchBuffers(FTargetBatch& Batch, uint64 VertexBytes, uint64 IndexBytes)
    {
        if (Batch.VertexBuffer.Gpu == 0 || Batch.VertexCapacity < VertexBytes)
        {
            RHI::Core::Retire(Batch.VertexBuffer);
            Batch.VertexCapacity = Math::Max<uint64>(VertexBytes + VertexBytes / 2, 4096);
            Batch.VertexBuffer   = RHI::Malloc(Batch.VertexCapacity, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            RHI::SetDebugName(Batch.VertexBuffer.Gpu, "RmlUi.BatchVertices");
        }

        if (Batch.IndexBuffer.Gpu == 0 || Batch.IndexCapacity < IndexBytes)
        {
            RHI::Core::Retire(Batch.IndexBuffer);
            Batch.IndexCapacity = Math::Max<uint64>(IndexBytes + IndexBytes / 2, 4096);
            Batch.IndexBuffer   = RHI::Malloc(Batch.IndexCapacity, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            RHI::SetDebugName(Batch.IndexBuffer.Gpu, "RmlUi.BatchIndices");
        }
    }

    void FRmlUiRenderer::EvictStaleBatches()
    {
        // A destroyed RT recycles its handle, so an unclaimed batch can match an unrelated target.
        constexpr uint64 StaleFrames = 300;
        if (FrameCounter < StaleFrames || FrameCounter - LastEvictFrame < StaleFrames)
        {
            return;
        }
        LastEvictFrame = FrameCounter;

        for (auto It = TargetBatches.begin(); It != TargetBatches.end(); )
        {
            if (It->second.LastUsedFrame + StaleFrames < FrameCounter)
            {
                RHI::Core::Retire(It->second.VertexBuffer);
                RHI::Core::Retire(It->second.IndexBuffer);
                It = TargetBatches.erase(It);
            }
            else
            {
                ++It;
            }
        }
    }

    void FRmlUiRenderer::EndFrame()
    {
        if (!RHI::IsValid(CurrentCmdList))
        {
            return;
        }
        RHI::FCmdListH CL = CurrentCmdList;
        RHI::CmdBeginMarker(CL, "RmlUi");

        // Texture uploads must be outside a render pass.
        UploadPendingTextures();

        // Outside the UI render pass, since each brush opens its own pass before the UI samples it.
        RenderMaterialBrushes();

        // The clear rides the pass load op, so a frame that bails before recording one still owes it.
        auto Finish = [&]()
        {
            if (bClearTarget && RHI::IsValid(CurrentTarget))
            {
                RHI::FRenderAttachment ClearOnly;
                ClearOnly.Texture  = CurrentTarget;
                ClearOnly.LoadOp   = RHI::ELoadOp::Clear;
                ClearOnly.StoreOp  = RHI::EStoreOp::Store;
                ClearOnly.Color[0] = CurrentClearColor.x;
                ClearOnly.Color[1] = CurrentClearColor.y;
                ClearOnly.Color[2] = CurrentClearColor.z;
                ClearOnly.Color[3] = CurrentClearColor.w;

                RHI::FRenderPassDesc ClearPass;
                ClearPass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&ClearOnly, 1);
                ClearPass.RenderArea       = CurrentSize;
                RHI::CmdBeginRenderPass(CL, ClearPass);
                RHI::CmdEndRenderPass(CL);
            }
            RHI::CmdEndMarker(CL);
            ResetFrameState();
        };

        if (DrawCalls.empty() || !RHI::IsValid(CurrentTarget))
        {
            Finish();
            return;
        }

        LUMINA_PROFILE_SCOPE();

        FTargetBatch& Batch = TargetBatches[CurrentTarget.Handle];
        Batch.LastUsedFrame = FrameCounter;
        const uint64 Hash   = bCachedFrameHashValid ? CachedFrameHash : ComputeDrawCallHash();

        const bool bRebuild = !Batch.bValid || Batch.LastHash != Hash
            || Batch.VertexBuffer.Gpu == 0 || Batch.IndexBuffer.Gpu == 0;

        const float FullW = float(CurrentSize.x);
        const float FullH = float(CurrentSize.y);

        // Scissor rects arrive in layout pixels, but the shader tests SV_Position in target pixels.
        const float ClipScaleX = (CurrentLogicalSize.x > 0) ? FullW / float(CurrentLogicalSize.x) : 1.0f;
        const float ClipScaleY = (CurrentLogicalSize.y > 0) ? FullH / float(CurrentLogicalSize.y) : 1.0f;

        if (bRebuild)
        {
            BatchVertices.clear();
            BatchIndices.clear();
            BatchDrawData.clear();
            BatchStops.clear();
            DrawIndexOffsets.assign(DrawCalls.size(), 0);
            DrawIndexCounts.assign(DrawCalls.size(), 0);
            BatchDrawTextures.clear();
            uint32 DrawSlot = 0;

            for (const FDrawCall& Draw : DrawCalls)
            {
                const uint32 ThisDraw = DrawSlot++;
                DrawIndexOffsets[ThisDraw] = uint32(BatchIndices.size());

                auto GeomIt = Geometries.find(Draw.Geometry);
                if (GeomIt == Geometries.end())
                {
                    WarnDroppedDrawOnce("its geometry was released before the batch was built");
                    continue;
                }
                const FGeometry& Geom = GeomIt->second;
                if (Geom.VertexData.empty() || Geom.IndexData.empty() || Geom.IndexCount == 0)
                {
                    WarnDroppedDrawOnce("its geometry is empty");
                    continue;
                }

                uint32 ResourceID = DefaultWhite.SampledSlot;
                if (Draw.Texture != 0)
                {
                    auto TexIt = Textures.find(Draw.Texture);
                    if (TexIt != Textures.end() && TexIt->second.ResourceID != RHI::kInvalidHeapSlot)
                    {
                        ResourceID = TexIt->second.ResourceID;
                    }
                }
                if (ResourceID == RHI::kInvalidHeapSlot)
                {
                    WarnDroppedDrawOnce("no texture resolved and the default is unavailable");
                    continue;
                }

                // Per-draw entry; this draw's vertices reference it by index.
                const uint32 DrawIndex = uint32(BatchDrawData.size());
                FUiDraw DD;
                DD.MVP      = Draw.MVP;
                DD.ClipRect = Draw.bScissorEnabled
                    ? FVector4(float(Draw.Scissor.Position().x) * ClipScaleX,
                               float(Draw.Scissor.Position().y) * ClipScaleY,
                               float(Draw.Scissor.Position().x + Draw.Scissor.Width())  * ClipScaleX,
                               float(Draw.Scissor.Position().y + Draw.Scissor.Height()) * ClipScaleY)
                    : FVector4(0.0f, 0.0f, FullW, FullH);
                DD.TextureID    = ResourceID;
                DD.SamplerIndex = GRmlUiSamplerIndex;
                DD.ShaderType   = 0;
                DD.StopOffset   = 0;
                DD.ShaderParams = FVector4(0.0f);
                DD.StopCount    = 0;
                DD.bRepeating   = 0;
                DD.ShaderScale  = 1.0f;
                DD.ClipMaskRange = Draw.ClipMaskRange;


                if (Draw.Shader != 0)
                {
                    auto ShaderIt = Shaders.find(Draw.Shader);
                    if (ShaderIt == Shaders.end())
                    {
                        WarnDroppedDrawOnce("its gradient shader was released before the batch was built");
                        continue;
                    }
                    if (ShaderIt->second.Stops.empty())
                    {
                        WarnDroppedDrawOnce("its gradient shader compiled with no color stops");
                        continue;
                    }
                    const FShader& Sh = ShaderIt->second;
                    DD.ShaderType   = Sh.Type;
                    DD.ShaderParams = Sh.Params;
                    DD.ShaderScale  = Sh.Scale;
                    DD.bRepeating   = Sh.bRepeating ? 1u : 0u;
                    DD.StopOffset   = uint32(BatchStops.size());
                    DD.StopCount    = uint32(Sh.Stops.size());
                    BatchStops.insert(BatchStops.end(), Sh.Stops.begin(), Sh.Stops.end());
                }

                BatchDrawData.push_back(DD);
                BatchDrawTextures.push_back(Draw.Texture);

                const Rml::Vertex* SrcVerts = reinterpret_cast<const Rml::Vertex*>(Geom.VertexData.data());
                const uint32 VtxCount = uint32(Geom.VertexData.size() / sizeof(Rml::Vertex));
                const int*   SrcInds  = reinterpret_cast<const int*>(Geom.IndexData.data());

                const uint32 BaseVertex = uint32(BatchVertices.size());

                // Convert to the GPU vertex, baking translation and tagging with the draw index.
                BatchVertices.reserve(BatchVertices.size() + VtxCount);
                for (uint32 v = 0; v < VtxCount; ++v)
                {
                    const Rml::Vertex& Src = SrcVerts[v];
                    FUiVertex V;
                    V.Position[0] = Src.position.x + Draw.Translation.x;
                    V.Position[1] = Src.position.y + Draw.Translation.y;
                    Memory::Memcpy(&V.Color, &Src.colour, sizeof(uint32));
                    V.UV[0]     = Src.tex_coord.x;
                    V.UV[1]     = Src.tex_coord.y;
                    V.DrawIndex = DrawIndex;
                    BatchVertices.push_back(V);
                }

                // Rebase indices so we draw with VertexOffset = 0.
                BatchIndices.reserve(BatchIndices.size() + Geom.IndexCount);
                for (uint32 i = 0; i < Geom.IndexCount; ++i)
                {
                    BatchIndices.push_back(uint32(SrcInds[i]) + BaseVertex);
                }

                DrawIndexCounts[ThisDraw] = Geom.IndexCount;
            }

            if (BatchIndices.empty() || BatchDrawData.empty())
            {
                Batch.IndexCount = 0;
                Batch.bValid     = true;
                Batch.LastHash   = Hash;
                Batch.Draws.clear();
                Batch.Stops.clear();
                Batch.ClipMasks.clear();
                Finish();
                return;
            }

            const uint64 VBytes = BatchVertices.size() * sizeof(FUiVertex);
            const uint64 IBytes = BatchIndices.size()  * sizeof(uint32);
            EnsureBatchBuffers(Batch, VBytes, IBytes);

            // The leading barrier also orders against a prior frame still reading the old contents.
            RHI::FTransientAlloc VBStage = RHI::Core::AllocTransient(VBytes, 16);
            RHI::FTransientAlloc IBStage = RHI::Core::AllocTransient(IBytes, 4);
            Memory::Memcpy(VBStage.Cpu, BatchVertices.data(), VBytes);
            Memory::Memcpy(IBStage.Cpu, BatchIndices.data(),  IBytes);

            RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EStageFlags::Transfer);
            RHI::CmdMemcpy(CL, Batch.VertexBuffer.Gpu, VBStage.Gpu, VBytes);
            RHI::CmdMemcpy(CL, Batch.IndexBuffer.Gpu,  IBStage.Gpu, IBytes);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::AllCommands);

            Batch.Draws      = Move(BatchDrawData);
            Batch.Stops      = Move(BatchStops);
            Batch.ClipMasks  = PendingClipMasks;
            Batch.DrawFirstIndex = DrawIndexOffsets;
            Batch.DrawIndexCount = DrawIndexCounts;
            Batch.DrawTextures   = Move(BatchDrawTextures);
            Batch.IndexCount = uint32(BatchIndices.size());
            Batch.LastHash   = Hash;
            Batch.bValid     = true;
        }

        if (Batch.IndexCount == 0 || Batch.Draws.empty() || Batch.VertexBuffer.Gpu == 0 || Batch.IndexBuffer.Gpu == 0)
        {
            Finish();
            return;
        }

        RHI::FPipelineH Pipeline = GetPipelineForFormat(RHI::GetTextureDesc(CurrentTarget).Format);
        if (!RHI::IsValid(Pipeline))
        {
            Finish();
            return;
        }

        // Resolved every frame rather than from the cache, so a released texture cannot leave a dead slot.
        ResolveBatchTextures(Batch);

        const RHI::GPUPtr DrawsPtr = RHI::Core::CopyTransientArray(Batch.Draws.data(), Batch.Draws.size());

        // Always upload at least one stop so the args block never carries a null device address.
        const FUiColorStop DummyStop {};
        const RHI::GPUPtr StopsPtr = Batch.Stops.empty()
            ? RHI::Core::CopyTransientArray(&DummyStop, 1)
            : RHI::Core::CopyTransientArray(Batch.Stops.data(), Batch.Stops.size());

        const FUiClipMask DummyMask {};
        const RHI::GPUPtr MasksPtr = Batch.ClipMasks.empty()
            ? RHI::Core::CopyTransientArray(&DummyMask, 1)
            : RHI::Core::CopyTransientArray(Batch.ClipMasks.data(), Batch.ClipMasks.size());

        // Composite passes read the same mask array the geometry batch does.
        PassClipMasksPtr = MasksPtr;

        const FRmlUiArgs Args { DrawsPtr, Batch.VertexBuffer.Gpu, StopsPtr, MasksPtr };
        const RHI::GPUPtr ArgsPtr = RHI::Core::CopyTransient(Args);

        ReplayFrame(CL, Batch, Pipeline, ArgsPtr);

        RHI::CmdEndMarker(CL);
        ResetFrameState();
    }

    void FRmlUiRenderer::CopyLayer(RHI::FCmdListH CL, uint32 SourceLayer, uint32 DestLayer, bool bBlend)
    {
        FUiFilterArgs Args {};
        Args.Kind = (uint32)EUiFilterPass::Copy;
        RunFilterPass(CL, SourceLayer, DestLayer, Args, bBlend);
    }

    // The frame's own target has no bindless slot, so a backdrop filter reaches it by transfer copy.
    void FRmlUiRenderer::BlitTargetToLayer(RHI::FCmdListH CL, uint32 DestLayer)
    {
        if (DestLayer == kNoLayer || !RHI::IsValid(CurrentTarget))
        {
            return;
        }

        RHI::FTextureSlice Slice;
        Slice.Extent = FUIntVector3(CurrentSize.x, CurrentSize.y, 1);

        RHI::CmdBarrier(CL, RHI::EStageFlags::RasterColorOut, RHI::EStageFlags::Transfer);
        RHI::CmdBlitTexture(CL, CurrentTarget, Slice, LayerTexture(DestLayer), Slice, RHI::EFilter::Nearest);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::AllCommands);
    }

    void FRmlUiRenderer::CopyLayerToTexture(RHI::FCmdListH CL, uint32 SourceLayer, RHI::FTextureH Dest,
                                            const FUIntVector2& DestSize, const FVector4& SourceRect)
    {
        const uint32 SourceSlot = LayerSlot(SourceLayer);
        RHI::FPipelineH Pipeline = GetFilterPipeline(EFormat::RGBA8_UNORM, false);
        if (SourceSlot == RHI::kInvalidHeapSlot || !RHI::IsValid(Pipeline))
        {
            LOG_WARN("[RmlUi] Saved layer copy skipped (source slot {}, pipeline {}); the texture keeps "
                     "undefined contents and RmlUi caches it.", SourceSlot, RHI::IsValid(Pipeline) ? 1 : 0);
            return;
        }

        FUiFilterArgs Args {};
        Args.Kind         = (uint32)EUiFilterPass::Copy;
        Args.SourceID     = SourceSlot;
        Args.SamplerIndex = GRmlUiSamplerIndex;
        Args.SourceRect   = SourceRect;

        RHI::CmdBarrier(CL, RHI::EStageFlags::RasterColorOut, RHI::EStageFlags::PixelShader);
        OpenPassSized(CL, Dest, true, DestSize);
        RHI::CmdSetPipeline(CL, Pipeline);
        RHI::CmdDraw(CL, RHI::Core::CopyTransient(Args), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
    }

    // Two separable passes, so the result always lands back in InOut and the caller owns no swap.
    void FRmlUiRenderer::ApplyBlur(RHI::FCmdListH CL, uint32 InOut, uint32 Scratch, float Sigma)
    {
        if (InOut == kNoLayer || Scratch == kNoLayer || Sigma <= 0.0f)
        {
            return;
        }

        const float Clamped = Math::Min(Sigma, float(GMaxBlurTaps) * 0.5f);
        const uint32 Taps   = Math::Min(uint32(Math::Ceil(Clamped * 3.0f)), GMaxBlurTaps);

        FUiFilterArgs Args {};
        Args.Kind     = (uint32)EUiFilterPass::Blur;
        Args.Sigma    = Clamped;
        Args.TapCount = Math::Max(Taps, 1u);

        Args.TexelStep = FVector4(1.0f / float(Math::Max(CurrentSize.x, 1u)), 0.0f, 0.0f, 0.0f);
        RunFilterPass(CL, InOut, Scratch, Args, false);

        Args.TexelStep = FVector4(0.0f, 1.0f / float(Math::Max(CurrentSize.y, 1u)), 0.0f, 0.0f);
        RunFilterPass(CL, Scratch, InOut, Args, false);
    }

    void FRmlUiRenderer::ApplyFilters(RHI::FCmdListH CL, uint32& Primary, uint32 Scratch,
                                      uint32 FilterOffset, uint32 FilterCount)
    {
        auto SwapLayers = [](uint32& A, uint32& B) { const uint32 T = A; A = B; B = T; };
        uint32 Secondary = Scratch;

        for (uint32 i = 0; i < FilterCount; ++i)
        {
            const uint32 Ref = PendingFilterRefs[FilterOffset + i];
            auto It = Filters.find(Rml::CompiledFilterHandle(Ref));
            if (It == Filters.end())
            {
                continue;
            }
            const FFilter& Filter = It->second;

            FUiFilterArgs Args {};
            Args.ColorMatrix = Filter.ColorMatrix;
            Args.Opacity     = Filter.Opacity;

            switch (Filter.Kind)
            {
            case EFilterKind::ColorMatrix:
                Args.Kind = (uint32)EUiFilterPass::ColorMatrix;
                RunFilterPass(CL, Primary, Secondary, Args, false);
                SwapLayers(Primary, Secondary);
                break;

            case EFilterKind::Opacity:
                Args.Kind = (uint32)EUiFilterPass::Opacity;
                RunFilterPass(CL, Primary, Secondary, Args, false);
                SwapLayers(Primary, Secondary);
                break;

            case EFilterKind::MaskImage:
                Args.Kind   = (uint32)EUiFilterPass::MaskImage;
                Args.MaskID = LayerSlot(Filter.MaskLayer);
                if (Args.MaskID != RHI::kInvalidHeapSlot)
                {
                    RunFilterPass(CL, Primary, Secondary, Args, false);
                    SwapLayers(Primary, Secondary);
                }
                break;

            case EFilterKind::Blur:
                ApplyBlur(CL, Primary, Secondary, Filter.Sigma);
                break;

            case EFilterKind::DropShadow:
            {
                const uint32 Shadow = AcquireLayer();
                if (Shadow == kNoLayer)
                {
                    break;
                }

                FUiFilterArgs Shadowed {};
                Shadowed.Kind      = (uint32)EUiFilterPass::DropShadow;
                Shadowed.Color     = Filter.Color;
                Shadowed.TexelStep = FVector4(0.0f, 0.0f,
                                              Filter.Offset.x / float(Math::Max(CurrentSize.x, 1u)),
                                              Filter.Offset.y / float(Math::Max(CurrentSize.y, 1u)));
                RunFilterPass(CL, Primary, Shadow, Shadowed, false);
                ApplyBlur(CL, Shadow, Secondary, Filter.Sigma);

                // The element sits on top of its own shadow, so lay the shadow down first.
                CopyLayer(CL, Shadow, Secondary, false);
                CopyLayer(CL, Primary, Secondary, true);
                SwapLayers(Primary, Secondary);
                ReleaseLayerToPool(Shadow);
                break;
            }

            default:
                break;
            }
        }
    }

    void FRmlUiRenderer::ReplayFrame(RHI::FCmdListH CL, const FTargetBatch& Batch, RHI::FPipelineH TargetPipeline, RHI::GPUPtr ArgsPtr)
    {
        // Anything the stream names is withheld for the whole replay, or a composite scratch lands on its own source.
        auto Withhold = [&](uint32 Index)
        {
            if (Index == kNoLayer)
            {
                return;
            }
            for (size_t i = 0; i < FreeLayers.size(); ++i)
            {
                if (FreeLayers[i] == Index)
                {
                    FreeLayers[i] = FreeLayers.back();
                    FreeLayers.pop_back();
                    return;
                }
            }
        };

        for (const FLayerCommand& Cmd : LayerCommands)
        {
            Withhold(Cmd.TargetLayer);
            Withhold(Cmd.Source);
            Withhold(Cmd.Destination);
        }

        RHI::FPipelineH LayerPipeline = GetPipelineForFormat(EFormat::RGBA8_UNORM);

        static thread_local TVector<uint32> Stack;
        Stack.clear();

        uint32 Active = kNoLayer;
        uint32 NextDraw = 0;
        bool   bTargetClearPending = bClearTarget;

        const uint32 DrawCount = uint32(Batch.DrawFirstIndex.size());

        auto FlushDraws = [&](uint32 UpTo)
        {
            UpTo = Math::Min(UpTo, DrawCount);
            if (UpTo <= NextDraw)
            {
                return;
            }

            const uint32 First = Batch.DrawFirstIndex[NextDraw];
            const uint32 Last  = UpTo - 1;
            const uint32 Count = (Batch.DrawFirstIndex[Last] + Batch.DrawIndexCount[Last]) - First;
            NextDraw = UpTo;

            if (Count == 0)
            {
                return;
            }

            const bool bBase  = (Active == kNoLayer);
            const bool bClear = bBase && bTargetClearPending;
            const bool bSavedScissor = bPassScissorSet;
            bPassScissorSet = false;
            RHI::CmdBarrier(CL, RHI::EStageFlags::RasterColorOut, RHI::EStageFlags::PixelShader);
            OpenPass(CL, LayerTexture(Active), bClear);
            bPassScissorSet = bSavedScissor;
            if (bClear)
            {
                bTargetClearPending = false;
            }

            RHI::CmdSetPipeline(CL, bBase ? TargetPipeline : LayerPipeline);
            RHI::CmdDrawIndexed(CL, Batch.IndexBuffer.Gpu, 0, ArgsPtr, Count, 1, First, 0, 0, RHI::EIndexType::Uint32);
            RHI::CmdEndRenderPass(CL);
        };

        const float ScissorScaleX = (CurrentLogicalSize.x > 0) ? float(CurrentSize.x) / float(CurrentLogicalSize.x) : 1.0f;
        const float ScissorScaleY = (CurrentLogicalSize.y > 0) ? float(CurrentSize.y) / float(CurrentLogicalSize.y) : 1.0f;

        auto SetOpScissor = [&](const FLayerCommand& Cmd)
        {
            bPassScissorSet = Cmd.bScissorEnabled && Cmd.Scissor.Width() > 0 && Cmd.Scissor.Height() > 0;
            if (!bPassScissorSet)
            {
                return;
            }
            const int32 MinX = int32(float(Cmd.Scissor.Position().x) * ScissorScaleX);
            const int32 MinY = int32(float(Cmd.Scissor.Position().y) * ScissorScaleY);
            const int32 MaxX = int32(float(Cmd.Scissor.Position().x + Cmd.Scissor.Width())  * ScissorScaleX);
            const int32 MaxY = int32(float(Cmd.Scissor.Position().y + Cmd.Scissor.Height()) * ScissorScaleY);
            PassScissor = RHI::FRect{ Math::Max(MinX, 0), Math::Min(MaxX, (int32)CurrentSize.x),
                                      Math::Max(MinY, 0), Math::Min(MaxY, (int32)CurrentSize.y) };
        };

        PassClipScale = FVector2(ScissorScaleX, ScissorScaleY);

        for (const FLayerCommand& Cmd : LayerCommands)
        {
            FlushDraws(Cmd.DrawsBefore);
            SetOpScissor(Cmd);
            PassClipMaskRange = Cmd.ClipMaskRange;

            switch (Cmd.Op)
            {
            case ELayerOp::Push:
                if (Cmd.TargetLayer != kNoLayer)
                {
                    const bool bSaved = bPassScissorSet;
                    bPassScissorSet = false;
                    ClearLayer(CL, Cmd.TargetLayer);
                    bPassScissorSet = bSaved;
                }
                Stack.push_back(Cmd.TargetLayer);
                Active = Cmd.TargetLayer;
                break;

            case ELayerOp::Pop:
                if (!Stack.empty())
                {
                    Stack.pop_back();
                }
                Active = Stack.empty() ? kNoLayer : Stack.back();
                break;

            case ELayerOp::Composite:
            {
                uint32 SourceLayer = Cmd.Source;
                uint32 BaseCopy    = kNoLayer;

                // A backdrop filter names layer zero as its source, which is the frame's own target.
                if (SourceLayer == kNoLayer)
                {
                    BaseCopy = AcquireLayer();
                    if (BaseCopy == kNoLayer)
                    {
                        break;
                    }
                    BlitTargetToLayer(CL, BaseCopy);
                    SourceLayer = BaseCopy;
                }

                uint32 Primary = SourceLayer;
                uint32 Scratch = kNoLayer;
                uint32 Working = kNoLayer;

                if (Cmd.FilterCount > 0)
                {
                    // Filters ping-pong, so the chain needs a scratch of its own plus a copy to own.
                    Scratch = AcquireLayer();
                    Working = AcquireLayer();
                    if (Scratch != kNoLayer && Working != kNoLayer)
                    {
                        CopyLayer(CL, SourceLayer, Working, false);
                        Primary = Working;
                        ApplyFilters(CL, Primary, Scratch, Cmd.FilterOffset, Cmd.FilterCount);
                    }
                }

                if (Cmd.Destination == kNoLayer && bTargetClearPending)
                {
                    OpenPass(CL, CurrentTarget, true);
                    RHI::CmdEndRenderPass(CL);
                    bTargetClearPending = false;
                }
                CopyLayer(CL, Primary, Cmd.Destination, !Cmd.bReplace);

                ReleaseLayerToPool(Scratch);
                ReleaseLayerToPool(Working);
                ReleaseLayerToPool(BaseCopy);
                break;
            }

            case ELayerOp::SaveTexture:
                if (RHI::IsValid(Cmd.DestTexture))
                {
                    bPassScissorSet   = false;
                    PassClipMaskRange = 0;
                    CopyLayerToTexture(CL, Cmd.Source, Cmd.DestTexture, Cmd.DestSize, Cmd.SourceRect);
                }
                break;

            case ELayerOp::SaveMaskImage:
                if (Cmd.TargetLayer != kNoLayer)
                {
                    CopyLayer(CL, Cmd.Source, Cmd.TargetLayer, false);
                }
                break;
            }
        }

        bPassScissorSet   = false;
        PassClipMaskRange = 0;
        FlushDraws(DrawCount);

        // A frame whose draws were all gated away still owes its clear.
        if (bTargetClearPending)
        {
            OpenPass(CL, CurrentTarget, true);
            RHI::CmdEndRenderPass(CL);
        }
    }

    void FRmlUiRenderer::OpenPass(RHI::FCmdListH CL, RHI::FTextureH Target, bool bClear)
    {
        OpenPassSized(CL, Target, bClear, CurrentSize);
    }

    void FRmlUiRenderer::OpenPassSized(RHI::FCmdListH CL, RHI::FTextureH Target, bool bClear, const FUIntVector2& Extent)
    {
        RHI::FRenderAttachment Color;
        Color.Texture  = Target;
        Color.LoadOp   = bClear ? RHI::ELoadOp::Clear : RHI::ELoadOp::Load;
        Color.StoreOp  = RHI::EStoreOp::Store;
        Color.Color[0] = bClear ? CurrentClearColor.x : 0.0f;
        Color.Color[1] = bClear ? CurrentClearColor.y : 0.0f;
        Color.Color[2] = bClear ? CurrentClearColor.z : 0.0f;
        Color.Color[3] = bClear ? CurrentClearColor.w : 0.0f;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        RHI::CmdSetDepthStencilState(CL, DepthState);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
        RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CW);
        RHI::CmdSetViewport(CL, RHI::FRect{ 0, (int)Extent.x, 0, (int)Extent.y });
        RHI::CmdSetScissor(CL, bPassScissorSet ? PassScissor : RHI::FRect{ 0, (int)Extent.x, 0, (int)Extent.y });
    }

    void FRmlUiRenderer::ClearLayer(RHI::FCmdListH CL, uint32 LayerIndex)
    {
        RHI::FRenderAttachment Color;
        Color.Texture  = LayerTexture(LayerIndex);
        Color.LoadOp   = RHI::ELoadOp::Clear;
        Color.StoreOp  = RHI::EStoreOp::Store;
        Color.Color[0] = 0.0f;
        Color.Color[1] = 0.0f;
        Color.Color[2] = 0.0f;
        Color.Color[3] = 0.0f;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = CurrentSize;
        RHI::CmdBeginRenderPass(CL, Pass);
        RHI::CmdEndRenderPass(CL);
    }

    void FRmlUiRenderer::RunFilterPass(RHI::FCmdListH CL, uint32 SourceLayer, uint32 DestLayer, FUiFilterArgs Args, bool bBlend)
    {
        const uint32 SourceSlot = LayerSlot(SourceLayer);
        if (SourceSlot == RHI::kInvalidHeapSlot)
        {
            return;
        }

        Args.SourceID     = SourceSlot;
        Args.SamplerIndex = GRmlUiSamplerIndex;
        Args.ClipMaskRange = PassClipMaskRange;
        Args.ClipMasks     = PassClipMasksPtr;
        Args.ClipScale     = PassClipScale;

        const EFormat DestFormat = (DestLayer == kNoLayer)
            ? RHI::GetTextureDesc(CurrentTarget).Format
            : EFormat::RGBA8_UNORM;

        RHI::FPipelineH Pipeline = GetFilterPipeline(DestFormat, bBlend);
        if (!RHI::IsValid(Pipeline))
        {
            return;
        }

        // The source was just written as a color attachment, so make those writes visible to sampling.
        RHI::CmdBarrier(CL, RHI::EStageFlags::RasterColorOut, RHI::EStageFlags::PixelShader);

        OpenPass(CL, LayerTexture(DestLayer), !bBlend);
        RHI::CmdSetPipeline(CL, Pipeline);
        RHI::CmdDraw(CL, RHI::Core::CopyTransient(Args), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
    }

    Rml::CompiledGeometryHandle FRmlUiRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices)
    {
        const size_t VBSize = Vertices.size() * sizeof(Rml::Vertex);
        const size_t IBSize = Indices.size()  * sizeof(int);
        if (VBSize == 0 || IBSize == 0)
        {
            return 0;
        }

        // CPU-side cache only, concatenated into the target's resident buffers at EndFrame.
        FGeometry Geom;
        const uint8* VBytes = reinterpret_cast<const uint8*>(Vertices.data());
        const uint8* IBytes = reinterpret_cast<const uint8*>(Indices.data());
        Geom.VertexData.assign(VBytes, VBytes + VBSize);
        Geom.IndexData.assign(IBytes, IBytes + IBSize);
        Geom.IndexCount = uint32(Indices.size());

        const Rml::CompiledGeometryHandle Handle = NextGeometryHandle++;
        Geometries.emplace(Handle, Move(Geom));
        return Handle;
    }

    void FRmlUiRenderer::RenderGeometry(Rml::CompiledGeometryHandle Geometry, Rml::Vector2f Translation, Rml::TextureHandle Texture)
    {
        FDrawCall Draw;
        Draw.Geometry        = Geometry;
        Draw.Texture         = Texture;
        Draw.Translation     = FVector2(Translation.x, Translation.y);
        Draw.MVP             = CachedMVP;
        Draw.bScissorEnabled = bScissorEnabled;
        Draw.Scissor         = CurrentScissor;
        Draw.ClipMaskRange   = PackClipRange(ActiveClipOffset, ActiveClipCount);
        DrawCalls.push_back(Move(Draw));
    }

    void FRmlUiRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle Geometry)
    {
        // RmlUi frees temporary geometry during Context::Render, and box shadows did exactly that mid-frame.
        if (RHI::IsValid(CurrentCmdList))
        {
            DeferredGeometryReleases.push_back(Geometry);
            return;
        }
        Geometries.erase(Geometry);
    }

    Rml::TextureHandle FRmlUiRenderer::LoadTexture(Rml::Vector2i& OutDimensions, const Rml::String& Source)
    {
        Rml::String Path = Source;
        if (Path.rfind("material:", 0) == 0)
        {
            Path = Path.substr(std::strlen("material:"));
        }

        // Optional `?w=NNN&h=NNN` brush-resolution override (materials only).
        uint32 Width = 256, Height = 256;
        const size_t QueryPos = Path.find('?');
        if (QueryPos != Rml::String::npos)
        {
            const Rml::String Query = Path.substr(QueryPos + 1);
            Path = Path.substr(0, QueryPos);
            auto ReadParam = [&](const char* Key, uint32& Out)
            {
                const Rml::String Token = Rml::String(Key) + "=";
                const size_t At = Query.find(Token);
                if (At != Rml::String::npos)
                {
                    const int Parsed = std::atoi(Query.c_str() + At + Token.size());
                    if (Parsed > 0) Out = (uint32)(Parsed < 4096 ? Parsed : 4096);
                }
            };
            ReadParam("w", Width);
            ReadParam("h", Height);
        }

        // RmlUi strips the leading '/' when joining src to the document dir; restore it for the absolute VFS lookup.
        if (!Path.empty() && Path[0] != '/')
        {
            Path = "/" + Path;
        }

        const FStringView PathView(Path.c_str(), Path.size());
        CObject* Asset = LoadObject<CObject>(PathView);
        if (Asset == nullptr)
        {
            LOG_WARN("[RmlUi] LoadTexture: no asset at '{}'.", Path.c_str());
            return 0;
        }

        if (CMaterialInterface* Material = Cast<CMaterialInterface>(Asset))
        {
            return LoadMaterialBrush(OutDimensions, Material, PathView, Width, Height);
        }
        if (CTexture* Texture = Cast<CTexture>(Asset))
        {
            return LoadTextureAsset(OutDimensions, Texture);
        }

        LOG_WARN("[RmlUi] LoadTexture: '{}' is neither a material nor a texture.", Path.c_str());
        return 0;
    }

    Rml::TextureHandle FRmlUiRenderer::LoadTextureAsset(Rml::Vector2i& OutDimensions, CTexture* Texture)
    {
        const FUIntVector2 Extent = Texture->GetTextureResource().ImageDescription.Extent;
        if (Extent.x == 0 || Extent.y == 0)
        {
            LOG_WARN("[RmlUi] LoadTexture: '{}' has zero extent.", Texture->GetName().c_str());
            return 0;
        }

        const int32 ResourceID = Texture->GetResourceID();
        if (ResourceID < 0)
        {
            LOG_WARN("[RmlUi] LoadTexture: '{}' has no bindless resource id.", Texture->GetName().c_str());
            return 0;
        }

        // Pinned for the lifetime of the RmlUi handle so an unload cannot dangle the bindless slot.
        Texture->AddToRoot();

        const Rml::TextureHandle Handle = NextTextureHandle++;
        FTexture Tex;
        Tex.ResourceID      = (uint32)ResourceID;
        Tex.AssetKeepalive  = Texture;
        Textures.emplace(Handle, Move(Tex));

        OutDimensions.x = int(Extent.x);
        OutDimensions.y = int(Extent.y);
        return Handle;
    }

    Rml::TextureHandle FRmlUiRenderer::LoadMaterialBrush(Rml::Vector2i& OutDimensions, CMaterialInterface* Material, const FStringView& SourcePath, uint32 Width, uint32 Height)
    {
        if (Material->GetMaterialType() != EMaterialType::UI)
        {
            LOG_WARN("[RmlUi] LoadTexture: '{}' is not a UI material (MaterialType must be UI).", Material->GetName().c_str());
            return 0;
        }

        const FString BrushName = FString("RmlUi.Brush.") + Material->GetName().ToString();

        // Persistent RGBA8 brush RT, rendered each frame in RenderMaterialBrushes and sampled by the UI.
        RHI::FManagedTexture Image = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = Width,
            .Height = Height,
            .Format = EFormat::RGBA8_UNORM,
            .bRenderTarget = true,
            .DebugName = BrushName.c_str(),
        });
        if (!Image.IsValid() || Image.SampledSlot == RHI::kInvalidHeapSlot)
        {
            LOG_WARN("[RmlUi] LoadTexture: failed to create brush RT for '{}'.", Material->GetName().c_str());
            return 0;
        }

        Material->AddToRoot();

        const Rml::TextureHandle Handle = NextTextureHandle++;
        FTexture Tex;
        Tex.Managed         = Image;
        Tex.ResourceID      = Image.SampledSlot;
        Tex.BrushMaterial   = Material;
        Tex.BrushSize       = FUIntVector2(Width, Height);
        Tex.BrushSourcePath = FString(SourcePath.data(), SourcePath.size());
        Textures.emplace(Handle, Move(Tex));
        ++LiveBrushCount;

        OutDimensions.x = int(Width);
        OutDimensions.y = int(Height);
        return Handle;
    }

    Rml::TextureHandle FRmlUiRenderer::GenerateTexture(Rml::Span<const Rml::byte> Bytes, Rml::Vector2i Dimensions)
    {
        TVector<uint8> Copy;
        Copy.assign(Bytes.data(), Bytes.data() + Bytes.size());
        return RegisterTexturePending(Move(Copy), Dimensions.x, Dimensions.y);
    }

    Rml::TextureHandle FRmlUiRenderer::RegisterTexturePending(TVector<uint8>&& Bytes, int Width, int Height)
    {
        if (Width <= 0 || Height <= 0 || Bytes.size() < size_t(Width) * size_t(Height) * 4)
        {
            return 0;
        }

        RHI::FManagedTexture Image = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = (uint32)Width,
            .Height = (uint32)Height,
            .Format = EFormat::RGBA8_UNORM,
            .DebugName = "RmlUi.GeneratedTexture",
        });
        if (!Image.IsValid() || Image.SampledSlot == RHI::kInvalidHeapSlot)
        {
            return 0;
        }

        const Rml::TextureHandle Handle = NextTextureHandle++;
        FTexture Tex;
        Tex.Managed    = Image;
        Tex.ResourceID = Image.SampledSlot;
        Textures.emplace(Handle, Move(Tex));

        FPendingTexture Pending;
        Pending.Handle = Handle;
        Pending.Width  = Width;
        Pending.Height = Height;
        Pending.Bytes  = Move(Bytes);
        PendingTextureUploads.push_back(Move(Pending));
        return Handle;
    }

    void FRmlUiRenderer::UploadPendingTextures()
    {
        if (PendingTextureUploads.empty())
        {
            return;
        }

        for (FPendingTexture& Pending : PendingTextureUploads)
        {
            auto It = Textures.find(Pending.Handle);
            if (It == Textures.end() || !It->second.Managed.IsValid())
            {
                continue;
            }
            RHI::Textures::Upload(It->second.Managed, 0, Pending.Bytes.data(), Pending.Bytes.size(), (uint32)Pending.Width);
        }

        PendingTextureUploads.clear();
    }

    void FRmlUiRenderer::ReleaseTexture(Rml::TextureHandle Texture)
    {
        if (RHI::IsValid(CurrentCmdList))
        {
            DeferredTextureReleases.push_back(FRetiredTexture{ Texture, EngineFrameCounter });
            return;
        }
        ReleaseTextureNow(Texture);
    }

    void FRmlUiRenderer::ReleaseTextureNow(Rml::TextureHandle Texture)
    {

        auto It = Textures.find(Texture);
        if (It != Textures.end())
        {
            if (It->second.AssetKeepalive != nullptr)
            {
                It->second.AssetKeepalive->RemoveFromRoot();
                It->second.AssetKeepalive = nullptr;
            }
            if (It->second.BrushMaterial != nullptr)
            {
                It->second.BrushMaterial->RemoveFromRoot();
                It->second.BrushMaterial = nullptr;
                --LiveBrushCount;
            }
            if (It->second.Managed.IsValid())
            {
                RHI::Textures::Release(It->second.Managed);
            }
            Textures.erase(It);
        }
    }

    void FRmlUiRenderer::EnableScissorRegion(bool bEnable)
    {
        bScissorEnabled = bEnable;
    }

    void FRmlUiRenderer::SetScissorRegion(Rml::Rectanglei Region)
    {
        CurrentScissor = Region;
    }

    // Every clip mask comes from MeshUtilities::GenerateBackground, so its shape is recoverable from the vertices.
    bool FRmlUiRenderer::InferRoundedRect(const FGeometry& Geom, FVector2 Translation, FUiClipMask& Out)
    {
        const uint32 Count = uint32(Geom.VertexData.size() / sizeof(Rml::Vertex));
        if (Count < 3)
        {
            return false;
        }
        const Rml::Vertex* Verts = reinterpret_cast<const Rml::Vertex*>(Geom.VertexData.data());

        float MinX = Verts[0].position.x, MaxX = MinX;
        float MinY = Verts[0].position.y, MaxY = MinY;
        for (uint32 i = 1; i < Count; ++i)
        {
            MinX = Math::Min(MinX, Verts[i].position.x);
            MaxX = Math::Max(MaxX, Verts[i].position.x);
            MinY = Math::Min(MinY, Verts[i].position.y);
            MaxY = Math::Max(MaxY, Verts[i].position.y);
        }

        const float Width  = MaxX - MinX;
        const float Height = MaxY - MinY;
        if (Width <= 0.0f || Height <= 0.0f)
        {
            return false;
        }

        // Each corner arc ends tangent to the edge, so the tangent point sits one radius along it.
        constexpr float EdgeEpsilon = 0.01f;
        float TopLeftX = MaxX, TopRightX = MinX, BottomLeftX = MaxX, BottomRightX = MinX;
        for (uint32 i = 0; i < Count; ++i)
        {
            const float X = Verts[i].position.x;
            const float Y = Verts[i].position.y;
            if (Y <= MinY + EdgeEpsilon)
            {
                TopLeftX  = Math::Min(TopLeftX, X);
                TopRightX = Math::Max(TopRightX, X);
            }
            if (Y >= MaxY - EdgeEpsilon)
            {
                BottomLeftX  = Math::Min(BottomLeftX, X);
                BottomRightX = Math::Max(BottomRightX, X);
            }
        }

        const float MaxRadius = Math::Min(Width, Height) * 0.5f;
        auto Clamp = [MaxRadius](float R) { return Math::Clamp(R, 0.0f, MaxRadius); };

        Out.Radii = FVector4(Clamp(TopLeftX - MinX), Clamp(MaxX - TopRightX),
                             Clamp(MaxX - BottomRightX), Clamp(BottomLeftX - MinX));
        Out.Rect  = FVector4((MinX + MaxX) * 0.5f + Translation.x, (MinY + MaxY) * 0.5f + Translation.y,
                             Width * 0.5f, Height * 0.5f);
        return true;
    }

    void FRmlUiRenderer::EnableClipMask(bool bEnable)
    {
        // RmlUi enables the mask and then re-issues every layer of it, so this always starts fresh.
        ActiveClipOffset = 0;
        ActiveClipCount  = 0;
        (void)bEnable;
    }

    void FRmlUiRenderer::RenderToClipMask(Rml::ClipMaskOperation Operation, Rml::CompiledGeometryHandle Geometry,
                                          Rml::Vector2f Translation)
    {
        // Set and SetInverse begin a list, Intersect appends to it, which is how RmlUi nests clips.
        const bool bInvert    = (Operation == Rml::ClipMaskOperation::SetInverse);
        const bool bIntersect = (Operation == Rml::ClipMaskOperation::Intersect);

        if (bIntersect && (ActiveClipCount == 0 || ActiveClipCount >= kMaxClipMasksPerDraw))
        {
            return;
        }

        auto It = Geometries.find(Geometry);
        if (It == Geometries.end())
        {
            return;
        }

        FUiClipMask Mask;
        if (!InferRoundedRect(It->second, FVector2(Translation.x, Translation.y), Mask))
        {
            return;
        }
        Mask.Params = FVector4(bInvert ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

        // An intersection must stay contiguous, so a new list always starts at the current end.
        if (!bIntersect)
        {
            ActiveClipOffset = uint32(PendingClipMasks.size());
            ActiveClipCount  = 0;
        }

        PendingClipMasks.push_back(Mask);
        ++ActiveClipCount;
    }

    void FRmlUiRenderer::SetTransform(const Rml::Matrix4f* Transform)
    {
        if (Transform)
        {
            // RmlUi defaults column-major (matches our matrices); RMLUI_MATRIX_ROW_MAJOR would require a transpose.
            std::memcpy(Math::ValuePtr(UserTransform), Transform->data(), sizeof(float) * 16);
        }
        else
        {
            UserTransform = FMatrix4(1.0f);
        }
        CachedMVP = ProjectionMatrix * UserTransform;
    }

    Rml::CompiledShaderHandle FRmlUiRenderer::CompileShader(const Rml::String& Name, const Rml::Dictionary& Parameters)
    {
        auto Lookup = [&Parameters](const char* Key) -> const Rml::Variant*
        {
            const auto It = Parameters.find(Rml::String(Key));
            return It != Parameters.end() ? &It->second : nullptr;
        };
        auto GetVec2 = [&](const char* Key)
        {
            const Rml::Variant* V = Lookup(Key);
            return V != nullptr ? V->Get<Rml::Vector2f>() : Rml::Vector2f(0.0f, 0.0f);
        };
        auto GetFloat = [&](const char* Key)
        {
            const Rml::Variant* V = Lookup(Key);
            return V != nullptr ? V->Get<float>() : 0.0f;
        };

        FShader Shader;
        if (Name == "linear-gradient")
        {
            const Rml::Vector2f P0 = GetVec2("p0");
            const Rml::Vector2f P1 = GetVec2("p1");
            const float LengthSq = (P1.x - P0.x) * (P1.x - P0.x) + (P1.y - P0.y) * (P1.y - P0.y);
            if (LengthSq <= 0.0f)
            {
                return 0;
            }
            Shader.Type   = 1;
            Shader.Params = FVector4(P0.x, P0.y, P1.x, P1.y);
            Shader.Scale  = 1.0f / LengthSq;
        }
        else if (Name == "radial-gradient")
        {
            const Rml::Vector2f Center = GetVec2("center");
            const Rml::Vector2f Radius = GetVec2("radius");
            Shader.Type   = 2;
            Shader.Params = FVector4(Center.x, Center.y, Radius.x, Radius.y);
        }
        else if (Name == "conic-gradient")
        {
            const Rml::Vector2f Center = GetVec2("center");
            const float Angle = GetFloat("angle");
            // Angle zero points up and sweeps clockwise, the CSS convention RmlUi encodes.
            Shader.Type   = 3;
            Shader.Params = FVector4(Center.x, Center.y, Math::Sin(Angle), -Math::Cos(Angle));
        }
        else
        {
            return 0;
        }

        const Rml::Variant* StopsVar = Lookup("color_stop_list");
        if (StopsVar == nullptr || StopsVar->GetType() != Rml::Variant::COLORSTOPLIST)
        {
            return 0;
        }

        const Rml::ColorStopList& List = StopsVar->GetReference<Rml::ColorStopList>();
        if (List.empty())
        {
            return 0;
        }

        const size_t StopCount = Math::Min<size_t>(List.size(), GMaxColorStops);
        Shader.Stops.reserve(StopCount);
        for (size_t i = 0; i < StopCount; ++i)
        {
            const Rml::ColorStop& Stop = List[i];
            FUiColorStop Out;
            Out.Color    = FVector4(float(Stop.color.red)  / 255.0f, float(Stop.color.green) / 255.0f,
                                    float(Stop.color.blue) / 255.0f, float(Stop.color.alpha) / 255.0f);
            Out.Position = FVector4(Stop.position.number, 0.0f, 0.0f, 0.0f);
            Shader.Stops.push_back(Out);
        }

        const Rml::Variant* RepeatVar = Lookup("repeating");
        Shader.bRepeating = (RepeatVar != nullptr) && RepeatVar->Get<bool>();

        const Rml::CompiledShaderHandle Handle = NextShaderHandle++;
        Shaders.emplace(Handle, Move(Shader));
        return Handle;
    }

    void FRmlUiRenderer::RenderShader(Rml::CompiledShaderHandle Shader, Rml::CompiledGeometryHandle Geometry,
                                      Rml::Vector2f Translation, Rml::TextureHandle Texture)
    {
        if (Shaders.find(Shader) == Shaders.end())
        {
            return;
        }
        RenderGeometry(Geometry, Translation, Texture);
        DrawCalls.back().Shader = Shader;
    }

    Rml::LayerHandle FRmlUiRenderer::PushLayer()
    {
        const uint32 Index = AcquireLayer();
        LayerStack.push_back(Index);

        FLayerCommand Cmd;
        Cmd.Op          = ELayerOp::Push;
        Cmd.DrawsBefore = uint32(DrawCalls.size());
        Cmd.bScissorEnabled = bScissorEnabled;
        Cmd.Scissor         = CurrentScissor;
        Cmd.TargetLayer = Index;
        LayerCommands.push_back(Cmd);

        // RmlUi identifies a layer by its depth, and zero is reserved for the frame's own target.
        return Rml::LayerHandle(LayerStack.size());
    }

    void FRmlUiRenderer::CompositeLayers(Rml::LayerHandle Source, Rml::LayerHandle Destination, Rml::BlendMode BlendMode,
                                         Rml::Span<const Rml::CompiledFilterHandle> InFilters)
    {
        FLayerCommand Cmd;
        Cmd.Op           = ELayerOp::Composite;
        Cmd.DrawsBefore  = uint32(DrawCalls.size());
        Cmd.bScissorEnabled = bScissorEnabled;
        Cmd.Scissor         = CurrentScissor;

        // RmlUi clips the composite as well as the geometry, which is what rounds a backdrop filter.
        Cmd.ClipMaskRange = PackClipRange(ActiveClipOffset, ActiveClipCount);
        // Resolved here rather than at replay, while the recording stack still matches the handles.
        Cmd.Source       = ResolveLayerHandle(Source);
        Cmd.Destination  = ResolveLayerHandle(Destination);
        Cmd.bReplace     = (BlendMode == Rml::BlendMode::Replace);
        Cmd.FilterOffset = uint32(PendingFilterRefs.size());

        for (const Rml::CompiledFilterHandle Handle : InFilters)
        {
            if (Filters.find(Handle) != Filters.end())
            {
                PendingFilterRefs.push_back(uint32(Handle));
            }
        }
        Cmd.FilterCount = uint32(PendingFilterRefs.size()) - Cmd.FilterOffset;
        LayerCommands.push_back(Cmd);
    }

    void FRmlUiRenderer::PopLayer()
    {
        FLayerCommand Cmd;
        Cmd.Op          = ELayerOp::Pop;
        Cmd.DrawsBefore = uint32(DrawCalls.size());
        Cmd.bScissorEnabled = bScissorEnabled;
        Cmd.Scissor         = CurrentScissor;
        LayerCommands.push_back(Cmd);

        if (!LayerStack.empty())
        {
            // Replay runs in record order, so a layer freed here is only reused by a push that also replays later.
            const uint32 Popped = LayerStack.back();
            LayerStack.pop_back();
            ReleaseLayerToPool(Popped);
        }
    }

    Rml::TextureHandle FRmlUiRenderer::SaveLayerAsTexture()
    {
        // RmlUi sets the scissor to the region it wants saved, so the result must cover exactly that.
        const float ScaleX = (CurrentLogicalSize.x > 0) ? float(CurrentSize.x) / float(CurrentLogicalSize.x) : 1.0f;
        const float ScaleY = (CurrentLogicalSize.y > 0) ? float(CurrentSize.y) / float(CurrentLogicalSize.y) : 1.0f;

        FVector2 RegionMin(0.0f, 0.0f);
        FVector2 RegionSize(float(CurrentSize.x), float(CurrentSize.y));
        if (bScissorEnabled && CurrentScissor.Width() > 0 && CurrentScissor.Height() > 0)
        {
            RegionMin  = FVector2(float(CurrentScissor.Position().x) * ScaleX, float(CurrentScissor.Position().y) * ScaleY);
            RegionSize = FVector2(float(CurrentScissor.Width()) * ScaleX, float(CurrentScissor.Height()) * ScaleY);
        }

        const uint32 RegionW = Math::Max(uint32(RegionSize.x + 0.5f), 1u);
        const uint32 RegionH = Math::Max(uint32(RegionSize.y + 0.5f), 1u);

        // Owned outright, since sharing an FManagedTexture with the layer pool retired the same handle twice.
        RHI::FManagedTexture Image = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = RegionW,
            .Height = RegionH,
            .Format = EFormat::RGBA8_UNORM,
            .bRenderTarget = true,
            .DebugName = "RmlUi.SavedLayer",
        });
        if (!Image.IsValid() || Image.SampledSlot == RHI::kInvalidHeapSlot)
        {
            LOG_WARN("[RmlUi] SaveLayerAsTexture could not create a {}x{} target, so the element owning "
                     "this box-shadow draws nothing at all.", RegionW, RegionH);
            return 0;
        }

        const Rml::TextureHandle Handle = NextTextureHandle++;
        FTexture Tex;
        Tex.Managed    = Image;
        Tex.ResourceID = Image.SampledSlot;
        Textures.emplace(Handle, Move(Tex));

        FLayerCommand Cmd;
        Cmd.Op           = ELayerOp::SaveTexture;
        Cmd.DrawsBefore  = uint32(DrawCalls.size());
        Cmd.Source       = CurrentLayerIndex();
        Cmd.DestTexture  = Image.Texture;
        Cmd.DestSize     = FUIntVector2(RegionW, RegionH);
        Cmd.SourceRect   = FVector4(RegionSize.x / float(Math::Max(CurrentSize.x, 1u)),
                                    RegionSize.y / float(Math::Max(CurrentSize.y, 1u)),
                                    RegionMin.x  / float(Math::Max(CurrentSize.x, 1u)),
                                    RegionMin.y  / float(Math::Max(CurrentSize.y, 1u)));
        Cmd.SavedTexture = Handle;
        LayerCommands.push_back(Cmd);

        return Handle;
    }

    Rml::CompiledFilterHandle FRmlUiRenderer::SaveLayerAsMaskImage()
    {
        const uint32 Index = AcquireLayer();
        if (Index == kNoLayer)
        {
            LOG_WARN("[RmlUi] SaveLayerAsMaskImage got no layer, so the masked element draws nothing.");
            return 0;
        }

        FLayerCommand Cmd;
        Cmd.Op          = ELayerOp::SaveMaskImage;
        Cmd.DrawsBefore = uint32(DrawCalls.size());
        Cmd.Source      = CurrentLayerIndex();
        Cmd.TargetLayer = Index;
        LayerCommands.push_back(Cmd);

        FFilter Filter;
        Filter.Kind      = EFilterKind::MaskImage;
        Filter.MaskLayer = Index;

        const Rml::CompiledFilterHandle Handle = NextFilterHandle++;
        Filters.emplace(Handle, Filter);

        return Handle;
    }

    Rml::CompiledFilterHandle FRmlUiRenderer::CompileFilter(const Rml::String& Name, const Rml::Dictionary& Parameters)
    {
        FFilter Filter;

        auto Diagonal = [](float X, float Y, float Z)
        {
            FMatrix4 M(1.0f);
            M[0][0] = X; M[1][1] = Y; M[2][2] = Z;
            return M;
        };

        // Column-major storage, so one row of the color matrix is written across the columns.
        auto FromRows = [](const FVector4& R0, const FVector4& R1, const FVector4& R2)
        {
            FMatrix4 M(1.0f);
            for (int32 C = 0; C < 4; ++C)
            {
                M[C][0] = R0[C];
                M[C][1] = R1[C];
                M[C][2] = R2[C];
                M[C][3] = (C == 3) ? 1.0f : 0.0f;
            }
            return M;
        };

        if (Name == "opacity")
        {
            Filter.Kind    = EFilterKind::Opacity;
            Filter.Opacity = Rml::Get(Parameters, "value", 1.0f);
        }
        else if (Name == "blur")
        {
            Filter.Kind  = EFilterKind::Blur;
            Filter.Sigma = Rml::Get(Parameters, "sigma", 1.0f);
        }
        else if (Name == "drop-shadow")
        {
            const auto Premultiplied = Rml::Get(Parameters, "color", Rml::Colourb()).ToPremultiplied();
            const Rml::Vector2f Offset = Rml::Get(Parameters, "offset", Rml::Vector2f(0.0f));
            Filter.Kind   = EFilterKind::DropShadow;
            Filter.Sigma  = Rml::Get(Parameters, "sigma", 0.0f);
            Filter.Color  = FVector4(Premultiplied.red / 255.0f, Premultiplied.green / 255.0f,
                                     Premultiplied.blue / 255.0f, Premultiplied.alpha / 255.0f);
            Filter.Offset = FVector2(Offset.x, Offset.y);
        }
        else if (Name == "brightness")
        {
            const float V = Rml::Get(Parameters, "value", 1.0f);
            Filter.Kind = EFilterKind::ColorMatrix;
            Filter.ColorMatrix = Diagonal(V, V, V);
        }
        else if (Name == "contrast")
        {
            const float V = Rml::Get(Parameters, "value", 1.0f);
            const float Gray = 0.5f - 0.5f * V;
            Filter.Kind = EFilterKind::ColorMatrix;
            Filter.ColorMatrix = Diagonal(V, V, V);
            Filter.ColorMatrix[3][0] = Gray;
            Filter.ColorMatrix[3][1] = Gray;
            Filter.ColorMatrix[3][2] = Gray;
        }
        else if (Name == "invert")
        {
            const float V = Math::Clamp(Rml::Get(Parameters, "value", 1.0f), 0.0f, 1.0f);
            const float Inverted = 1.0f - 2.0f * V;
            Filter.Kind = EFilterKind::ColorMatrix;
            Filter.ColorMatrix = Diagonal(Inverted, Inverted, Inverted);
            Filter.ColorMatrix[3][0] = V;
            Filter.ColorMatrix[3][1] = V;
            Filter.ColorMatrix[3][2] = V;
        }
        else if (Name == "grayscale")
        {
            const float V = Rml::Get(Parameters, "value", 1.0f);
            const float Rev = 1.0f - V;
            const FVector3 G = FVector3(0.2126f, 0.7152f, 0.0722f) * V;
            Filter.Kind = EFilterKind::ColorMatrix;
            Filter.ColorMatrix = FromRows(FVector4(G.x + Rev, G.y, G.z, 0.0f),
                                          FVector4(G.x, G.y + Rev, G.z, 0.0f),
                                          FVector4(G.x, G.y, G.z + Rev, 0.0f));
        }
        else if (Name == "sepia")
        {
            const float V = Rml::Get(Parameters, "value", 1.0f);
            const float Rev = 1.0f - V;
            const FVector3 R = FVector3(0.393f, 0.769f, 0.189f) * V;
            const FVector3 G = FVector3(0.349f, 0.686f, 0.168f) * V;
            const FVector3 B = FVector3(0.272f, 0.534f, 0.131f) * V;
            Filter.Kind = EFilterKind::ColorMatrix;
            Filter.ColorMatrix = FromRows(FVector4(R.x + Rev, R.y, R.z, 0.0f),
                                          FVector4(G.x, G.y + Rev, G.z, 0.0f),
                                          FVector4(B.x, B.y, B.z + Rev, 0.0f));
        }
        else if (Name == "hue-rotate")
        {
            const float V = Rml::Get(Parameters, "value", 1.0f);
            const float S = Math::Sin(V);
            const float C = Math::Cos(V);
            Filter.Kind = EFilterKind::ColorMatrix;
            Filter.ColorMatrix = FromRows(
                FVector4(0.213f + 0.787f * C - 0.213f * S, 0.715f - 0.715f * C - 0.715f * S, 0.072f - 0.072f * C + 0.928f * S, 0.0f),
                FVector4(0.213f - 0.213f * C + 0.143f * S, 0.715f + 0.285f * C + 0.140f * S, 0.072f - 0.072f * C - 0.283f * S, 0.0f),
                FVector4(0.213f - 0.213f * C - 0.787f * S, 0.715f - 0.715f * C + 0.715f * S, 0.072f + 0.928f * C + 0.072f * S, 0.0f));
        }
        else if (Name == "saturate")
        {
            const float V = Rml::Get(Parameters, "value", 1.0f);
            Filter.Kind = EFilterKind::ColorMatrix;
            Filter.ColorMatrix = FromRows(
                FVector4(0.213f + 0.787f * V, 0.715f - 0.715f * V, 0.072f - 0.072f * V, 0.0f),
                FVector4(0.213f - 0.213f * V, 0.715f + 0.285f * V, 0.072f - 0.072f * V, 0.0f),
                FVector4(0.213f - 0.213f * V, 0.715f - 0.715f * V, 0.072f + 0.928f * V, 0.0f));
        }
        else
        {
            LOG_WARN("[RmlUi] Unsupported filter '{}'.", Name.c_str());
            return 0;
        }

        const Rml::CompiledFilterHandle Handle = NextFilterHandle++;
        Filters.emplace(Handle, Filter);
        return Handle;
    }

    void FRmlUiRenderer::ReleaseFilter(Rml::CompiledFilterHandle Filter)
    {
        // Box shadows release their blur the moment the composite is recorded, long before it runs.
        if (RHI::IsValid(CurrentCmdList))
        {
            DeferredFilterReleases.push_back(Filter);
            return;
        }
        Filters.erase(Filter);
    }

    void FRmlUiRenderer::ReleaseShader(Rml::CompiledShaderHandle Shader)
    {
        if (RHI::IsValid(CurrentCmdList))
        {
            DeferredShaderReleases.push_back(Shader);
            return;
        }
        Shaders.erase(Shader);
    }

    void FRmlUiRenderer::RevalidateBrushes(RHI::FCmdListH CmdList)
    {
        // Driven by the asset-registry broadcast, not per frame; a rename-back resumes without a reload.
        for (auto& KV : Textures)
        {
            FTexture& Tex = KV.second;
            if (Tex.BrushMaterial == nullptr || Tex.BrushSourcePath.empty())
            {
                continue;
            }

            const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(
                FStringView(Tex.BrushSourcePath.c_str(), Tex.BrushSourcePath.size()));
            const bool bResolves = (Data != nullptr) && (Data->AssetGUID == Tex.BrushMaterial->GetGUID());

            if (!bResolves && !Tex.bBrushStale)
            {
                // Clear to transparent so the document breaks instead of showing the old asset.
                const float Transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                RHI::CmdBarrier(CmdList, RHI::EStageFlags::AllCommands, RHI::EStageFlags::Transfer);
                RHI::CmdClearTexture(CmdList, Tex.Managed.Texture, Transparent);
                RHI::CmdBarrier(CmdList, RHI::EStageFlags::Transfer, RHI::EStageFlags::AllCommands);
                Tex.bBrushStale   = true;
                Tex.bBrushCleared = true;
            }
            else if (bResolves && Tex.bBrushStale)
            {
                // An asset returned at this path (renamed back, say) resumes next frame.
                Tex.bBrushStale = false;
            }
        }
    }

    void FRmlUiRenderer::RenderMaterialBrushes()
    {
        FRenderManager* RenderManager = TryRender();
        if (!RHI::IsValid(CurrentCmdList) || RenderManager == nullptr)
        {
            return;
        }
        RHI::FCmdListH CL = CurrentCmdList;

        // Event-driven off the registry broadcast, so there is no per-frame validation cost.
        if (bBrushRevalidatePending.exchange(false, std::memory_order_acquire))
        {
            RevalidateBrushes(CL);
        }

        if (DrawCalls.empty())
        {
            return;
        }

        // Monotonic wall clock drives animated UI materials (GetTime() in-shader).
        const float Time = static_cast<float>(PlatformTime::Seconds());

        // Brushes are shared across contexts, so scope the work to the drawing context and de-dup.
        static thread_local TVector<Rml::TextureHandle> Rendered;
        Rendered.clear();

        bool bAnyWrites = false;

        // A brush RT is created undefined, so give it transparent black once before anything samples it.
        auto ClearBrushOnce = [&](FTexture& Tex)
        {
            if (Tex.bBrushCleared)
            {
                return;
            }
            RHI::FRenderAttachment Color;
            Color.Texture  = Tex.Managed.Texture;
            Color.LoadOp   = RHI::ELoadOp::Clear;
            Color.StoreOp  = RHI::EStoreOp::Store;
            Color.Color[0] = 0.0f; Color.Color[1] = 0.0f; Color.Color[2] = 0.0f; Color.Color[3] = 0.0f;

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            Pass.RenderArea       = Tex.BrushSize;
            RHI::CmdBeginRenderPass(CL, Pass);
            RHI::CmdEndRenderPass(CL);
            Tex.bBrushCleared = true;
            bAnyWrites = true;
        };

        for (const FDrawCall& Draw : DrawCalls)
        {
            if (Draw.Texture == 0)
            {
                continue;
            }
            auto It = Textures.find(Draw.Texture);
            if (It == Textures.end() || It->second.BrushMaterial == nullptr || It->second.bBrushStale)
            {
                continue;
            }

            bool bAlready = false;
            for (Rml::TextureHandle H : Rendered)
            {
                if (H == Draw.Texture) { bAlready = true; break; }
            }
            if (bAlready)
            {
                continue;
            }
            Rendered.push_back(Draw.Texture);

            FTexture& Tex = It->second;
            CMaterialInterface* Material = Tex.BrushMaterial;
            if (!Material->IsReadyForRender() || Material->GetMaterialType() != EMaterialType::UI)
            {
                ClearBrushOnce(Tex);
                continue;
            }
            const FShaderH VS = Material->GetVertexShader();
            const FShaderH PS = Material->GetPixelShader();
            if (VS == nullptr || PS == nullptr)
            {
                ClearBrushOnce(Tex);
                continue;
            }

            RHI::FPipelineH Pipeline = GetBrushPipeline(VS, PS);
            if (!RHI::IsValid(Pipeline))
            {
                ClearBrushOnce(Tex);
                continue;
            }

            FUIMaterialBrushArgs Args = {};
            Args.Materials     = RenderManager->GetMaterialManager().GetMaterialBuffer();
            Args.ScreenSize[0] = Tex.BrushSize.x;
            Args.ScreenSize[1] = Tex.BrushSize.y;
            Args.Time          = Time;
            Args.MaterialIndex = (uint32)Material->GetMaterialIndex();
            const RHI::GPUPtr ArgsPtr = RHI::Core::CopyTransient(Args);

            RHI::FRenderAttachment Color;
            Color.Texture  = Tex.Managed.Texture;
            Color.LoadOp   = RHI::ELoadOp::Clear;
            Color.StoreOp  = RHI::EStoreOp::Store;
            Color.Color[0] = 0.0f; Color.Color[1] = 0.0f; Color.Color[2] = 0.0f; Color.Color[3] = 0.0f;

            RHI::FRenderPassDesc Pass;
            Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
            Pass.RenderArea       = Tex.BrushSize;

            RHI::CmdBeginRenderPass(CL, Pass);
            RHI::CmdSetDepthStencilState(CL, DepthState);
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
            RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CW);
            RHI::CmdSetPipeline(CL, Pipeline);
            RHI::CmdSetViewport(CL, RHI::FRect{ 0, (int)Tex.BrushSize.x, 0, (int)Tex.BrushSize.y });
            RHI::CmdSetScissor(CL, RHI::FRect{ 0, (int)Tex.BrushSize.x, 0, (int)Tex.BrushSize.y });

            RHI::CmdDraw(CL, ArgsPtr, 3, 1, 0, 0);

            RHI::CmdEndRenderPass(CL);
            Tex.bBrushCleared = true;
            bAnyWrites = true;
        }

        if (bAnyWrites)
        {
            // Brush RT writes visible to the UI pass sampling them.
            RHI::CmdBarrier(CL, RHI::EStageFlags::RasterColorOut, RHI::EStageFlags::PixelShader);
        }
    }
}
