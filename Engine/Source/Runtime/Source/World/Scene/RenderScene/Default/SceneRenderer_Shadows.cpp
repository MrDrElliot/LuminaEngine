#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
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
            float         VoxelSize;
            uint32        _Pad0;
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
            uint32                      _Pad0;
        };
        static_assert(sizeof(FSunDFShadowGrid) == 112);

        // Mirror of FPushConstants in SunDFShadowCull.slang.
        struct FSunDFShadowCullArgs
        {
            RHI::TGPUSpan<uint32>             Blocks;
            RHI::TGPUSpan<FInstanceCullEntry> RetainedCullEntries;
            RHI::TGPUSpan<FTransform3x4>      RetainedTransforms;
            RHI::TGPUSpan<FInstanceStatic>    RetainedStatic;
            RHI::TGPUSpan<uint32>             LargeObjects;
            FSunDFShadowGrid                  Grid;
        };

        // Mirror of FPushConstants in SunDFShadowBin.slang.
        struct FSunDFShadowBinArgs
        {
            RHI::TGPUSpan<uint32> LargeObjects;
            FSunDFShadowGrid      Grid;
            uint32                bScatter;
        };

        // Mirror of FPushConstants in SunDFShadowScan.slang.
        struct FSunDFShadowScanArgs
        {
            RHI::TGPUSpan<uint32> CellCounts;
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

        // Mirror of FPushConstants in SunContactShadow.slang.
        struct FSunContactShadowArgs
        {
            uint32   ScreenSize[2];
            uint32   DepthIndex;
            uint32   GBufferBIndex;
            FVector3 ToSun;
            float    Length;
            uint32   SlotImageIndex;
            uint32   FarMaskIndex;
            uint32   OutputUAV;
            uint32   Steps;
            float    MinPixels;
            uint32   _Pad[3];
        };
        static_assert(sizeof(FSunContactShadowArgs) == 64);

        // Past this distance the reach covers too few pixels for the march to find a gap the cascades miss.
        constexpr float kSunContactMinPixels = 2.0f;
    }

    bool FDefaultSceneRenderer::WantsSunFarShadowMask() const
    {
        if (RenderFrame == nullptr || !RenderFrame->Lighting.LightData.bHasSun)
        {
            return false;
        }
        return RenderFrame->Lighting.SunFarShadow.ContactSteps > 0u || WantsSunFarShadowTrace();
    }

    bool FDefaultSceneRenderer::WantsSunFarShadowTrace() const
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
        if (!WantsSunFarShadowTrace() || !Mask.IsValid())
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
            Args.DFGrid.CellCounts   = { SunDFCellCountBuffer, kSunDFGridDimension * kSunDFGridDimension + 1u };
            Args.DFGrid.CellItems    = { SunDFCellItemBuffer, kSunDFGridDimension * kSunDFGridDimension * kSunDFCellCapacity };
            Args.DFGrid.Counters     = { SunDFCounterBuffer, 4u };
            Args.DFGrid.SunRight     = Settings.SunRight;
            Args.DFGrid.CellSize     = Settings.GridCellSize;
            Args.DFGrid.SunUp        = Settings.SunUp;
            Args.DFGrid.MinRadius    = Settings.DFMinRadius;
            Args.DFGrid.Origin       = Settings.GridOrigin;
            Args.DFGrid.Dimension    = kSunDFGridDimension;
        }

        // Every casting terrain, not only those in view, since a ridge behind the camera still shades what is in front.
        for (const FFrameData::FTerrainExtract& Terrain : Frame.Extracts.TerrainExtracts)
        {
            if (!Terrain.bCastShadow || Args.NumTerrains >= kMaxTerrainShadowRecords || Terrain.Resolution < 2)
            {
                continue;
            }
            const auto StateIt = TerrainGPUStates.Find(Terrain.Entity);
            if (StateIt == nullptr || !StateIt->HeightmapTexture)
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
            Record.HeightmapIndex = (uint32)StateIt->HeightmapTexture.GetResourceID();
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

    // Runs after the GBuffer for its opt-out bit, and multiplies into whatever the far trace left.
    void FDefaultSceneRenderer::SunContactShadowPass(RHI::FCmdListH CL)
    {
        const auto& Settings    = RenderFrame->Lighting.SunFarShadow;
        const FSceneImage& Mask = GetNamedImage(ENamedImage::SunFarShadowMask);
        if (Settings.ContactSteps == 0u || !WantsSunFarShadowMask() || !Mask.IsValid())
        {
            return;
        }

        static const FShaderH ContactCS = FShaderLibrary::Get("SunContactShadow.slang");
        const int32 OutputSlot = Mask.GetMipUAVIndex(0);
        if (!ContactCS || OutputSlot < 0)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Sun Contact Shadows");

        FSunContactShadowArgs Args = {};
        Args.ScreenSize[0]  = Mask.GetSizeX();
        Args.ScreenSize[1]  = Mask.GetSizeY();
        Args.DepthIndex     = (uint32)GetNamedImage(ENamedImage::DepthAttachment).GetResourceID();
        Args.GBufferBIndex  = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        Args.ToSun          = Settings.ToSun;
        Args.Length         = Settings.ContactLength;
        Args.SlotImageIndex = (uint32)GetNamedImage(ENamedImage::MaterialSlot).GetResourceID();
        Args.FarMaskIndex   = WantsSunFarShadowTrace() ? (uint32)Mask.GetResourceID() : RHI::kInvalidHeapSlot;
        Args.OutputUAV      = (uint32)OutputSlot;
        Args.Steps          = Settings.ContactSteps;
        Args.MinPixels      = kSunContactMinPixels;

        DispatchCompute(CL, ContactCS, Args,
            RenderUtils::GetGroupCount(Args.ScreenSize[0], 8u),
            RenderUtils::GetGroupCount(Args.ScreenSize[1], 8u), 1);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderRead);
    }

    bool FDefaultSceneRenderer::SunDFShadowCullPass(RHI::FCmdListH CL)
    {
        bSunDFGridReady = false;
        const FFrameData& Frame = *RenderFrame;
        const auto& Settings = Frame.Lighting.SunFarShadow;
        if (!WantsSunFarShadowTrace() || !Settings.bDistanceField)
        {
            return false;
        }

        static const FShaderH CullCS = FShaderLibrary::Get("SunDFShadowCull.slang");
        static const FShaderH BinCS  = FShaderLibrary::Get("SunDFShadowBin.slang");
        static const FShaderH ScanCS = FShaderLibrary::Get("SunDFShadowScan.slang");
        const uint32 RetainedSlots = Frame.Geometry.RetainedUpload.SlotCount;
        const bool bRetainedFits = RetainedSlots > 0
                                && RetainedCullEntryBuffer.CapacityOf<FInstanceCullEntry>() >= RetainedSlots
                                && RetainedTransformBuffer.CapacityOf<FTransform3x4>()     >= RetainedSlots
                                && RetainedStaticBuffer.CapacityOf<FInstanceStatic>()      >= RetainedSlots;
        if (!CullCS || !BinCS || !ScanCS || !bRetainedFits)
        {
            return false;
        }

        constexpr uint32 NumCells = kSunDFGridDimension * kSunDFGridDimension;
        constexpr uint32 NumItems = NumCells * kSunDFCellCapacity;
        // The extra slot past the last cell is where the scan leaves the total.
        constexpr uint32 NumCellSlots = NumCells + 1u;

        FSceneBuffer& ItemTotalReadback = SunDFItemTotalReadback[CurrentFrameSlot];
        if (const uint32* ItemTotal = ItemTotalReadback.CpuAs<const uint32>(); ItemTotal != nullptr && *ItemTotal > NumItems)
        {
            LOG_WARN_ONCE("Renderer: the sun's distance-field grid needed {} cell items but holds {}, so far shadows lose casters.", *ItemTotal, NumItems);
        }
        if (ItemTotalReadback.Gpu == 0)
        {
            ItemTotalReadback.Adopt(RHI::Malloc(sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::CPURead));
            if (void* Host = ItemTotalReadback.Cpu)
            {
                RHI::SetDebugName(ItemTotalReadback.Gpu, ItemTotalReadback.DebugName);
                Memory::Memzero(Host, sizeof(uint32));
            }
        }

        ReserveBuffer(CL, SunDFObjectBuffer,    (uint64)kSunDFMaxObjects * sizeof(FSunDFObject), false, false);
        ReserveBuffer(CL, SunDFCellCountBuffer, (uint64)NumCellSlots * sizeof(uint32), false, false);
        ReserveBuffer(CL, SunDFCellItemBuffer,  (uint64)NumItems * sizeof(uint32), false, false);
        ReserveBuffer(CL, SunDFCounterBuffer,   4u * sizeof(uint32), false, false);
        ReserveBuffer(CL, SunDFLargeObjectBuffer, (uint64)kSunDFMaxLargeCasters * sizeof(uint32), false, false);
        if (SunDFObjectBuffer.CapacityOf<FSunDFObject>() < kSunDFMaxObjects ||
            SunDFLargeObjectBuffer.CapacityOf<uint32>() < kSunDFMaxLargeCasters ||
            SunDFCellCountBuffer.CapacityOf<uint32>() < NumCellSlots ||
            SunDFCellItemBuffer.CapacityOf<uint32>() < NumItems ||
            SunDFCounterBuffer.CapacityOf<uint32>() < 4u)
        {
            return false;
        }

        RHI::CmdMemzero(CL, RHI::FGPURange{ SunDFCellCountBuffer.Gpu, (uint64)NumCellSlots * sizeof(uint32) });
        RHI::CmdMemzero(CL, RHI::FGPURange{ SunDFCounterBuffer.Gpu, 4u * sizeof(uint32) });
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        FSunDFShadowCullArgs Args = {};
        Args.RetainedCullEntries = { RetainedCullEntryBuffer, RetainedSlots };
        Args.RetainedTransforms  = { RetainedTransformBuffer, RetainedSlots };
        Args.RetainedStatic      = { RetainedStaticBuffer, RetainedSlots };
        Args.LargeObjects        = { SunDFLargeObjectBuffer, kSunDFMaxLargeCasters };
        Args.Grid.Objects        = { SunDFObjectBuffer, kSunDFMaxObjects };
        Args.Grid.CellCounts     = { SunDFCellCountBuffer, NumCellSlots };
        Args.Grid.CellItems      = { SunDFCellItemBuffer, NumItems };
        Args.Grid.Counters       = { SunDFCounterBuffer, 4u };
        Args.Grid.SunRight       = Settings.SunRight;
        Args.Grid.CellSize       = Settings.GridCellSize;
        Args.Grid.SunUp          = Settings.SunUp;
        Args.Grid.MinRadius      = Settings.DFMinRadius;
        Args.Grid.Origin         = Settings.GridOrigin;
        Args.Grid.Dimension      = kSunDFGridDimension;

        // Only the blocks holding a distance-field caster, unless the block table is not current.
        static_assert(kInstanceCullBlockSize == 64u, "SunDFShadowCull.slang walks a block per 64-lane group.");
        const bool bBlockList = bInstanceBlockBoundsValid && !bDistanceFieldBlocksDirty;
        if (bBlockList && DistanceFieldBlocks.IsEmpty())
        {
            bSunDFGridReady = true;
            return true;
        }
        const uint32 NumGroups = bBlockList ? (uint32)DistanceFieldBlocks.Num() : RenderUtils::GetGroupCount(RetainedSlots, 64u);
        if (bBlockList)
        {
            Args.Blocks = { SunDFBlockListBuffer, (uint32)DistanceFieldBlocks.Num() };
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullCS));
        // SunDFShadowCull.slang undoes the fold with GroupID.y * MAX_DISPATCH_AXIS.
        const FUIntVector2 Groups = RenderUtils::FoldGroupCount(NumGroups);
        RHI::CmdDispatch(CL, MakeArgs(Args), Groups.x, Groups.y, 1u);

        // A group per large caster, then a lane per remaining caster, counted first and scattered once the scan has placed every cell.
        constexpr uint32 BinGroups = kSunDFMaxLargeCasters + kSunDFMaxObjects / 64u;
        FSunDFShadowBinArgs BinArgs = {};
        BinArgs.LargeObjects = Args.LargeObjects;
        BinArgs.Grid         = Args.Grid;
        RHI::Barriers::ComputeToCompute(CL);
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BinCS));
        RHI::CmdDispatch(CL, MakeArgs(BinArgs), BinGroups, 1u, 1u);

        FSunDFShadowScanArgs ScanArgs = {};
        ScanArgs.CellCounts = Args.Grid.CellCounts;
        RHI::Barriers::ComputeToCompute(CL);
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ScanCS));
        RHI::CmdDispatch(CL, MakeArgs(ScanArgs), 1u, 1u, 1u);

        BinArgs.bScatter = 1u;
        RHI::Barriers::ComputeToCompute(CL);
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BinCS));
        RHI::CmdDispatch(CL, MakeArgs(BinArgs), BinGroups, 1u, 1u);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::Transfer, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::TransferRead);
        if (ItemTotalReadback.Gpu != 0)
        {
            RHI::CmdMemcpy(CL, { ItemTotalReadback.Gpu, sizeof(uint32) }, { SunDFCellCountBuffer.Gpu + NumCells * sizeof(uint32), sizeof(uint32) });
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite, RHI::EStageFlags::Host, RHI::EAccessFlags::HostRead);
        }
        bSunDFGridReady = true;
        return true;
    }
}
