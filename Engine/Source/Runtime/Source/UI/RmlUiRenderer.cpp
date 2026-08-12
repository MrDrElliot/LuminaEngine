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

#include <chrono>

namespace Lumina
{
    // Mirrors RmlUiCommon.slang::FRmlUiArgs.
    struct FRmlUiArgs
    {
        RHI::GPUPtr Draws;      // per-draw FUiDraw array (transient)
        RHI::GPUPtr Vertices;   // resident batch vertex buffer (vertex pulling)
    };

    // Mirrors UIMaterialGlobals.slang::FUIMaterialArgs (scalar layout). ScreenSize must stay at offset
    // 16: the shader reads this block by device address, where relaxed block layout forbids a vector
    // straddling a 16-byte boundary -- see the note on the Slang side.
    struct FUIMaterialBrushArgs
    {
        RHI::GPUPtr Materials;
        uint32      MaterialIndex;
        float       Time;
        uint32      ScreenSize[4];   // .xy = brush resolution
    };

    static_assert(sizeof(FUIMaterialBrushArgs) == 32, "FUIMaterialArgs layout must match UIMaterialGlobals.slang");
    static_assert(offsetof(FUIMaterialBrushArgs, ScreenSize) == 16, "ScreenSize must not straddle a 16-byte boundary");

    // RmlUi wants bilinear + clamp; stock sampler heap slot 1.
    static constexpr uint32 GRmlUiSamplerIndex = (uint32)RHI::EStockSampler::LinearClamp;

    // Vertex layout: pos(8) colour(4) uv(8).
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
        static_assert(sizeof(FUiDraw) == 96,   "FUiDraw must match RmlUiCommon.slang::FUiDraw (std430).");

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

        // Premultiplied alpha: RmlUi pre-multiplies vertex colors.
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

    void FRmlUiRenderer::BeginFrame(RHI::FCmdListH CmdList, RHI::FTextureH Target, const FUIntVector2& ViewportSize, const FUIntVector2& LogicalSize)
    {
        CurrentCmdList   = CmdList;
        CurrentTarget    = Target;
        CurrentSize      = ViewportSize;
        bCachedFrameHashValid = false;
        ++FrameCounter;

        DrawCalls.clear();

        const FUIntVector2 ProjSize = (LogicalSize.x > 0 && LogicalSize.y > 0) ? LogicalSize : ViewportSize;

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
        // Reset the leftover clip rect too: a stale region from the previously rendered context
        // would otherwise clip this one's first draws (preview-into-live bleed).
        CurrentScissor  = Rml::Rectanglei();
    }

    uint64 FRmlUiRenderer::ComputeDrawCallHash() const
    {
        uint64 Hash = 1469598103934665603ull;
        auto Mix = [&Hash](const void* Data, size_t Size)
        {
            const uint8* Bytes = static_cast<const uint8*>(Data);
            for (size_t i = 0; i < Size; ++i)
            {
                Hash ^= Bytes[i];
                Hash *= 1099511628211ull;
            }
        };

        const uint64 Count = DrawCalls.size();
        Mix(&Count, sizeof(Count));

        bool bHasBrush = false;
        for (const FDrawCall& Draw : DrawCalls)
        {
            Mix(&Draw.Geometry, sizeof(Draw.Geometry));
            Mix(&Draw.Texture, sizeof(Draw.Texture));
            Mix(&Draw.Translation, sizeof(Draw.Translation));
            Mix(&Draw.MVP, sizeof(Draw.MVP));
            Mix(&Draw.bScissorEnabled, sizeof(Draw.bScissorEnabled));
            Mix(&Draw.Scissor, sizeof(Draw.Scissor));

            if (Draw.Texture != 0)
            {
                auto It = Textures.find(Draw.Texture);
                if (It != Textures.end() && It->second.BrushMaterial != nullptr)
                {
                    bHasBrush = true;
                }
            }
        }

        // Without per-frame salt an animated material brush would freeze (draw list unchanged).
        if (bHasBrush)
        {
            Mix(&FrameCounter, sizeof(FrameCounter));
        }
        return Hash;
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
        DrawCalls.clear();
        CurrentCmdList = {};
        CurrentTarget  = {};
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
        if (bStable)
        {
            ++It->second.StableFrames;
        }
        else
        {
            It->second.StableFrames = 0;
        }
    }

