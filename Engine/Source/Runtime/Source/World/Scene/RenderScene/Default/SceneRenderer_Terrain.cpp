#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    namespace TerrainPassInternal
    {
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
                    RetireSceneImage(Dead.HeightmapTexture);
                    RetireSceneImage(Dead.NormalTexture);
                    RetireSceneImage(Dead.LayerWeightTexture);
                    if (Dead.ChunkInfoBuffer)      { DeferFree(Dead.ChunkInfoBuffer); }
                    if (Dead.MeshletInfoBuffer)    { DeferFree(Dead.MeshletInfoBuffer); }
                    if (Dead.VisibleMeshletBuffer) { DeferFree(Dead.VisibleMeshletBuffer); }
                    if (Dead.ChunkLevelBuffer)     { DeferFree(Dead.ChunkLevelBuffer); }
                    if (Dead.IndirectDrawBuffer)   { DeferFree(Dead.IndirectDrawBuffer); }
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
        static const FShaderH NormalShader = FShaderLibrary::Get("TerrainNormalCompute.slang");

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
                    RHI::EStageFlags::Compute | RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::IndirectArguments,
                    RHI::EAccessFlags::None,
                    RHI::EStageFlags::Transfer, RHI::EAccessFlags::None);
                bUploadBarrierIssued = true;
            }
            bAnyUpload = true;
        };

        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            if (TerrainItem.Resolution < 2 || TerrainItem.ChunkResolution < 2)
            {
                continue;
            }

            FTerrainGPUState& State = TerrainGPUStates[TerrainItem.Entity];
            const uint32 Res        = (uint32)TerrainItem.Resolution;
            const uint32 LayerCount = (uint32)Math::Max(TerrainItem.LayerCount, 1);
            const int32 SlicePixels = static_cast<int32>(Res * Res);

            const bool bRealloc = State.AllocatedResolution != Res || State.AllocatedLayerCount != LayerCount;
            if (bRealloc)
            {
                RetireSceneImage(State.HeightmapTexture);
                RetireSceneImage(State.NormalTexture);
                RetireSceneImage(State.LayerWeightTexture);

                State.HeightmapTexture   = TerrainPassInternal::CreateTerrainImage(Res, 1u,          EFormat::R32_FLOAT, false, false, "Terrain.Heightmap");
                State.NormalTexture      = TerrainPassInternal::CreateTerrainImage(Res, 1u,          EFormat::RG8_UNORM, true, false, "Terrain.Normal");
                State.LayerWeightTexture = TerrainPassInternal::CreateTerrainImage(Res, (uint16)Math::Max(LayerCount, 1u), EFormat::R8_UNORM, false, true, "Terrain.LayerWeight");
                State.AllocatedResolution = Res;
                State.AllocatedLayerCount = LayerCount;

                if (TerrainItem.WeightUpload == 0)
                {
                    const float Zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                    RHI::CmdClearTexture(CL, State.LayerWeightTexture.Texture, Zero);
                    bAnyUpload = true;
                }
            }

            // Height upload (from the snapshot)
            const int32 ResI = (int32)Res;
            const bool  bHeightDirty = TerrainItem.HeightUpload != 0;
            FIntVector2 RectMin = FIntVector2(0);
            FIntVector2 RectMax = FIntVector2(ResI - 1);

            if (TerrainItem.HeightUpload == 1 && TerrainItem.HeightBytes.Num() == SlicePixels)
            {
                const RHI::FGPURange Src = RHI::CopyTransientArray(TerrainItem.HeightBytes.GetData(), TerrainItem.HeightBytes.Num());
                BeginUpload();
                RHI::CmdCopyMemoryToTexture(CL, Src, Res, State.HeightmapTexture.Texture);
            }
            else if (TerrainItem.HeightUpload == 2)
            {
                RectMin = TerrainItem.HeightRectMin;
                RectMax = TerrainItem.HeightRectMax;
                const uint32 RegionW = uint32(RectMax.x - RectMin.x + 1);
                const uint32 RegionH = uint32(RectMax.y - RectMin.y + 1);
                // Snapshot rect is tightly packed, so the source row pitch is RegionW, not Res.
                const RHI::FGPURange Src = RHI::CopyTransientArray(TerrainItem.HeightBytes.GetData(), TerrainItem.HeightBytes.Num());

                RHI::FTextureSlice Slice;
                Slice.Offset = FUIntVector3((uint32)RectMin.x, (uint32)RectMin.y, 0);
                Slice.Extent = FUIntVector3(RegionW, RegionH, 1);
                BeginUpload();
                RHI::CmdCopyMemoryToTexture(CL, Src, RegionW, State.HeightmapTexture.Texture, Slice);
            }

            // Weights upload as whole slices, packed ascending in the snapshot.
            if (TerrainItem.WeightUpload != 0 && !TerrainItem.WeightBytes.IsEmpty())
            {
                const uint8* Cursor = TerrainItem.WeightBytes.GetData();
                const uint8* End    = Cursor + TerrainItem.WeightBytes.Num();
                for (uint32 L = 0; L < LayerCount; ++L)
                {
                    if ((TerrainItem.WeightSliceMask & (1u << L)) == 0u)
                    {
                        continue;
                    }
                    if (Cursor + SlicePixels > End)
                    {
                        break;
                    }
                    const RHI::FGPURange Src = RHI::CopyTransientArray(Cursor, SlicePixels);

                    RHI::FTextureSlice Slice;
                    Slice.Layer = L;
                    BeginUpload();
                    RHI::CmdCopyMemoryToTexture(CL, Src, Res, State.LayerWeightTexture.Texture, Slice);
                    Cursor += SlicePixels;
                }
            }

            if (bHeightDirty && NormalShader)
            {
                // Heightmap upload must land before the normal recompute samples it.
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                    RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

                const int32 NMinX = Math::Max(RectMin.x - 1, 0);
                const int32 NMinY = Math::Max(RectMin.y - 1, 0);
                const int32 NMaxX = Math::Min(RectMax.x + 1, ResI - 1);
                const int32 NMaxY = Math::Min(RectMax.y + 1, ResI - 1);
                const int32 NW    = NMaxX - NMinX + 1;
                const int32 NH    = NMaxY - NMinY + 1;

                struct FTerrainNormalArgs
                {
                    FTerrainNormalParams Params;
                    uint32 HeightmapIndex;
                    uint32 NormalUAV;
                    uint32 _Pad0;
                    uint32 _Pad1;
                };

                FTerrainNormalArgs NormalArgs{};
                NormalArgs.Params.Resolution    = ResI;
                NormalArgs.Params.RegionMinX    = NMinX;
                NormalArgs.Params.RegionMinY    = NMinY;
                NormalArgs.Params.RegionSizeX   = NW;
                NormalArgs.Params.RegionSizeY   = NH;
                NormalArgs.Params.TileWorldSize = TerrainItem.TileWorldSize;
                NormalArgs.Params.MaxHeight     = TerrainItem.MaxHeight;
                NormalArgs.HeightmapIndex       = (uint32)State.HeightmapTexture.GetResourceID();
                NormalArgs.NormalUAV            = (uint32)State.NormalTexture.GetMipUAVIndex(0);

                DispatchCompute(CL, NormalShader, NormalArgs, RenderUtils::GetGroupCount((uint32)NW, 8u),
                                                   RenderUtils::GetGroupCount((uint32)NH, 8u), 1u);

                // Normals are sampled by the terrain VS/PS.
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                    RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
            }

            if (TerrainItem.bGeometryRebuilt)
            {
                const uint32 ChunkCount   = (uint32)TerrainItem.Chunks.Num();
                const uint32 MeshletCount = (uint32)TerrainItem.Meshlets.Num();

                if (ChunkCount > 0 && MeshletCount > 0)
                {
                    auto AllocSSBO = [&](RHI::FGPUAllocation& Buffer, uint64 SizeBytes, const char* DebugName)
                    {
                        if (!Buffer || Buffer.Size < SizeBytes)
                        {
                            if (Buffer)
                            {
                                DeferFree(Buffer);
                            }
                            Buffer = CreateSceneBuffer(Math::Max<uint64>(SizeBytes, 16ull), DebugName);
                        }
                    };

                    AllocSSBO(State.ChunkInfoBuffer,      uint64(ChunkCount)   * sizeof(FTerrainChunkInfo),      "Terrain.ChunkInfo");
                    AllocSSBO(State.MeshletInfoBuffer,    uint64(MeshletCount) * sizeof(FTerrainMeshletInfo),   "Terrain.MeshletInfo");
                    AllocSSBO(State.VisibleMeshletBuffer, uint64(MeshletCount) * sizeof(FTerrainVisibleMeshlet), "Terrain.VisibleMeshlets");
                    AllocSSBO(State.ChunkLevelBuffer,     uint64(ChunkCount)   * sizeof(uint32),                 "Terrain.ChunkLevels");
                    AllocSSBO(State.IndirectDrawBuffer,   sizeof(FTerrainDrawArgs),                              "Terrain.IndirectDraw");

                    BeginUpload();
                    WriteBuffer(CL, State.ChunkInfoBuffer.Gpu,   TerrainItem.Chunks.GetData(),   ChunkCount * sizeof(FTerrainChunkInfo));
                    WriteBuffer(CL, State.MeshletInfoBuffer.Gpu, TerrainItem.Meshlets.GetData(), MeshletCount * sizeof(FTerrainMeshletInfo));

                    State.AllocatedChunkCount   = ChunkCount;
                    State.AllocatedMeshletCount = MeshletCount;
                }
            }
        }

        if (bAnyUpload)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader,
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
            if (State != nullptr && State->HeightmapTexture && State->NormalTexture && State->LayerWeightTexture
                && State->ChunkInfoBuffer && State->MeshletInfoBuffer && State->VisibleMeshletBuffer && State->ChunkLevelBuffer)
            {
                const FVector3 WorldOrigin = FVector3(Item.WorldMatrix[3]);
                const float    HalfSize    = Item.TileWorldSize * 0.5f;
                const int32    QuadsPerChunk = Math::Max(1, Item.ChunkResolution - 1);

                FTerrainRenderParams& Params = Record.Params;
                Params.OriginXZ             = FVector2(WorldOrigin.x - HalfSize, WorldOrigin.z - HalfSize);
                Params.TileWorldSize        = Item.TileWorldSize;
                Params.MaxHeight            = Item.MaxHeight;
                Params.Resolution           = Item.Resolution;
                Params.ChunkResolution      = Item.ChunkResolution;
                Params.ChunksPerSide        = GetTerrainChunksPerSide(Item.Resolution, QuadsPerChunk);
                Params.LayerCount           = Item.LayerCount;
                Params.WorldOriginY         = FVector3(WorldOrigin.y, 0.0f, 0.0f);
                Params.EntityID             = (uint32)Item.Entity;
                Params.MaterialIndex        = Item.MaterialIndex;
                Params.MeshletsPerChunkSide = (QuadsPerChunk + GTerrainMeshletQuads - 1) / GTerrainMeshletQuads;
                Params.MeshletQuadSide      = GTerrainMeshletQuads;

                Record.Chunks            = { State->ChunkInfoBuffer, State->AllocatedChunkCount };
                Record.Meshlets          = { State->MeshletInfoBuffer, State->AllocatedMeshletCount };
                Record.Visible           = { State->VisibleMeshletBuffer, State->AllocatedMeshletCount };
                Record.ChunkLevels       = { State->ChunkLevelBuffer, State->AllocatedChunkCount };
                Record.HeightmapIndex    = (uint32)State->HeightmapTexture.GetResourceID();
                Record.NormalIndex       = (uint32)State->NormalTexture.GetResourceID();
                Record.LayerWeightsIndex = (uint32)State->LayerWeightTexture.GetResourceID();
                Record.VisibleBase       = VisibleBase;
                VisibleBase += State->AllocatedMeshletCount;
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

        // A world split into many terrains used to pay two pipeline barriers per terrain, which serialized the GPU.
        TInlineVector<FTerrainGPUState*, 64> Visible;
        TInlineVector<const FFrameData::FTerrainExtract*, 64> VisibleItems;
        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            auto TerrainStateIt = TerrainGPUStates.Find(TerrainItem.Entity);
            if (TerrainStateIt == nullptr)
            {
                continue;
            }
            FTerrainGPUState& State = (*TerrainStateIt);
            if (!State.ChunkInfoBuffer || !State.MeshletInfoBuffer || !State.VisibleMeshletBuffer || !State.IndirectDrawBuffer || !State.ChunkLevelBuffer)
            {
                continue;
            }
            if (State.AllocatedChunkCount == 0u || State.AllocatedMeshletCount == 0u || !IsTerrainInView(TerrainItem))
            {
                continue;
            }
            Visible.Add(&State);
            VisibleItems.Add(&TerrainItem);
        }
        if (Visible.IsEmpty())
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

        // A capture view resets these while the primary view's terrain draws may still be fetching them.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::IndirectArguments | RHI::EStageFlags::VertexShader | RHI::EStageFlags::Compute, RHI::EAccessFlags::None,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::None);
        for (FTerrainGPUState* State : Visible)
        {
            WriteBuffer(CL, State->IndirectDrawBuffer.Gpu, &InitialArgs, sizeof(InitialArgs));
        }
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead);

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(CullShader));
        for (int32 i = 0; i < Visible.Num(); ++i)
        {
            FTerrainGPUState* State = Visible[i];
            const FFrameData::FTerrainExtract& Item = *VisibleItems[i];
            FTerrainCullPushConstants Push{};
            Push.Chunks          = { State->ChunkInfoBuffer, State->AllocatedChunkCount };
            Push.Meshlets        = { State->MeshletInfoBuffer, State->AllocatedMeshletCount };
            Push.VisibleMeshlets = { State->VisibleMeshletBuffer, State->AllocatedMeshletCount };
            Push.TerrainIndirect = { State->IndirectDrawBuffer };
            Push.ChunkLevels     = { State->ChunkLevelBuffer, State->AllocatedChunkCount };
            Push.QuadsPerChunk   = Item.ChunkResolution - 1;
            Push.Resolution      = Item.Resolution;
            Push.SampleSpacing   = Item.TileWorldSize / (float)Math::Max(Item.Resolution - 1, 1);
            RHI::CmdDispatch(CL, MakeArgs(Push), State->AllocatedChunkCount, 1u, 1u);
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::VertexShader | RHI::EStageFlags::IndirectArguments,
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

        uint32  HeightmapIndex    = 0;
        uint32  NormalIndex       = 0;
        uint32  LayerWeightsIndex = 0;
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
    };
    static_assert(sizeof(FGrassScatterPushConstants) == 200,
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
            if (TerrainState == nullptr || !TerrainState->HeightmapTexture || !TerrainState->NormalTexture || !TerrainState->LayerWeightTexture)
            {
                ScenePrimitives.KeepGrassSpecies(TerrainItem.Entity);
                continue;
            }
            const FTerrainGPUState& Terrain = *TerrainState;

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
                Push.HeightmapIndex     = (uint32)Terrain.HeightmapTexture.GetResourceID();
                Push.NormalIndex        = (uint32)Terrain.NormalTexture.GetResourceID();
                Push.LayerWeightsIndex  = (uint32)Terrain.LayerWeightTexture.GetResourceID();
                Push.LayerIndex         = Species.LayerIndex;
                Push.LayerCount         = Terrain.AllocatedLayerCount;
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
                Push.BlockBounds = bInstanceBlockBoundsValid
                                 ? RHI::TGPUSpan<FInstanceBlockBounds>{ InstanceBlockBoundsBuffer, (uint32)InstanceBlockBounds.Num() }
                                 : RHI::TGPUSpan<FInstanceBlockBounds>{};
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

        const RHI::FGPURange Indices = bMeshPath ? RHI::FGPURange{}
                                                 : RHI::CopyTransientArray(GTerrainMeshletIndices.Indices, GTerrainMeshletMaxQuads * 6);
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
            if (State == nullptr || !State->IndirectDrawBuffer || State->AllocatedMeshletCount == 0u)
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
