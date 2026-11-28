#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace TerrainPassInternal
    {
        // Where terrain vertices are built and their heights, normals and visible list read, the mesh stage or the vertex fallback.
        constexpr RHI::EStageFlags kTerrainGeometryStages = RHI::EStageFlags::MeshShader | RHI::EStageFlags::VertexShader;

        FSceneImage CreateTerrainImage(uint32 Size, uint16 ArraySize, EFormat Format, bool bUav, bool bArrayView = false,
            const char* DebugName = "Terrain.Image")
        {
            RHI::FTextureDesc Desc;
            // Shader samples weight maps as Sampler2DArray, so array textures get an array view.
            Desc.Type       = (bArrayView || ArraySize > 1) ? RHI::ETextureType::Tex2DArray : RHI::ETextureType::Tex2D;
            Desc.Dimension  = FUIntVector3(Size, Size, 1);
            Desc.LayerCount = ArraySize;
            Desc.Format     = Format;
            Desc.Usage      = RHI::EImageUsageFlags::Sampled | RHI::EImageUsageFlags::TransferDst;
            if (bUav)
            {
                Desc.Usage = Desc.Usage | RHI::EImageUsageFlags::Storage;
            }
            FSceneImage Image = CreateSceneImage(Desc, /*bSampled*/ true, /*bMipUAVs*/ bUav);
            RHI::SetDebugName(Image.Texture, DebugName);
            return Image;
        }

        template<typename TFree>
        void FreeCullBuffers(FTerrainGPUState& State, TFree&& Free)
        {
            State.ForEachBuffer([&](RHI::FGPUAllocation& Buffer)
            {
                if (Buffer)
                {
                    Free(Buffer);
                    Buffer = {};
                }
            });
            State.AllocatedNodeCount = 0u;
            State.PageTableCount     = 0u;
        }

        // A grass block's first size, which the scatter's readback then grows toward the terrain's budget.
        constexpr uint32 kInitialGrassCapacity = 1u << 16;

        void RetireGrassState(FGrassGPUState& State)
        {
            if (State.PrevCursorBuffer.Gpu != 0)
            {
                RHI::Retire(State.PrevCursorBuffer);
                State.PrevCursorBuffer = {};
            }
            for (RHI::FGPUAllocation& Readback : State.CursorReadback)
            {
                if (Readback.Gpu != 0)
                {
                    RHI::Retire(Readback);
                    Readback = {};
                }
            }
        }
    }

    void FDefaultSceneRenderer::PruneTerrainStates()
    {
        const FFrameData& Frame = *RenderFrame;
        if (!TerrainGPUStates.IsEmpty())
        {
            const TVector<ECS::FEntity>& Live = Frame.Extracts.LiveTerrainEntities;
            auto IsLive = [&](ECS::FEntity E)
            {
                return Algo::Contains(Live, E);
            };

            for (auto It = TerrainGPUStates.begin(); It != TerrainGPUStates.end();)
            {
                if (!IsLive(It->Key))
                {
                    FTerrainGPUState& Dead = It->Value;
                    TerrainPassInternal::FreeCullBuffers(Dead, [this](const RHI::FGPUAllocation& Buffer) { DeferFree(Buffer); });
                    It = TerrainGPUStates.Remove(It);
                }
                else
                {
                    ++It;
                }
            }

            for (auto It = GrassGPUStates.begin(); It != GrassGPUStates.end();)
            {
                if (!IsLive(It->Key))
                {
                    for (FGrassGPUState& Dead : It->Value)
                    {
                        TerrainPassInternal::RetireGrassState(Dead);
                    }
                    It = GrassGPUStates.Remove(It);
                }
                else
                {
                    ++It;
                }
            }
        }
    }

    void FDefaultSceneRenderer::TerrainUpdatePass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        PruneTerrainStates();
        if (Frame.Extracts.TerrainExtracts.IsEmpty())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Terrain Update");
        bool bAnyUpload = false;

        // Last frame's terrain passes and this frame's grass scatter may still read what the uploads below overwrite.
        bool bUploadBarrierIssued = false;
        auto BeginUpload = [&]()
        {
            if (!bUploadBarrierIssued)
            {
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Compute | TerrainPassInternal::kTerrainGeometryStages | RHI::EStageFlags::PixelShader | RHI::EStageFlags::IndirectArguments,
                    RHI::EAccessFlags::None,
                    RHI::EStageFlags::Transfer, RHI::EAccessFlags::None);
                bUploadBarrierIssued = true;
            }
            bAnyUpload = true;
        };

        if (!TerrainHeightAtlas)
        {
            const uint32 AtlasSize = GTerrainPageAtlasSlotsPerSide * (uint32)GTerrainPageTexels;
            TerrainHeightAtlas = TerrainPassInternal::CreateTerrainImage(AtlasSize, 1u, EFormat::R16_UNORM, false, false, "Terrain.HeightAtlas");
            TerrainNormalAtlas = TerrainPassInternal::CreateTerrainImage(AtlasSize, 1u, EFormat::RG8_UNORM, false, false, "Terrain.NormalAtlas");
            TerrainWeightAtlas = TerrainPassInternal::CreateTerrainImage(AtlasSize, 1u, EFormat::RGBA8_UNORM, false, false, "Terrain.WeightAtlas");
        }

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            if (TerrainItem.Resolution < 2)
            {
                continue;
            }

            FTerrainGPUState& State = TerrainGPUStates[TerrainItem.Entity];

            FTerrainStore* Store = TerrainItem.Store.Get();
            if (Store == nullptr)
            {
                continue;
            }
            const int32  TreeDepth = Store->GetTreeDepth();
            const uint32 NodeCount = Store->GetTileCount();
            bool bAllBounds = State.BoundsStore.Get() != Store;
            if (State.AllocatedNodeCount != NodeCount)
            {
                TerrainPassInternal::FreeCullBuffers(State, [this](const RHI::FGPUAllocation& Buffer) { DeferFree(Buffer); });

                const uint32 LeafCount = 1u << (2 * TreeDepth);
                State.ListCapacity     = Math::Min(LeafCount, GTerrainMaxCullNodes);
                State.VisibleCapacity  = Math::Min(GetTerrainNodeCount(TreeDepth), GTerrainMaxCullNodes);
                State.TreeDepth        = TreeDepth;

                State.NodeBoundsBuffer   = CreateSceneBuffer(uint64(NodeCount) * sizeof(FVector2), "Terrain.NodeBounds");
                State.NodeLists[0]       = CreateSceneBuffer(uint64(State.ListCapacity) * sizeof(uint32), "Terrain.NodeListA");
                State.NodeLists[1]       = CreateSceneBuffer(uint64(State.ListCapacity) * sizeof(uint32), "Terrain.NodeListB");
                State.LevelArgsBuffer    = CreateSceneBuffer(uint64(TreeDepth + 2) * kTerrainLevelArgsWords * sizeof(uint32), "Terrain.LevelArgs");
                State.VisibleNodeBuffer  = CreateSceneBuffer(uint64(State.VisibleCapacity) * sizeof(FTerrainVisibleNode), "Terrain.VisibleNodes");
                State.IndirectDrawBuffer = CreateSceneBuffer(sizeof(FTerrainDrawArgs), "Terrain.IndirectDraw");
                State.AllocatedNodeCount = NodeCount;
                bAllBounds = true;
            }

            // A sculpt stroke changes a handful of bounds a frame, which upload alone rather than all of them.
            TVector<uint32> ChangedNodes;
            if (!bAllBounds && State.BoundsSerial != Store->GetChangeSerial() && !Store->GetChangedNodes(State.BoundsSerial, ChangedNodes))
            {
                bAllBounds = true;
            }
            if (bAllBounds)
            {
                BeginUpload();
                WriteBuffer(CL, State.NodeBoundsBuffer.Gpu, Store->GetBounds().GetData(), uint64(NodeCount) * sizeof(FVector2));
            }
            else if (!ChangedNodes.IsEmpty())
            {
                Algo::Sort(ChangedNodes);
                TVector<FUIntVector2> Runs;
                for (uint32 Node : ChangedNodes)
                {
                    if (!Runs.IsEmpty() && Runs.Last().x + Runs.Last().y >= Node)
                    {
                        Runs.Last().y = Math::Max(Runs.Last().y, Node - Runs.Last().x + 1u);
                        continue;
                    }
                    Runs.Add(FUIntVector2(Node, 1u));
                }
                BeginUpload();
                WriteBufferRuns(CL, State.NodeBoundsBuffer.Gpu, Store->GetBounds().GetData(), sizeof(FVector2), Runs);
            }
            State.BoundsStore  = TerrainItem.Store;
            State.BoundsSerial = Store->GetChangeSerial();

            FTerrainPageTable* Table = TerrainPageStreamer.FindTable(TerrainItem.Entity);
            if (Table == nullptr || Table->Slots.IsEmpty())
            {
                continue;
            }
            const uint32 TableCount = (uint32)Table->Slots.Num();
            const bool   bRealloc   = !State.PageTableBuffer || State.PageTableCount != TableCount;
            if (bRealloc)
            {
                if (State.PageTableBuffer)
                {
                    DeferFree(State.PageTableBuffer);
                }
                State.PageTableBuffer = CreateSceneBuffer(uint64(TableCount) * sizeof(uint32), "Terrain.PageTable");
                State.PageTableCount  = TableCount;
            }
            if (bRealloc || Table->bReset)
            {
                BeginUpload();
                WriteBuffer(CL, State.PageTableBuffer.Gpu, Table->Slots.GetData(), uint64(TableCount) * sizeof(uint32));
            }
            else if (!Table->DirtyRuns.IsEmpty())
            {
                BeginUpload();
                WriteBufferRuns(CL, State.PageTableBuffer.Gpu, Table->Slots.GetData(), sizeof(uint32), Table->DirtyRuns);
            }
            Table->DirtyRuns.Reset();
            Table->bReset = false;
        }

        // One staging copy per atlas, and each page copies its 72 texel square out of it into its slot.
        const uint32 PageCount = (uint32)TerrainPageStreamer.UploadSlots.Num();
        if (PageCount > 0u)
        {
            BeginUpload();
            const RHI::FGPURange Heights = RHI::CopyTransientArray(TerrainPageStreamer.UploadHeights.GetData(), TerrainPageStreamer.UploadHeights.Num());
            const RHI::FGPURange Normals = RHI::CopyTransientArray(TerrainPageStreamer.UploadNormals.GetData(), TerrainPageStreamer.UploadNormals.Num());
            const RHI::FGPURange Weights = RHI::CopyTransientArray(TerrainPageStreamer.UploadWeights.GetData(), TerrainPageStreamer.UploadWeights.Num());
            const uint64 HeightBytes = uint64(GTerrainPageTexelCount) * sizeof(uint16);
            const uint64 WeightBytes = uint64(GTerrainPageTexelCount) * sizeof(uint32);
            for (uint32 Page = 0; Page < PageCount; ++Page)
            {
                const uint32 Slot = TerrainPageStreamer.UploadSlots[Page];
                RHI::FTextureSlice Slice;
                Slice.Offset = FUIntVector3((Slot % GTerrainPageAtlasSlotsPerSide) * (uint32)GTerrainPageTexels,
                                            (Slot / GTerrainPageAtlasSlotsPerSide) * (uint32)GTerrainPageTexels, 0u);
                Slice.Extent = FUIntVector3((uint32)GTerrainPageTexels, (uint32)GTerrainPageTexels, 1u);
                RHI::CmdCopyMemoryToTexture(CL, Heights.Sub(Page * HeightBytes, HeightBytes), (uint32)GTerrainPageTexels, TerrainHeightAtlas.Texture, Slice);
                RHI::CmdCopyMemoryToTexture(CL, Normals.Sub(Page * HeightBytes, HeightBytes), (uint32)GTerrainPageTexels, TerrainNormalAtlas.Texture, Slice);
                RHI::CmdCopyMemoryToTexture(CL, Weights.Sub(Page * WeightBytes, WeightBytes), (uint32)GTerrainPageTexels, TerrainWeightAtlas.Texture, Slice);
            }
            TerrainPageStreamer.UploadSlots.Reset();
            TerrainPageStreamer.UploadHeights.Reset();
            TerrainPageStreamer.UploadNormals.Reset();
            TerrainPageStreamer.UploadWeights.Reset();
        }

        if (bAnyUpload)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute | TerrainPassInternal::kTerrainGeometryStages | RHI::EStageFlags::PixelShader,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
        }
    }

    void FDefaultSceneRenderer::PublishTerrainRecords()
    {
        const FFrameData& Frame = *RenderFrame;

        // Records follow the extract order, so a terrain's draw names its record by its extract index.
        uint32 VisibleBase = 0u;
        for (int32 Index = 0; Index < Frame.Extracts.TerrainExtracts.Num() && (uint32)Index < TerrainRecordCount; ++Index)
        {
            const FFrameData::FTerrainExtract& Item = Frame.Extracts.TerrainExtracts[Index];
            FTerrainRecord Record{};

            const FTerrainGPUState* State = TerrainGPUStates.Find(Item.Entity);
            if (State != nullptr && State->AllocatedNodeCount != 0u && State->PageTableCount != 0u && TerrainHeightAtlas)
            {
                const FVector3 WorldOrigin = FVector3(Item.WorldMatrix[3]);
                const float    HalfSize    = Item.TileWorldSize * 0.5f;

                FTerrainRenderParams& Params = Record.Params;
                Params.OriginXZ      = FVector2(WorldOrigin.x - HalfSize, WorldOrigin.z - HalfSize);
                Params.TileWorldSize = Item.TileWorldSize;
                Params.MaxHeight     = Item.MaxHeight;
                Params.Resolution    = Item.Resolution;
                Params.TreeDepth     = State->TreeDepth;
                Params.LayerCount    = Item.LayerCount;
                Params.SampleSpacing = Item.TileWorldSize / (float)Math::Max(Item.Resolution - 1, 1);
                Params.WorldOriginY  = FVector3(WorldOrigin.y, 0.0f, 0.0f);
                Params.EntityID      = (uint32)Item.Entity;
                Params.MaterialIndex = Item.MaterialIndex;
                Params.PageDepth     = GetTerrainPageDepth(State->TreeDepth);
                Params.Flags         = Item.bCastShadow ? GTerrainFlagCastShadow : 0u;

                Record.NodeBounds        = { State->NodeBoundsBuffer, State->AllocatedNodeCount };
                Record.Visible           = { State->VisibleNodeBuffer, State->VisibleCapacity };
                Record.PageTable         = { State->PageTableBuffer, State->PageTableCount };
                Record.HeightAtlasIndex  = (uint32)TerrainHeightAtlas.GetResourceID();
                Record.NormalAtlasIndex  = (uint32)TerrainNormalAtlas.GetResourceID();
                Record.WeightAtlasIndex  = (uint32)TerrainWeightAtlas.GetResourceID();
                Record.VisibleBase       = VisibleBase;
                VisibleBase += State->VisibleCapacity;
            }
            TerrainRecords[Index] = Record;
        }
    }

    void FDefaultSceneRenderer::TerrainCullPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;
        if (Frame.Extracts.TerrainExtracts.IsEmpty())
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Terrain Cull");

        static const FShaderH CullShader = FShaderLibrary::Get("TerrainCull.slang");
        if (!CullShader)
        {
            return;
        }

        PublishTerrainRecords();

        // Barriers are per depth across every terrain, since a world split into many terrains once serialized the GPU per terrain.
        struct FCullTarget
        {
            FTerrainGPUState* State;
            uint32            RecordIndex;
        };
        TInlineVector<FCullTarget, 64> Targets;
        int32 MaxTreeDepth = 0;
        for (int32 Index = 0; Index < Frame.Extracts.TerrainExtracts.Num() && (uint32)Index < TerrainRecordCount; ++Index)
        {
            const FFrameData::FTerrainExtract& TerrainItem = Frame.Extracts.TerrainExtracts[Index];
            FTerrainGPUState* State = TerrainGPUStates.Find(TerrainItem.Entity);
            if (State == nullptr || State->AllocatedNodeCount == 0u || !IsTerrainInView(TerrainItem))
            {
                continue;
            }
            Targets.Add({ State, (uint32)Index });
            MaxTreeDepth = Math::Max(MaxTreeDepth, State->TreeDepth);
        }
        if (Targets.IsEmpty())
        {
            return;
        }

        FTerrainDrawArgs InitialArgs{};
        InitialArgs.Indexed.IndexCount    = (uint32)(GTerrainMeshletMaxQuads * 6);
        InitialArgs.Indexed.InstanceCount = 0u;
        InitialArgs.Indexed.FirstIndex    = 0u;
        InitialArgs.Indexed.VertexOffset  = 0;
        InitialArgs.Indexed.FirstInstance = 0u;
        InitialArgs.MeshGroups[0]         = 0u;
        static_assert(kTerrainDrawInstanceWord == 1u && kTerrainDrawMeshGroupWord == 5u, "TerrainCull.slang counts into these words.");

        // The root alone is the first depth's work, one group of one node.
        uint32 InitialLevelArgs[(GTerrainMaxTreeDepth + 2) * kTerrainLevelArgsWords] = {};
        for (uint32 Depth = 0; Depth < GTerrainMaxTreeDepth + 2; ++Depth)
        {
            InitialLevelArgs[Depth * kTerrainLevelArgsWords + 1] = 1u;
            InitialLevelArgs[Depth * kTerrainLevelArgsWords + 2] = 1u;
        }
        InitialLevelArgs[0] = 1u;
        InitialLevelArgs[3] = 1u;
        const uint32 RootNode = 0u;

        // A capture view resets these while the primary view's terrain draws may still be fetching them.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::IndirectArguments | TerrainPassInternal::kTerrainGeometryStages | RHI::EStageFlags::Compute, RHI::EAccessFlags::None,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::None);
        for (const FCullTarget& Target : Targets)
        {
            FTerrainGPUState& State = *Target.State;
            WriteBuffer(CL, State.IndirectDrawBuffer.Gpu, &InitialArgs, sizeof(InitialArgs));
            WriteBuffer(CL, State.LevelArgsBuffer.Gpu, InitialLevelArgs, uint32(State.TreeDepth + 2) * kTerrainLevelArgsWords * sizeof(uint32));
            WriteBuffer(CL, State.NodeLists[0].Gpu, &RootNode, sizeof(RootNode));
        }

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullShader));
        for (int32 Depth = 0; Depth <= MaxTreeDepth; ++Depth)
        {
            // The first depth waits on the resets above, every later one on the depth that appended its nodes and group count.
            RHI::CmdBarrier(CL,
                Depth == 0 ? RHI::EStageFlags::Transfer : RHI::EStageFlags::Compute,
                Depth == 0 ? RHI::EAccessFlags::TransferWrite : RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);

            for (const FCullTarget& Target : Targets)
            {
                FTerrainGPUState& State = *Target.State;
                if (Depth > State.TreeDepth)
                {
                    continue;
                }

                FTerrainCullPushConstants Push{};
                Push.NodesIn         = { State.NodeLists[Depth & 1], State.ListCapacity };
                Push.NodesOut        = { State.NodeLists[(Depth + 1) & 1], State.ListCapacity };
                Push.LevelArgs       = { State.LevelArgsBuffer, uint32(State.TreeDepth + 2) * kTerrainLevelArgsWords };
                Push.TerrainIndirect = { State.IndirectDrawBuffer };
                Push.TerrainIndex    = Target.RecordIndex;
                Push.Depth           = (uint32)Depth;
                RHI::CmdDispatchIndirect(CL, MakeArgs(Push), State.LevelArgsBuffer.Sub(uint64(Depth) * kTerrainLevelArgsWords * sizeof(uint32), sizeof(uint32) * 3u));
            }
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            TerrainPassInternal::kTerrainGeometryStages | RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::Compute,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead | RHI::EAccessFlags::IndexRead);
    }

    bool FDefaultSceneRenderer::IsTerrainInView(const FFrameData::FTerrainExtract& Terrain) const
    {
        const FFrameData& Frame = *RenderFrame;
        if (!Frame.SceneGlobalData.CullData.bFrustumCull)
        {
            return true;
        }
        const FVector3 Origin = FVector3(Terrain.WorldMatrix[3]);
        const float HalfSize = Terrain.TileWorldSize * 0.5f;
        SAABB Bounds;
        Bounds.Min = FVector3(Origin.x - HalfSize, Origin.y, Origin.z - HalfSize);
        Bounds.Max = FVector3(Origin.x + HalfSize, Origin.y + Terrain.MaxHeight, Origin.z + HalfSize);
        if (CurrentView == nullptr || CurrentView->bIsPrimary)
        {
            return Frame.CameraFrustum.IsInside(Bounds);
        }

        if (CurrentCameraEarlyView >= (uint32)Frame.Views.CullViews.Num())
        {
            return true;
        }
        const FCullView& View = Frame.Views.CullViews[CurrentCameraEarlyView];
        FFrustum ViewFrustum;
        for (int32 Plane = 0; Plane < FFrustum::NUM; ++Plane)
        {
            ViewFrustum.Planes[Plane] = View.FrustumPlanes[Plane];
        }
        ViewFrustum.RebuildSoA();
        return ViewFrustum.IsInside(Bounds);
    }

    // Matches FGrassRetirePushConstants in GrassRetire.slang.
    struct FGrassRetirePushConstants
    {
        RHI::TGPUSpan<FInstanceCullEntry> CullEntries;
        RHI::TGPUSpan<uint32>             Cursor;
        RHI::TGPUSpan<uint32>             PrevCursor;
        RHI::TGPUSpan<FInstanceBlockBounds> BlockBounds;
        uint32                            SlotBase = 0;
        uint32                            Capacity = 0;
        uint32                            _Pad0    = 0;
        uint32                            _Pad1    = 0;
    };
    static_assert(sizeof(FGrassRetirePushConstants) == 80, "FGrassRetirePushConstants must match GrassRetire.slang.");

    // Matches FGrassScatterPushConstants in GrassScatter.slang.
    struct FGrassScatterPushConstants
    {
        RHI::TGPUSpan<FInstanceCullEntry> OutCullEntries;
        RHI::TGPUSpan<FTransform3x4>      OutTransforms;
        RHI::TGPUSpan<FInstanceStatic>    OutStatic;
        RHI::TGPUSpan<uint32>             OutCursor;

        // The terrain's record in the scene root, which holds its pages.
        uint32  TerrainIndex      = 0;
        uint32  _Pad0             = 0;
        uint32  _Pad1             = 0;
        uint32  LayerIndex        = 0;

        float   CameraX = 0.0f;
        float   CameraZ = 0.0f;
        float   Radius  = 0.0f;
        float   CellSize = 1.0f;

        float   MinWeight   = 0.0f;
        float   MaxSlopeCos = 0.0f;
        float   ScaleMin    = 1.0f;
        float   ScaleMax    = 1.0f;

        float   AlignToNormal = 0.0f;
        float   ZOffset       = 0.0f;
        uint32  bRandomYaw    = 1;
        uint32  Seed          = 0;

        uint32  GridSide     = 0;
        float   TerrainNormalDistance = 0.0f;

        uint32  LayerCount        = 0;
        uint32  DrawIDAndFlags    = 0;
        uint32  SurfaceDescIndex  = 0;
        uint32  MeshletHeaderSlot = 0;
        uint32  MaterialIndex     = 0;
        uint32  EntityID          = 0;
        float   MeshBoundsRadius  = 0.0f;
        float   MaxDrawDistance   = 0.0f;

        float   TerrainOriginX  = 0.0f;
        float   TerrainOriginZ  = 0.0f;
        float   TileWorldSize   = 0.0f;
        float   MaxHeight       = 0.0f;
        float   TerrainBaseY    = 0.0f;
        uint32  Resolution      = 0;
        float   FullDensityDistance = 0.0f;
        float   RootDarkening = 0.0f;
        uint32  MaskTexture   = Constants::kIndexNoneU32;
        uint32  MaskChannel   = 0;
        float   MaskStrength  = 0.0f;
        uint32  _MaskPad      = 0;
    };
    static_assert(sizeof(FGrassScatterPushConstants) == 216,
        "FGrassScatterPushConstants must match GrassScatter.slang.");

    void FDefaultSceneRenderer::GrassScatterPass(RHI::FCmdListH CL)
    {
        const FFrameData& Frame = *RenderFrame;

        // Before any early out, so a block whose species left last frame is freed even on a frame that scatters nothing.
        ScenePrimitives.ReleaseUnseenGrassSpecies();

        if (Frame.Extracts.TerrainExtracts.IsEmpty())
        {
            return;
        }

        // The blades land in the retained arrays, so this runs between their upload and the cull that reads them.
        if (!RetainedCullEntryBuffer || !RetainedTransformBuffer || !RetainedStaticBuffer)
        {
            return;
        }

        static const FShaderH ScatterShader = FShaderLibrary::Get("GrassScatter.slang");
        if (!ScatterShader)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "Grass Scatter");

        const FVector3 CameraPos = FVector3(Frame.SceneGlobalData.CameraData.Location);
        bool bAnyDispatched = false;
        uint64 BladesWanted  = 0;
        uint64 BladesDropped = 0;

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            if (TerrainItem.Grass.IsEmpty() || TerrainItem.GrassMaxInstances == 0u)
            {
                continue;
            }

            const FTerrainGPUState* TerrainState = TerrainGPUStates.Find(TerrainItem.Entity);
            if (TerrainState == nullptr || TerrainState->AllocatedNodeCount == 0u || TerrainState->PageTableCount == 0u)
            {
                ScenePrimitives.KeepGrassSpecies(TerrainItem.Entity);
                continue;
            }
            // Records follow the extract order, which TerrainCullPass publishes them in before this list is submitted.
            const uint32 TerrainIndex = (uint32)(&TerrainItem - Frame.Extracts.TerrainExtracts.GetData());

            TVector<FGrassGPUState>& States = GrassGPUStates.FindOrAdd(TerrainItem.Entity);
            for (int32 Removed = TerrainItem.Grass.Num(); Removed < States.Num(); ++Removed)
            {
                TerrainPassInternal::RetireGrassState(States[Removed]);
            }
            States.SetNum(TerrainItem.Grass.Num());

            // One scratch cursor per species, in extract order, matching what BeginFrameScratch sized.
            const uint32 FirstCursor = GrassCursorCursor;
            GrassCursorCursor += (uint32)TerrainItem.Grass.Num();

            const FVector3 Origin = FVector3(TerrainItem.WorldMatrix[3]);

            for (int32 Index = 0; Index < TerrainItem.Grass.Num(); ++Index)
            {
                const FFrameData::FGrassSpeciesExtract& Species = TerrainItem.Grass[Index];
                FGrassGPUState& State = States[Index];

                // Read back from kFramesInFlight ago, which the frame fence has already retired.
                const uint8 ReadbackSlot = CurrentFrameSlot;
                const uint32* Landed = State.CursorReadback[ReadbackSlot].CpuAs<const uint32>();

                // Every slot of a block costs the scene's per-slot buffers, so the block follows what the scatter needs and the budget only caps it.
                const uint32 MaxCapacity = TerrainItem.GrassMaxInstances;
                uint32 WantedCapacity = State.WantedCapacity != 0u ? State.WantedCapacity : TerrainPassInternal::kInitialGrassCapacity;
                // Grows past the need at once and shrinks only once the need falls below a quarter, so a block never thrashes between sizes.
                if (Landed != nullptr && (*Landed > WantedCapacity || *Landed * 4u < WantedCapacity))
                {
                    WantedCapacity = Math::Max(TerrainPassInternal::kInitialGrassCapacity, (uint32)Math::NextPowerOfTwo(*Landed + *Landed / 4u));
                }
                State.WantedCapacity = Math::Min(WantedCapacity, MaxCapacity);
                if (Landed != nullptr)
                {
                    BladesWanted  += *Landed;
                    BladesDropped += *Landed > State.BoundCapacity ? *Landed - State.BoundCapacity : 0u;
                }

                // A new or grown block can reallocate the retained arrays, which the staged fills still read through raw pointers.
                const FScenePrimitiveSet::FGrassSpeciesKey Key{ TerrainItem.Entity, Species.Type, Species.LayerIndex, Species.Occurrence };
                if (!ScenePrimitives.GrassSpeciesFits(Key, Species.Mesh, State.WantedCapacity))
                {
                    WaitDeferredStageFills();
                }
                const FScenePrimitiveSet::FGrassSpeciesBinding Binding =
                    ScenePrimitives.AcquireGrassSpecies(Key, Species.Mesh, Species.ResolveHandle, State.WantedCapacity);
                if (!Binding.bValid)
                {
                    continue;
                }

                // Dense grass keeps a bounded candidate grid rather than a huge dispatch, which can cut a long radius short.
                constexpr float GrassMaxGridSide = 2048.0f;
                const float GridRadius = Species.CellSize * GrassMaxGridSide * 0.5f;
                const float Radius     = Math::Min(Species.CullDistance, GridRadius);
                if (Radius <= 0.0f || Species.CellSize <= 0.0f)
                {
                    continue;
                }

                if (Landed == nullptr && State.CursorReadback[ReadbackSlot].Gpu == 0)
                {
                    State.CursorReadback[ReadbackSlot] = RHI::Malloc(sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
                    RHI::SetDebugName(State.CursorReadback[ReadbackSlot].Gpu, "Readback.GrassCursor");
                    if (void* Host = State.CursorReadback[ReadbackSlot].Cpu)
                    {
                        Memory::Memzero(Host, sizeof(uint32));
                    }
                }

                // Candidates cover the square inscribing the radius; the shader rejects the corners.
                const uint32 GridSide = (uint32)Math::Ceil((Radius * 2.0f) / Species.CellSize);
                if (GridSide == 0u)
                {
                    continue;
                }

                if (!State.PrevCursorBuffer)
                {
                    State.PrevCursorBuffer = CreateSceneBuffer(sizeof(uint32), "Grass.PrevCursor");
                }

                // A block new to this state may hold another species' blades, so its first retire walks all of it.
                if (State.PrevCursorBuffer && (State.BoundSlotBase != Binding.InstanceSlotBase || State.BoundCapacity != Binding.Capacity))
                {
                    RHI::CmdMemset(CL, { State.PrevCursorBuffer.Gpu, sizeof(uint32) }, Binding.Capacity);
                    Barriers::TransferToCompute(CL);
                    State.BoundSlotBase = Binding.InstanceSlotBase;
                    State.BoundCapacity = Binding.Capacity;
                }
                if (!State.PrevCursorBuffer)
                {
                    continue;
                }

                // The append cursor restarts every frame; the scratch block already zeroed it.
                const RHI::FGPURange Cursor = GetGrassCursor(FirstCursor + (uint32)Index);

                // Whatever the scatter leaves past its cursor is retired afterward, not zeroed in advance.
                FGrassRetireItem& Retire = GrassRetireScratch.EmplaceGetRef();
                Retire.SlotBase   = Binding.InstanceSlotBase;
                Retire.Capacity   = Binding.Capacity;
                Retire.Cursor     = Cursor;
                Retire.PrevCursor = State.PrevCursorBuffer;
                Retire.CursorReadback = State.CursorReadback[ReadbackSlot];

                if (!bAnyDispatched)
                {
                    // The last frame's culls, raster and shading still read these slots, and the upload's barrier only runs on a frame that sends something.
                    RHI::CmdBarrier(CL,
                        RHI::EStageFlags::AllCommands, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::TransferWrite,
                        RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
                    RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ScatterShader));
                }

                FGrassScatterPushConstants Push{};
                // Sliced to the reserved block, so the shader cannot name a retained slot that belongs to something else.
                Push.OutCullEntries = RHI::TGPUSpan<FInstanceCullEntry>::Slice(
                    RetainedCullEntryBuffer, Binding.InstanceSlotBase, Binding.Capacity);
                Push.OutTransforms  = RHI::TGPUSpan<FTransform3x4>::Slice(
                    RetainedTransformBuffer, Binding.InstanceSlotBase, Binding.Capacity);
                Push.OutStatic      = RHI::TGPUSpan<FInstanceStatic>::Slice(
                    RetainedStaticBuffer, Binding.InstanceSlotBase, Binding.Capacity);
                Push.OutCursor      = { Cursor };
                Push.TerrainIndex       = TerrainIndex;
                Push.LayerIndex         = Species.LayerIndex;
                Push.LayerCount         = (uint32)Math::Max(TerrainItem.LayerCount, 1);
                Push.CameraX            = CameraPos.x;
                Push.CameraZ            = CameraPos.z;
                Push.Radius             = Radius;
                Push.CellSize           = Species.CellSize;
                Push.MinWeight          = Species.MinWeight;
                Push.MaxSlopeCos        = Species.MaxSlopeCos;
                Push.ScaleMin           = Species.ScaleMin;
                Push.ScaleMax           = Species.ScaleMax;
                Push.AlignToNormal      = Species.AlignToNormal;
                Push.ZOffset            = Species.ZOffset;
                Push.bRandomYaw         = Species.bRandomYaw ? 1u : 0u;
                Push.Seed               = Species.Seed;
                Push.GridSide           = GridSide;

                // The block is shared by every species on this mesh, so per-type shading flags join here, not in the binding.
                EInstanceFlags SpeciesFlags = EInstanceFlags::Foliage | (Species.bReceiveShadow ? EInstanceFlags::ReceiveShadow : EInstanceFlags::None);
                if (!Species.bContactShadows)
                {
                    SpeciesFlags |= EInstanceFlags::NoContactShadows;
                }
                Push.DrawIDAndFlags     = Binding.DrawIDAndFlags | PackDrawIDAndFlags(0u, SpeciesFlags);
                Push.SurfaceDescIndex   = Binding.SurfaceDescIndex;
                Push.MeshletHeaderSlot  = Binding.MeshletHeaderSlot;
                Push.MaterialIndex      = Binding.MaterialIndex;
                Push.EntityID           = TerrainItem.Entity.GetPacked();
                Push.MeshBoundsRadius   = Binding.MeshBoundsRadius;
                Push.MaxDrawDistance    = Radius;
                Push.TerrainOriginX     = Origin.x;
                Push.TerrainOriginZ     = Origin.z;
                Push.TileWorldSize      = TerrainItem.TileWorldSize;
                Push.MaxHeight          = TerrainItem.MaxHeight;
                Push.TerrainBaseY       = Origin.y;
                Push.Resolution         = (uint32)TerrainItem.Resolution;
                Push.FullDensityDistance = Species.FullDensityDistance;
                Push.TerrainNormalDistance = Species.TerrainNormalDistance;
                Push.RootDarkening      = Species.RootDarkening;
                Push.MaskTexture        = Species.MaskTexture;
                Push.MaskChannel        = Species.MaskChannel;
                Push.MaskStrength       = Species.MaskStrength;

                const uint32 Groups = RenderUtils::GetGroupCount(GridSide, 8u);
                RHI::CmdDispatch(CL, MakeArgs(Push), Groups, Groups, 1u);
                bAnyDispatched = true;
            }
        }

        if (bAnyDispatched)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }

        RetireGrassBlades(CL);
        if (bAnyDispatched && (IsSMAAT2xEnabledFor(*CurrentView) || IsTemporalUpscalerFor(*CurrentView)))
        {
            SnapshotGrassMotionState(CL);
        }
        LUMINA_PROFILE_VALUE("Grass/BladesWanted",  BladesWanted);
        LUMINA_PROFILE_VALUE("Grass/BladesDropped", BladesDropped);
    }

    void FDefaultSceneRenderer::RetireGrassBlades(RHI::FCmdListH CL)
    {
        if (GrassRetireScratch.IsEmpty())
        {
            return;
        }

        static const FShaderH RetireShader = FShaderLibrary::Get("GrassRetire.slang");
        if (RetireShader)
        {
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(RetireShader));
            for (const FGrassRetireItem& Item : GrassRetireScratch)
            {
                FGrassRetirePushConstants Push{};
                // Sliced like the scatter's span, so the clamp to the allocation is the same one.
                Push.CullEntries = RHI::TGPUSpan<FInstanceCullEntry>::Slice(RetainedCullEntryBuffer, Item.SlotBase, Item.Capacity);
                Push.Cursor      = { Item.Cursor };
                Push.PrevCursor  = { Item.PrevCursor };
                Push.BlockBounds = CurrentInstanceBlockBounds();
                Push.SlotBase    = Item.SlotBase;
                Push.Capacity    = Item.Capacity;
                RHI::CmdDispatch(CL, MakeArgs(Push), RenderUtils::GetGroupCount(Item.Capacity, 64u), 1u, 1u);
            }

            // Four bytes each, kept on the GPU for the retire and copied out for the budget warning.
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            for (const FGrassRetireItem& Item : GrassRetireScratch)
            {
                RHI::CmdMemcpy(CL, { Item.PrevCursor.Gpu, sizeof(uint32) }, Item.Cursor);
                if (Item.CursorReadback.Gpu != 0)
                {
                    RHI::CmdMemcpy(CL, { Item.CursorReadback.Gpu, sizeof(uint32) }, Item.Cursor);
                }
            }
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }
        GrassRetireScratch.Reset();
    }

    void FDefaultSceneRenderer::DrawTerrainVisBuffer(RHI::FCmdListH CL, EFormat VisFormat)
    {
        const FFrameData& Frame = *RenderFrame;
        const bool bMeshPath = RHI::SupportsMeshShaders();

        static const FShaderH TerrainVertexPathPS = FShaderLibrary::Get("TerrainVisBufferPixel.slang");
        const FShaderH PixelShader = bMeshPath ? GetVisBufferPixelShader() : TerrainVertexPathPS;
        if (!PixelShader)
        {
            return;
        }

        SCENE_GPU_SCOPE(CL, "VisBuffer Terrain");
        
        RHI::FGPURange Indices{};
        if (bMeshPath == false)
        {
            Indices = RHI::CopyTransientArray(GTerrainMeshletIndices.Indices, GTerrainMeshletMaxQuads * 6);
        }
        
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        // The record index is the extract index, which TerrainCullPass published the records under.
        for (int32 TerrainIndex = 0; TerrainIndex < Frame.Extracts.TerrainExtracts.Num(); ++TerrainIndex)
        {
            const FFrameData::FTerrainExtract& TerrainItem = Frame.Extracts.TerrainExtracts[TerrainIndex];
            if (!TerrainItem.VisBufferShader || !TerrainItem.DeferredShader || !IsTerrainInView(TerrainItem))
            {
                continue;
            }
            const FTerrainGPUState* State = TerrainGPUStates.Find(TerrainItem.Entity);
            if (State == nullptr || State->AllocatedNodeCount == 0u)
            {
                continue;
            }

            FGraphicsPipelineKey Key;
            if (bMeshPath)
            {
                Key.MS = TerrainItem.VisBufferShader;
            }
            else
            {
                Key.VS = TerrainItem.VisBufferShader;
            }
            Key.PS          = PixelShader;
            Key.bWireframe  = FrameSettings.bWireframe;
            Key.DepthFormat = EFormat::D32;
            Key.ColorTargets.Add({ VisFormat, {} });
            const RHI::FPipelineH Pipeline = GetOrCreatePipeline(Key);
            if (!Pipeline)
            {
                continue;
            }
            RHI::CmdSetPipeline(CL, Pipeline);

            FTerrainDrawPush Push;
            Push.TerrainIndex = (uint32)TerrainIndex;

            const RHI::FGPURange Args{ State->IndirectDrawBuffer.Gpu, sizeof(FTerrainDrawArgs) };
            if (bMeshPath)
            {
                RHI::CmdDrawMeshTasksIndirect(CL, MakeArgs(Push), Args.Sub(offsetof(FTerrainDrawArgs, MeshGroups), sizeof(uint32) * 3u),
                                              1u, sizeof(uint32) * 3u);
            }
            else
            {
                RHI::CmdDrawIndexedIndirect(CL, Indices, MakeArgs(Push), Args.Sub(0u, sizeof(RHI::FDrawIndexedIndirectArguments)), 1u,
                                            sizeof(RHI::FDrawIndexedIndirectArguments), RHI::EIndexType::Uint16);
            }
        }
    }
}
