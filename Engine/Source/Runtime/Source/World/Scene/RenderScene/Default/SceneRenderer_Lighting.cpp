#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "Platform/Time/PlatformTime.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
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
            if (!bWarnedDeferredBinExtent)
            {
                bWarnedDeferredBinExtent = true;
                LOG_WARN("Render extent {}x{} exceeds the 16-bit pixel-list packing; deferred shading is disabled for this view.",
                    Extent.x, Extent.y);
            }
            return false;
        }

        const auto& DeferredMaterials = Frame.Geometry.DeferredMaterials;

        BinnedDeferredSlotShaders.Reset();
        BinnedDeferredSlotKinds.Reset();
        BinnedDeferredSlotLookup.Reset();
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

        BinnedDeferredSlotByMaterial.Init(Constants::kIndexNoneU32, IntCastChecked<int32>(static_cast<int64>(MaxMaterialIndex) + 1u));
        for (const auto& M : DeferredMaterials)
        {
            if (!M.DeferredShader)
            {
                continue;
            }

            // Looked up rather than scanned; a scan cost visible materials times distinct shaders.
            uint32 Slot = Constants::kIndexNoneU32;
            if (auto It = BinnedDeferredSlotLookup.Find(M.DeferredShader.Handle); It != nullptr)
            {
                Slot = (*It);
            }

            if (Slot == Constants::kIndexNoneU32)
            {
                const bool bFull = (uint32)BinnedDeferredSlotShaders.Num() >= SlotBudget;
                if (bFull)
                {
                    if (!bWarnedDeferredSlotCap)
                    {
                        bWarnedDeferredSlotCap = true;
                        LOG_WARN("More than {} distinct deferred material shaders are visible; the excess {}.",
                            SlotBudget, FallbackShader ? "shades as the default material" : "will not shade");
                    }
                    #if USING(WITH_EDITOR)
                    // A log line scrolls away, and the symptom is surfaces quietly wearing the wrong material.
                    if (const double Now = PlatformTime::Seconds(); Threading::IsMainThread() && Now - LastDeferredSlotCapToast > 10.0)
                    {
                        LastDeferredSlotCapToast = Now;
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
                if (auto Found = BinnedDeferredSlotLookup.Find(BinShader.Handle); Found != nullptr)
                {
                    Slot = (*Found);
                }
                else
                {
                    Slot = (uint32)BinnedDeferredSlotShaders.Num();
                    BinnedDeferredSlotShaders.Add(BinShader);
                    BinnedDeferredSlotKinds.Add(0u);
                    BinnedDeferredSlotLookup.TryAdd(BinShader.Handle, Slot);
                }
            }

            BinnedDeferredSlotByMaterial[M.MaterialIndex] = Slot;
            BinnedDeferredSlotKinds[Slot] |= M.InstanceKinds;
        }

        const uint32 NumSlots = (uint32)BinnedDeferredSlotShaders.Num();
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
        // Sorted runs, each tile's inline records, the pixels of thin pairs, then the pairs past a tile's inline records.
        const uint64 TileSlotEntries = NumTiles * MATERIAL_TILE_RECORD;
        const uint64 PixelBound      = NumTiles * Math::Min<uint64>(MaxSlotsPerTile, Math::Min<uint64>(NumSlots, MATERIAL_TILE_SLOTS) * (MATERIAL_SPARSE_PIXELS - 1u));
        const uint64 OverflowBound   = NumTiles * (Math::Min<uint64>(NumSlots, MaxSlotsPerTile) - Math::Min<uint64>(NumSlots, MATERIAL_TILE_SLOTS))
                                     * MATERIAL_OVERFLOW_PAIR_WORDS;
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
        // In pairs, from the allocation rather than the request, since the classify bounds overflow writes on this.
        Layout.OverflowCapacity = (uint32)Math::Min<uint64>((GetMaterialPairList().Size - Layout.OverflowOffset) / (sizeof(uint32) * MATERIAL_OVERFLOW_PAIR_WORDS), 0xFFFFFFFFull);

        MaterialClassifyLayout = Layout;
        return true;
    }

    // Counts, prefix-sums and scatters, and emits the indirect args so the CPU never learns them.
    void FDefaultSceneRenderer::VisBufferClassifyPass(RHI::FCmdListH CL)
    {
        MaterialClassifyLayout = FMaterialClassifyLayout{};

        if (RenderFrame->Geometry.DrawCommands.IsEmpty())
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
            RHI::CopyTransientArray(BinnedDeferredSlotByMaterial.GetData(), BinnedDeferredSlotByMaterial.Num());

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
        const RHI::TGPUSpan<uint32> OverflowSpan  = RHI::TGPUSpan<uint32>::FromAddress(PairAlloc.Gpu + Layout.OverflowOffset, Layout.OverflowCapacity * MATERIAL_OVERFLOW_PAIR_WORDS);

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

        // One group scans every slot, each thread taking a contiguous run sized to the slots binned this frame.
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

        // One allocation written in place, since the blocks differ only in SlotIndex, each on a root-data boundary.
        constexpr uint64 ArgsStride = Math::AlignUp<uint64>(sizeof(FDeferredMaterialPC), RHI::kRootDataAlign);
        const RHI::FTransientAlloc ArgsAlloc =
            RHI::AllocTransient(ArgsStride * Layout.NumSlots);
        if (ArgsAlloc.Cpu == nullptr)
        {
            LOG_ERROR("Deferred material pass: could not stage {} argument blocks; skipping the pass.",
                Layout.NumSlots);
            return;
        }

        for (uint32 Slot = 0; Slot < Layout.NumSlots; ++Slot)
        {
            FDeferredMaterialPC* Block = (FDeferredMaterialPC*)((uint8*)ArgsAlloc.Cpu + Slot * ArgsStride);
            *Block           = PC;
            Block->SlotIndex = Slot;
        }

        const uint64 bVelocity = PC.VelocityUAV != RHI::kInvalidHeapSlot ? 1u : 0u;

        for (uint32 Slot = 0; Slot < Layout.NumSlots; ++Slot)
        {
            // Specialized per slot on velocity and vertex kinds, since each unused path costs about a third of the registers.
            const uint8  Kinds   = Slot < (uint32)BinnedDeferredSlotKinds.Num() ? BinnedDeferredSlotKinds[Slot] : 3u;
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
            RHI::CmdDispatchIndirect(CL, ArgsAlloc.Gpu + Slot * ArgsStride, Classify.Skip(Layout.MaterialArgsOffset + Slot * (uint32)sizeof(RHI::FDispatchIndirectArguments)));
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
    }

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
        for (uint32 Index = 0; Index < NumLiveLights && Index < (uint32)RenderFrame->Lighting.Lights.Num(); ++Index)
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

    // Weighted blended OIT (McGuire and Bavoil 2013), one pass into a weighted sum and a revealage product.
    void FDefaultSceneRenderer::TransparentPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const auto& TranslucentDrawList = Frame.Geometry.TranslucentDrawList;

        if (TranslucentDrawList.IsEmpty())
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
                Key.ColorTargets.Emplace(Accum.Desc.Format, AccumBlend);
                Key.ColorTargets.Emplace(Revealage.Desc.Format, RevealageBlend);
                #if USING(WITH_EDITOR)
                Key.ColorTargets.Add({ Picker.Desc.Format, {} });
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

        if (TranslucentDrawList.IsEmpty())
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
        Key.ColorTargets.Emplace(HDR.Desc.Format, CompositeBlend);
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
                Key.ColorTargets.Emplace(HDR.Desc.Format, Batch.bModulate ? ModulateBlend : AdditiveBlend);
                #if USING(WITH_EDITOR)
                Key.ColorTargets.Add({ Picker.Desc.Format, {} });
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

    void FDefaultSceneRenderer::WaterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        const TVector<FGPUWater>& Waters = Frame.Water.Surfaces;
        if (Waters.IsEmpty())
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
        Key.ColorTargets.Emplace(HDR.Desc.Format, WaterBlend);
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
        PC.Waters          = RHI::CopyTransientArray(Waters.GetData(), Waters.Num());
        PC.SceneColorIndex = (uint32)SceneColor.GetResourceID();
        PC.AerialInScatterIndex     = AtmosphereTerms.AerialInScatterIndex;
        PC.AerialTransmittanceIndex = AtmosphereTerms.AerialTransmittanceIndex;
        PC.AerialRange              = AtmosphereTerms.AerialRange;
        PC.AerialIntensity          = AtmosphereTerms.AerialIntensity;
        PC.SceneDepthIndex = (uint32)SceneDepthCopy.GetResourceID();

        const RHI::GPUPtr Args = MakeArgs(PC);

        for (int32 i = 0; i < Waters.Num(); ++i)
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
        Key.ColorTargets.Add({ HDR.Desc.Format, {} });
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
}