    uint32 FRmlUiRenderer::GetTargetStableFrames(RHI::FTextureH Target) const
    {
        auto It = TargetBatches.find(Target.Handle);
        return It != TargetBatches.end() ? It->second.StableFrames : 0;
    }

    void FRmlUiRenderer::EnsureBatchBuffers(FTargetBatch& Batch, uint64 VertexBytes, uint64 IndexBytes)
    {
        if (Batch.VertexBuffer == 0 || Batch.VertexCapacity < VertexBytes)
        {
            RHI::Core::Retire(Batch.VertexBuffer);
            Batch.VertexCapacity = Math::Max<uint64>(VertexBytes + VertexBytes / 2, 4096);
            Batch.VertexBuffer   = RHI::Malloc(Batch.VertexCapacity, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            RHI::SetDebugName(Batch.VertexBuffer, "RmlUi.BatchVertices");
        }

        if (Batch.IndexBuffer == 0 || Batch.IndexCapacity < IndexBytes)
        {
            RHI::Core::Retire(Batch.IndexBuffer);
            Batch.IndexCapacity = Math::Max<uint64>(IndexBytes + IndexBytes / 2, 4096);
            Batch.IndexBuffer   = RHI::Malloc(Batch.IndexCapacity, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            RHI::SetDebugName(Batch.IndexBuffer, "RmlUi.BatchIndices");
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

        // UI materials evaluate into their brush RTs before the UI samples them.
        // Also outside the UI render pass (each brush opens its own pass).
        RenderMaterialBrushes();

        if (DrawCalls.empty() || !RHI::IsValid(CurrentTarget))
        {
            RHI::CmdEndMarker(CL);
            ResetFrameState();
            return;
        }

        LUMINA_PROFILE_SCOPE();

        FTargetBatch& Batch = TargetBatches[CurrentTarget.Handle];
        const uint64 Hash   = bCachedFrameHashValid ? CachedFrameHash : ComputeDrawCallHash();

        const bool bRebuild = !Batch.bValid || Batch.LastHash != Hash
            || Batch.VertexBuffer == 0 || Batch.IndexBuffer == 0;

        const float FullW = float(CurrentSize.x);
        const float FullH = float(CurrentSize.y);

        if (bRebuild)
        {
            BatchVertices.clear();
            BatchIndices.clear();
            BatchDrawData.clear();

            for (const FDrawCall& Draw : DrawCalls)
            {
                auto GeomIt = Geometries.find(Draw.Geometry);
                if (GeomIt == Geometries.end())
                {
                    continue;
                }
                const FGeometry& Geom = GeomIt->second;
                if (Geom.VertexData.empty() || Geom.IndexData.empty() || Geom.IndexCount == 0)
                {
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
                    continue;
                }

                // Per-draw entry; this draw's vertices reference it by index.
                const uint32 DrawIndex = uint32(BatchDrawData.size());
                FUiDraw DD;
                DD.MVP      = Draw.MVP;
                DD.ClipRect = Draw.bScissorEnabled
                    ? FVector4(float(Draw.Scissor.Position().x), float(Draw.Scissor.Position().y),
                                float(Draw.Scissor.Position().x + Draw.Scissor.Width()),
                                float(Draw.Scissor.Position().y + Draw.Scissor.Height()))
                    : FVector4(0.0f, 0.0f, FullW, FullH);
                DD.TextureID    = ResourceID;
                DD.SamplerIndex = GRmlUiSamplerIndex;
                DD.Pad0 = 0;
                DD.Pad1 = 0;
                BatchDrawData.push_back(DD);

                const Rml::Vertex* SrcVerts = reinterpret_cast<const Rml::Vertex*>(Geom.VertexData.data());
                const uint32 VtxCount = uint32(Geom.VertexData.size() / sizeof(Rml::Vertex));
                const int*   SrcInds  = reinterpret_cast<const int*>(Geom.IndexData.data());

                const uint32 BaseVertex = uint32(BatchVertices.size());

                // Convert to the GPU vertex: bake translation, tag with the draw index.
                BatchVertices.reserve(BatchVertices.size() + VtxCount);
                for (uint32 v = 0; v < VtxCount; ++v)
                {
                    const Rml::Vertex& Src = SrcVerts[v];
                    FUiVertex V;
                    V.Position[0] = Src.position.x + Draw.Translation.x;
                    V.Position[1] = Src.position.y + Draw.Translation.y;
                    Memory::Memcpy(&V.Colour, &Src.colour, sizeof(uint32));
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
            }

            if (BatchIndices.empty() || BatchDrawData.empty())
            {
                Batch.IndexCount = 0;
                Batch.bValid     = true;
                Batch.LastHash   = Hash;
                Batch.Draws.clear();
                RHI::CmdEndMarker(CL);
                ResetFrameState();
                return;
            }

            const uint64 VBytes = BatchVertices.size() * sizeof(FUiVertex);
            const uint64 IBytes = BatchIndices.size()  * sizeof(uint32);
            EnsureBatchBuffers(Batch, VBytes, IBytes);

            // Stage through the transient ring and copy into the resident buffers. The leading barrier
            // also orders against any still-executing prior frame reading the old contents.
            RHI::FTransientAlloc VBStage = RHI::Core::AllocTransient(VBytes, 16);
            RHI::FTransientAlloc IBStage = RHI::Core::AllocTransient(IBytes, 4);
            Memory::Memcpy(VBStage.Cpu, BatchVertices.data(), VBytes);
            Memory::Memcpy(IBStage.Cpu, BatchIndices.data(),  IBytes);

            RHI::CmdBarrier(CL, RHI::EStageFlags::AllCommands, RHI::EStageFlags::Transfer);
            RHI::CmdMemcpy(CL, Batch.VertexBuffer, VBStage.Gpu, VBytes);
            RHI::CmdMemcpy(CL, Batch.IndexBuffer,  IBStage.Gpu, IBytes);
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::AllCommands);

            Batch.Draws      = BatchDrawData;
            Batch.IndexCount = uint32(BatchIndices.size());
            Batch.LastHash   = Hash;
            Batch.bValid     = true;
        }

        if (Batch.IndexCount == 0 || Batch.Draws.empty() || Batch.VertexBuffer == 0 || Batch.IndexBuffer == 0)
        {
            RHI::CmdEndMarker(CL);
            ResetFrameState();
            return;
        }

        RHI::FPipelineH Pipeline = GetPipelineForFormat(RHI::GetTextureDesc(CurrentTarget).Format);
        if (!RHI::IsValid(Pipeline))
        {
            RHI::CmdEndMarker(CL);
            ResetFrameState();
            return;
        }

        // Only the small per-draw data is transient (read in-shader via device address); the bulk
        // vertex/index data lives in the resident buffers above.
        const RHI::GPUPtr DrawsPtr = RHI::Core::CopyTransientArray(Batch.Draws.data(), Batch.Draws.size());

        const FRmlUiArgs Args { DrawsPtr, Batch.VertexBuffer };
        const RHI::GPUPtr ArgsPtr = RHI::Core::CopyTransient(Args);

        RHI::FRenderAttachment Color;
        Color.Texture = CurrentTarget;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = CurrentSize;

        RHI::CmdBeginRenderPass(CL, Pass);
        RHI::CmdSetDepthStencilState(CL, DepthState);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
        // CW to match the scene's winding; culling is off so it only matters for state leaking into later passes.
        RHI::CmdSetFrontFace(CL, RHI::EFrontFace::CW);
        RHI::CmdSetPipeline(CL, Pipeline);
        RHI::CmdSetViewport(CL, RHI::FRect{ 0, (int)CurrentSize.x, 0, (int)CurrentSize.y });
        RHI::CmdSetScissor(CL, RHI::FRect{ 0, (int)CurrentSize.x, 0, (int)CurrentSize.y });

        RHI::CmdDrawIndexed(CL, Batch.IndexBuffer, 0, ArgsPtr, Batch.IndexCount, 1, 0, 0, 0, RHI::EIndexType::Uint32);

        RHI::CmdEndRenderPass(CL);
        RHI::CmdEndMarker(CL);
        ResetFrameState();
    }

    Rml::CompiledGeometryHandle FRmlUiRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices)
    {
        const size_t VBSize = Vertices.size() * sizeof(Rml::Vertex);
        const size_t IBSize = Indices.size()  * sizeof(int);
        if (VBSize == 0 || IBSize == 0)
        {
            return 0;
        }

        // CPU-side cache only. At EndFrame the referenced geometry is concatenated into the
        // target's resident VB/IB (translation baked in), rebuilt only when the draw list changes.
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
        DrawCalls.push_back(Move(Draw));
    }

    void FRmlUiRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle Geometry)
    {
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

        // Pin the asset for the lifetime of the RmlUi handle so an unload doesn't
        // dangle the bindless slot. Released in ReleaseTexture.
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

    void FRmlUiRenderer::RevalidateBrushes(RHI::FCmdListH CmdList)
    {
        // Mark each brush stale by whether its source path still resolves to the cached material (stays rooted,
        // so a rename-back resumes without a reload). Driven by the asset-registry broadcast, not per frame.
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
                // Source renamed/deleted: clear to transparent and stop rendering
                // so the document breaks instead of showing the old asset.
                const float Transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                RHI::CmdBarrier(CmdList, RHI::EStageFlags::AllCommands, RHI::EStageFlags::Transfer);
                RHI::CmdClearTexture(CmdList, Tex.Managed.Texture, Transparent);
                RHI::CmdBarrier(CmdList, RHI::EStageFlags::Transfer, RHI::EStageFlags::AllCommands);
                Tex.bBrushStale = true;
            }
            else if (bResolves && Tex.bBrushStale)
            {
                // Asset returned at this path (e.g. renamed back): resume next frame.
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

        // Drop brushes whose source asset went away. Event-driven (set on the
        // registry broadcast), so there is no per-frame validation cost.
        if (bBrushRevalidatePending.exchange(false, std::memory_order_acquire))
        {
            RevalidateBrushes(CL);
        }

        if (DrawCalls.empty())
        {
            return;
        }

        // Monotonic wall clock drives animated UI materials (GetTime() in-shader).
        static const auto StartTime = std::chrono::steady_clock::now();
        const float Time = std::chrono::duration<float>(std::chrono::steady_clock::now() - StartTime).count();

        // Only render brushes referenced this frame (brushes are shared across all
        // contexts; this scopes work to the drawing context). De-dup repeats.
        TVector<Rml::TextureHandle> Rendered;

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
                continue;
            }
            const FShaderH VS = Material->GetVertexShader();
            const FShaderH PS = Material->GetPixelShader();
            if (VS == nullptr || PS == nullptr)
            {
                continue;
            }

            RHI::FPipelineH Pipeline = GetBrushPipeline(VS, PS);
            if (!RHI::IsValid(Pipeline))
            {
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
        }

        if (!Rendered.empty())
        {
            // Brush RT writes visible to the UI pass sampling them.
            RHI::CmdBarrier(CL, RHI::EStageFlags::RasterColorOut, RHI::EStageFlags::PixelShader);
        }
    }
}
