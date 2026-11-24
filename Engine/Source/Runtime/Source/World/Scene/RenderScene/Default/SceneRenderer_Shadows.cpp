#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace
    {
        // Covers a 4x4 open-world terrain tiling with shadow casting enabled on every tile.
        constexpr uint32 kMaxTerrainHeightfields = 16u;
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

        // Mirror of FTerrainHeightfield in TerrainHeightfield.slang.
        struct FTerrainHeightfieldGPU
        {
            FVector2 OriginXZ;
            float    Stride;
            float    OriginY;
            float    MaxHeight;
            float    Resolution;
            uint32   HeightmapIndex;
            float    _Pad;
        };
        static_assert(sizeof(FTerrainHeightfieldGPU) == 32);

        // Mirror of FPushConstants in WorldDistanceFieldUpdate.slang.
        struct FWorldDFUpdateArgs
        {
            RHI::TGPUSpan<FWorldDFBrickGPU> Bricks;
            float                           LevelBoxes[FWorldDistanceField::kLevels][4];
            uint32                          Levels[FWorldDistanceField::kLevels];
            uint32                          _Pad;
            FDFObjectGrid                   Grid;
            uint32                          NumTerrains;
            uint32                          _Pad1[3];
            FTerrainHeightfieldGPU          Terrains[kMaxTerrainHeightfields];
        };
        static_assert(offsetof(FWorldDFUpdateArgs, Grid) == 160 && offsetof(FWorldDFUpdateArgs, Terrains) == 288);

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
            FWorldDistanceFieldGPU Field;
            uint32        NumTerrains;
            FTerrainHeightfieldGPU Terrains[kMaxTerrainHeightfields];
        };

        // The field update reads a built grid's spans, and a grid of no cells, which no brick column falls inside, when there is none.
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


        // Every casting terrain, not only those in view, since a ridge behind the camera still shades and occludes what is in front. Returns how many there were.
        template<typename TExtracts, typename TStates>
        uint32 GatherTerrainHeightfields(const TExtracts& Extracts, const TStates& States, FTerrainHeightfieldGPU (&Out)[kMaxTerrainHeightfields], uint32& OutCount)
        {
            OutCount = 0u;
            uint32 NumCasting = 0u;
            for (const auto& Terrain : Extracts)
            {
                if (!Terrain.bCastShadow || Terrain.Resolution < 2)
                {
                    continue;
                }
                ++NumCasting;
                const auto StateIt = States.Find(Terrain.Entity);
                if (OutCount >= kMaxTerrainHeightfields || StateIt == nullptr || !StateIt->HeightmapTexture)
                {
                    continue;
                }

                const FVector3 Origin = FVector3(Terrain.WorldMatrix[3]);
                const float    Half   = Terrain.TileWorldSize * 0.5f;

                FTerrainHeightfieldGPU& Record = Out[OutCount++];
                Record.OriginXZ       = FVector2(Origin.x - Half, Origin.z - Half);
                Record.Stride         = Terrain.TileWorldSize / (float)(Terrain.Resolution - 1);
                Record.OriginY        = Origin.y;
                Record.MaxHeight      = Terrain.MaxHeight;
                Record.Resolution     = (float)Terrain.Resolution;
                Record.HeightmapIndex = (uint32)StateIt->HeightmapTexture.GetResourceID();
            }
            return NumCasting;
        }

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
            FWorldDistanceFieldGPU Field;
            float    DFNear;
            float    DFFar;
            float    DFMaxTrace;
            uint32   bDistanceField;
            FTerrainHeightfieldGPU Terrains[kMaxTerrainHeightfields];
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
        Args.Field              = MakeWorldDistanceFieldArgs();
        Args.bDistanceField     = (Settings.bDistanceField && Args.Field.NumLevels > 0u) ? 1u : 0u;

        const uint32 NumCastingTerrains = GatherTerrainHeightfields(Frame.Extracts.TerrainExtracts, TerrainGPUStates, Args.Terrains, Args.NumTerrains);
        if (NumCastingTerrains > kMaxTerrainHeightfields)
        {
            LOG_WARN_ONCE("Renderer: the sun far shadow needed {} casting terrains but can only hold {}, so far shadows lose casters.", NumCastingTerrains, kMaxTerrainHeightfields);
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

    void FDefaultSceneRenderer::UpdateWorldDistanceField(FFrameData& Frame)
    {
        // The idle frames after which the field's 28 MB go back to the pool.
        constexpr uint32 kIdleReleaseFrames = 600u;

        FFrameData::FWorldDF& Out = Frame.WorldDF;
        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        const bool bWanted = (Settings != nullptr && Settings->bDistanceFieldAO)
                          || (Frame.Lighting.LightData.bHasSun && Frame.Lighting.SunFarShadow.bDistanceField)
                          || Frame.SceneGlobalData.CullData.DebugMode == (uint32)ERenderSceneDebugFlags::WorldDistanceField;
        Out.bActive  = false;
        Out.bRelease = false;
        ScenePrimitives.SetStreamDistanceFieldSpheres(bWanted);
        if (!bWanted)
        {
            if (WorldDFIdleFrames < kIdleReleaseFrames && ++WorldDFIdleFrames == kIdleReleaseFrames)
            {
                Out.bRelease = true;
                WorldDistanceField.Reset();
            }
            return;
        }
        WorldDFIdleFrames = 0u;

        const FFrameData::FGeometry::FRetainedUpload& Upload = Frame.Geometry.RetainedUpload;
        const TVector<uint32>& StreamedBlocks = ScenePrimitives.GetStreamedBlocks();
        FSunShadowCache::FSlotChanges Changes;
        Changes.Dirty          = TSpan<const uint32>(Upload.DirtySlots.GetData(), Upload.DirtySlots.Num());
        Changes.DirtyStatic    = TSpan<const uint32>(Upload.DirtyStaticSlots.GetData(), Upload.DirtyStaticSlots.Num());
        Changes.Deactivated    = TSpan<const uint32>(Upload.DeactivatedSlots.GetData(), Upload.DeactivatedSlots.Num());
        Changes.StreamedBlocks = TSpan<const uint32>(StreamedBlocks.GetData(), StreamedBlocks.Num());
        Changes.bFull          = Upload.bFull || Upload.bFullStatic;

        TouchWorldDFTerrains(Frame);
        const TSpan<const FInstanceCullEntry> Entries(ScenePrimitives.GetRetainedCullEntries(), Upload.SlotCount);
        WorldDistanceField.Update(Frame.ViewVolume.GetViewPosition(), Changes, Entries, Out.Update);
        Out.bActive = true;
    }

    FWorldDistanceFieldGPU FDefaultSceneRenderer::MakeWorldDistanceFieldArgs() const
    {
        FWorldDistanceFieldGPU Args = {};
        // Zero levels, which every trace reads as open space, until a fresh update has covered the whole field.
        if (RenderFrame == nullptr || !RenderFrame->WorldDF.bActive || bWorldDFAwaitingFresh)
        {
            return Args;
        }
        for (uint32 Level = 0; Level < FWorldDistanceField::kLevels; ++Level)
        {
            const int32 Index = WorldDFLevels[Level].GetResourceID();
            if (Index < 0)
            {
                return {};
            }
            Args.Levels[Level] = (uint32)Index;
        }
        Memory::Memcpy(Args.LevelBoxes, RenderFrame->WorldDF.Update.LevelBoxes, sizeof(Args.LevelBoxes));
        Args.NumLevels = FWorldDistanceField::kLevels;
        return Args;
    }

    void FDefaultSceneRenderer::TouchWorldDFTerrains(const FFrameData& Frame)
    {
        // Terrain heights live outside the retained slots, so their uploads, moves and removals mark the field here.
        const uint32 Update = ++WorldDFUpdateCount;
        for (const FFrameData::FTerrainExtract& Terrain : Frame.Extracts.TerrainExtracts)
        {
            if (Terrain.Resolution < 2)
            {
                continue;
            }
            const FVector3 Origin = FVector3(Terrain.WorldMatrix[3]);
            const float    Half   = Terrain.TileWorldSize * 0.5f;
            const FVector3 Min(Origin.x - Half, Origin.y, Origin.z - Half);
            const FVector3 Max(Origin.x + Half, Origin.y + Terrain.MaxHeight, Origin.z + Half);

            FWorldDFTerrainBox* Known = WorldDFTerrainBoxes.Find(Terrain.Entity);
            if (Known == nullptr || Known->Min != Min || Known->Max != Max)
            {
                if (Known != nullptr)
                {
                    WorldDistanceField.TouchBox(Known->Min, Known->Max);
                }
                WorldDistanceField.TouchBox(Min, Max);
                Known = &WorldDFTerrainBoxes.FindOrAdd(Terrain.Entity);
                Known->Min = Min;
                Known->Max = Max;
            }
            else if (Terrain.HeightUpload == 1u)
            {
                WorldDistanceField.TouchBox(Min, Max);
            }
            else if (Terrain.HeightUpload == 2u)
            {
                const float Stride = Terrain.TileWorldSize / (float)(Terrain.Resolution - 1);
                WorldDistanceField.TouchBox(FVector3(Min.x + Terrain.HeightRectMin.x * Stride, Min.y, Min.z + Terrain.HeightRectMin.y * Stride),
                                            FVector3(Min.x + (Terrain.HeightRectMax.x + 1) * Stride, Max.y, Min.z + (Terrain.HeightRectMax.y + 1) * Stride));
            }
            Known->SeenUpdate = Update;
        }

        TVector<ECS::FEntity> Gone;
        for (const auto& [Entity, Box] : WorldDFTerrainBoxes)
        {
            if (Box.SeenUpdate != Update)
            {
                WorldDistanceField.TouchBox(Box.Min, Box.Max);
                Gone.Add(Entity);
            }
        }
        for (ECS::FEntity Entity : Gone)
        {
            WorldDFTerrainBoxes.Remove(Entity);
        }
    }

    void FDefaultSceneRenderer::WorldDistanceFieldPass(RHI::FCmdListH CL)
    {
        const FFrameData::FWorldDF& WorldDF = RenderFrame->WorldDF;
        if (WorldDF.bRelease)
        {
            for (FSceneImage& Level : WorldDFLevels)
            {
                RetireSceneImage(Level);
            }
            WorldDFGrid.Release();
            bWorldDFAwaitingFresh = true;
        }
        // Once per frame, since every view of it shares the one camera-centered field.
        if (!WorldDF.bActive || CurrentView == nullptr || !CurrentView->bIsPrimary)
        {
            return;
        }

        static const FShaderH UpdateCS = FShaderLibrary::Get("WorldDistanceFieldUpdate.slang");
        if (!UpdateCS)
        {
            WorldDistanceField.RequestReset();
            bWorldDFAwaitingFresh = true;
            return;
        }

        bool bCreated = false;
        for (uint32 Level = 0; Level < FWorldDistanceField::kLevels; ++Level)
        {
            FSceneImage& Image = WorldDFLevels[Level];
            if (Image.IsValid())
            {
                continue;
            }
            RHI::FTextureDesc Desc;
            Desc.Type      = RHI::ETextureType::Tex3D;
            Desc.Dimension = FUIntVector3(FWorldDistanceField::kResolution);
            Desc.Format    = EFormat::R16_FLOAT;
            Desc.Usage     = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::Storage;
            Image = CreateSceneImage(Desc, true, true);
            RHI::SetDebugName(Image.Texture, "WorldDF.Level");
            bCreated = true;
        }
        // New textures hold nothing a brick list cleared, so the field stays dark to its readers until a fresh update covers every brick.
        if (bCreated && !WorldDF.Update.bFresh)
        {
            WorldDistanceField.RequestReset();
            bWorldDFAwaitingFresh = true;
        }

        const FWorldDistanceField::FFrame& Update = WorldDF.Update;
        if (Update.Bricks.IsEmpty())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "World Distance Field");

        FWorldDFUpdateArgs Args = {};
        for (uint32 Level = 0; Level < FWorldDistanceField::kLevels; ++Level)
        {
            const int32 UAV = WorldDFLevels[Level].GetMipUAVIndex(0);
            if (UAV < 0)
            {
                WorldDistanceField.RequestReset();
                bWorldDFAwaitingFresh = true;
                return;
            }
            Args.Levels[Level] = (uint32)UAV;
        }
        Memory::Memcpy(Args.LevelBoxes, Update.LevelBoxes, sizeof(Args.LevelBoxes));

        if (Update.CompositeLevel < FWorldDistanceField::kLevels)
        {
            const uint32 Level = Update.CompositeLevel;
            const float* Box = Update.LevelBoxes[Level];
            FDistanceFieldGridPlacement Placement;
            Placement.Right     = FVector3(1.0f, 0.0f, 0.0f);
            Placement.Up        = FVector3(0.0f, 0.0f, 1.0f);
            Placement.Origin    = FVector2(Box[0], Box[2]);
            Placement.CellSize  = FWorldDistanceField::BrickWorldSize(Level);
            Placement.Dimension = FWorldDistanceField::kBricksPerAxis;
            Placement.SpherePad = FWorldDistanceField::Band(Level) + FWorldDistanceField::VoxelSize(Level);
            Placement.MinRadius = FWorldDistanceField::MinCasterRadius(Level);

            GatherTerrainHeightfields(RenderFrame->Extracts.TerrainExtracts, TerrainGPUStates, Args.Terrains, Args.NumTerrains);

            // An empty scene has no grid to build and no caster to merge, so its bricks rightly come out empty.
            if (BuildDistanceFieldGrid(CL, WorldDFGrid, Placement))
            {
                Args.Grid = MakeGridArgs(WorldDFGrid, Placement);
                RHI::Barriers::ComputeToCompute(CL);
            }
            else if (RenderFrame->Geometry.RetainedUpload.SlotCount > 0u)
            {
                WorldDistanceField.RequestReset();
                bWorldDFAwaitingFresh = true;
                return;
            }
        }

        Args.Bricks = RHI::CopyTransientArray(Update.Bricks.GetData(), Update.Bricks.Num());
        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(UpdateCS));
        // One group per brick, folded the way WorldDistanceFieldUpdate.slang unfolds it with MAX_DISPATCH_AXIS.
        const FUIntVector2 Groups = RenderUtils::FoldGroupCount((uint32)Update.Bricks.Num());
        RHI::CmdDispatch(CL, MakeArgs(Args), Groups.x, Groups.y, 1u);
        RHI::Barriers::ComputeToCompute(CL);

        if (Update.bFresh)
        {
            bWorldDFAwaitingFresh = false;
        }
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

        // Traced even without a field, since the lighting reads whatever this leaves behind.
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
        Args.Field         = MakeWorldDistanceFieldArgs();
        GatherTerrainHeightfields(RenderFrame->Extracts.TerrainExtracts, TerrainGPUStates, Args.Terrains, Args.NumTerrains);

        DispatchCompute(CL, TraceCS, Args,
            RenderUtils::GetGroupCount(Args.OutputSize[0], 8u),
            RenderUtils::GetGroupCount(Args.OutputSize[1], 8u), 1);
        RHI::Barriers::ComputeToCompute(CL);

        BilateralUpsample(CL, UpsampleCS, Trace, Output, Depth, 2u, /*bRawHalfDepth*/ true);
    }
}
