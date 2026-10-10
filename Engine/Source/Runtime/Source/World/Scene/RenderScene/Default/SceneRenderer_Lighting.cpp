#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "Platform/Time/PlatformTime.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    static constexpr uint32 GPrefilterSampleCount = 256;

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

    void FDefaultSceneRenderer::ReflectionProbeBakePass(RHI::FCmdListH CL)
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

        const FSceneImage& CaptureCube = NamedImages[(int)ENamedImage::ProbeCaptureCube];
        const FSceneImage& ProbeArray  = NamedImages[(int)ENamedImage::ProbePrefiltered];
        if (!CaptureCube.IsValid() || !ProbeArray.IsValid())
        {
            LOG_WARN("[Probe] Bake SKIPPED: captureCube={} probeArray={} (targets not allocated)",
                     CaptureCube.IsValid() ? 1 : 0, ProbeArray.IsValid() ? 1 : 0);
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Reflection Probe Bake", tracy::Color::SkyBlue3);
        SCENE_GPU_SCOPE(CL, "Reflection Probe Bake");

        FSceneView& View = SceneViews[Bake.BakeViewIndex];

        bCapturingProbe = true;

        const uint32 FaceSize = Bake.BakeFaceSize;

        const bool     bClearToColor = Bake.Captures[Bake.BakingProbe].bClearToColor;
        const FVector3 ClearRGB      = Bake.Captures[Bake.BakingProbe].ClearColor;

        for (int32 Face = 0; Face < 6; ++Face)
        {
            if (Bake.FaceCullViews[Face] == Constants::kIndexNoneU32)
            {
                continue;
            }

            PointAtView(View);
            CurrentCameraEarlyView = Bake.FaceCullViews[Face];

            SetSceneRoot(CL, View,
                RHI::CopyTransient(MakeSecondaryViewGlobals(Bake.FaceGlobals[Face])));

            TerrainCullPass(CL);
            const bool bTerrainCleared = TerrainDepthPrePass(CL, /*bClear*/ true);
            VisBufferPass(CL, CurrentCameraEarlyView, /*bClear*/ !bTerrainCleared);
            ClusterBuildPass(CL);
            LightCullPass(CL);

            if (bClearToColor)
            {
                const FSceneImage& ClearRT = GetNamedImage(ENamedImage::HDR);

                RHI::FRenderAttachment ClearColorAttachment;
                ClearColorAttachment.Texture        = ClearRT.Texture;
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
            VisBufferClassifyPass(CL);
            MaterialGBufferPass(CL);
            DeferredLightingPass(CL);
            TerrainRenderPass(CL);

            const FSceneImage& FaceColor = GetNamedImage(ENamedImage::HDR);

            RHI::FTextureSlice SrcSlice;
            SrcSlice.Mip        = 0;
            SrcSlice.Layer      = 0;
            SrcSlice.LayerCount = 1;
            SrcSlice.Extent     = FUIntVector3(FaceSize, FaceSize, 1);

            RHI::FTextureSlice DstSlice = SrcSlice;
            DstSlice.Layer = (uint32)Face;

            RHI::CmdBarrier(CL,
                RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite,
                RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            RHI::CmdCopyTexture(CL, FaceColor.Texture, SrcSlice, CaptureCube.Texture, DstSlice);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }

        {
            static const FShaderH ComputeShader = FShaderLibrary::Get("PrefilterEnvMap.slang");
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

                    const uint32 MipFaceSize = std::max<uint32>(BaseFaceSize >> Mip, 1u);
                    const uint32 GroupsXY    = RenderUtils::GetGroupCount(MipFaceSize, PrefilterTile);
                    RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
                }

                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                    RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            }
        }

        bCapturingProbe = false;

        PointAtView(SceneViews[0]);
        CurrentCameraEarlyView = 0u;

        CompletedProbeBakes.fetch_or(1u << (uint32)Bake.BakingProbe, std::memory_order_acq_rel);
    }

    void FDefaultSceneRenderer::ClusterBuildPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        const bool bHasTerrain = !Frame.Extracts.TerrainExtracts.empty();
        if (DrawCommands.empty() && !bHasTerrain)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cluster Build Pass", tracy::Color::Pink2);
        
        static const FShaderH ComputeShader = FShaderLibrary::Get("ClusterBuild.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        constexpr uint32 ClusterBuildGroupSize = 64;
        constexpr uint32 ClusterDispatchGroups = (MaxClusters + ClusterBuildGroupSize - 1) / ClusterBuildGroupSize;
        RHI::CmdDispatch(CL, MakeArgs(), ClusterDispatchGroups, 1, 1);

        // LightCull consumes the cluster AABBs next.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::LightCullPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        const bool bHasTerrain = !Frame.Extracts.TerrainExtracts.empty();
        if (DrawCommands.empty() && !bHasTerrain)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Light Cull Pass", tracy::Color::Pink2);

        static const FShaderH ComputeShader = FShaderLibrary::Get("LightCull.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        // One group per cull block of the largest grid a view can have; groups past this view's grid exit at once.
        RHI::CmdDispatch(CL, MakeArgs(), MaxClusterCullBlocks, 1, 1);

        // Cluster light lists feed the lit pixel shaders.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    static constexpr uint32 GMaterialMaxSlots = MATERIAL_MAX_SLOTS;

    // One allocation means one barrier between the three classify dispatches instead of three.
    // Indirect-argument offsets need only 4-byte alignment; 16 costs nothing and keeps the triples tidy.
    static uint32 AlignClassifyRegion(uint32 Offset) { return (Offset + 15u) & ~15u; }

    bool FDefaultSceneRenderer::BuildDeferredMaterialBinning(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        MaterialClassifyLayout = FMaterialClassifyLayout{};

        const FUIntVector2 Extent = GetNamedImage(ENamedImage::HDR).GetExtent();
        if (Extent.x == 0u || Extent.y == 0u)
        {
            return false;
        }

        // The pixel list packs 16 bits per axis, so refuse rather than alias two pixels onto one.
        if (Extent.x > 0xFFFFu || Extent.y > 0xFFFFu)
        {
            static bool bWarnedExtent = false;
            if (!bWarnedExtent)
            {
                bWarnedExtent = true;
                LOG_WARN("Render extent {}x{} exceeds the 16-bit pixel-list packing; deferred shading is disabled for this view.",
                    Extent.x, Extent.y);
            }
            return false;
        }

        const auto& DeferredMaterials = Frame.Geometry.DeferredMaterials;

        BinnedDeferredSlotShaders.clear();
        BinnedDeferredSlotKinds.clear();
        BinnedDeferredSlotLookup.clear();
        BinnedShadingModelMask = 0u;

        const RHI::FMaterialManager& MaterialManager = Render().GetMaterialManager();
        uint32 MaxMaterialIndex = 0u;
        for (const auto& M : DeferredMaterials)
        {
            if (M.DeferredShader)
            {
                MaxMaterialIndex = Math::Max(MaxMaterialIndex, M.MaterialIndex);

                const uint32 ShadingModel = (MaterialManager.GetSlotFlags(M.MaterialIndex) >> kMaterialShadingModelShift) & kMaterialShadingModelMask;
                BinnedShadingModelMask |= 1u << ShadingModel;
            }
        }

        // Overflow shades through the default material rather than not at all. It reads the overflowed
        // material's own uniforms, so the surface is wrong but lit, which reads as a bug instead of a hole.
        CMaterial* DefaultMaterial = CMaterial::GetDefaultMaterial();
        const FShaderH FallbackShader = IsValid(DefaultMaterial) ? DefaultMaterial->GetStage(EMaterialShaderStage::Deferred) : FShaderH{};

        // One slot held back for the fallback, so a frame that fills the table can still claim it.
        const uint32 SlotBudget = FallbackShader ? (GMaterialMaxSlots - 1u) : GMaterialMaxSlots;

        BinnedDeferredSlotByMaterial.assign((size_t)MaxMaterialIndex + 1u, Constants::kIndexNoneU32);
        for (const auto& M : DeferredMaterials)
        {
            if (!M.DeferredShader)
            {
                continue;
            }

            // Looked up rather than scanned; a scan cost visible materials times distinct shaders.
            uint32 Slot = Constants::kIndexNoneU32;
            if (auto It = BinnedDeferredSlotLookup.find(M.DeferredShader.Handle); It != BinnedDeferredSlotLookup.end())
            {
                Slot = It->second;
            }

            if (Slot == Constants::kIndexNoneU32)
            {
                const bool bFull = (uint32)BinnedDeferredSlotShaders.size() >= SlotBudget;
                if (bFull)
                {
                    static bool bWarnedSlotCap = false;
                    if (!bWarnedSlotCap)
                    {
                        bWarnedSlotCap = true;
                        LOG_WARN("More than {} distinct deferred material shaders are visible; the excess {}.",
                            SlotBudget, FallbackShader ? "shades as the default material" : "will not shade");
                    }
                    #if USING(WITH_EDITOR)
                    // A log line scrolls away, and the symptom is surfaces quietly wearing the wrong material.
                    static double LastSlotCapToast = -1.0e9;
                    if (const double Now = PlatformTime::Seconds(); Now - LastSlotCapToast > 10.0)
                    {
                        LastSlotCapToast = Now;
                        ImGuiX::Notifications::NotifyWarning("Over {} distinct material shaders are on screen; the excess draws as the default material.", SlotBudget);
                    }
                    #endif

                    if (!FallbackShader)
                    {
                        continue;
                    }
                }

                const FShaderH BinShader = bFull ? FallbackShader : M.DeferredShader;

                // The fallback may already own a bin, either from a visible default material or an
                // earlier overflow, in which case it costs no extra slot.
                if (auto Found = BinnedDeferredSlotLookup.find(BinShader.Handle); Found != BinnedDeferredSlotLookup.end())
                {
                    Slot = Found->second;
                }
                else
                {
                    Slot = (uint32)BinnedDeferredSlotShaders.size();
                    BinnedDeferredSlotShaders.push_back(BinShader);
                    BinnedDeferredSlotKinds.push_back(0u);
                    BinnedDeferredSlotLookup.emplace(BinShader.Handle, Slot);
                }
            }

            BinnedDeferredSlotByMaterial[M.MaterialIndex] = Slot;
            BinnedDeferredSlotKinds[Slot] |= M.InstanceKinds;
        }

        const uint32 NumSlots = (uint32)BinnedDeferredSlotShaders.size();
        if (NumSlots == 0u)
        {
            return false;
        }

        const uint32 TilesX   = RenderUtils::GetGroupCount(Extent.x, (uint32)MATERIAL_CLASSIFY_TILE);
        const uint32 TilesY   = RenderUtils::GetGroupCount(Extent.y, (uint32)MATERIAL_CLASSIFY_TILE);
        const uint64 NumTiles = (uint64)TilesX * (uint64)TilesY;

        // A tile holds at most one slot per pixel, so this bound is exact and never overflows, however many shaders are visible.
        constexpr uint64 MaxSlotsPerTile = (uint64)MATERIAL_CLASSIFY_TILE * MATERIAL_CLASSIFY_TILE;
        const uint64 PairBound = NumTiles * Math::Min<uint64>(NumSlots, MaxSlotsPerTile);
        if (NumTiles > (1ull << (32u - MATERIAL_PAIR_SLOT_BITS)))
        {
            return false;
        }
        // Sorted runs, each tile's inline records, the pixels of thin pairs, then the pairs past a tile's inline records.
        const uint64 TileSlotEntries = NumTiles * MATERIAL_TILE_RECORD;
        const uint64 PixelBound      = NumTiles * Math::Min<uint64>(MaxSlotsPerTile, Math::Min<uint64>(NumSlots, MATERIAL_TILE_SLOTS) * (MATERIAL_SPARSE_PIXELS - 1u));
        const uint64 OverflowBound   = NumTiles * (Math::Min<uint64>(NumSlots, MaxSlotsPerTile) - Math::Min<uint64>(NumSlots, MATERIAL_TILE_SLOTS));
        const uint64 PairListSize    = (PairBound + TileSlotEntries + PixelBound + Math::Max<uint64>(OverflowBound, 1u)) * sizeof(uint32);

        // Built into a local so a bail-out below leaves the member zeroed rather than half-filled.
        FMaterialClassifyLayout Layout;
        Layout.NumSlots = NumSlots;

        uint32 Cursor = 0u;
        Layout.OverflowCountOffset = Cursor; Cursor += (uint32)sizeof(uint32);

        // Each per-slot region holds the dense half, then the sparse half at NumSlots.
        const uint32 RunEntries = NumSlots * 2u;

        Cursor = AlignClassifyRegion(Cursor);
        Layout.CountsOffset = Cursor; Cursor += RunEntries * (uint32)sizeof(uint32);

        Cursor = AlignClassifyRegion(Cursor);
        Layout.OffsetsOffset = Cursor; Cursor += RunEntries * (uint32)sizeof(uint32);

        Cursor = AlignClassifyRegion(Cursor);
        Layout.CursorsOffset = Cursor; Cursor += RunEntries * (uint32)sizeof(uint32);

        Cursor = AlignClassifyRegion(Cursor);
        Layout.MaterialArgsOffset = Cursor;
        Cursor += RunEntries * (uint32)sizeof(RHI::FDispatchIndirectArguments);
        Layout.ScatterArgsOffset = Cursor;
        Cursor += (uint32)sizeof(RHI::FDispatchIndirectArguments);

        Layout.BlockSize = AlignClassifyRegion(Cursor);

        ReserveBuffer(CL, MaterialClassifyRing[CurrentFrameSlot], Layout.BlockSize);
        ReserveBuffer(CL, MaterialPairList, PairListSize);

        if (!GetMaterialClassify() || !GetMaterialPairList())
        {
            return false;
        }

        Layout.ScreenW = Extent.x;
        Layout.ScreenH = Extent.y;
        Layout.TilesX  = TilesX;
        Layout.NumTiles = (uint32)NumTiles;
        Layout.PairCapacity = (uint32)PairBound;
        Layout.TileSlotsOffset = PairBound * sizeof(uint32);
        Layout.PixelListOffset = (PairBound + TileSlotEntries) * sizeof(uint32);
        Layout.PixelCapacity   = (uint32)PixelBound;
        Layout.OverflowOffset  = (PairBound + TileSlotEntries + PixelBound) * sizeof(uint32);
        // From the allocation, not from what was asked for, since the classify bounds overflow writes on this.
        Layout.OverflowCapacity = (uint32)Math::Min<uint64>((GetMaterialPairList().Size - Layout.OverflowOffset) / sizeof(uint32), 0xFFFFFFFFull);

        MaterialClassifyLayout = Layout;
        return true;
    }

    // Counts, prefix-sums and scatters, and emits the indirect args so the CPU never learns them.
    void FDefaultSceneRenderer::VisBufferClassifyPass(RHI::FCmdListH CL)
    {
        MaterialClassifyLayout = FMaterialClassifyLayout{};

        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("VisBuffer Classify", tracy::Color::Orange3);

        static const FShaderH ClassifyCS = FShaderLibrary::Get("VisBufferMaterialClassify.slang");
        static const FShaderH ArgsCS     = FShaderLibrary::Get("VisBufferMaterialArgs.slang");
        static const FShaderH ScatterCS  = FShaderLibrary::Get("VisBufferMaterialScatter.slang");
        if (!ClassifyCS || !ArgsCS || !ScatterCS)
        {
            return;
        }

        if (!BuildDeferredMaterialBinning(CL))
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "VisBuffer Classify");

        const FMaterialClassifyLayout Layout = MaterialClassifyLayout;

        const FSceneImage& VisRT   = GetNamedImage(ENamedImage::VisBuffer);
        const FSceneImage& SlotRT  = GetNamedImage(ENamedImage::MaterialSlot);
        const RHI::GPUPtr  Base    = GetMaterialClassify().Gpu;
        const int32        SlotUAV = SlotRT.GetMipUAVIndex(0);
        if (SlotUAV < 0)
        {
            MaterialClassifyLayout = FMaterialClassifyLayout{};
            return;
        }

        // MaterialIndex -> dense slot; uploaded to the transient ring and read by device address.
        const RHI::FGPURange SlotByMaterialRange =
            RHI::CopyTransientArray(BinnedDeferredSlotByMaterial.data(), BinnedDeferredSlotByMaterial.size());

        // Every view shares this ring slot, so a capture view must not clear it under the previous view's reads.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments, RHI::EAccessFlags::None,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::None);

        // Only the counters are cleared, and they lead the block; the args pass rewrites the dispatch triples.
        RHI::CmdMemset(CL, { Base + Layout.OverflowCountOffset, Layout.CountsOffset + sizeof(uint32) * Layout.NumSlots * 2u }, 0u);
        Barriers::TransferToCompute(CL);

        const RHI::TGPUSpan<uint32> CountsSpan = RHI::TGPUSpan<uint32>::FromAddress(Base + Layout.CountsOffset, Layout.NumSlots * 2u);
        const RHI::TGPUSpan<uint32> OverflowCountSpan = RHI::TGPUSpan<uint32>::FromAddress(Base + Layout.OverflowCountOffset, 1u);
        const RHI::FGPUAllocation   PairAlloc  = GetMaterialPairList();
        const RHI::TGPUSpan<uint32> SortedSpan    = RHI::TGPUSpan<uint32>::FromAddress(PairAlloc.Gpu, Layout.PairCapacity);
        const RHI::TGPUSpan<uint32> TileSlotsSpan = RHI::TGPUSpan<uint32>::FromAddress(PairAlloc.Gpu + Layout.TileSlotsOffset, Layout.NumTiles * MATERIAL_TILE_RECORD);
        const RHI::TGPUSpan<uint32> PixelListSpan = RHI::TGPUSpan<uint32>::FromAddress(PairAlloc.Gpu + Layout.PixelListOffset, Layout.PixelCapacity);
        const RHI::TGPUSpan<uint32> OverflowSpan  = RHI::TGPUSpan<uint32>::FromAddress(PairAlloc.Gpu + Layout.OverflowOffset, Layout.OverflowCapacity);

        struct FMaterialClassifyPC
        {
            RHI::TGPUSpan<uint32> Counts;
            RHI::TGPUSpan<uint32> SlotByMaterial;
            RHI::TGPUSpan<uint32> OverflowCount;
            RHI::TGPUSpan<uint32> TileSlots;
            RHI::TGPUSpan<uint32> Overflow;
            uint32      VisBufferIndex;
            uint32      SlotImageUAV;
            uint32      ScreenW;
            uint32      ScreenH;
            uint32      DrawListCount;
            uint32      _Pad0;
            uint32      _Pad1;
            uint32      _Pad2;
        } ClassifyPC = {};
        static_assert(sizeof(FMaterialClassifyPC) == 112, "FMaterialClassifyPC must match VisBufferMaterialClassify.slang FMaterialClassifyArgs.");
        ClassifyPC.Counts         = CountsSpan;
        ClassifyPC.OverflowCount  = OverflowCountSpan;
        ClassifyPC.TileSlots      = TileSlotsSpan;
        ClassifyPC.Overflow       = OverflowSpan;
        ClassifyPC.SlotByMaterial = SlotByMaterialRange;
        ClassifyPC.VisBufferIndex = (uint32)VisRT.GetResourceID();
        ClassifyPC.SlotImageUAV   = (uint32)SlotUAV;
        ClassifyPC.ScreenW        = Layout.ScreenW;
        ClassifyPC.ScreenH        = Layout.ScreenH;
        ClassifyPC.DrawListCount  = DrawListCapacity;

        DispatchCompute(CL, ClassifyCS, ClassifyPC,
            RenderUtils::GetGroupCount(Layout.ScreenW, (uint32)MATERIAL_CLASSIFY_TILE),
            RenderUtils::GetGroupCount(Layout.ScreenH, (uint32)MATERIAL_CLASSIFY_TILE), 1u);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        const RHI::TGPUSpan<uint32> OffsetsSpan = RHI::TGPUSpan<uint32>::FromAddress(Base + Layout.OffsetsOffset, Layout.NumSlots * 2u);
        const RHI::TGPUSpan<uint32> CursorsSpan = RHI::TGPUSpan<uint32>::FromAddress(Base + Layout.CursorsOffset, Layout.NumSlots * 2u);

        struct FMaterialArgsPC
        {
            RHI::TGPUSpan<uint32> Counts;
            RHI::TGPUSpan<uint32> Offsets;
            RHI::TGPUSpan<uint32> Cursors;
            RHI::TGPUSpan<uint32> Args;
            RHI::TGPUSpan<uint32> OverflowCount;
            uint32                OverflowCapacity;
            uint32                NumTiles;
            uint32                _Pad1;
            uint32                _Pad2;
        } ArgsPC = {};
        static_assert(sizeof(FMaterialArgsPC) == 96, "FMaterialArgsPC must match VisBufferMaterialArgs.slang FMaterialArgsArgs.");
        ArgsPC.OverflowCount    = OverflowCountSpan;
        ArgsPC.OverflowCapacity = Layout.OverflowCapacity;
        ArgsPC.NumTiles         = Layout.NumTiles;
        ArgsPC.Counts  = CountsSpan;
        ArgsPC.Offsets = OffsetsSpan;
        ArgsPC.Cursors = CursorsSpan;
        ArgsPC.Args    = RHI::TGPUSpan<uint32>::FromAddress(Base + Layout.MaterialArgsOffset, Layout.NumSlots * 6u + 3u);

        // One group scans every slot, which MATERIAL_MAX_SLOTS keeps within two slots per thread.
        DispatchCompute(CL, ArgsCS, ArgsPC, 1u, 1u, 1u);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);

        struct FMaterialScatterPC
        {
            RHI::TGPUSpan<uint32> Offsets;
            RHI::TGPUSpan<uint32> Cursors;
            RHI::TGPUSpan<uint32> PairList;
            RHI::TGPUSpan<uint32> TileSlots;
            RHI::TGPUSpan<uint32> Overflow;
            RHI::TGPUSpan<uint32> OverflowCount;
            RHI::TGPUSpan<uint32> PixelList;
            uint32      TilesX;
            uint32      NumTiles;
            uint32      NumSlots;
            uint32      _Pad0;
        } ScatterPC = {};
        static_assert(sizeof(FMaterialScatterPC) == 128, "FMaterialScatterPC must match VisBufferMaterialScatter.slang FMaterialScatterArgs.");
        ScatterPC.Offsets        = OffsetsSpan;
        ScatterPC.Cursors        = CursorsSpan;
        ScatterPC.PairList       = SortedSpan;
        ScatterPC.TileSlots      = TileSlotsSpan;
        ScatterPC.Overflow       = OverflowSpan;
        ScatterPC.OverflowCount  = OverflowCountSpan;
        ScatterPC.PixelList      = PixelListSpan;
        ScatterPC.TilesX         = Layout.TilesX;
        ScatterPC.NumTiles       = Layout.NumTiles;
        ScatterPC.NumSlots       = Layout.NumSlots;

        DispatchComputeIndirect(CL, ScatterCS, ScatterPC, GetMaterialClassify().Skip(Layout.ScatterArgsOffset));

        // The pair list and stored slots feed the material dispatches; the argument triples feed the indirect fetch.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);
    }

    // Compute, not a rasterized quad, since a pixel shader runs the graph 4x on a 1-pixel tri.
    void FDefaultSceneRenderer::MaterialGBufferPass(RHI::FCmdListH CL)
    {
        // Set by VisBufferClassifyPass, which runs immediately before this and zeroes it on any bail-out.
        const FMaterialClassifyLayout Layout = MaterialClassifyLayout;
        if (Layout.NumSlots == 0u)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Material GBuffer Pass", tracy::Color::Red);
        SCENE_GPU_SCOPE(CL, "Material GBuffer");

        const FSceneImage& VisRT = GetNamedImage(ENamedImage::VisBuffer);

        const RHI::FGPUAllocation Classify = GetMaterialClassify();
        const RHI::GPUPtr         Base     = Classify.Gpu;

        struct FDeferredMaterialPC
        {
            uint32      VisBufferIndex;
            uint32      FeedbackPhase;
            uint32      NumSlots;
            uint32      SlotImageIndex;
            uint32      DrawListCount;
            uint32      SlotIndex;
            uint32      ScreenW;
            uint32      ScreenH;
            uint32      GBufferAUAV;
            uint32      GBufferBUAV;
            uint32      GBufferCUAV;
            uint32      GBufferDUAV;
            uint32      VelocityUAV;
            uint32      _PadVelocity;
            RHI::TGPUSpan<uint32> PairList;
            RHI::TGPUSpan<uint32> Counts;
            RHI::TGPUSpan<uint32> Offsets;
            RHI::TGPUSpan<uint32> PixelList;
        } PC = {};
        static_assert(sizeof(FDeferredMaterialPC) == 120, "FDeferredMaterialPC must match DeferredMaterial.slang FDeferredMaterialArgs.");

        PC.VisBufferIndex = (uint32)VisRT.GetResourceID();
        PC.FeedbackPhase  = (uint32)(StreamingFeedbackTick % STREAMING_FEEDBACK_WINDOW);
        PC.DrawListCount = DrawListCapacity;
        PC.ScreenW       = Layout.ScreenW;
        PC.ScreenH       = Layout.ScreenH;

        const int32 UAVA = GetNamedImage(ENamedImage::GBufferA).GetMipUAVIndex(0);
        const int32 UAVB = GetNamedImage(ENamedImage::GBufferB).GetMipUAVIndex(0);
        const int32 UAVC = GetNamedImage(ENamedImage::GBufferC).GetMipUAVIndex(0);
        const int32 UAVD = GetNamedImage(ENamedImage::GBufferD).GetMipUAVIndex(0);
        if (UAVA < 0 || UAVB < 0 || UAVC < 0 || UAVD < 0)
        {
            LOG_ERROR("Deferred material pass: a GBuffer target has no storage heap slot; skipping the pass.");
            return;
        }
        PC.GBufferAUAV = (uint32)UAVA;
        PC.GBufferBUAV = (uint32)UAVB;
        PC.GBufferCUAV = (uint32)UAVC;
        PC.GBufferDUAV = (uint32)UAVD;

        PC.SlotImageIndex = (uint32)GetNamedImage(ENamedImage::MaterialSlot).GetResourceID();
        PC.PairList       = { GetMaterialPairList(), Layout.PairCapacity };
        PC.NumSlots       = Layout.NumSlots;
        PC.Counts         = RHI::TGPUSpan<uint32>::FromAddress(Base + Layout.CountsOffset, Layout.NumSlots * 2u);
        PC.Offsets        = RHI::TGPUSpan<uint32>::FromAddress(Base + Layout.OffsetsOffset, Layout.NumSlots * 2u);
        PC.PixelList      = RHI::TGPUSpan<uint32>::FromAddress(GetMaterialPairList().Gpu + Layout.PixelListOffset, Layout.PixelCapacity);

        // Invalid disables the write, which leaves the camera-only base the fullscreen pass laid down.
        PC.VelocityUAV = RHI::kInvalidHeapSlot;
        if (IsVelocityWanted())
        {
            const int32 UAVVel = GetNamedImage(ENamedImage::Velocity).GetMipUAVIndex(0);
            if (UAVVel >= 0)
            {
                PC.VelocityUAV = (uint32)UAVVel;
            }
        }

        // One allocation written in place, since the blocks differ only in SlotIndex.
        const RHI::FTransientAlloc ArgsAlloc =
            RHI::AllocTransient(sizeof(FDeferredMaterialPC) * Layout.NumSlots);
        if (ArgsAlloc.Cpu == nullptr)
        {
            LOG_ERROR("Deferred material pass: could not stage {} argument blocks; skipping the pass.",
                Layout.NumSlots);
            return;
        }

        FDeferredMaterialPC* ArgsCpu = (FDeferredMaterialPC*)ArgsAlloc.Cpu;
        for (uint32 Slot = 0; Slot < Layout.NumSlots; ++Slot)
        {
            ArgsCpu[Slot]           = PC;
            ArgsCpu[Slot].SlotIndex = Slot;
        }

        const uint64 bVelocity = PC.VelocityUAV != RHI::kInvalidHeapSlot ? 1u : 0u;

        for (uint32 Slot = 0; Slot < Layout.NumSlots; ++Slot)
        {
            // Specialized per slot on velocity and vertex kinds, since each unused path costs about a third of the registers.
            const uint8  Kinds   = Slot < (uint32)BinnedDeferredSlotKinds.size() ? BinnedDeferredSlotKinds[Slot] : 3u;
            const uint64 Skinned = Kinds == 1u ? 0u : (Kinds == 2u ? 1u : 2u);
            const RHI::FSpecializationConstant DeferredConsts[] =
            {
                RHI::FSpecializationConstant{ .ConstantID = 9u, .AsInt = bVelocity, .Type = RHI::ESpecializationConstantType::UInt32 },
                RHI::FSpecializationConstant{ .ConstantID = 5u, .AsInt = Skinned,   .Type = RHI::ESpecializationConstantType::UInt32 },
            };
            const TSpan<const RHI::FSpecializationConstant> DeferredSpec(DeferredConsts, 2);

            // Every pixel in the bin has to be shaded, so a slot still compiling shades as the default material.
            RHI::FPipelineH SlotPipeline = FindComputePipeline(BinnedDeferredSlotShaders[Slot], DeferredSpec);
            if (!SlotPipeline)
            {
                const FShaderH DefaultDeferred = DefaultMaterialStage(EMaterialShaderStage::Deferred);
                SlotPipeline = GetOrCreateComputePipeline(DefaultDeferred != nullptr ? DefaultDeferred : BinnedDeferredSlotShaders[Slot], DeferredSpec);
            }
            RHI::CmdSetPipeline(CL, SlotPipeline);
            RHI::CmdDispatchIndirect(CL, ArgsAlloc.Gpu + Slot * sizeof(FDeferredMaterialPC), Classify.Skip(Layout.MaterialArgsOffset + Slot * (uint32)sizeof(RHI::FDispatchIndirectArguments)));
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    namespace
    {
        // Must match FLightFunctionArgs in LightFunctionPixelPass.slang.
        struct FLightFunctionArgs
        {
            uint32   MaterialIndex;
            uint32   LightFlags;
            float    _Pad[2];
            FVector4 Position;
            FVector4 Forward;
            FVector4 Right;
            FVector4 Up;
        };
        static_assert(sizeof(FLightFunctionArgs) == 80, "FLightFunctionArgs must match LightFunctionPixelPass.slang");
    }

    void FDefaultSceneRenderer::LightFunctionPass(RHI::FCmdListH CL)
    {
        const auto& Draws = RenderFrame->Lighting.LightFunctionDraws;
        if (Draws.empty() || !LightFunctionAtlas)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Light Function Pass", tracy::Color::Gold);

        // Cleared to white, so a tile whose pipeline is still compiling leaves its light unmasked.
        RHI::FRenderAttachment Color;
        Color.Texture = LightFunctionAtlas.Texture;
        Color.LoadOp  = RHI::ELoadOp::Clear;
        Color.StoreOp = RHI::EStoreOp::Store;
        Color.Color[0] = Color.Color[1] = Color.Color[2] = Color.Color[3] = 1.0f;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = LightFunctionAtlas.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        for (const FFrameData::FLighting::FLightFunctionDraw& Draw : Draws)
        {
            FGraphicsPipelineKey Key;
            Key.VS = Draw.Shaders.VertexShader;
            Key.PS = Draw.Shaders.PixelShader;
            Key.ColorTargets.push_back({ LightFunctionAtlas.Desc.Format, {} });
            const RHI::FPipelineH Pipeline = FindPipeline(Key);
            if (!Pipeline)
            {
                continue;
            }

            const int32 TileX = (int32)((Draw.Slot % LIGHT_FUNCTION_ATLAS_TILES) * LIGHT_FUNCTION_TILE_SIZE);
            const int32 TileY = (int32)((Draw.Slot / LIGHT_FUNCTION_ATLAS_TILES) * LIGHT_FUNCTION_TILE_SIZE);
            const RHI::FRect TileRect{ TileX, TileX + (int32)LIGHT_FUNCTION_TILE_SIZE, TileY, TileY + (int32)LIGHT_FUNCTION_TILE_SIZE };
            RHI::CmdSetViewport(CL, TileRect);
            RHI::CmdSetScissor(CL, TileRect);
            RHI::CmdSetPipeline(CL, Pipeline);

            const FLightFunctionRequest& Request = Draw.Request;
            FLightFunctionArgs Args = {};
            Args.MaterialIndex = Draw.MaterialIndex;
            Args.LightFlags    = (uint32)Request.Type;
            Args.Position      = FVector4(Request.Position, 1.0f);
            Args.Forward       = FVector4(Request.Forward, 0.0f);
            Args.Right         = FVector4(Request.Right, Request.ProjectionScale);
            Args.Up            = FVector4(Request.Up, 0.0f);
            RHI::CmdDraw(CL, MakeArgs(Args), 3, 1, 0, 0);
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    // Background, terrain and undeferred materials were never classified, so they keep the env pass.
    void FDefaultSceneRenderer::DeferredLightingPass(RHI::FCmdListH CL)
    {
        const FMaterialClassifyLayout Layout = MaterialClassifyLayout;
        if (Layout.NumSlots == 0u)
        {
            return;
        }

        static const FShaderH LightingCS = FShaderLibrary::Get("DeferredLighting.slang");
        if (!LightingCS)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Deferred Lighting Pass", tracy::Color::Gold);
        SCENE_GPU_SCOPE(CL, "Deferred Lighting");

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);
        const int32 HDRUAV = HDR.GetMipUAVIndex(0);
        if (HDRUAV < 0)
        {
            LOG_ERROR("Deferred lighting: the HDR target has no storage heap slot; skipping the pass.");
            return;
        }

        struct FDeferredLightingPC
        {
            uint32      GBufferAIndex;
            uint32      GBufferBIndex;
            uint32      GBufferCIndex;
            uint32      GBufferDIndex;
            uint32      DepthIndex;
            uint32      HDRUAV;
            uint32      ScreenW;
            uint32      ScreenH;
            uint32      SlotImageIndex;
            uint32      _Pad0;
            uint32      _Pad1;
            uint32      _Pad2;
        } PC = {};
        static_assert(sizeof(FDeferredLightingPC) == 48, "FDeferredLightingPC must match DeferredLighting.slang FDeferredLightingArgs.");
        PC.GBufferAIndex  = (uint32)GetNamedImage(ENamedImage::GBufferA).GetResourceID();
        PC.GBufferBIndex  = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        PC.GBufferCIndex  = (uint32)GetNamedImage(ENamedImage::GBufferC).GetResourceID();
        PC.GBufferDIndex  = (uint32)GetNamedImage(ENamedImage::GBufferD).GetResourceID();
        PC.DepthIndex     = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.HDRUAV         = (uint32)HDRUAV;
        PC.ScreenW        = Layout.ScreenW;
        PC.ScreenH        = Layout.ScreenH;
        PC.SlotImageIndex = (uint32)GetNamedImage(ENamedImage::MaterialSlot).GetResourceID();

        // Frame-uniform features the light loop would otherwise carry registers for on every pixel.
        bool bLocalShadows = false;
        bool bLocalContact = false;
        for (uint32 Index = 0; Index < NumLiveLights && Index < (uint32)RenderFrame->Lighting.Lights.size(); ++Index)
        {
            const FLight& Light = RenderFrame->Lighting.Lights[Index];
            if (EnumHasAnyFlags(Light.Flags, ELightFlags::Directional))
            {
                continue;
            }
            bLocalShadows |= Light.ShadowDataIndex != Constants::kIndexNone;
            bLocalContact |= EnumHasAnyFlags(Light.Flags, ELightFlags::ContactShadow);
        }

        auto HasShadingModel = [this](EMaterialShadingModel Model) -> uint32
        {
            return (BinnedShadingModelMask >> (uint32)Model) & 1u;
        };

        const RHI::FSpecializationConstant LightingConsts[] =
        {
            RHI::FSpecializationConstant{ .ConstantID = 10u, .AsInt = bLocalShadows ? 1u : 0u,     .Type = RHI::ESpecializationConstantType::UInt32 },
            RHI::FSpecializationConstant{ .ConstantID = 11u, .AsInt = bLocalContact ? 1u : 0u,     .Type = RHI::ESpecializationConstantType::UInt32 },
            RHI::FSpecializationConstant{ .ConstantID = 12u, .AsInt = NumActiveProbes > 0 ? 1u : 0u, .Type = RHI::ESpecializationConstantType::UInt32 },
            RHI::FSpecializationConstant{ .ConstantID = 18u, .AsInt = HasShadingModel(EMaterialShadingModel::Clearcoat), .Type = RHI::ESpecializationConstantType::UInt32 },
            RHI::FSpecializationConstant{ .ConstantID = 19u, .AsInt = HasShadingModel(EMaterialShadingModel::Foliage), .Type = RHI::ESpecializationConstantType::UInt32 },
        };
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(LightingCS, TSpan<const RHI::FSpecializationConstant>(LightingConsts, 5)));
        // Screen tiles rather than the material-sorted pixel list, so a wave shares its clusters, shadow texels and GBuffer lines.
        RHI::CmdDispatch(CL, MakeArgs(PC), RenderUtils::GetGroupCount(Layout.ScreenW, (uint32)MATERIAL_CLASSIFY_TILE),
                         RenderUtils::GetGroupCount(Layout.ScreenH, (uint32)MATERIAL_CLASSIFY_TILE), 1u);

        // The lit HDR target is drawn into by the forward passes, sampled, and read by the post chain.
        RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                        RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorRead | RHI::EAccessFlags::ColorWrite);
    }
    
    bool FDefaultSceneRenderer::GTAOPass(RHI::FCmdListH CL, EGTAOStage Stage)
    {
        FFrameData& Frame = *RenderFrame;
        if (Frame.Geometry.DrawCommands.empty() || !IsGTAOEnabled())
        {
            return false;
        }

        LUMINA_PROFILE_SECTION_COLORED("GTAO Pass", tracy::Color::Red);

        static const FShaderH PrefilterCS = FShaderLibrary::Get("GTAOPrefilterDepth.slang");
        static const FShaderH MainCS      = FShaderLibrary::Get("GTAOMain.slang");
        static const FShaderH DenoiseCS   = FShaderLibrary::Get("GTAODenoise.slang");
        static const FShaderH UpsampleCS  = FShaderLibrary::Get("GTAOUpsample.slang");
        if (!PrefilterCS || !MainCS || !DenoiseCS || !UpsampleCS)
        {
            return false;
        }

        const FSceneImage& Depth        = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& WorkingDepth = GetNamedImage(ENamedImage::GTAOWorkingDepth);
        const FSceneImage& Edges        = GetNamedImage(ENamedImage::GTAOEdges);
        const FSceneImage& TermA        = GetNamedImage(ENamedImage::GTAO);
        const FSceneImage& TermB        = GetNamedImage(ENamedImage::GTAODenoise);
        const FSceneImage& Output       = GetNamedImage(ENamedImage::GTAOBlur);

        const int32 DepthSlot = Depth.GetResourceID();
        if (DepthSlot < 0 || !WorkingDepth.IsValid() || WorkingDepth.GetNumMips() < GTAODepthMipLevels)
        {
            LOG_ERROR("GTAO working depth pyramid is missing or too shallow; skipping the pass.");
            return false;
        }

        // The trace runs at the stage targets' size, half the view below ultra quality, and upsamples into Output.
        const uint32 Width       = TermA.GetSizeX();
        const uint32 Height      = TermA.GetSizeY();
        const bool   bHalfTrace  = Width != Output.GetSizeX() || Height != Output.GetSizeY();

        float Radius                   = 0.5f;
        float Intensity                = 1.0f;
        float FinalValuePower          = 2.2f;
        float RadiusMultiplier         = 1.457f;
        float FalloffRange             = 0.615f;
        float SampleDistributionPower  = 2.0f;
        float ThinOccluderCompensation = 0.0f;
        float DepthMipSamplingOffset   = 3.3f;
        int32 QualityLevel             = 3;
        int32 DenoisePasses            = 1;

        if (const CRendererSettings* Settings = GetDefault<CRendererSettings>())
        {
            Radius                   = Settings->GTAORadius;
            Intensity                = Settings->GTAOIntensity;
            FinalValuePower          = Settings->GTAOPower;
            RadiusMultiplier         = Settings->GTAORadiusMultiplier;
            FalloffRange             = Settings->GTAOFalloffRange;
            SampleDistributionPower  = Settings->GTAOSampleDistributionPower;
            ThinOccluderCompensation = Settings->GTAOThinOccluderCompensation;
            DepthMipSamplingOffset   = Settings->GTAODepthMipSamplingOffset;
            QualityLevel             = Math::Clamp(Settings->GTAOQualityLevel, 0, 3);
            DenoisePasses            = Math::Clamp(Settings->GTAODenoisePasses, 0, 3);
        }

        // Every heuristic in the reference is tuned against this pre-multiplied radius, never the raw one.
        const float EffectRadius = Radius * RadiusMultiplier;

        if (Stage == EGTAOStage::Prefilter)
        {
            SCENE_GPU_SCOPE(CL, "GTAO Prefilter Depth");

            struct FPrefilterConstants
            {
                uint32 ViewportSize[2];
                uint32 SrcDepthIndex;
                uint32 MipUAV[GTAODepthMipLevels];
                float  EffectRadius;
                float  EffectFalloffRange;
                uint32 SrcScale;
            } PC = {};

            PC.SrcScale           = bHalfTrace ? 2u : 1u;
            PC.ViewportSize[0]    = Width;
            PC.ViewportSize[1]    = Height;
            PC.SrcDepthIndex      = (uint32)DepthSlot;
            PC.EffectRadius       = EffectRadius;
            PC.EffectFalloffRange = FalloffRange;

            for (uint32 Mip = 0; Mip < GTAODepthMipLevels; ++Mip)
            {
                const int32 Slot = WorkingDepth.GetMipUAVIndex(Mip);
                if (Slot < 0)
                {
                    LOG_ERROR("GTAO working depth mip {} has no storage heap slot; skipping the pass.", Mip);
                    return false;
                }
                PC.MipUAV[Mip] = (uint32)Slot;
            }

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(PrefilterCS));

            // One group covers a 16x16 tile, which reduces to exactly one texel of the last mip.
            constexpr uint32 PrefilterTile = 16u;
            RHI::CmdDispatch(CL, MakeArgs(PC),
                RenderUtils::GetGroupCount(Width, PrefilterTile),
                RenderUtils::GetGroupCount(Height, PrefilterTile), 1);

            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            return true;
        }

        {
            SCENE_GPU_SCOPE(CL, "GTAO Main");

            struct FMainConstants
            {
                uint32 ViewportSize[2];
                uint32 WorkingDepthIndex;
                uint32 AOTermUAV;
                uint32 EdgesUAV;
                uint32 SliceCount;
                uint32 StepsPerSlice;
                uint32 NoiseIndex;
                float  EffectRadius;
                float  EffectFalloffRange;
                float  SampleDistributionPower;
                float  ThinOccluderCompensation;
                float  FinalValuePower;
                float  DepthMIPSamplingOffset;
            } PC = {};

            const int32 WorkingDepthSlot = WorkingDepth.GetResourceID();
            const int32 TermSlot         = TermA.GetMipUAVIndex(0);
            const int32 EdgesSlot        = Edges.GetMipUAVIndex(0);
            if (WorkingDepthSlot < 0 || TermSlot < 0 || EdgesSlot < 0)
            {
                LOG_ERROR("GTAO main pass is missing a heap slot; skipping the pass.");
                return false;
            }

            // Rotating the noise only pays off once something downstream accumulates across frames.
            const uint32 FrameIndex = (CurrentView != nullptr) ? CurrentView->TemporalFrameIndex : 0u;

            PC.ViewportSize[0]          = Width;
            PC.ViewportSize[1]          = Height;
            PC.WorkingDepthIndex        = (uint32)WorkingDepthSlot;
            PC.AOTermUAV                = (uint32)TermSlot;
            PC.EdgesUAV                 = (uint32)EdgesSlot;
            PC.SliceCount               = GGTAOSliceCounts[QualityLevel];
            PC.StepsPerSlice            = GGTAOStepsPerSlice[QualityLevel];
            PC.NoiseIndex               = (DenoisePasses > 0) ? (FrameIndex % 64u) : 0u;
            PC.EffectRadius             = EffectRadius;
            PC.EffectFalloffRange       = FalloffRange;
            PC.SampleDistributionPower  = SampleDistributionPower;
            PC.ThinOccluderCompensation = ThinOccluderCompensation;
            PC.FinalValuePower          = FinalValuePower;
            PC.DepthMIPSamplingOffset   = DepthMipSamplingOffset;

            DispatchCompute(CL, MainCS, PC, 
                RenderUtils::GetGroupCount(Width, 8u),
                RenderUtils::GetGroupCount(Height, 8u), 1);

            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }

        {
            SCENE_GPU_SCOPE(CL, "GTAO Denoise");

            struct FDenoiseConstants
            {
                uint32 ViewportSize[2];
                uint32 AOTermIndex;
                uint32 EdgesIndex;
                uint32 OutputUAV;
                uint32 FinalApply;
                float  DenoiseBlurBeta;
                float  Intensity;
            } PC = {};

            const int32 EdgesSlot = Edges.GetResourceID();
            if (EdgesSlot < 0)
            {
                LOG_ERROR("GTAO edge texture has no sampled heap slot; skipping the denoise.");
                return false;
            }

            PC.ViewportSize[0] = Width;
            PC.ViewportSize[1] = Height;
            PC.EdgesIndex      = (uint32)EdgesSlot;
            PC.DenoiseBlurBeta = (DenoisePasses == 0) ? GGTAODenoiseDisabledBeta : GGTAODenoiseBeta;
            PC.Intensity       = Intensity;

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(DenoiseCS));

            // A disabled denoise still runs one pass, because the final apply is what undoes the packing scale.
            const uint32 NumPasses = (uint32)Math::Max(DenoisePasses, 1);
            for (uint32 PassIndex = 0; PassIndex < NumPasses; ++PassIndex)
            {
                const bool bFinal = (PassIndex + 1u) == NumPasses;
                const bool bEven  = (PassIndex & 1u) == 0u;

                const FSceneImage& Src = bEven ? TermA : TermB;
                const FSceneImage& Dst = (bFinal && !bHalfTrace) ? Output : (bEven ? TermB : TermA);

                const int32 SrcSlot = Src.GetResourceID();
                const int32 DstSlot = Dst.GetMipUAVIndex(0);
                if (SrcSlot < 0 || DstSlot < 0)
                {
                    LOG_ERROR("GTAO denoise pass {} is missing a heap slot; stopping the chain.", PassIndex);
                    return false;
                }

                PC.AOTermIndex = (uint32)SrcSlot;
                PC.OutputUAV   = (uint32)DstSlot;
                PC.FinalApply  = bFinal ? 1u : 0u;

                // Each thread denoises two horizontal pixels, so a group spans 16 across and 8 down.
                RHI::CmdDispatch(CL, MakeArgs(PC),
                    RenderUtils::GetGroupCount(Width, 16u),
                    RenderUtils::GetGroupCount(Height, 8u), 1);

                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                    RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            }
        }

        if (bHalfTrace)
        {
            SCENE_GPU_SCOPE(CL, "GTAO Upsample");

            struct FUpsampleConstants
            {
                uint32 FullSize[2];
                uint32 HalfSize[2];
                uint32 AOIndex;
                uint32 HalfDepthIndex;
                uint32 FullDepthIndex;
                uint32 OutputUAV;
            } PC = {};

            // The final denoise pass wrote the ping-pong target its parity selects.
            const uint32 NumPasses = (uint32)Math::Max(DenoisePasses, 1);
            const FSceneImage& Denoised = ((NumPasses - 1u) & 1u) == 0u ? TermB : TermA;

            const int32 AOSlot        = Denoised.GetResourceID();
            const int32 HalfDepthSlot = WorkingDepth.GetResourceID();
            const int32 OutputSlot    = Output.GetMipUAVIndex(0);
            if (AOSlot < 0 || HalfDepthSlot < 0 || OutputSlot < 0)
            {
                LOG_ERROR("GTAO upsample is missing a heap slot; skipping it.");
                return false;
            }

            PC.FullSize[0]    = Output.GetSizeX();
            PC.FullSize[1]    = Output.GetSizeY();
            PC.HalfSize[0]    = Width;
            PC.HalfSize[1]    = Height;
            PC.AOIndex        = (uint32)AOSlot;
            PC.HalfDepthIndex = (uint32)HalfDepthSlot;
            PC.FullDepthIndex = (uint32)DepthSlot;
            PC.OutputUAV      = (uint32)OutputSlot;

            DispatchCompute(CL, UpsampleCS, PC,
                RenderUtils::GetGroupCount(PC.FullSize[0], 8u),
                RenderUtils::GetGroupCount(PC.FullSize[1], 8u), 1);

            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }
        return true;
    }

    // Weighted blended OIT (McGuire and Bavoil 2013), one pass into a weighted sum and a revealage product.
    void FDefaultSceneRenderer::TransparentPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

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
        // Fully revealed until a layer covers it.
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

        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencil(CL, (DepthDesc));

        RHI::FBlendDesc AccumBlend;
        AccumBlend.bBlendEnable   = true;
        AccumBlend.SrcColorFactor = RHI::EFactor::One;
        AccumBlend.DstColorFactor = RHI::EFactor::One;
        AccumBlend.SrcAlphaFactor = RHI::EFactor::One;
        AccumBlend.DstAlphaFactor = RHI::EFactor::One;

        RHI::FBlendDesc RevealageBlend;
        RevealageBlend.bBlendEnable   = true;
        RevealageBlend.SrcColorFactor = RHI::EFactor::Zero;
        RevealageBlend.DstColorFactor = RHI::EFactor::OneMinusSrcColor;
        RevealageBlend.SrcAlphaFactor = RHI::EFactor::Zero;
        RevealageBlend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;

        FMeshletPassContext Ctx;
        Ctx.CullViewIndex = CurrentCameraEarlyView;
        Ctx.ViewportW     = (float)Extent.x;
        Ctx.ViewportH     = (float)Extent.y;

        ForEachMeshletBatch(CL, TranslucentDrawList, Ctx,
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand& Batch)
            {
                if (Batch.bAdditive || Batch.bModulate)
                {
                    return false;   // UnorderedTranslucentPass owns these
                }

                Key.MS          = Batch.MeshShaderBase;
                Key.PS          = Batch.PixelShader;
                Key.DepthFormat = EFormat::D32;
                Key.TriCullMode = (uint8)(Batch.bTwoSided ? 0u : (uint32)TriCull_Backface);
                Key.ColorTargets.push_back({ Accum.Desc.Format, AccumBlend });
                Key.ColorTargets.push_back({ Revealage.Desc.Format, RevealageBlend });
                #if USING(WITH_EDITOR)
                Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
                #endif
                return true;
            },
            [&](const FMeshDrawCommand& Batch)
            {
                RHI::CmdSetCullMode(CL, Batch.bTwoSided ? RHI::ECullMode::None : RHI::ECullMode::Back);
            });

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::OITResolvePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        if (TranslucentDrawList.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("OIT Resolve Pass", tracy::Color::GreenYellow);

        static const FShaderH VertexShader = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PixelShader = FShaderLibrary::Get("OITResolve.slang");
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
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // The resolve emits the layers' color already scaled by their coverage and the revealage in alpha.
        RHI::FBlendDesc CompositeBlend;
        CompositeBlend.bBlendEnable   = true;
        CompositeBlend.SrcColorFactor = RHI::EFactor::One;
        CompositeBlend.DstColorFactor = RHI::EFactor::SrcAlpha;
        CompositeBlend.SrcAlphaFactor = RHI::EFactor::Zero;
        CompositeBlend.DstAlphaFactor = RHI::EFactor::One;

        FGraphicsPipelineKey Key;
        Key.VS = VertexShader;
        Key.PS = PixelShader;
        Key.ColorTargets.push_back({ HDR.Desc.Format, CompositeBlend });
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

    void FDefaultSceneRenderer::UnorderedTranslucentPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands        = Frame.Geometry.DrawCommands;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        bool bHasUnordered = false;
        for (uint32 Idx : TranslucentDrawList)
        {
            if (DrawCommands[Idx].bAdditive || DrawCommands[Idx].bModulate)
            {
                bHasUnordered = true;
                break;
            }
        }
        if (!bHasUnordered)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Unordered Translucent Pass", tracy::Color::CadetBlue3);

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);
        const FUIntVector2 Extent = HDR.GetExtent();

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

        RHI::FBlendDesc AdditiveBlend;
        AdditiveBlend.bBlendEnable   = true;
        AdditiveBlend.SrcColorFactor = RHI::EFactor::SrcAlpha;
        AdditiveBlend.DstColorFactor = RHI::EFactor::One;
        AdditiveBlend.SrcAlphaFactor = RHI::EFactor::One;
        AdditiveBlend.DstAlphaFactor = RHI::EFactor::One;

        // Src is the shader's lerp toward white, so this is scene color times the faded material color.
        RHI::FBlendDesc ModulateBlend;
        ModulateBlend.bBlendEnable   = true;
        ModulateBlend.SrcColorFactor = RHI::EFactor::DstColor;
        ModulateBlend.DstColorFactor = RHI::EFactor::Zero;
        ModulateBlend.SrcAlphaFactor = RHI::EFactor::Zero;
        ModulateBlend.DstAlphaFactor = RHI::EFactor::One;

        // Resolved for the same reason as the WBOIT pass above, after the mid pyramid rebuild.
        FMeshletPassContext Ctx;
        Ctx.CullViewIndex = CurrentCameraEarlyView;
        Ctx.ViewportW     = (float)Extent.x;
        Ctx.ViewportH     = (float)Extent.y;

        bool bPreviousWroteDepth = false;
        ForEachMeshletBatch(CL, TranslucentDrawList, Ctx,
            [&](FGraphicsPipelineKey& Key, const FMeshDrawCommand& Batch)
            {
                if (!Batch.bAdditive && !Batch.bModulate)
                {
                    return false;   // TranslucentPass owns these
                }

                Key.MS          = Batch.MeshShaderBase;
                Key.PS          = Batch.PixelShader;
                Key.DepthFormat = EFormat::D32;
                Key.ColorTargets.push_back({ HDR.Desc.Format, Batch.bModulate ? ModulateBlend : AdditiveBlend });
                #if USING(WITH_EDITOR)
                Key.ColorTargets.push_back({ Picker.Desc.Format, {} });
                #endif
                return true;
            },
            [&](const FMeshDrawCommand& Batch)
            {
                // A material may sample scene depth, so a batch that writes it gets pass breaks on both sides.
                if (Batch.bWriteDepth || bPreviousWroteDepth)
                {
                    RHI::CmdEndRenderPass(CL);
                    Barriers::RasterToRead(CL);
                    RHI::CmdBeginRenderPass(CL, Pass);
                    SetViewportScissor(CL, HDR.GetExtent());
                    RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
                }
                bPreviousWroteDepth = Batch.bWriteDepth;

                // Per batch, since a depth-writing blend is a material choice and the pass mixes both.
                RHI::FDepthStencilDesc DepthDesc;
                DepthDesc.DepthMode = Batch.bWriteDepth
                                    ? (RHI::EDepthFlags::Read | RHI::EDepthFlags::Write)
                                    : RHI::EDepthFlags::Read;
                DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
                RHI::CmdSetDepthStencil(CL, (DepthDesc));
            });

        // Depth write is dynamic state, so a writing batch would otherwise hand it to the next pass.
        RHI::FDepthStencilDesc RestoreDepth;
        RestoreDepth.DepthMode = RHI::EDepthFlags::Read;
        RestoreDepth.DepthTest = RHI::EOp::GreaterEqual;
        RHI::CmdSetDepthStencil(CL, (RestoreDepth));

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    namespace
    {
        // Mirrors the FPushConstants in the three VolumetricFog*.slang shaders.
        struct FFroxelInjectPushConstants
        {
            uint32   GridSize[3];
            float    NearPlane;

            float    FogRange;            // froxel far plane (max fog distance, view units)
            uint32   bSunVolumetric;      // 1 if light 0 (sun) opted into volumetrics
            float    Time;
            uint32   ScatterUAV;          // bindless 3D UAV index of the scatter volume

            uint32   LocalLightSamples;   // 0 skips local lights, 1 is the froxel center, 4 supersamples
            uint32   _PadNumFogVolumes;
            uint32   CloudShadowIndex;    // bindless 2D SRV, ~0u when no cloud shadow was built
            float    CloudShadowExtent;

            float    CloudShadowCenter[2];
            float    _Pad0[2];

            RHI::TGPUSpan<FGPUFogVolume> FogVolumes;   // offset 64, 8-aligned
        };
        static_assert(sizeof(FFroxelInjectPushConstants) <= 128, "Froxel inject PC must fit 128B");
        static_assert(offsetof(FFroxelInjectPushConstants, FogVolumes) % 8 == 0, "PC pointer must be 8-aligned");

        struct FCloudShadowPushConstants
        {
            // Mirrors FCloudMedium in Includes/CloudCommon.slang.
            uint32 NoiseIndex;
            float  ShapeScale;
            float  DetailScale;
            float  DetailStrength;

            float  Billow;
            float  Coverage;
            float  Density;
            float  LayerBottom;

            float  LayerTop;
            float  _MediumPad0;
            float  WindOffset[2];

            float  DetailWindOffset[2];
            float  _MediumPad1[2];

            uint32 ShadowUAV;
            uint32 Resolution;
            uint32 MarchSteps;
            float  HalfExtent;

            float  Center[2];
            float  Absorption;
            float  _Pad0;

            float  SunDirection[3];
            float  _Pad1;
        };
        static_assert(sizeof(FCloudShadowPushConstants) <= 128, "Cloud shadow PC must fit 128B");

        struct FFroxelIntegratePushConstants
        {
            uint32 GridSize[3];
            float  NearPlane;
            float  FogRange;
            uint32 ScatterSRV;     // bindless 3D SRV index of the scatter volume
            uint32 IntegratedUAV;  // bindless 3D UAV index of the integrated volume
            uint32 _Pad0;
        };

        struct FAtmosphereCompositePushConstants
        {
            uint32 HDRUAV;
            uint32 DepthIndex;       // bindless 2D SRV of scene depth
            uint32 ScreenW;
            uint32 ScreenH;

            uint32 AerialInScatterIndex;      // ~0u disables aerial perspective
            uint32 AerialTransmittanceIndex;
            float  AerialRange;
            float  AerialIntensity;

            uint32 CloudScatterIndex;         // ~0u disables clouds
            uint32 bFog;
            uint32 FroxelIntegratedIndex;     // bindless 3D SRV of the integrated froxel volume
            uint32 GridZ;

            float  NearPlane;
            float  FogRange;
            uint32 bVolumetric;      // froxel volume valid this frame; 0 = analytic height fog only
            float  FarShaftDistance;

            uint32 FogShaftIndex;      // bindless 2D SRV of the half-res shaft segment, ~0u keeps the far field unshadowed
            uint32 FogShaftDepthIndex; // bindless 2D SRV of the depth each shaft texel was marched against
            uint32 CloudDepthIndex;    // bindless 2D SRV of the depth each cloud texel was marched against
            float  CloudFloor;         // altitude of the cloud layer's base, so the composite can skip clouds below reach
        };
        static_assert(sizeof(FAtmosphereCompositePushConstants) == 80,
            "FAtmosphereCompositePushConstants must match AtmosphereComposite.slang::FPushConstants.");

        constexpr uint32 AtmosphereTileSize = 8;

        struct FFogShaftPushConstants
        {
            uint32 DepthIndex;
            uint32 ShaftUAV;
            uint32 ShaftDepthUAV;
            uint32 ScreenW;

            uint32 ScreenH;
            uint32 TraceW;
            uint32 TraceH;
            uint32 Steps;

            float  FogRange;
            float  ShaftDistance;
            uint32 bVolumetric;
            uint32 CloudShadowIndex;

            float  CloudShadowExtent;
            float  CloudShadowCenterX;
            float  CloudShadowCenterY;
            uint32 _Pad0;
        };
        static_assert(sizeof(FFogShaftPushConstants) == 64,
            "FFogShaftPushConstants must match FogShafts.slang::FPushConstants.");

        constexpr uint32 FogShaftTileSize = 8;
    }

    void FDefaultSceneRenderer::PublishFogGlobals(FSceneGlobalData& Globals) const
    {
        const FFrameData& Frame = *RenderFrame;
        const bool bHasFog      = Frame.Volumetrics.bHasFog;
        const bool bVolumetric  = bHasFog && Frame.Volumetrics.bVolumetricFog;

        Globals.FogParams           = Frame.Volumetrics.FogParams;
        Globals.bFogEnabled         = bHasFog ? 1u : 0u;
        Globals.FogGridZ            = FroxelGridSize.z;
        Globals.FogNearPlane        = Math::Max(Globals.NearPlane, 0.05f);
        Globals.FogRange            = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, Globals.FarPlane);
        Globals.FogFarShaftSteps    = Frame.Volumetrics.FarShaftSteps;
        Globals.FogFarShaftDistance = Frame.Volumetrics.FarShaftDistance;

        Globals.FogIntegratedIndex = bVolumetric
            ? (uint32)CurrentView->Images[(int)ENamedImage::FroxelIntegrated].GetResourceID()
            : Constants::kIndexNoneU32;

        Globals.FogCloudShadowIndex  = Constants::kIndexNoneU32;
        Globals.FogCloudShadowExtent = 0.0f;
        Globals.FogCloudShadowCenter = FVector2(0.0f, 0.0f);

        bool  bCloudShadows = true;
        float Extent        = 4000.0f;
        if (const CRendererSettings* RS = GetDefault<CRendererSettings>())
        {
            bCloudShadows = RS->bCloudShadows;
            Extent        = Math::Max(RS->CloudShadowExtent, 100.0f);
        }
        if (!bCloudShadows || !Frame.Volumetrics.bClouds)
        {
            return;
        }

        const FSceneImage& Shadow = CurrentView->Images[(int)ENamedImage::CloudShadow];
        if (!Shadow.IsValid())
        {
            return;
        }

        // Snapped to its own texel grid, or every camera step reshuffles the integral and the fog crawls.
        const float Texel = (2.0f * Extent) / (float)Math::Max(Shadow.GetSizeX(), 1u);
        const FVector3 Cam = FVector3(Globals.CameraData.Location);

        Globals.FogCloudShadowIndex  = (uint32)Shadow.GetResourceID();
        Globals.FogCloudShadowExtent = Extent;
        Globals.FogCloudShadowCenter = FVector2(Math::Floor(Cam.x / Texel) * Texel,
                                                Math::Floor(Cam.z / Texel) * Texel);
    }

    bool FDefaultSceneRenderer::CloudShadowMapPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.SceneGlobalData.FogCloudShadowIndex == Constants::kIndexNoneU32)
        {
            return false;
        }

        static const FShaderH CS = FShaderLibrary::Get("CloudShadowMap.slang");
        if (!CS)
        {
            return false;
        }

        const FSceneImage& Noise  = GetNamedImage(ENamedImage::CloudNoise);
        const FSceneImage& Shadow = GetNamedImage(ENamedImage::CloudShadow);
        if (!Noise.IsValid() || !Shadow.IsValid() || !BakeCloudNoiseIfNeeded(CL))
        {
            return false;
        }

        const int32 ShadowUAV = Shadow.GetMipUAVIndex(0);
        if (ShadowUAV < 0)
        {
            return false;
        }

        LUMINA_PROFILE_SECTION_COLORED("Cloud Shadow Map", tracy::Color::LightSlateGray);

        const SCloudComponent& C = Frame.Volumetrics.Clouds;
        const auto& LightData    = Frame.Lighting.LightData;

        const FVector3 SunDir = LightData.bHasSun
            ? Math::Normalize(LightData.SunDirection)
            : Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));

        FVector2 Wind       = C.WindDirection;
        const float WindLen = Math::Sqrt(Wind.x * Wind.x + Wind.y * Wind.y);
        Wind = (WindLen > 1e-4f) ? FVector2(Wind.x / WindLen, Wind.y / WindLen) : FVector2(1.0f, 0.0f);
        const float Drift = C.WindSpeed * Frame.SceneGlobalData.Time;

        int32 Steps = 8;
        if (const CRendererSettings* RS = GetDefault<CRendererSettings>())
        {
            Steps = Math::Clamp(RS->CloudShadowSteps, 1, 32);
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        FCloudShadowPushConstants PC = {};
        PC.NoiseIndex          = (uint32)Noise.GetResourceID();
        PC.ShapeScale          = Math::Max(C.ShapeScale, 100.0f);
        PC.DetailScale         = Math::Max(C.DetailScale, 10.0f);
        PC.DetailStrength      = Math::Clamp(C.DetailStrength, 0.0f, 1.0f);
        PC.Billow              = Math::Clamp(C.Billow, 0.0f, 1.0f);
        PC.Coverage            = Math::Clamp(C.Coverage, 0.0f, 1.0f);
        PC.Density             = Math::Max(C.Density, 0.0f);
        PC.LayerBottom         = Math::Max(C.LayerBottom, 100.0f);
        PC.LayerTop            = Math::Max(C.LayerTop, C.LayerBottom + 1.0f);
        PC.WindOffset[0]       = Wind.x * Drift;
        PC.WindOffset[1]       = Wind.y * Drift;
        PC.DetailWindOffset[0] = Wind.x * Drift * 2.0f;
        PC.DetailWindOffset[1] = Wind.y * Drift * 2.0f;

        PC.ShadowUAV       = (uint32)ShadowUAV;
        PC.Resolution      = Shadow.GetSizeX();
        PC.MarchSteps      = (uint32)Steps;
        PC.HalfExtent      = Frame.SceneGlobalData.FogCloudShadowExtent;
        PC.Center[0]       = Frame.SceneGlobalData.FogCloudShadowCenter.x;
        PC.Center[1]       = Frame.SceneGlobalData.FogCloudShadowCenter.y;
        PC.Absorption      = 1.0f;
        PC.SunDirection[0] = SunDir.x;
        PC.SunDirection[1] = SunDir.y;
        PC.SunDirection[2] = SunDir.z;

        const uint32 Groups = RenderUtils::GetGroupCount(Shadow.GetSizeX(), 8);
        RHI::CmdDispatch(CL, MakeArgs(PC), Groups, Groups, 1u);
        // Particles light in the vertex stage, surfaces and water in the pixel stage, fog in compute.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        return true;
    }

    void FDefaultSceneRenderer::FroxelInjectPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog)
        {
            return;
        }

        const auto& LightData       = Frame.Lighting.LightData;
        const auto& SceneGlobalData = Frame.SceneGlobalData;

        const bool bSunVolumetric = LightData.Lights.Count > 0
            && EnumHasAnyFlags(Frame.Lighting.Lights[0].Flags, ELightFlags::Directional)
            && EnumHasAnyFlags(Frame.Lighting.Lights[0].Flags, ELightFlags::Volumetric);

        LUMINA_PROFILE_SECTION_COLORED("Froxel Inject Pass", tracy::Color::SlateBlue);

        static const FShaderH CS = FShaderLibrary::Get("VolumetricFogInject.slang");
        if (!CS)
        {
            return;
        }

        const FSceneImage& Scatter = GetNamedImage(ENamedImage::FroxelScatter);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS));

        const float FogRange = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, SceneGlobalData.FarPlane);
        const uint32 NumVolumes = Math::Min((uint32)Frame.Volumetrics.FogVolumes.size(), GFogMaxVolumes);

        FFroxelInjectPushConstants PC = {};
        PC.ScatterUAV           = (uint32)Scatter.GetMipUAVIndex(0);
        PC.GridSize[0]          = FroxelGridSize.x;
        PC.GridSize[1]          = FroxelGridSize.y;
        PC.GridSize[2]          = FroxelGridSize.z;
        PC.NearPlane            = Math::Max(SceneGlobalData.NearPlane, 0.05f);
        PC.FogRange             = FogRange;
        PC.bSunVolumetric       = bSunVolumetric ? 1u : 0u;
        PC.Time                 = SceneGlobalData.Time;

        PC.CloudShadowIndex     = SceneGlobalData.FogCloudShadowIndex;
        PC.CloudShadowExtent    = SceneGlobalData.FogCloudShadowExtent;
        PC.CloudShadowCenter[0] = SceneGlobalData.FogCloudShadowCenter.x;
        PC.CloudShadowCenter[1] = SceneGlobalData.FogCloudShadowCenter.y;
        const CRendererSettings* RS = GetDefault<CRendererSettings>();
        const bool bSupersample = RS == nullptr || RS->bSupersampleVolumetricLights;
        PC.LocalLightSamples    = !Frame.Volumetrics.bLocalVolumetricLights ? 0u : (bSupersample ? 4u : 1u);
        if (NumVolumes > 0)
        {
            PC.FogVolumes = RHI::CopyTransientArray(Frame.Volumetrics.FogVolumes.data(), NumVolumes);
        }

        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(FroxelGridSize.x, 4),
                         RenderUtils::GetGroupCount(FroxelGridSize.y, 4),
                         RenderUtils::GetGroupCount(FroxelGridSize.z, 4));

        // Integrate reads the scatter volume next.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::FroxelIntegratePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Volumetrics.bHasFog || !Frame.Volumetrics.bVolumetricFog)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Froxel Integrate Pass", tracy::Color::MediumPurple);

        static const FShaderH CS = FShaderLibrary::Get("VolumetricFogIntegrate.slang");
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

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::FogShaftPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        AtmosphereTerms.FogShaftIndex      = Constants::kIndexNoneU32;
        AtmosphereTerms.FogShaftDepthIndex = Constants::kIndexNoneU32;

        if (!Frame.Volumetrics.bHasFog || Frame.Volumetrics.FarShaftSteps == 0u)
        {
            return;
        }

        static const FShaderH CS = FShaderLibrary::Get("FogShafts.slang");
        const FSceneImage& Shaft      = GetNamedImage(ENamedImage::FogShaft);
        const FSceneImage& ShaftDepth = GetNamedImage(ENamedImage::FogShaftDepth);
        if (!CS || !Shaft.IsValid() || !ShaftDepth.IsValid())
        {
            return;
        }

        const int32 ShaftUAV      = Shaft.GetMipUAVIndex(0);
        const int32 ShaftDepthUAV = ShaftDepth.GetMipUAVIndex(0);
        if (ShaftUAV < 0 || ShaftDepthUAV < 0)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Fog Shafts Pass", tracy::Color::Orange3);

        const FSceneImage& HDR = GetNamedImage(ENamedImage::HDR);

        FFogShaftPushConstants PC = {};
        PC.DepthIndex         = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        PC.ShaftUAV           = (uint32)ShaftUAV;
        PC.ShaftDepthUAV      = (uint32)ShaftDepthUAV;
        PC.ScreenW            = HDR.GetSizeX();
        PC.ScreenH            = HDR.GetSizeY();
        PC.TraceW             = Shaft.GetSizeX();
        PC.TraceH             = Shaft.GetSizeY();
        PC.Steps              = Frame.Volumetrics.FarShaftSteps;
        PC.FogRange           = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, Frame.SceneGlobalData.FarPlane);
        PC.ShaftDistance      = Frame.Volumetrics.FarShaftDistance;
        PC.bVolumetric        = Frame.Volumetrics.bVolumetricFog ? 1u : 0u;
        PC.CloudShadowIndex   = Frame.SceneGlobalData.FogCloudShadowIndex;
        PC.CloudShadowExtent  = Frame.SceneGlobalData.FogCloudShadowExtent;
        PC.CloudShadowCenterX = Frame.SceneGlobalData.FogCloudShadowCenter.x;
        PC.CloudShadowCenterY = Frame.SceneGlobalData.FogCloudShadowCenter.y;

        DispatchCompute(CL, CS, PC,
                        RenderUtils::GetGroupCount(PC.TraceW, FogShaftTileSize),
                        RenderUtils::GetGroupCount(PC.TraceH, FogShaftTileSize), 1);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        AtmosphereTerms.FogShaftIndex      = (uint32)Shaft.GetResourceID();
        AtmosphereTerms.FogShaftDepthIndex = (uint32)ShaftDepth.GetResourceID();
    }

    void FDefaultSceneRenderer::AtmosphereCompositePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        const bool bFog    = Frame.Volumetrics.bHasFog;
        const bool bAerial = AtmosphereTerms.AerialInScatterIndex != Constants::kIndexNoneU32;
        const bool bClouds = AtmosphereTerms.CloudScatterIndex != Constants::kIndexNoneU32;

        if (!bFog && !bAerial && !bClouds)
        {
            return;
        }

        static const FShaderH CS = FShaderLibrary::Get("AtmosphereComposite.slang");
        if (!CS)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Atmosphere Composite Pass", tracy::Color::Orange3);

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& Integrated = GetNamedImage(ENamedImage::FroxelIntegrated);

        const int32 HDRUAV = HDR.GetMipUAVIndex(0);
        if (HDRUAV < 0)
        {
            return;
        }

        const uint32 Width  = HDR.GetSizeX();
        const uint32 Height = HDR.GetSizeY();

        FAtmosphereCompositePushConstants PC = {};
        PC.HDRUAV     = (uint32)HDRUAV;
        PC.DepthIndex = (uint32)SceneDepth.GetResourceID();
        PC.ScreenW    = Width;
        PC.ScreenH    = Height;

        PC.AerialInScatterIndex     = AtmosphereTerms.AerialInScatterIndex;
        PC.AerialTransmittanceIndex = AtmosphereTerms.AerialTransmittanceIndex;
        PC.AerialRange              = AtmosphereTerms.AerialRange;
        PC.AerialIntensity          = AtmosphereTerms.AerialIntensity;

        PC.CloudScatterIndex     = AtmosphereTerms.CloudScatterIndex;
        PC.CloudDepthIndex       = AtmosphereTerms.CloudDepthIndex;
        PC.bFog                  = bFog ? 1u : 0u;
        PC.FroxelIntegratedIndex = (uint32)Integrated.GetResourceID();
        PC.GridZ                 = FroxelGridSize.z;

        PC.NearPlane          = Math::Max(Frame.SceneGlobalData.NearPlane, 0.05f);
        PC.FogRange           = Math::Clamp(Frame.Volumetrics.FogParams.VolumetricParams.z, 1.0f, Frame.SceneGlobalData.FarPlane);
        PC.bVolumetric        = Frame.Volumetrics.bVolumetricFog ? 1u : 0u;
        PC.FarShaftDistance   = Frame.Volumetrics.FarShaftDistance;
        PC.FogShaftIndex      = AtmosphereTerms.FogShaftIndex;
        PC.FogShaftDepthIndex = AtmosphereTerms.FogShaftDepthIndex;

        PC.CloudFloor         = Math::Max(Frame.Volumetrics.Clouds.LayerBottom, 100.0f);

        auto Spec = [](uint32 Id, bool bOn)
        {
            return RHI::FSpecializationConstant{ .ConstantID = Id, .AsInt = bOn ? 1u : 0u, .Type = RHI::ESpecializationConstantType::UInt32 };
        };
        const RHI::FSpecializationConstant CompositeConsts[] =
        {
            Spec(13u, PC.AerialInScatterIndex != Constants::kIndexNoneU32),
            Spec(14u, PC.CloudScatterIndex != Constants::kIndexNoneU32),
            Spec(15u, PC.bFog != 0u),
            Spec(16u, PC.bVolumetric != 0u),
            Spec(17u, PC.FogShaftIndex != Constants::kIndexNoneU32),
        };
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CS, TSpan<const RHI::FSpecializationConstant>(CompositeConsts, std::size(CompositeConsts))));

        RHI::CmdDispatch(CL, MakeArgs(PC),
                         RenderUtils::GetGroupCount(Width,  AtmosphereTileSize),
                         RenderUtils::GetGroupCount(Height, AtmosphereTileSize), 1);

        // Translucent, particle and overlay passes load and blend HDR next, with no raster barrier in between when nothing is translucent.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute | RHI::EStageFlags::RasterColorOut,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorRead | RHI::EAccessFlags::ColorWrite);
    }

    void FDefaultSceneRenderer::WaterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const TVector<FGPUWater>& Waters = Frame.Water.Surfaces;
        if (Waters.empty())
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Water Pass", tracy::Color::CadetBlue);

        static const FShaderH VS = FShaderLibrary::Get("WaterVert.slang");
        static const FShaderH PS = FShaderLibrary::Get("WaterPixel.slang");
        if (!VS || !PS)
        {
            return;
        }

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneColor = GetNamedImage(ENamedImage::WaterRefraction);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);

        // The water writes scene depth, so what it samples for refraction and its shoreline is a copy taken first.
        const FSceneImage& SceneDepthCopy = GetNamedImage(ENamedImage::SceneDepthCopy);

        Barriers::SceneToTransfer(CL);
        RHI::CmdCopyTexture(CL, HDR.Texture, RHI::FTextureSlice{}, SceneColor.Texture, RHI::FTextureSlice{});
        RHI::CmdCopyTexture(CL, SceneDepth.Texture, RHI::FTextureSlice{}, SceneDepthCopy.Texture, RHI::FTextureSlice{});
        Barriers::TransferToShaders(CL);
        RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead,
            RHI::EStageFlags::FragmentTests, RHI::EAccessFlags::DepthStencilRead | RHI::EAccessFlags::DepthStencilWrite);

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        // Tested and written against the real depth, so the water hides itself and what lies beneath it from every later pass.
        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments          = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture   = SceneDepth.Texture;
        Pass.DepthAttachment.LoadOp    = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp   = RHI::EStoreOp::Store;
        Pass.RenderArea                = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::FDepthStencilDesc WaterDepthTest;
        WaterDepthTest.DepthMode = RHI::EDepthFlags::Read | RHI::EDepthFlags::Write;
        WaterDepthTest.DepthTest = RHI::EOp::Greater;
        RHI::CmdSetDepthStencil(CL, WaterDepthTest);
        // Double-sided so the surface is visible from below (camera submerged) too.
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // Standard alpha over, where the PS composites scene and water and alpha softens the shore.
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
        Key.DepthFormat = EFormat::D32;
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FWaterPushConstants
        {
            RHI::TGPUSpan<FGPUWater> Waters;
            uint32 SceneColorIndex;
            uint32 SceneDepthIndex;
            uint32 AerialInScatterIndex;
            uint32 AerialTransmittanceIndex;
            float  AerialRange;
            float  AerialIntensity;
        };
        static_assert(sizeof(FWaterPushConstants) == 40, "FWaterPushConstants must match Includes/Water.slang.");

        FWaterPushConstants PC = {};
        PC.Waters          = RHI::CopyTransientArray(Waters.data(), Waters.size());
        PC.SceneColorIndex = (uint32)SceneColor.GetResourceID();
        PC.AerialInScatterIndex     = AtmosphereTerms.AerialInScatterIndex;
        PC.AerialTransmittanceIndex = AtmosphereTerms.AerialTransmittanceIndex;
        PC.AerialRange              = AtmosphereTerms.AerialRange;
        PC.AerialIntensity          = AtmosphereTerms.AerialIntensity;
        PC.SceneDepthIndex = (uint32)SceneDepthCopy.GetResourceID();

        const RHI::GPUPtr Args = MakeArgs(PC);

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

    void FDefaultSceneRenderer::UnderwaterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.Water.bUnderwaterActive)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Underwater Pass", tracy::Color::SteelBlue);

        static const FShaderH VS = FShaderLibrary::Get("FullscreenQuad.slang");
        static const FShaderH PS = FShaderLibrary::Get("WaterUnderwater.slang");
        if (!VS || !PS)
        {
            return;
        }

        const FSceneImage& HDR        = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& SceneColor = GetNamedImage(ENamedImage::WaterRefraction);
        const FSceneImage& SceneDepth = GetNamedImage(ENamedImage::DepthAttachment);

        Barriers::SceneToTransfer(CL);
        RHI::CmdCopyTexture(CL, HDR.Texture, RHI::FTextureSlice{}, SceneColor.Texture, RHI::FTextureSlice{});
        Barriers::TransferToShaders(CL);

        RHI::FRenderAttachment Color;
        Color.Texture = HDR.Texture;
        Color.LoadOp  = RHI::ELoadOp::Load;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetDepthStencil(CL, (RHI::FDepthStencilDesc{}));
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
        PC.ParamsAddr      = RHI::CopyTransient(Frame.Water.Underwater);
        PC.SceneColorIndex = (uint32)SceneColor.GetResourceID();
        PC.SceneDepthIndex = (uint32)SceneDepth.GetResourceID();

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
    
    void FDefaultSceneRenderer::SkyCubeCapturePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Sky Cube Capture", tracy::Color::SkyBlue);

        const FFrameData& Frame = *RenderFrame;
        const auto& LightData         = Frame.Lighting.LightData;
        const auto& SceneGlobalData   = Frame.SceneGlobalData;
        const int32 EnvironmentMapID  = Frame.Volumetrics.EnvironmentMapID;
        const bool bIBLDirty          = Frame.Volumetrics.bIBLDirty;

        if (!FrameFlags.bHasEnvironment)
        {
            if (bSkyTargetsBlack)
            {
                return;
            }
            bSkyTargetsBlack = true;

            const float Black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyCube).Texture, Black);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyIrradiance).Texture, Black);
            RHI::CmdClearTexture(CL, GetNamedImage(ENamedImage::SkyPrefilter).Texture, Black);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            return;
        }
        bSkyTargetsBlack = false;

        if (!bIBLDirty)
        {
            return;
        }

        const FSceneImage& SkyCube = GetNamedImage(ENamedImage::SkyCube);
        if (!SkyCube.IsValid())
        {
            return;
        }

        // HDRI path where equirect to cube replaces the procedural fill.
        if (EnvironmentMapID >= 0)
        {
            static const FShaderH ComputeShader = FShaderLibrary::Get("EquirectToCubemap.slang");
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
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
            return;
        }

        static const FShaderH ComputeShader = FShaderLibrary::Get("SkyCubeCapture.slang");
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
        PC.EnvAddr    = RHI::CopyTransient(Frame.Volumetrics.EnvironmentParams);
        PC.SkyCubeUAV = (uint32)SkyCube.GetMipUAVIndex(0);

        if (LightData.bHasSun)
        {
            PC.SunDirection = Math::Normalize(LightData.SkySunDirection);
        }
        else
        {
            PC.SunDirection = Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));
        }
        PC.Time = SceneGlobalData.Time;

        constexpr uint32 SkyCaptureTile = 8u;
        const uint32 FaceSize = SkyCube.GetSizeX();
        const uint32 GroupsXY = RenderUtils::GetGroupCount(FaceSize, SkyCaptureTile);
        // Z is 6 layers, one per cube face, and each thread owns one (face, x, y).
        RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);

        // Convolution + environment pass read the cube next.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::IrradianceConvolutionPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Sky Irradiance Convolution", tracy::Color::SkyBlue1);

        const FFrameData& Frame = *RenderFrame;
        const bool bIBLConvolutionDirty = Frame.Volumetrics.bIBLConvolutionDirty;

        if (!FrameFlags.bHasEnvironment)
        {
            return;
        }

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

        static const FShaderH ComputeShader = FShaderLibrary::Get("IrradianceConvolution.slang");
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

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::PrefilterEnvMapPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const bool bIBLConvolutionDirty = Frame.Volumetrics.bIBLConvolutionDirty;

        if (!FrameFlags.bHasEnvironment)
        {
            return;
        }

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

        static const FShaderH ComputeShader = FShaderLibrary::Get("PrefilterEnvMap.slang");
        if (!ComputeShader)
        {
            return;
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

        const uint32 NumMips      = PrefilterCube.GetNumMips();
        const uint32 BaseFaceSize = PrefilterCube.GetSizeX();

        constexpr uint32 PrefilterTile = 8u;

        for (uint32 Mip = 0; Mip < NumMips; ++Mip)
        {
            FPrefilterPC PC = {};
            PC.SrcCubeSRV = (uint32)SkyCube.GetResourceID();
            PC.OutMipUAV  = (uint32)PrefilterCube.GetMipUAVIndex(Mip);
            PC.Roughness  = (NumMips <= 1u) ? 0.0f
                                            : (float)Mip / (float)(NumMips - 1u);
            PC.NumSamples = GPrefilterSampleCount;

            const uint32 MipFaceSize = std::max<uint32>(BaseFaceSize >> Mip, 1u);
            const uint32 GroupsXY    = RenderUtils::GetGroupCount(MipFaceSize, PrefilterTile);
            RHI::CmdDispatch(CL, MakeArgs(PC), GroupsXY, GroupsXY, 6u);
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

    void FDefaultSceneRenderer::EnvironmentPass(RHI::FCmdListH CL)
    {
        if (!FrameFlags.bHasEnvironment)
        {
            const FSceneImage& ColorRT = GetNamedImage(ENamedImage::HDR);

            RHI::FRenderAttachment Color;
            Color.Texture        = ColorRT.Texture;
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

        static const FString   FarPlaneDefine = "FULLSCREEN_AT_FAR_PLANE";
        static const FShaderH VertexShader   = FShaderLibrary::Get("FullscreenQuad.slang", TSpan<const FString>(&FarPlaneDefine, 1));
        static const FShaderH PixelShader    = FShaderLibrary::Get("Environment.slang");
        if (!VertexShader || !PixelShader)
        {
            return;
        }

        const FSceneImage& ColorRT = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& DepthRT = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& SkyCube = GetNamedImage(ENamedImage::SkyCube);
        const FUIntVector2 Extent  = GetNamedImage(ENamedImage::HDR).GetExtent();

        const int32  EnvMapID    = RenderFrame->Volumetrics.EnvironmentMapID;
        const uint32 EquirectIdx = EnvMapID >= 0 ? (uint32)EnvMapID : (uint32)GetNamedImage(ENamedImage::BRDFLut).GetResourceID();
        const uint32 EquirectW   = EnvMapID >= 0 ? RenderFrame->Volumetrics.EnvironmentMapWidth : 256u;

        RHI::FRenderAttachment Color;
        Color.Texture        = ColorRT.Texture;
        Color.LoadOp         = RHI::ELoadOp::Clear;
        Color.StoreOp        = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments        = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture = DepthRT.Texture;
        Pass.DepthAttachment.LoadOp  = RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp = RHI::EStoreOp::Store;
        Pass.RenderArea              = Extent;

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, Extent);

        // Only pixels still at the cleared far plane show sky; every other one is shaded over by the passes after this.
        RHI::FDepthStencilDesc DepthDesc;
        DepthDesc.DepthMode = RHI::EDepthFlags::Read;
        DepthDesc.DepthTest = RHI::EOp::Equal;
        RHI::CmdSetDepthStencil(CL, DepthDesc);
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // Mirrors the bit-cast the extract does into Misc.x; specializing on it strips the other modes.
        uint32 SkyModeBits = GSkyMode_Runtime;
        std::memcpy(&SkyModeBits, &RenderFrame->Volumetrics.EnvironmentParams.Misc.x, sizeof(uint32));

        FGraphicsPipelineKey Key;
        Key.VS          = VertexShader;
        Key.PS          = PixelShader;
        Key.SkyMode     = (SkyModeBits <= GSkyMode_HDRI) ? (uint8)SkyModeBits : (uint8)GSkyMode_Runtime;
        Key.DepthFormat = DepthRT.Desc.Format;
        Key.ColorTargets.push_back({ ColorRT.Desc.Format, {} });
        RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

        struct FEnvPushConstants
        {
            uint64 EnvAddr;
            uint32 SkyCubeIndex;
            uint32 EquirectIndex;
            uint32 EquirectWidth;   // HDRI mode LOD that anti-aliases the sky
            uint32 _Pad1;
        };
        static_assert(sizeof(FEnvPushConstants) == 24, "FEnvPushConstants must match the slang pass block.");

        FEnvPushConstants PC = {};
        PC.EnvAddr       = RHI::CopyTransient(RenderFrame->Volumetrics.EnvironmentParams);
        PC.SkyCubeIndex  = (uint32)SkyCube.GetResourceID();
        PC.EquirectIndex = EquirectIdx;
        PC.EquirectWidth = EquirectW;

        RHI::CmdDraw(CL, MakeArgs(PC), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    namespace
    {
        struct FAerialLUTPushConstants
        {
            uint64   EnvAddr;
            uint32   InScatterUAV;
            uint32   TransmittanceUAV;

            FVector3 SunDirection;
            float    Range;
        };
        static_assert(sizeof(FAerialLUTPushConstants) == 32,
            "FAerialLUTPushConstants must match AerialPerspectiveLUT.slang::FPushConstants.");

        constexpr uint32 AerialTileSize = 8;
    }

    namespace
    {
        struct FCloudNoisePushConstants
        {
            uint32 NoiseUAV;
            uint32 _Pad0;
            uint32 _Pad1;
            uint32 _Pad2;
        };
        static_assert(sizeof(FCloudNoisePushConstants) == 16,
            "FCloudNoisePushConstants must match CloudNoiseBake.slang::FPushConstants.");

        struct FCloudPushConstants
        {
            uint32   ScatterUAV;
            uint32   DepthIndex;
            uint32   NoiseIndex;
            uint32   ScreenW;

            uint32   ScreenH;
            uint32   MarchSteps;
            uint32   LightSteps;
            float    MaxDistance;

            FVector3 SunDirection;
            float    LayerBottom;

            FVector3 SunColor;
            float    LayerTop;

            FVector3 AmbientColor;
            float    Coverage;

            float    Density;
            float    ShapeScale;
            float    DetailScale;
            float    DetailStrength;

            float    Billow;
            float    ForwardScattering;
            float    BackScattering;
            float    PowderStrength;

            FVector2 WindOffset;
            FVector2 DetailWindOffset;

            uint32   CloudDepthUAV;
            uint32   _Pad0;
            uint32   _Pad1;
            uint32   _Pad2;
        };
        static_assert(sizeof(FCloudPushConstants) == 144,
            "FCloudPushConstants must match VolumetricClouds.slang::FPushConstants.");

        constexpr uint32 CloudTileSize      = 8;
        constexpr uint32 CloudNoiseTileSize = 4;
    }

    bool FDefaultSceneRenderer::BakeCloudNoiseIfNeeded(RHI::FCmdListH CL)
    {
        const RHI::EQueueType Queue = RHI::GetCommandListQueue(CL);
        if (CurrentView->bCloudNoiseBaked && CurrentView->CloudNoiseQueue == Queue)
        {
            return true;
        }

        static const FShaderH BakeCS = FShaderLibrary::Get("CloudNoiseBake.slang");
        const FSceneImage& Noise     = GetNamedImage(ENamedImage::CloudNoise);
        if (!BakeCS || !Noise.IsValid())
        {
            return false;
        }

        // The volume is view-independent and static, so it is generated once and kept.
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BakeCS));

        FCloudNoisePushConstants BakePC = {};
        BakePC.NoiseUAV = (uint32)Noise.GetMipUAVIndex(0);

        const uint32 BakeGroups = RenderUtils::GetGroupCount(kCloudNoiseSize, CloudNoiseTileSize);
        RHI::CmdDispatch(CL, MakeArgs(BakePC), BakeGroups, BakeGroups, BakeGroups);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        CurrentView->bCloudNoiseBaked = true;
        CurrentView->CloudNoiseQueue  = Queue;
        return true;
    }

    bool FDefaultSceneRenderer::VolumetricCloudPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        AtmosphereTerms.CloudScatterIndex = Constants::kIndexNoneU32;
        AtmosphereTerms.CloudDepthIndex   = Constants::kIndexNoneU32;

        if (!Frame.Volumetrics.bClouds)
        {
            return false;
        }

        static const FShaderH CloudCS = FShaderLibrary::Get("VolumetricClouds.slang");
        if (!CloudCS)
        {
            return false;
        }

        const FSceneImage& Noise   = GetNamedImage(ENamedImage::CloudNoise);
        const FSceneImage& Scatter    = GetNamedImage(ENamedImage::CloudScatter);
        const FSceneImage& CloudDepth = GetNamedImage(ENamedImage::CloudDepth);
        const FSceneImage& Depth      = GetNamedImage(ENamedImage::DepthAttachment);
        if (!Noise.IsValid() || !Scatter.IsValid() || !CloudDepth.IsValid())
        {
            return false;
        }

        LUMINA_PROFILE_SECTION_COLORED("Volumetric Clouds", tracy::Color::White);

        if (!BakeCloudNoiseIfNeeded(CL))
        {
            return false;
        }

        const SCloudComponent& C = Frame.Volumetrics.Clouds;
        const auto& LightData    = Frame.Lighting.LightData;

        FVector3 SunDir = LightData.bHasSun
            ? Math::Normalize(LightData.SunDirection)
            : Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));

        FVector3 SunColor = FVector3(1.0f);
        if (LightData.bHasSun && LightData.Lights.Count > 0)
        {
            const FLight&  Sun    = Frame.Lighting.Lights[0];
            const FVector4 Unpack = UnpackColor(Sun.Color);
            SunColor = FVector3(Unpack.x, Unpack.y, Unpack.z) * Sun.Intensity;
        }

        const float Time     = Frame.SceneGlobalData.Time;
        FVector2 Wind        = C.WindDirection;
        const float WindLen  = Math::Sqrt(Wind.x * Wind.x + Wind.y * Wind.y);
        Wind = (WindLen > 1e-4f) ? FVector2(Wind.x / WindLen, Wind.y / WindLen) : FVector2(1.0f, 0.0f);

        const float Drift = C.WindSpeed * Time;

        const int32 ScatterUAV    = Scatter.GetMipUAVIndex(0);
        const int32 CloudDepthUAV = CloudDepth.GetMipUAVIndex(0);
        if (ScatterUAV < 0 || CloudDepthUAV < 0)
        {
            return false;
        }

        FCloudPushConstants PC = {};
        PC.ScatterUAV        = (uint32)ScatterUAV;
        PC.DepthIndex        = (uint32)Depth.GetResourceID();
        PC.NoiseIndex        = (uint32)Noise.GetResourceID();
        PC.ScreenW           = Scatter.GetSizeX();
        PC.ScreenH           = Scatter.GetSizeY();
        PC.MarchSteps        = (uint32)Math::Clamp(C.MarchSteps, 16, 256);
        PC.LightSteps        = (uint32)Math::Clamp(C.LightSteps, 1, 16);
        PC.MaxDistance       = Math::Max(C.MaxDistance, 1000.0f);
        PC.SunDirection      = SunDir;
        PC.LayerBottom       = Math::Max(C.LayerBottom, 100.0f);
        PC.SunColor          = SunColor * Math::Max(C.SunIntensity, 0.0f);
        PC.LayerTop          = Math::Max(C.LayerTop, C.LayerBottom + 1.0f);
        PC.AmbientColor      = FVector3(LightData.AmbientLight.x, LightData.AmbientLight.y, LightData.AmbientLight.z)
                             * LightData.AmbientLight.w * Math::Max(C.AmbientIntensity, 0.0f);
        PC.Coverage          = Math::Clamp(C.Coverage, 0.0f, 1.0f);
        PC.Density           = Math::Max(C.Density, 0.0f);
        PC.ShapeScale        = Math::Max(C.ShapeScale, 100.0f);
        PC.DetailScale       = Math::Max(C.DetailScale, 10.0f);
        PC.DetailStrength    = Math::Clamp(C.DetailStrength, 0.0f, 1.0f);
        PC.Billow            = Math::Clamp(C.Billow, 0.0f, 1.0f);
        PC.ForwardScattering = Math::Clamp(C.ForwardScattering, 0.0f, 0.95f);
        PC.BackScattering    = Math::Clamp(C.BackScattering, -0.95f, 0.0f);
        PC.PowderStrength    = Math::Clamp(C.PowderStrength, 0.0f, 1.0f);
        PC.WindOffset        = FVector2(Wind.x * Drift, Wind.y * Drift);
        PC.DetailWindOffset  = FVector2(PC.WindOffset.x * C.DetailWindFactor,
                                        PC.WindOffset.y * C.DetailWindFactor);
        PC.CloudDepthUAV     = (uint32)CloudDepthUAV;

        DispatchCompute(CL, CloudCS, PC, 
                         RenderUtils::GetGroupCount(PC.ScreenW, CloudTileSize),
                         RenderUtils::GetGroupCount(PC.ScreenH, CloudTileSize), 1);

        // The scatter volume feeds AtmosphereCompositePass and the cloud shadow map, both dispatches.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        AtmosphereTerms.CloudScatterIndex = (uint32)Scatter.GetResourceID();
        AtmosphereTerms.CloudDepthIndex   = (uint32)CloudDepth.GetResourceID();
        return true;
    }

    void FDefaultSceneRenderer::ScreenSpaceReflectionsPass(RHI::FCmdListH CL)
    {
        const CRendererSettings* RS = GetDefault<CRendererSettings>();
        if (RS == nullptr || !RS->bScreenSpaceReflections || RS->SSRIntensity <= 0.0f)
        {
            return;
        }

        if (RenderFrame->Geometry.DrawCommands.empty())
        {
            return;
        }

        static const FShaderH DownsampleCS = FShaderLibrary::Get("SSRDownsample.slang");
        static const FShaderH PyramidCS    = FShaderLibrary::Get("SSRHiZ.slang");
        static const FShaderH TraceCS      = FShaderLibrary::Get("ScreenSpaceReflections.slang");
        static const FShaderH FilterCS     = FShaderLibrary::Get("SSRFilter.slang");
        static const FShaderH ResolveCS    = FShaderLibrary::Get("SSRResolve.slang");
        if (!DownsampleCS || !PyramidCS || !TraceCS || !FilterCS || !ResolveCS)
        {
            return;
        }

        const FSceneImage& Chain    = GetNamedImage(ENamedImage::SSRTrace);
        const FSceneImage& Pyramid  = GetNamedImage(ENamedImage::SSRPyramid);
        const FSceneImage& Surface  = GetNamedImage(ENamedImage::SSRSurface);
        const FSceneImage& MipLevel = GetNamedImage(ENamedImage::SSRMipLevel);
        if (!Chain.IsValid() || !Pyramid.IsValid() || !Surface.IsValid() || !MipLevel.IsValid())
        {
            return;
        }

        // Only GBuffer pixels reflect, and the slot table that identifies them exists once classify has run.
        if (MaterialClassifyLayout.NumSlots == 0u)
        {
            return;
        }

        const FSceneImage& HDR   = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Depth = GetNamedImage(ENamedImage::DepthAttachment);
        const int32 HDRUAV = HDR.GetMipUAVIndex(0);
        if (HDRUAV < 0)
        {
            return;
        }

        LUMINA_PROFILE_SECTION_COLORED("Screen Space Reflections", tracy::Color::Cyan3);

        const uint32 ScreenW  = HDR.GetSizeX();
        const uint32 ScreenH  = HDR.GetSizeY();
        const uint32 TraceW   = Chain.GetSizeX();
        const uint32 TraceH   = Chain.GetSizeY();
        const uint32 Scale    = GetSSRTraceScale();
        const uint32 MipCount = Math::Min(Chain.GetNumMips(), Pyramid.GetNumMips());

        const uint32 GBufferA  = (uint32)GetNamedImage(ENamedImage::GBufferA).GetResourceID();
        const uint32 GBufferB  = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        const uint32 GBufferC  = (uint32)GetNamedImage(ENamedImage::GBufferC).GetResourceID();
        const uint32 GBufferD  = (uint32)GetNamedImage(ENamedImage::GBufferD).GetResourceID();
        const uint32 SlotImage = (uint32)GetNamedImage(ENamedImage::MaterialSlot).GetResourceID();

        const auto ComputeToCompute = [CL]()
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        };

        // Lighting and the forward passes before this wrote HDR, which the trace samples as scene color.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute | RHI::EStageFlags::RasterColorOut, RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        {
            struct FDownsamplePushConstants
            {
                uint32 GBufferAIndex;
                uint32 GBufferBIndex;
                uint32 GBufferCIndex;
                uint32 GBufferDIndex;

                uint32 DepthIndex;
                uint32 SlotImageIndex;
                uint32 OutDepthUAV;
                uint32 OutSurfaceUAV;

                uint32 ScreenW;
                uint32 ScreenH;
                uint32 TraceW;
                uint32 TraceH;

                uint32 Scale;
                uint32 _Pad0;
                uint32 _Pad1;
                uint32 _Pad2;
            } PC = {};
            static_assert(sizeof(FDownsamplePushConstants) == 64, "FDownsamplePushConstants must match SSRDownsample.slang FSSRDownsampleArgs.");

            PC.GBufferAIndex  = GBufferA;
            PC.GBufferBIndex  = GBufferB;
            PC.GBufferCIndex  = GBufferC;
            PC.GBufferDIndex  = GBufferD;
            PC.DepthIndex     = (uint32)Depth.GetResourceID();
            PC.SlotImageIndex = SlotImage;
            PC.OutDepthUAV    = (uint32)Pyramid.GetMipUAVIndex(0);
            PC.OutSurfaceUAV  = (uint32)Surface.GetMipUAVIndex(0);
            PC.ScreenW        = ScreenW;
            PC.ScreenH        = ScreenH;
            PC.TraceW         = TraceW;
            PC.TraceH         = TraceH;
            PC.Scale          = Scale;

            DispatchCompute(CL, DownsampleCS, PC, RenderUtils::GetGroupCount(TraceW, 8u), RenderUtils::GetGroupCount(TraceH, 8u), 1u);
            ComputeToCompute();
        }

        // Shared by SSRHiZ.slang and SSRFilter.slang, which both build one level of a chain from the level below.
        struct FChainLevelPushConstants
        {
            uint32 ChainIndex;
            uint32 SourceMip;
            uint32 DestUAV;
            uint32 DestMip;

            uint32 SourceW;
            uint32 SourceH;
            uint32 DestW;
            uint32 DestH;
        };
        static_assert(sizeof(FChainLevelPushConstants) == 32, "FChainLevelPushConstants must match FSSRChainLevelArgs.");

        for (uint32 Mip = 1; Mip < MipCount; ++Mip)
        {
            FChainLevelPushConstants PC = {};
            PC.ChainIndex = (uint32)Pyramid.GetResourceID();
            PC.SourceMip  = Mip - 1u;
            PC.DestUAV    = (uint32)Pyramid.GetMipUAVIndex(Mip);
            PC.DestMip    = Mip;
            PC.SourceW    = Math::Max(TraceW >> (Mip - 1u), 1u);
            PC.SourceH    = Math::Max(TraceH >> (Mip - 1u), 1u);
            PC.DestW      = Math::Max(TraceW >> Mip, 1u);
            PC.DestH      = Math::Max(TraceH >> Mip, 1u);

            DispatchCompute(CL, PyramidCS, PC, RenderUtils::GetGroupCount(PC.DestW, 8u), RenderUtils::GetGroupCount(PC.DestH, 8u), 1u);
            ComputeToCompute();
        }

        {
            struct FTracePushConstants
            {
                uint32 PyramidIndex;
                uint32 SurfaceIndex;
                uint32 SceneColorIndex;
                uint32 OutColorUAV;

                uint32 OutMipUAV;
                uint32 TraceW;
                uint32 TraceH;
                uint32 MipCount;

                uint32 MaxIterations;
                float  DepthTolerance;
                float  FadeIn;
                float  FadeOut;

                float  MaxRoughness;
                uint32 DepthIndex;
                uint32 ScreenW;
                uint32 ScreenH;

                uint32 Scale;
                uint32 _Pad0;
                uint32 _Pad1;
                uint32 _Pad2;
            } PC = {};
            static_assert(sizeof(FTracePushConstants) == 80, "FTracePushConstants must match ScreenSpaceReflections.slang FSSRTraceArgs.");

            PC.PyramidIndex    = (uint32)Pyramid.GetResourceID();
            PC.SurfaceIndex    = (uint32)Surface.GetResourceID();
            PC.SceneColorIndex = (uint32)HDR.GetResourceID();
            PC.OutColorUAV     = (uint32)Chain.GetMipUAVIndex(0);
            PC.OutMipUAV       = (uint32)MipLevel.GetMipUAVIndex(0);
            PC.TraceW          = TraceW;
            PC.TraceH          = TraceH;
            PC.MipCount        = MipCount;
            PC.MaxIterations   = GetSSRMaxIterations();
            PC.DepthTolerance  = Math::Max(RS->SSRDepthTolerance, 0.01f);
            PC.FadeIn          = Math::Max(RS->SSRFadeIn, 0.0f);
            PC.FadeOut         = Math::Max(RS->SSRFadeOut, 0.0f);
            PC.MaxRoughness    = Math::Clamp(RS->SSRMaxRoughness, 0.0f, 1.0f);
            PC.DepthIndex      = (uint32)Depth.GetResourceID();
            PC.ScreenW         = ScreenW;
            PC.ScreenH         = ScreenH;
            PC.Scale           = Scale;

            DispatchCompute(CL, TraceCS, PC, RenderUtils::GetGroupCount(TraceW, 8u), RenderUtils::GetGroupCount(TraceH, 8u), 1u);
            ComputeToCompute();
        }

        for (uint32 Mip = 1; Mip < MipCount; ++Mip)
        {
            FChainLevelPushConstants PC = {};
            PC.ChainIndex = (uint32)Chain.GetResourceID();
            PC.SourceMip  = Mip - 1u;
            PC.DestUAV    = (uint32)Chain.GetMipUAVIndex(Mip);
            PC.DestMip    = Mip;
            PC.SourceW    = Math::Max(TraceW >> (Mip - 1u), 1u);
            PC.SourceH    = Math::Max(TraceH >> (Mip - 1u), 1u);
            PC.DestW      = Math::Max(TraceW >> Mip, 1u);
            PC.DestH      = Math::Max(TraceH >> Mip, 1u);

            DispatchCompute(CL, FilterCS, PC, RenderUtils::GetGroupCount(PC.DestW, 8u), RenderUtils::GetGroupCount(PC.DestH, 8u), 1u);
            ComputeToCompute();
        }

        {
            struct FResolvePushConstants
            {
                uint32 GBufferAIndex;
                uint32 GBufferBIndex;
                uint32 GBufferCIndex;
                uint32 GBufferDIndex;

                uint32 DepthIndex;
                uint32 SlotImageIndex;
                uint32 PyramidIndex;
                uint32 SurfaceIndex;

                uint32 ChainIndex;
                uint32 MipLevelIndex;
                uint32 HDRUAV;
                uint32 Scale;

                uint32 ScreenW;
                uint32 ScreenH;
                uint32 TraceW;
                uint32 TraceH;

                float  Intensity;
                float  MaxRoughness;
                uint32 _Pad1;
                uint32 _Pad2;
            } PC = {};
            static_assert(sizeof(FResolvePushConstants) == 80, "FResolvePushConstants must match SSRResolve.slang FSSRResolveArgs.");

            PC.GBufferAIndex  = GBufferA;
            PC.GBufferBIndex  = GBufferB;
            PC.GBufferCIndex  = GBufferC;
            PC.GBufferDIndex  = GBufferD;
            PC.DepthIndex     = (uint32)Depth.GetResourceID();
            PC.SlotImageIndex = SlotImage;
            PC.PyramidIndex   = (uint32)Pyramid.GetResourceID();
            PC.SurfaceIndex   = (uint32)Surface.GetResourceID();
            PC.ChainIndex     = (uint32)Chain.GetResourceID();
            PC.MipLevelIndex  = (uint32)MipLevel.GetResourceID();
            PC.HDRUAV         = (uint32)HDRUAV;
            PC.Scale          = Scale;
            PC.ScreenW        = ScreenW;
            PC.ScreenH        = ScreenH;
            PC.TraceW         = TraceW;
            PC.TraceH         = TraceH;
            PC.Intensity      = Math::Clamp(RS->SSRIntensity, 0.0f, 1.0f);
            PC.MaxRoughness   = Math::Clamp(RS->SSRMaxRoughness, 0.0f, 1.0f);

            DispatchCompute(CL, ResolveCS, PC, RenderUtils::GetGroupCount(ScreenW, 8u), RenderUtils::GetGroupCount(ScreenH, 8u), 1u);
            ComputeToCompute();
        }
    }

    bool FDefaultSceneRenderer::AerialPerspectivePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        AtmosphereTerms.AerialInScatterIndex     = Constants::kIndexNoneU32;
        AtmosphereTerms.AerialTransmittanceIndex = Constants::kIndexNoneU32;

        if (!FrameFlags.bHasEnvironment
            || !Frame.Volumetrics.bAerialPerspective
            || Frame.Volumetrics.AerialIntensity <= 0.0f)
        {
            return false;
        }

        static const FShaderH LutCS = FShaderLibrary::Get("AerialPerspectiveLUT.slang");
        if (!LutCS)
        {
            return false;
        }

        LUMINA_PROFILE_SECTION_COLORED("Aerial Perspective Pass", tracy::Color::LightSkyBlue);

        const FSceneImage& InScatter     = GetNamedImage(ENamedImage::AerialInScatter);
        const FSceneImage& Transmittance = GetNamedImage(ENamedImage::AerialTransmittance);

        if (!InScatter.IsValid() || !Transmittance.IsValid())
        {
            return false;
        }

        const float Range = Math::Max(Frame.Volumetrics.AerialRange, 100.0f);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(LutCS));

        FAerialLUTPushConstants LutPC = {};
        LutPC.EnvAddr          = RHI::CopyTransient(Frame.Volumetrics.EnvironmentParams);
        LutPC.InScatterUAV     = (uint32)InScatter.GetMipUAVIndex(0);
        LutPC.TransmittanceUAV = (uint32)Transmittance.GetMipUAVIndex(0);
        LutPC.Range            = Range;

        if (Frame.Lighting.LightData.bHasSun)
        {
            LutPC.SunDirection = Math::Normalize(Frame.Lighting.LightData.SkySunDirection);
        }
        else
        {
            LutPC.SunDirection = Math::Normalize(FVector3(0.3f, 0.8f, 0.4f));
        }

        const uint32 LutGroups = RenderUtils::GetGroupCount(kAerialLUTSize, AerialTileSize);
        RHI::CmdDispatch(CL, MakeArgs(LutPC), LutGroups, LutGroups, 1);

        // Only AtmosphereCompositePass reads the LUT, and it is a dispatch.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        AtmosphereTerms.AerialInScatterIndex     = (uint32)InScatter.GetResourceID();
        AtmosphereTerms.AerialTransmittanceIndex = (uint32)Transmittance.GetResourceID();
        AtmosphereTerms.AerialRange              = Range;
        AtmosphereTerms.AerialIntensity          = Frame.Volumetrics.AerialIntensity;
        return true;
    }

    namespace
    {
        constexpr uint32 kMaxTerrainShadowRecords = 8u;
        constexpr uint32 kTerrainShadowSteps      = 48u;

        // How far past a receiver a distance-field caster can still shade it.
        constexpr float  kSunDFMaxTrace           = 1000.0f;

        // Mirror of FSunDFObject in SunDFShadowCommon.slang.
        struct FSunDFObject
        {
            FTransform3x4 WorldToLocal;
            FVector4      Sphere;
            FVector3      VolumeMin;
            float         MaxDistance;
            FVector3      VolumeSize;
            float         MinScale;
            uint32        TextureIndex;
            uint32        Flags;
            uint32        _Pad0;
            uint32        _Pad1;
        };
        static_assert(sizeof(FSunDFObject) == 112);

        // Mirror of FSunDFShadowGrid in SunDFShadowCommon.slang.
        struct FSunDFShadowGrid
        {
            RHI::TGPUSpan<FSunDFObject> Objects;
            RHI::TGPUSpan<uint32>       CellCounts;
            RHI::TGPUSpan<uint32>       CellItems;
            RHI::TGPUSpan<uint32>       Counters;
            FVector3                    SunRight;
            float                       CellSize;
            FVector3                    SunUp;
            float                       MinRadius;
            FVector2                    Origin;
            uint32                      Dimension;
            uint32                      CellCapacity;
        };
        static_assert(sizeof(FSunDFShadowGrid) == 112);

        // Mirror of FPushConstants in SunDFShadowCull.slang.
        struct FSunDFShadowCullArgs
        {
            RHI::TGPUSpan<uint32>             Blocks;
            RHI::TGPUSpan<FInstanceCullEntry> RetainedCullEntries;
            RHI::TGPUSpan<FTransform3x4>      RetainedTransforms;
            RHI::TGPUSpan<FInstanceStatic>    RetainedStatic;
            FSunDFShadowGrid                  Grid;
        };

        // Mirror of FTerrainShadowRecord in SunFarShadow.slang.
        struct FTerrainShadowRecord
        {
            FVector2 OriginXZ;
            float    Stride;
            float    OriginY;
            float    MaxHeight;
            float    Resolution;
            uint32   HeightmapIndex;
            float    _Pad;
        };
        static_assert(sizeof(FTerrainShadowRecord) == 32);

        // Mirror of FPushConstants in SunFarShadow.slang.
        struct FSunFarShadowArgs
        {
            uint32   ScreenSize[2];
            uint32   DepthIndex;
            uint32   OutputUAV;
            FVector3 ToSun;
            float    TanHalfAngle;
            float    TerrainMaxDistance;
            uint32   TerrainSteps;
            uint32   NumTerrains;
            float    _Pad;
            FSunDFShadowGrid DFGrid;
            float    DFNear;
            float    DFFar;
            float    DFMaxTrace;
            uint32   bDistanceField;
            FTerrainShadowRecord Terrains[kMaxTerrainShadowRecords];
        };
    }

    bool FDefaultSceneRenderer::WantsSunFarShadowMask() const
    {
        if (RenderFrame == nullptr || !RenderFrame->Lighting.LightData.bHasSun)
        {
            return false;
        }
        const FFrameData& Frame = *RenderFrame;
        if (Frame.Lighting.SunFarShadow.bDistanceField)
        {
            return true;
        }
        if (!Frame.Lighting.SunFarShadow.bTerrain)
        {
            return false;
        }
        for (const FFrameData::FTerrainExtract& Terrain : Frame.Extracts.TerrainExtracts)
        {
            if (Terrain.bCastShadow)
            {
                return true;
            }
        }
        return false;
    }

    bool FDefaultSceneRenderer::SunFarShadowPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const FSceneImage& Mask  = GetNamedImage(ENamedImage::SunFarShadowMask);
        if (!WantsSunFarShadowMask() || !Mask.IsValid())
        {
            return false;
        }

        static const FShaderH TraceCS = FShaderLibrary::Get("SunFarShadow.slang");
        const FSceneImage& Depth = GetNamedImage(ENamedImage::DepthAttachment);
        const int32 OutputSlot   = Mask.GetMipUAVIndex(0);
        if (!TraceCS || Depth.GetResourceID() < 0 || OutputSlot < 0)
        {
            return false;
        }

        const auto& Settings = Frame.Lighting.SunFarShadow;

        FSunFarShadowArgs Args = {};
        Args.ScreenSize[0]      = Mask.GetSizeX();
        Args.ScreenSize[1]      = Mask.GetSizeY();
        Args.DepthIndex         = (uint32)Depth.GetResourceID();
        Args.OutputUAV          = (uint32)OutputSlot;
        Args.ToSun              = Settings.ToSun;
        Args.TanHalfAngle       = Settings.TanHalfAngle;
        Args.TerrainMaxDistance = Settings.TerrainDistance;
        Args.TerrainSteps       = kTerrainShadowSteps;
        Args.DFNear             = Settings.DFNear;
        Args.DFFar              = Settings.DFFar;
        Args.DFMaxTrace         = kSunDFMaxTrace;
        Args.bDistanceField     = (Settings.bDistanceField && bSunDFGridReady) ? 1u : 0u;
        if (Args.bDistanceField != 0u)
        {
            Args.DFGrid.Objects      = { SunDFObjectBuffer, kSunDFMaxObjects };
            Args.DFGrid.CellCounts   = { SunDFCellCountBuffer, kSunDFGridDimension * kSunDFGridDimension };
            Args.DFGrid.CellItems    = { SunDFCellItemBuffer, kSunDFGridDimension * kSunDFGridDimension * kSunDFCellCapacity };
            Args.DFGrid.Counters     = { SunDFCounterBuffer, 4u };
            Args.DFGrid.SunRight     = Settings.SunRight;
            Args.DFGrid.CellSize     = Settings.GridCellSize;
            Args.DFGrid.SunUp        = Settings.SunUp;
            Args.DFGrid.MinRadius    = Settings.DFMinRadius;
            Args.DFGrid.Origin       = Settings.GridOrigin;
            Args.DFGrid.Dimension    = kSunDFGridDimension;
            Args.DFGrid.CellCapacity = kSunDFCellCapacity;
        }

        // Every casting terrain, not only those in view, since a ridge behind the camera still shades what is in front.
        for (const FFrameData::FTerrainExtract& Terrain : Frame.Extracts.TerrainExtracts)
        {
            if (!Terrain.bCastShadow || Args.NumTerrains >= kMaxTerrainShadowRecords || Terrain.Resolution < 2)
            {
                continue;
            }
            const auto StateIt = TerrainGPUStates.find(Terrain.Entity);
            if (StateIt == TerrainGPUStates.end() || !StateIt->second.HeightmapTexture)
            {
                continue;
            }

            const FVector3 Origin = FVector3(Terrain.WorldMatrix[3]);
            const float    Half   = Terrain.TileWorldSize * 0.5f;

            FTerrainShadowRecord& Record = Args.Terrains[Args.NumTerrains++];
            Record.OriginXZ       = FVector2(Origin.x - Half, Origin.z - Half);
            Record.Stride         = Terrain.TileWorldSize / (float)(Terrain.Resolution - 1);
            Record.OriginY        = Origin.y;
            Record.MaxHeight      = Terrain.MaxHeight;
            Record.Resolution     = (float)Terrain.Resolution;
            Record.HeightmapIndex = (uint32)StateIt->second.HeightmapTexture.GetResourceID();
        }

        // Runs even with nothing to trace yet, since the lighting reads whatever this leaves in the mask.
        DispatchCompute(CL, TraceCS, Args,
            RenderUtils::GetGroupCount(Args.ScreenSize[0], 8u),
            RenderUtils::GetGroupCount(Args.ScreenSize[1], 8u), 1);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderRead);
        return true;
    }

    bool FDefaultSceneRenderer::SunDFShadowCullPass(RHI::FCmdListH CL)
    {
        bSunDFGridReady = false;
        const FFrameData& Frame = *RenderFrame;
        const auto& Settings = Frame.Lighting.SunFarShadow;
        if (!WantsSunFarShadowMask() || !Settings.bDistanceField)
        {
            return false;
        }

        static const FShaderH CullCS = FShaderLibrary::Get("SunDFShadowCull.slang");
        const uint32 RetainedSlots = Frame.Geometry.RetainedUpload.SlotCount;
        const bool bRetainedFits = RetainedSlots > 0
                                && RetainedCullEntryBuffer.CapacityOf<FInstanceCullEntry>() >= RetainedSlots
                                && RetainedTransformBuffer.CapacityOf<FTransform3x4>()     >= RetainedSlots
                                && RetainedStaticBuffer.CapacityOf<FInstanceStatic>()      >= RetainedSlots;
        if (!CullCS || !bRetainedFits)
        {
            return false;
        }

        constexpr uint32 NumCells = kSunDFGridDimension * kSunDFGridDimension;
        ReserveBuffer(CL, SunDFObjectBuffer,    (uint64)kSunDFMaxObjects * sizeof(FSunDFObject), false, false);
        ReserveBuffer(CL, SunDFCellCountBuffer, (uint64)NumCells * sizeof(uint32), false, false);
        ReserveBuffer(CL, SunDFCellItemBuffer,  (uint64)NumCells * kSunDFCellCapacity * sizeof(uint32), false, false);
        ReserveBuffer(CL, SunDFCounterBuffer,   4u * sizeof(uint32), false, false);
        if (SunDFObjectBuffer.CapacityOf<FSunDFObject>() < kSunDFMaxObjects ||
            SunDFCellCountBuffer.CapacityOf<uint32>() < NumCells ||
            SunDFCellItemBuffer.CapacityOf<uint32>() < NumCells * kSunDFCellCapacity ||
            SunDFCounterBuffer.CapacityOf<uint32>() < 4u)
        {
            return false;
        }

        RHI::CmdMemzero(CL, RHI::FGPURange{ SunDFCellCountBuffer.Gpu, (uint64)NumCells * sizeof(uint32) });
        RHI::CmdMemzero(CL, RHI::FGPURange{ SunDFCounterBuffer.Gpu, 4u * sizeof(uint32) });
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        FSunDFShadowCullArgs Args = {};
        Args.RetainedCullEntries = { RetainedCullEntryBuffer, RetainedSlots };
        Args.RetainedTransforms  = { RetainedTransformBuffer, RetainedSlots };
        Args.RetainedStatic      = { RetainedStaticBuffer, RetainedSlots };
        Args.Grid.Objects        = { SunDFObjectBuffer, kSunDFMaxObjects };
        Args.Grid.CellCounts     = { SunDFCellCountBuffer, NumCells };
        Args.Grid.CellItems      = { SunDFCellItemBuffer, NumCells * kSunDFCellCapacity };
        Args.Grid.Counters       = { SunDFCounterBuffer, 4u };
        Args.Grid.SunRight       = Settings.SunRight;
        Args.Grid.CellSize       = Settings.GridCellSize;
        Args.Grid.SunUp          = Settings.SunUp;
        Args.Grid.MinRadius      = Settings.DFMinRadius;
        Args.Grid.Origin         = Settings.GridOrigin;
        Args.Grid.Dimension      = kSunDFGridDimension;
        Args.Grid.CellCapacity   = kSunDFCellCapacity;

        // Only the blocks holding a distance-field caster, unless the block table is not current.
        static_assert(kInstanceCullBlockSize == 64u, "SunDFShadowCull.slang walks a block per 64-lane group.");
        const bool bBlockList = bInstanceBlockBoundsValid && !bDistanceFieldBlocksDirty;
        if (bBlockList && DistanceFieldBlocks.empty())
        {
            bSunDFGridReady = true;
            return true;
        }
        const uint32 NumGroups = bBlockList ? (uint32)DistanceFieldBlocks.size() : RenderUtils::GetGroupCount(RetainedSlots, 64u);
        if (bBlockList)
        {
            Args.Blocks = { SunDFBlockListBuffer, (uint32)DistanceFieldBlocks.size() };
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullCS));
        // SunDFShadowCull.slang undoes the fold with GroupID.y * MAX_DISPATCH_AXIS.
        const FUIntVector2 Groups = RenderUtils::FoldGroupCount(NumGroups);
        RHI::CmdDispatch(CL, MakeArgs(Args), Groups.x, Groups.y, 1u);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead);
        bSunDFGridReady = true;
        return true;
    }
}
