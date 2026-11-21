#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace
    {
        // Covers a 4x4 open-world terrain tiling with shadow casting enabled on every tile.
        constexpr uint32 kMaxTerrainShadowRecords = 16u;
        constexpr uint32 kTerrainShadowSteps      = 48u;

        // How far past a receiver a distance-field caster can still shade it.
        constexpr float  kSunDFMaxTrace           = 1000.0f;

        // Mirror of FDFObject in DistanceFieldGrid.slang.
        struct FDFObject
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
        static_assert(sizeof(FDFObject) == 112);

        // Mirror of FDFObjectGrid in DistanceFieldGrid.slang.
        struct FDFObjectGrid
        {
            RHI::TGPUSpan<FDFObject> Objects;
            RHI::TGPUSpan<uint32>    CellCounts;
            RHI::TGPUSpan<uint32>    CellItems;
            RHI::TGPUSpan<uint32>    Counters;
            FVector3                 Right;
            float                    CellSize;
            FVector3                 Up;
            float                    MinRadius;
            FVector2                 Origin;
            uint32                   Dimension;
            float                    SpherePad;
        };
        static_assert(sizeof(FDFObjectGrid) == 112);

        // Mirror of FPushConstants in DistanceFieldGridCull.slang.
        struct FDFGridCullArgs
        {
            RHI::TGPUSpan<uint32>             Blocks;
            RHI::TGPUSpan<FInstanceCullEntry> RetainedCullEntries;
            RHI::TGPUSpan<FTransform3x4>      RetainedTransforms;
            RHI::TGPUSpan<FInstanceStatic>    RetainedStatic;
            RHI::TGPUSpan<uint32>             LargeObjects;
            FDFObjectGrid                     Grid;
        };

        // Mirror of FPushConstants in DistanceFieldGridBin.slang.
        struct FDFGridBinArgs
        {
            RHI::TGPUSpan<uint32> LargeObjects;
            FDFObjectGrid         Grid;
            uint32                bScatter;
        };

        // Mirror of FPushConstants in DistanceFieldGridScan.slang.
        struct FDFGridScanArgs
        {
            RHI::TGPUSpan<uint32> CellCounts;
        };

        // Matches kConeTanHalf in DistanceFieldAO.slang, whose cones widen by this much past the reach.
        constexpr float kDistanceFieldAOConeTanHalf = 0.55f;

        // Mirror of FPushConstants in DistanceFieldAO.slang.
        struct FDistanceFieldAOArgs
        {
            uint32        OutputSize[2];
            uint32        DepthSize[2];
            uint32        DepthIndex;
            uint32        GBufferBIndex;
            uint32        SlotImageIndex;
            uint32        OutputUAV;
            float         MaxDistance;
            float         ViewDistance;
            FDFObjectGrid Grid;
        };

        // The trace reads a built grid's spans, and a grid of no cells, which every receiver falls outside, when there is none.
        FDFObjectGrid MakeGridArgs(const FDistanceFieldGrid& Grid, const FDistanceFieldGridPlacement& Placement)
        {
            FDFObjectGrid Args = {};
            if (!Grid.bReady)
            {
                return Args;
            }
            Args.Objects    = { Grid.Objects, Grid.Objects.CapacityOf<FDFObject>() };
            Args.CellCounts = { Grid.CellCounts, Placement.Dimension * Placement.Dimension + 1u };
            Args.CellItems  = { Grid.CellItems, Grid.CellItems.CapacityOf<uint32>() };
            Args.Counters   = { Grid.Counters, 4u };
            Args.Right      = Placement.Right;
            Args.CellSize   = Placement.CellSize;
            Args.Up         = Placement.Up;
            Args.MinRadius  = Placement.MinRadius;
            Args.Origin     = Placement.Origin;
            Args.Dimension  = Placement.Dimension;
            Args.SpherePad  = Placement.SpherePad;
            return Args;
        }

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
            FDFObjectGrid    DFGrid;
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

        // Past this distance the reach covers too few pixels for the march to find a gap the shadow map misses.
        constexpr float kSunContactMinPixels = 2.0f;

        // Matches kTile in SunContactShadow.slang, whose groups resolve their ordered jitter in shared memory.
        constexpr uint32 kSunContactTile = 16u;
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

        SCENE_GPU_SCOPE(CL, "Sun Far Shadows");

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
        Args.bDistanceField     = (Settings.bDistanceField && SunDFGrid.bReady) ? 1u : 0u;
        Args.DFGrid             = MakeGridArgs(SunDFGrid, Settings.DFGrid);

        // Every casting terrain, not only those in view, since a ridge behind the camera still shades what is in front.
        uint32 NumCastingTerrains = 0u;
        for (const FFrameData::FTerrainExtract& Terrain : Frame.Extracts.TerrainExtracts)
        {
            if (!Terrain.bCastShadow || Terrain.Resolution < 2)
            {
                continue;
            }
            ++NumCastingTerrains;
            if (Args.NumTerrains >= kMaxTerrainShadowRecords)
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
        if (NumCastingTerrains > kMaxTerrainShadowRecords)
        {
            LOG_WARN_ONCE("Renderer: the sun far shadow needed {} casting terrains but can only hold {}, so far shadows lose casters.", NumCastingTerrains, kMaxTerrainShadowRecords);
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
            RenderUtils::GetGroupCount(Args.ScreenSize[0], kSunContactTile),
            RenderUtils::GetGroupCount(Args.ScreenSize[1], kSunContactTile), 1);

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderRead);
    }

    bool FDefaultSceneRenderer::SunDFShadowCullPass(RHI::FCmdListH CL)
    {
        SunDFGrid.bReady = false;
        const auto& Settings = RenderFrame->Lighting.SunFarShadow;
        if (!WantsSunFarShadowTrace() || !Settings.bDistanceField)
        {
            return false;
        }

        SCENE_GPU_SCOPE(CL, "Sun DF Shadow Cull");
        return BuildDistanceFieldGrid(CL, SunDFGrid, Settings.DFGrid);
    }

    bool FDefaultSceneRenderer::BuildDistanceFieldGrid(RHI::FCmdListH CL, FDistanceFieldGrid& Grid, const FDistanceFieldGridPlacement& Placement)
    {
        Grid.bReady = false;
        const FFrameData& Frame = *RenderFrame;

        static const FShaderH CullCS = FShaderLibrary::Get("DistanceFieldGridCull.slang");
        static const FShaderH BinCS  = FShaderLibrary::Get("DistanceFieldGridBin.slang");
        static const FShaderH ScanCS = FShaderLibrary::Get("DistanceFieldGridScan.slang");
        const uint32 RetainedSlots = Frame.Geometry.RetainedUpload.SlotCount;
        const bool bRetainedFits = RetainedSlots > 0
                                && RetainedCullEntryBuffer.CapacityOf<FInstanceCullEntry>() >= RetainedSlots
                                && RetainedTransformBuffer.CapacityOf<FTransform3x4>()     >= RetainedSlots
                                && RetainedStaticBuffer.CapacityOf<FInstanceStatic>()      >= RetainedSlots;
        if (!CullCS || !BinCS || !ScanCS || !bRetainedFits || Placement.Dimension == 0u)
        {
            return false;
        }

        const uint32 NumCells = Placement.Dimension * Placement.Dimension;
        const uint32 MinItems = NumCells * kDFGridCellCapacity;
        // The extra slot past the last cell is where the scan leaves the total.
        const uint32 NumCellSlots = NumCells + 1u;

        // Holds the scan's item total, then the cull's caster and large caster counts.
        constexpr uint32 NumDemands = 3u;
        FSceneBuffer& DemandReadback = Grid.DemandReadback[CurrentFrameSlot];
        const uint32* Demand = DemandReadback.CpuAs<const uint32>();
        const uint32 ItemsWanted   = Demand != nullptr ? Math::Max(Demand[0], MinItems) : MinItems;
        const uint32 ObjectsWanted = Demand != nullptr ? Math::Max(Demand[1], kDFGridMinObjects) : kDFGridMinObjects;
        if (Demand != nullptr && Demand[2] > kDFGridMaxLargeCasters)
        {
            LOG_WARN_ONCE("Renderer: the {} distance-field grid needed {} large casters but holds {}, so it loses casters.", Grid.Objects.DebugName, Demand[2], kDFGridMaxLargeCasters);
        }
        if (DemandReadback.Gpu == 0)
        {
            DemandReadback.Adopt(RHI::Malloc(NumDemands * sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::CPURead));
            if (void* Host = DemandReadback.Cpu)
            {
                RHI::SetDebugName(DemandReadback.Gpu, DemandReadback.DebugName);
                Memory::Memzero(Host, NumDemands * sizeof(uint32));
            }
        }

        ReserveBuffer(CL, Grid.Objects,      (uint64)ObjectsWanted * sizeof(FDFObject), false, false);
        ReserveBuffer(CL, Grid.CellCounts,   (uint64)NumCellSlots * sizeof(uint32), false, false);
        ReserveBuffer(CL, Grid.CellItems,    (uint64)ItemsWanted * sizeof(uint32), false, false);
        ReserveBuffer(CL, Grid.Counters,     4u * sizeof(uint32), false, false);
        ReserveBuffer(CL, Grid.LargeObjects, (uint64)kDFGridMaxLargeCasters * sizeof(uint32), false, false);
        const uint32 NumItems   = Grid.CellItems.CapacityOf<uint32>();
        const uint32 NumObjects = Grid.Objects.CapacityOf<FDFObject>();
        if (NumObjects < kDFGridMinObjects ||
            Grid.LargeObjects.CapacityOf<uint32>() < kDFGridMaxLargeCasters ||
            Grid.CellCounts.CapacityOf<uint32>() < NumCellSlots ||
            NumItems < MinItems ||
            Grid.Counters.CapacityOf<uint32>() < 4u)
        {
            return false;
        }
        if (NumItems < ItemsWanted)
        {
            LOG_WARN_ONCE("Renderer: the {} distance-field grid needed {} cell items but could only grow to {}, so it loses casters.", Grid.Objects.DebugName, ItemsWanted, NumItems);
        }
        if (NumObjects < ObjectsWanted)
        {
            LOG_WARN_ONCE("Renderer: the {} distance-field grid needed {} casters but could only grow to {}, so it loses casters.", Grid.Objects.DebugName, ObjectsWanted, NumObjects);
        }

        RHI::CmdMemzero(CL, RHI::FGPURange{ Grid.CellCounts.Gpu, (uint64)NumCellSlots * sizeof(uint32) });
        RHI::CmdMemzero(CL, RHI::FGPURange{ Grid.Counters.Gpu, 4u * sizeof(uint32) });
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

        // Spans sized to what was reserved, which MakeGridArgs reads back once the grid is ready.
        Grid.bReady = true;
        FDFGridCullArgs Args = {};
        Args.RetainedCullEntries = { RetainedCullEntryBuffer, RetainedSlots };
        Args.RetainedTransforms  = { RetainedTransformBuffer, RetainedSlots };
        Args.RetainedStatic      = { RetainedStaticBuffer, RetainedSlots };
        Args.LargeObjects        = { Grid.LargeObjects, kDFGridMaxLargeCasters };
        Args.Grid                = MakeGridArgs(Grid, Placement);
        Grid.bReady = false;

        // Only the blocks holding a distance-field caster, unless the block table is not current.
        static_assert(kInstanceCullBlockSize == 64u, "DistanceFieldGridCull.slang walks a block per 64-lane group.");
        const bool bBlockList = bInstanceBlockBoundsValid && !bDistanceFieldBlocksDirty;
        if (bBlockList && DistanceFieldBlocks.IsEmpty())
        {
            Grid.bReady = true;
            return true;
        }
        const uint32 NumGroups = bBlockList ? (uint32)DistanceFieldBlocks.Num() : RenderUtils::GetGroupCount(RetainedSlots, 64u);
        if (bBlockList)
        {
            Args.Blocks = { DistanceFieldBlockListBuffer, (uint32)DistanceFieldBlocks.Num() };
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullCS));
        // DistanceFieldGridCull.slang undoes the fold with GroupID.y * MAX_DISPATCH_AXIS.
        const FUIntVector2 Groups = RenderUtils::FoldGroupCount(NumGroups);
        RHI::CmdDispatch(CL, MakeArgs(Args), Groups.x, Groups.y, 1u);

        // A group per large caster, then a lane per remaining caster, counted first and scattered once the scan has placed every cell.
        const uint32 BinGroups = kDFGridMaxLargeCasters + RenderUtils::GetGroupCount(NumObjects, 64u);
        FDFGridBinArgs BinArgs = {};
        BinArgs.LargeObjects = Args.LargeObjects;
        BinArgs.Grid         = Args.Grid;
        RHI::Barriers::ComputeToCompute(CL);
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(BinCS));
        RHI::CmdDispatch(CL, MakeArgs(BinArgs), BinGroups, 1u, 1u);

        FDFGridScanArgs ScanArgs = {};
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
        if (DemandReadback.Gpu != 0)
        {
            RHI::CmdMemcpy(CL, { DemandReadback.Gpu, sizeof(uint32) }, { Grid.CellCounts.Gpu + NumCells * sizeof(uint32), sizeof(uint32) });
            RHI::CmdMemcpy(CL, { DemandReadback.Gpu + sizeof(uint32), 2u * sizeof(uint32) }, { Grid.Counters.Gpu, 2u * sizeof(uint32) });
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite, RHI::EStageFlags::Host, RHI::EAccessFlags::HostRead);
        }
        Grid.bReady = true;
        return true;
    }

    bool FDefaultSceneRenderer::WantsDistanceFieldAO(const FSceneView& View) const
    {
        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        return Settings != nullptr && Settings->bDistanceFieldAO && View.bIsPrimary;
    }

    bool FDefaultSceneRenderer::IsDistanceFieldAOTraced() const
    {
        return CurrentView != nullptr && WantsDistanceFieldAO(*CurrentView) && HasVisBufferGeometry()
            && GetNamedImage(ENamedImage::DistanceFieldAO).IsValid() && GetNamedImage(ENamedImage::DistanceFieldAOTrace).IsValid();
    }

    void FDefaultSceneRenderer::DistanceFieldAOPass(RHI::FCmdListH CL)
    {
        SkyDFGrid.bReady = false;
        if (!IsDistanceFieldAOTraced())
        {
            return;
        }

        static const FShaderH TraceCS    = FShaderLibrary::Get("DistanceFieldAO.slang");
        static const FShaderH UpsampleCS = FShaderLibrary::Get("GTAOUpsample.slang");
        const FSceneImage& Depth  = GetNamedImage(ENamedImage::DepthAttachment);
        const FSceneImage& Trace  = GetNamedImage(ENamedImage::DistanceFieldAOTrace);
        const FSceneImage& Output = GetNamedImage(ENamedImage::DistanceFieldAO);
        const int32 TraceSlot = Trace.GetMipUAVIndex(0);
        if (!TraceCS || !UpsampleCS || Depth.GetResourceID() < 0 || TraceSlot < 0)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Distance Field AO");

        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        const float MaxDistance  = Math::Max(Settings->DistanceFieldAOMaxDistance, 0.5f);
        const float ViewDistance = Math::Max(Settings->DistanceFieldAOViewDistance, 10.0f);

        // Looking straight down on the camera, every caster binned over its reach so a receiver's own cell holds all that can occlude it.
        const FVector3 Camera = RenderFrame->ViewVolume.GetViewPosition();
        FDistanceFieldGridPlacement Placement;
        Placement.Right     = FVector3(1.0f, 0.0f, 0.0f);
        Placement.Up        = FVector3(0.0f, 0.0f, 1.0f);
        Placement.Origin    = FVector2(Camera.x - ViewDistance, Camera.z - ViewDistance);
        Placement.CellSize  = 2.0f * ViewDistance / (float)kSkyDFGridDimension;
        Placement.Dimension = kSkyDFGridDimension;
        Placement.SpherePad = MaxDistance * (1.0f + kDistanceFieldAOConeTanHalf);
        BuildDistanceFieldGrid(CL, SkyDFGrid, Placement);

        // Traced even without a grid, since the lighting reads whatever this leaves behind.
        FDistanceFieldAOArgs Args = {};
        Args.OutputSize[0] = Trace.GetSizeX();
        Args.OutputSize[1] = Trace.GetSizeY();
        Args.DepthSize[0]  = Depth.GetSizeX();
        Args.DepthSize[1]  = Depth.GetSizeY();
        Args.DepthIndex     = (uint32)Depth.GetResourceID();
        Args.GBufferBIndex  = (uint32)GetNamedImage(ENamedImage::GBufferB).GetResourceID();
        Args.SlotImageIndex = (uint32)GetNamedImage(ENamedImage::MaterialSlot).GetResourceID();
        Args.OutputUAV      = (uint32)TraceSlot;
        Args.MaxDistance   = MaxDistance;
        Args.ViewDistance  = ViewDistance;
        Args.Grid          = MakeGridArgs(SkyDFGrid, Placement);

        DispatchCompute(CL, TraceCS, Args,
            RenderUtils::GetGroupCount(Args.OutputSize[0], 8u),
            RenderUtils::GetGroupCount(Args.OutputSize[1], 8u), 1);
        RHI::Barriers::ComputeToCompute(CL);

        BilateralUpsample(CL, UpsampleCS, Trace, Output, Depth, 2u, /*bRawHalfDepth*/ true);
    }
}
