#include "RuntimePCH.h"
#include "SceneRendererInternal.h"
#include "Memory/Memcpy.h"

namespace Lumina
{
    using FParticleExtract = FDefaultSceneRenderer::FFrameData::FParticleExtract;

    static constexpr uint64 ParticleCounterBytes    = 4 * sizeof(uint32);
    static constexpr uint64 ParticleEventCountBytes = 4 * sizeof(uint32);
    static constexpr uint64 ParticleEventBufferSize = ParticleEventCountBytes + (uint64)PARTICLE_EVENT_LISTS * PARTICLE_EVENT_CAPACITY * sizeof(FParticleEventGPU);
    static constexpr uint64 ParticlePoolByteBudget  = 128ull << 20;
    static constexpr float  ParticlePrewarmStep     = 1.0f / 30.0f;
    static constexpr uint32 ParticleMaxPrewarmSteps = 300;
    static constexpr uint32 ParticleMaxFixedSteps   = 4;
    static constexpr uint64 ParticleEmitterByteBudget = 512ull << 20;
    static constexpr uint64 ParticleBlockAlign      = 64;
    static constexpr uint32 ParticleSetupsPerFrame  = 2048;
    static constexpr uint32 ParticleMaxDispatchGroups = 65535;
    // Bytes of grid copied back to the CPU in one frame, past which the remaining emitters wait for a later frame.
    static constexpr uint64 ParticleGridReadbackBudget = 32ull << 20;

    // Saturates instead of casting, since an out-of-range float to integer cast is undefined.
    static uint32 ToClampedCount(float Value, uint32 Max)
    {
        if (!(Value > 0.0f))
        {
            return 0u;
        }
        return Value >= (float)Max ? Max : (uint32)Value;
    }

    // Mirrors FParticleSimArgs in ParticleSimCommon.slang, everything by device address.
    struct FParticleSimArgs
    {
        uint64 ParamsAddr;
        RHI::TGPUSpan<FGPUParticle>      Particles;
        RHI::TGPUSpan<uint32>            SpawnCounter;
        RHI::TGPUSpan<FVector4>          ModuleParams;
        RHI::TGPUSpan<float>             Attributes;
        RHI::TGPUSpan<FParticleEventGPU> OwnEvents;
        RHI::TGPUSpan<uint32>            OwnEventCounts;
        RHI::TGPUSpan<FParticleEventGPU> SourceEvents;
        RHI::TGPUSpan<uint32>            SourceEventCounts;
        RHI::TGPUSpan<FParticleEventGPU> ScriptEvents;
        RHI::TGPUSpan<FVector4>          RibbonHistory;
        RHI::TGPUSpan<FParticleShapeGPU> Attractors;
        RHI::TGPUSpan<FParticleShapeGPU> Colliders;
        RHI::TGPUSpan<FParticleTerrainGPU> Terrains;
        RHI::TGPUSpan<float>             GridRead;
        RHI::TGPUSpan<float>             GridWrite;
        RHI::TGPUSpan<int32>             GridDeposit;
        RHI::TGPUSpan<FVector4>          ArrayData;
        RHI::TGPUSpan<FUIntVector2>      ArrayRanges;
        RHI::TGPUSpan<uint32>            NeighborCounts;
        RHI::TGPUSpan<uint32>            NeighborIndices;
    };
    static_assert(sizeof(FParticleSimArgs) == 328, "FParticleSimArgs must match ParticleSimCommon.slang.");

    // Mirrors FParticleSimBatch in ParticleSimCommon.slang.
    struct FParticleSimBatch
    {
        RHI::TGPUSpan<FParticleSimArgs> Emitters;
        RHI::TGPUSpan<FUIntVector2>     Groups;
    };
    static_assert(sizeof(FParticleSimBatch) == 32, "FParticleSimBatch must match ParticleSimCommon.slang.");
    static_assert(PARTICLE_SIM_GROUP_SIZE == 64u, "The simulation shaders declare 64 lanes per workgroup.");

    static constexpr uint32 ParticleMaxCollisionSources   = 16;
    static constexpr uint64 ParticleCollisionListBytes    = (uint64)PARTICLE_EVENT_CAPACITY * sizeof(FParticleEventGPU);
    static constexpr uint64 ParticleCollisionListOffset   = ParticleEventCountBytes + (uint64)EParticleEventType::Collision * ParticleCollisionListBytes;
    static constexpr uint64 ParticleCollisionReadbackStride = ParticleEventCountBytes + ParticleCollisionListBytes;

    // Mirrors the render pass block in ParticleSpriteCommon.slang.
    struct FParticlePushConstants
    {
        RHI::TGPUSpan<FGPUParticle> Particles;
        FVector4 Tint;
        uint32   TextureIndex;
        uint32   FacingMode;
        float    VelocityStretch;
        uint32   SubUVColumns;
        uint32   SubUVRows;
        uint32   AttrFloats;
        RHI::TGPUSpan<float> Attributes;
        int32    AttrSlotSizeScaleX;
        int32    AttrSlotSizeScaleY;
        int32    AttrSlotPrevPosX;
        int32    AttrSlotPrevPosY;
        int32    AttrSlotPrevPosZ;
        uint32   MaterialIndex;
        uint32   bSorted;
        RHI::TGPUSpan<uint32> SortedIndices;
        float    SoftFadeDistance;
        uint32   RenderFlags;
        RHI::TGPUSpan<FVector4> RibbonHistory;
        uint32   RenderMode;
        uint32   VertsPerParticle;
        uint32   MeshletHeaderSlot;
        uint32   bAlignMeshToVelocity;
        uint32   RibbonSegments;
        uint32   RibbonHead;
        float    RibbonTailWidth;
        float    ExtrapolateTime;
        uint32   FlipbookMode;
        float    FlipbookFPS;
        uint32   bRandomStartFrame;
        uint32   bShadowPass;
        int32    ShadowDataIndex;
        uint32   ShadowCascade;
        float    CameraFadeDistance;
        float    Pad0;
    };
    static_assert(sizeof(FParticlePushConstants) == 208, "FParticlePushConstants must match ParticleSpriteCommon.slang.");
    static_assert(offsetof(FParticlePushConstants, Tint) % 16 == 0, "A vector read through a device address must not straddle 16 bytes.");

    // Mirrors FParticleDrawBatch in ParticleSpriteCommon.slang.
    struct FParticleDrawBatch
    {
        RHI::TGPUSpan<FParticlePushConstants> Draws;
    };
    static_assert(sizeof(FParticleDrawBatch) == 16, "FParticleDrawBatch must match ParticleSpriteCommon.slang.");

    // Mirrors FParticleDrawArgsSource and FParticleDrawArgsPass in ParticleDrawArgs.slang.
    struct FParticleDrawArgsSource
    {
        RHI::TGPUSpan<RHI::FDrawIndirectArguments> SortedArgs;
        uint32 VertexCount = 0;
        uint32 Pad[3] = {};
    };
    static_assert(sizeof(FParticleDrawArgsSource) == 32, "FParticleDrawArgsSource must match ParticleDrawArgs.slang.");

    struct FParticleDrawArgsPass
    {
        RHI::TGPUSpan<FParticleDrawArgsSource>     Sources;
        RHI::TGPUSpan<RHI::FDrawIndirectArguments> OutArgs;
    };
    static_assert(sizeof(FParticleDrawArgsPass) == 32, "FParticleDrawArgsPass must match ParticleDrawArgs.slang.");

    template <typename T>
    static void AppendBytes(TVector<uint8>& Bytes, const T& Value)
    {
        const size_t At = Bytes.size();
        Bytes.resize(At + sizeof(T));
        Memory::Memcpy(Bytes.data() + At, &Value, sizeof(T));
    }

    static float EmitterUniformScale(const FMatrix4& WorldMat)
    {
        const float Sum = Math::Length(FVector3(WorldMat[0])) + Math::Length(FVector3(WorldMat[1])) + Math::Length(FVector3(WorldMat[2]));
        return Math::Max(Sum / 3.0f, 1e-4f);
    }

    static uint32 ParticleVertsPerParticle(const FParticleExtract& Item)
    {
        switch (Item.Resolved.RenderMode)
        {
        case EParticleRenderMode::Mesh:
            return Item.MeshletCount * MESHLET_MAX_TRIANGLES * 3u;
        case EParticleRenderMode::Ribbon:
            return (uint32)Item.Resolved.RibbonSegments * 6u;
        default:
            return 6u;
        }
    }

    static bool IsParticleDrawable(const FParticleExtract& Item)
    {
        return Item.bOnDrawnLayer && Item.Resolved.RenderMode != EParticleRenderMode::None
            && (Item.Resolved.RenderMode != EParticleRenderMode::Mesh || Item.MeshletHeaderSlot != 0u);
    }

    RHI::FGPUAllocation FDefaultSceneRenderer::AcquireParticleBuffer(uint64 Size, const char* DebugName)
    {
        auto It = ParticleBufferPool.find(Size);
        if (It != ParticleBufferPool.end() && !It->second.empty())
        {
            const RHI::FGPUAllocation Reused = It->second.back();
            It->second.pop_back();
            ParticlePoolBytes -= Size;
            return Reused;
        }

        const RHI::FGPUAllocation Allocation = RHI::Malloc(Size, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
        RHI::SetDebugName(Allocation.Gpu, DebugName);
        return Allocation;
    }

    void FDefaultSceneRenderer::ReleaseParticleBuffer(RHI::FGPUAllocation& Allocation, uint64 Size)
    {
        if (!Allocation)
        {
            return;
        }

        if (ParticlePoolBytes + Size <= ParticlePoolByteBudget)
        {
            ParticleBufferPool[Size].push_back(Allocation);
            ParticlePoolBytes += Size;
        }
        else
        {
            DeferFree(Allocation);
        }
        Allocation = {};
    }

    void FDefaultSceneRenderer::ReleaseParticleState(FParticleGPUState& State)
    {
        ReleaseParticleBuffer(State.Block, State.BlockSize);
        State.BlockSize           = 0;
        State.ParticleBuffer      = {};
        State.SpawnCounterBuffer  = {};
        State.AttributeBuffer     = {};
        State.SortIndexBuffer     = {};
        State.SortDrawArgsBuffer  = {};
        State.SortKeyBuffer       = {};
        State.EventBuffer         = {};
        State.RibbonHistoryBuffer = {};
        State.GridBuffers[0]      = {};
        State.GridBuffers[1]      = {};
        State.GridDepositBuffer   = {};
        State.GridBufferSize      = 0;
        State.AllocatedGridCells  = 0;
        State.AllocatedGridChannels = 0;
        State.GridCurrent         = 0;
        State.NeighborCounts      = {};
        State.NeighborIndices     = {};
        State.AllocatedNeighborCells = 0;
        State.AllocatedNeighborSlots = 0;
        State.AllocatedMax = 0;
        State.SortCount    = 0;
    }

    bool FDefaultSceneRenderer::EnsureParticleBuffers(RHI::FCmdListH CL, const FFrameData::FParticleExtract& Item, FParticleGPUState& State)
    {
        const FResolvedParticleParams& Resolved = Item.Resolved;
        const uint32 MaxParticles    = (uint32)Resolved.MaxParticles;
        const uint32 RibbonSegments  = Resolved.RenderMode == EParticleRenderMode::Ribbon ? (uint32)Resolved.RibbonSegments : 0u;
        const bool   bWantsEvents    = Item.RaisedEventMask != 0u;
        const bool   bWantsSort      = Resolved.SortMode != EParticleSortMode::None && MaxParticles <= PARTICLE_GLOBAL_SORT_CAPACITY;
        const uint32 GridCells       = Item.GridChannels > 0u ? Item.GridResolution.x * Item.GridResolution.y : 0u;
        const uint32 GridChannels    = GridCells > 0u ? Item.GridChannels : 0u;
        const uint32 NeighborCells   = Item.NeighborResolution * Item.NeighborResolution * Item.NeighborResolution;
        const uint32 NeighborSlots   = NeighborCells > 0u ? Item.NeighborsPerCell : 0u;

        const bool bNeedsAlloc = (State.ParticleBuffer.Gpu == 0)
                              || (State.AllocatedMax != MaxParticles)
                              || (State.AllocatedAttributeFloats != Item.AttributeFloatCount)
                              || (State.AllocatedRibbonSegments != RibbonSegments)
                              || ((State.EventBuffer.Gpu != 0) != bWantsEvents)
                              || ((State.SortCount > 0) != bWantsSort)
                              || (State.AllocatedGridCells != GridCells)
                              || (State.AllocatedGridChannels != GridChannels)
                              || (State.AllocatedNeighborCells != NeighborCells)
                              || (State.AllocatedNeighborSlots != NeighborSlots);
        if (!bNeedsAlloc)
        {
            return true;
        }

        // A mass spawn would otherwise set every emitter up in one frame, so the rest wait for the next.
        if (ParticleSetupsThisFrame >= ParticleSetupsPerFrame)
        {
            return false;
        }
        ++ParticleSetupsThisFrame;

        ReleaseParticleState(State);

        const uint64 PerParticleBytes = sizeof(FGPUParticle) + (uint64)Item.AttributeFloatCount * sizeof(float)
                                      + (uint64)RibbonSegments * sizeof(FVector4) + (bWantsSort ? 2ull * sizeof(uint32) : 0ull);
        const uint64 GridBytes    = (uint64)GridCells * GridChannels * sizeof(float);
        const uint64 NeighborCountBytes = (uint64)NeighborCells * sizeof(uint32);
        const uint64 NeighborIndexBytes = (uint64)NeighborCells * NeighborSlots * sizeof(uint32);
        const uint64 EmitterBytes = (uint64)MaxParticles * PerParticleBytes + 3ull * GridBytes + NeighborCountBytes + NeighborIndexBytes;
        if (EmitterBytes > ParticleEmitterByteBudget)
        {
            if (State.RejectedMax == MaxParticles)
            {
                return false;
            }
            State.RejectedMax = MaxParticles;
            LOG_ERROR("Particle emitter {} of entity {} needs {} MB for {} particles and its grid, over the {} MB budget, and is skipped",
                Item.EmitterIndex, (uint32)Item.Entity, EmitterBytes >> 20, MaxParticles, ParticleEmitterByteBudget >> 20);
            return false;
        }

        // A pooled buffer may still be read by an earlier frame's draw, so its clear waits for those reads.
        if (!bParticlePoolReuseBarrierIssued)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute | RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::IndirectArguments,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite);
            bParticlePoolReuseBarrierIssued = true;
        }

        uint64 BlockCursor = 0;
        auto Carve = [&BlockCursor](uint64 Size)
        {
            const uint64 Offset = BlockCursor;
            BlockCursor = Math::AlignUp(BlockCursor + Size, ParticleBlockAlign);
            return Offset;
        };

        // Everything a fresh emitter must read as zero comes first, so one fill clears it all.
        State.ParticleBufferSize  = (uint64)MaxParticles * sizeof(FGPUParticle);
        State.AttributeBufferSize = (uint64)MaxParticles * (uint64)Item.AttributeFloatCount * sizeof(float);
        const uint64 ParticleOffset  = Carve(State.ParticleBufferSize);
        const uint64 AttributeOffset = Carve(State.AttributeBufferSize);
        const uint64 CounterOffset   = Carve(ParticleCounterBytes);
        // Stages start from an empty grid, and the first one folds in deposits that must start at zero.
        const uint64 GridOffsets[2]  = { Carve(GridBytes), Carve(GridBytes) };
        const uint64 DepositOffset   = Carve(GridBytes);
        const uint64 NeighborCountOffset = Carve(NeighborCountBytes);
        const uint64 NeighborIndexOffset = Carve(NeighborIndexBytes);
        // A sort that never runs must draw nothing rather than an uninitialized count.
        const uint64 SortArgsOffset  = bWantsSort ? Carve(sizeof(RHI::FDrawIndirectArguments)) : 0;
        // Only the event counts need clearing, so the event lists close the zeroed range.
        const uint64 EventOffset     = bWantsEvents ? Carve(ParticleEventBufferSize) : 0;
        const uint64 ZeroedBytes     = bWantsEvents ? EventOffset + ParticleEventCountBytes : BlockCursor;

        State.SortCount           = bWantsSort ? (uint32)Math::NextPowerOfTwo((int32)MaxParticles) : 0u;
        State.SortIndexBufferSize = bWantsSort ? (uint64)MaxParticles * sizeof(uint32) : 0;
        State.SortKeyBufferSize   = State.SortCount > PARTICLE_SORT_CAPACITY ? (uint64)State.SortCount * sizeof(uint32) : 0;
        State.RibbonHistorySize   = (uint64)MaxParticles * RibbonSegments * sizeof(FVector4);
        State.EventBufferSize     = bWantsEvents ? ParticleEventBufferSize : 0;
        const uint64 SortIndexOffset = Carve(State.SortIndexBufferSize);
        const uint64 SortKeyOffset   = Carve(State.SortKeyBufferSize);
        const uint64 RibbonOffset    = Carve(State.RibbonHistorySize);

        State.BlockSize = BlockCursor;
        State.Block     = AcquireParticleBuffer(State.BlockSize, "Particles.Emitter");
        auto Region = [&State](uint64 Offset, uint64 Size)
        {
            return Size > 0 ? RHI::FGPUAllocation{ .Cpu = nullptr, .Gpu = State.Block.Gpu + Offset, .Size = Size } : RHI::FGPUAllocation{};
        };
        State.ParticleBuffer      = Region(ParticleOffset, State.ParticleBufferSize);
        State.AttributeBuffer     = Region(AttributeOffset, State.AttributeBufferSize);
        State.SpawnCounterBuffer  = Region(CounterOffset, ParticleCounterBytes);
        State.SortDrawArgsBuffer  = bWantsSort ? Region(SortArgsOffset, sizeof(RHI::FDrawIndirectArguments)) : RHI::FGPUAllocation{};
        State.EventBuffer         = Region(EventOffset, State.EventBufferSize);
        State.SortIndexBuffer     = Region(SortIndexOffset, State.SortIndexBufferSize);
        State.SortKeyBuffer       = Region(SortKeyOffset, State.SortKeyBufferSize);
        State.RibbonHistoryBuffer = Region(RibbonOffset, State.RibbonHistorySize);
        State.GridBuffers[0]      = Region(GridOffsets[0], GridBytes);
        State.GridBuffers[1]      = Region(GridOffsets[1], GridBytes);
        State.GridDepositBuffer   = Region(DepositOffset, GridBytes);
        State.GridBufferSize      = GridBytes;
        State.NeighborCounts      = Region(NeighborCountOffset, NeighborCountBytes);
        State.NeighborIndices     = Region(NeighborIndexOffset, NeighborIndexBytes);

        RHI::CmdMemset(CL, { State.Block.Gpu, ZeroedBytes }, 0u);

        State.AllocatedMax             = MaxParticles;
        State.AllocatedAttributeFloats = Item.AttributeFloatCount;
        State.AllocatedRibbonSegments  = RibbonSegments;
        State.AllocatedGridCells       = GridCells;
        State.AllocatedGridChannels    = GridChannels;
        State.AllocatedNeighborCells   = NeighborCells;
        State.AllocatedNeighborSlots   = NeighborSlots;
        State.GridCurrent              = 0u;
        State.SpawnAccumulator         = 0.0f;
        State.SystemAge                = 0.0f;
        State.CycleTime                = -1.0f;
        State.RibbonHead               = 0u;
        State.RibbonTimer              = 0.0f;
        State.AliveTimeRemaining       = 0.0f;
        State.bBurstPending            = true;
        State.bPrewarmPending          = true;
        if (Resolved.bUseFixedSeed)
        {
            State.FrameSeed = (uint32)Resolved.Seed;
        }
        return true;
    }

    bool FDefaultSceneRenderer::IsParticleEmitterCulled(const FFrameData::FParticleExtract& Item, bool bForDraw) const
    {
        const FFrameData& Frame   = *RenderFrame;
        const FVector3    Center  = FVector3(Item.WorldMatrix * FVector4(Item.EmitterOffset, 1.0f));
        const FVector3    Camera  = Frame.ViewVolume.GetViewPosition();

        if (Item.Resolved.CullDistance > 0.0f && Math::Length(Center - Camera) > Item.Resolved.CullDistance)
        {
            return true;
        }

        if (bForDraw && Item.Resolved.VisibilityRadius > 0.0f)
        {
            const float Radius = Item.Resolved.VisibilityRadius * EmitterUniformScale(Item.WorldMatrix);
            return !Frame.CameraFrustum.IntersectsSphere(Center, Radius);
        }

        return false;
    }

    void FDefaultSceneRenderer::ParticleSimulatePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Particle Simulate", tracy::Color::Orange);

        const FFrameData& Frame = *RenderFrame;
        const float DeltaTime   = Frame.CachedWorldDeltaTime;

        bParticlePoolReuseBarrierIssued = false;
        ParticleSetupsThisFrame = 0;

        ReadParticleCollisions();
        ReadParticleGrids();

        if (!ParticleGPUStates.empty())
        {
            THashSet<ECS::FEntity> Live;
            Live.reserve(Frame.Extracts.LiveParticleEntities.size());
            for (ECS::FEntity Entity : Frame.Extracts.LiveParticleEntities)
            {
                Live.insert(Entity);
            }

            for (auto It = ParticleGPUStates.begin(); It != ParticleGPUStates.end();)
            {
                if (!Live.contains(It->first))
                {
                    for (FParticleGPUState& Dead : It->second)
                    {
                        ReleaseParticleState(Dead);
                    }
                    It = ParticleGPUStates.erase(It);
                }
                else
                {
                    ++It;
                }
            }
        }

        if (Frame.Extracts.ParticleExtracts.empty())
        {
            return;
        }

        // Shared by every emitter this frame, so each crosses to the GPU once.
        const RHI::TGPUSpan<FParticleShapeGPU> AttractorSpan = Frame.Extracts.ParticleAttractors.empty()
            ? RHI::TGPUSpan<FParticleShapeGPU>()
            : RHI::TGPUSpan<FParticleShapeGPU>(RHI::CopyTransientArray(Frame.Extracts.ParticleAttractors.data(), Frame.Extracts.ParticleAttractors.size()));
        const RHI::TGPUSpan<FParticleShapeGPU> ColliderSpan = Frame.Extracts.ParticleColliders.empty()
            ? RHI::TGPUSpan<FParticleShapeGPU>()
            : RHI::TGPUSpan<FParticleShapeGPU>(RHI::CopyTransientArray(Frame.Extracts.ParticleColliders.data(), Frame.Extracts.ParticleColliders.size()));

        TFixedVector<FParticleTerrainGPU, 4> Terrains;
        for (const FFrameData::FTerrainExtract& TerrainItem : Frame.Extracts.TerrainExtracts)
        {
            auto TerrainIt = TerrainGPUStates.find(TerrainItem.Entity);
            if (TerrainIt == TerrainGPUStates.end() || !TerrainIt->second.HeightmapTexture || !TerrainIt->second.NormalTexture)
            {
                continue;
            }

            const FVector3 Origin = FVector3(TerrainItem.WorldMatrix[3]);
            FParticleTerrainGPU& Terrain = Terrains.emplace_back();
            Terrain.OriginSize   = FVector4(Origin.x, Origin.z, TerrainItem.TileWorldSize, Origin.y);
            Terrain.HeightParams = FVector4(TerrainItem.MaxHeight, 0.0f, 0.0f, 0.0f);
            Terrain.Textures     = FUIntVector4((uint32)TerrainIt->second.HeightmapTexture.GetResourceID(), (uint32)TerrainIt->second.NormalTexture.GetResourceID(), 0u, 0u);
        }
        const RHI::TGPUSpan<FParticleTerrainGPU> TerrainSpan = Terrains.empty()
            ? RHI::TGPUSpan<FParticleTerrainGPU>()
            : RHI::TGPUSpan<FParticleTerrainGPU>(RHI::CopyTransientArray(Terrains.data(), Terrains.size()));

        static const FShaderH DefaultSimShader = FShaderLibrary::Get("ParticleSimulate.slang");

        // Sized for every emitter up front, since a slot's address goes into its arguments while planning.
        const uint32 SlotsNeeded = (uint32)Frame.Extracts.ParticleExtracts.size();
        if (SlotsNeeded > ParticleCounterArenaSlots)
        {
            if (ParticleCounterArena)
            {
                DeferFree(ParticleCounterArena);
            }
            ParticleCounterArenaSlots = Math::Max(SlotsNeeded + SlotsNeeded / 2u, 256u);
            ParticleCounterArena = RHI::Malloc((uint64)ParticleCounterArenaSlots * ParticleCounterBytes, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
            RHI::SetDebugName(ParticleCounterArena.Gpu, "Particles.CounterArena");
        }
        ParticleCounterSlotsUsed = 0;

        bool bAnySimulated = false;
        bool bAnyGridTargetWritten = false;

        // A system's emitters sit next to each other and most share a shader, so both lookups usually repeat the previous one.
        ECS::FEntity                LastEntity = ECS::FEntity{};
        TVector<FParticleGPUState>* LastEntityStates = nullptr;
        FShaderH                    LastSimShader;
        RHI::FPipelineH             LastSimPipeline;

        for (const FFrameData::FParticleExtract& Item : Frame.Extracts.ParticleExtracts)
        {
            if (!Item.bReady)
            {
                continue;
            }

            const FResolvedParticleParams& Resolved = Item.Resolved;

            const uint32 MaxParticles = (uint32)Resolved.MaxParticles;
            if (MaxParticles == 0)
            {
                continue;
            }

            if (LastEntityStates == nullptr || Item.Entity != LastEntity)
            {
                LastEntity       = Item.Entity;
                LastEntityStates = &ParticleGPUStates[Item.Entity];
            }
            TVector<FParticleGPUState>& EntityStates = *LastEntityStates;
            if ((int32)EntityStates.size() != Item.EmitterCount)
            {
                for (int32 Stale = Item.EmitterCount; Stale < (int32)EntityStates.size(); ++Stale)
                {
                    ReleaseParticleState(EntityStates[(size_t)Stale]);
                }
                EntityStates.resize((size_t)Math::Max(Item.EmitterCount, 1));
            }
            FParticleGPUState& State = EntityStates[(size_t)Item.EmitterIndex];

            // An emitter that reaches none of the dispatches below is fully dead, and draws nothing.
            State.bSimulatedThisFrame = false;

            if (IsParticleEmitterCulled(Item, false))
            {
                continue;
            }

            if (!EnsureParticleBuffers(CL, Item, State))
            {
                continue;
            }

            if (Item.bForceReset)
            {
                RHI::CmdMemset(CL, { State.ParticleBuffer.Gpu, State.ParticleBufferSize }, 0u);
                if (State.GridBufferSize > 0)
                {
                    // Both grids and the deposits are carved back to back.
                    RHI::CmdMemset(CL, { State.GridBuffers[0].Gpu, State.GridDepositBuffer.Gpu + State.GridBufferSize - State.GridBuffers[0].Gpu }, 0u);
                    State.GridCurrent = 0u;
                }
                State.AliveTimeRemaining = 0.0f;
                State.SpawnAccumulator   = 0.0f;
                State.SystemAge          = 0.0f;
                State.CycleTime          = -1.0f;
                State.bPrewarmPending    = true;
                if (Resolved.bUseFixedSeed)
                {
                    State.FrameSeed = (uint32)Resolved.Seed;
                }
            }
            if (Item.bForceBurst)
            {
                State.bBurstPending = true;
            }

            const FShaderH ComputeShader = Item.bUsesCustomShader ? Item.CustomComputeShader : DefaultSimShader;
            if (ComputeShader != LastSimShader || !LastSimPipeline)
            {
                const FShaderEntry* SimEntry = FShaderLibrary::Resolve(ComputeShader);
                if (SimEntry == nullptr || !SimEntry->IsValid())
                {
                    continue;
                }

                // A system's own simulation compiles in the background, and the emitter stays dormant until it lands.
                LastSimShader   = ComputeShader;
                LastSimPipeline = Item.bUsesCustomShader ? FindComputePipeline(ComputeShader) : GetOrCreateComputePipeline(ComputeShader);
            }
            const RHI::FPipelineH SimPipeline = LastSimPipeline;
            if (!SimPipeline)
            {
                continue;
            }

            // Steps are either one per frame or a whole number of fixed steps, preceded by any prewarm.
            const float FrameDelta = DeltaTime * Item.TimeScale;
            TFixedVector<float, 8> StepDeltas;
            uint32 PrewarmSteps = 0;
            if (State.bPrewarmPending && Resolved.PrewarmTime > 0.0f)
            {
                PrewarmSteps = ToClampedCount(Math::Ceil(Resolved.PrewarmTime / ParticlePrewarmStep), ParticleMaxPrewarmSteps);
            }
            State.bPrewarmPending = false;

            if (Resolved.FixedFPS > 0.0f)
            {
                const float Step = 1.0f / Resolved.FixedFPS;
                State.FixedStepRemainder += FrameDelta;
                const uint32 Steps = ToClampedCount(State.FixedStepRemainder / Step, ParticleMaxFixedSteps);
                State.FixedStepRemainder = Math::Min(State.FixedStepRemainder - (float)Steps * Step, Step);
                for (uint32 Index = 0; Index < Steps; ++Index)
                {
                    StepDeltas.push_back(Step);
                }
            }
            else
            {
                State.FixedStepRemainder = 0.0f;
                StepDeltas.push_back(FrameDelta);
            }

            const FMatrix4 WorldMat       = Item.WorldMatrix;
            const FVector3 EmitterWorld   = FVector3(WorldMat * FVector4(Item.EmitterOffset, 1.0f));
            const FVector3 EmitterRight   = Math::Normalize(FVector3(WorldMat[0]));
            const FVector3 EmitterUp      = Math::Normalize(FVector3(WorldMat[1]));
            const FVector3 EmitterForward = Math::Normalize(FVector3(WorldMat[2]));
            const float    EmitterScale   = EmitterUniformScale(WorldMat);

            FVector3 EmitterVelocity(0.0f);
            if (State.bHasPrevPosition && DeltaTime > 0.0f)
            {
                EmitterVelocity = (EmitterWorld - State.PrevEmitterPosition) / DeltaTime;
            }
            State.PrevEmitterPosition = EmitterWorld;
            State.bHasPrevPosition    = true;

            // Nearly every emitter stands still, and an unmoved one needs no inverse to know its delta is the identity.
            const bool bMoved = State.bHasPrevMatrix && Memory::Memcmp(&WorldMat, &State.PrevEmitterMatrix, sizeof(FMatrix4)) != 0;
            const FMatrix4 EmitterDelta = bMoved ? WorldMat * Math::Inverse(State.PrevEmitterMatrix) : FMatrix4(1.0f);
            State.PrevEmitterMatrix = WorldMat;
            State.bHasPrevMatrix    = true;

            // A sub-emitter keeps simulating while its parent runs, since the parent's events arrive unannounced.
            FParticleGPUState* Parent = nullptr;
            if (Item.SourceEmitterIndex >= 0 && Item.SourceEmitterIndex < (int32)EntityStates.size())
            {
                Parent = &EntityStates[(size_t)Item.SourceEmitterIndex];
                if (Parent->bSimulatedThisFrame || Parent->AliveTimeRemaining > 0.0f)
                {
                    State.ParentAliveTime = Math::Max(Resolved.LifetimeRange.y, 0.0f) + 0.5f;
                }
            }

            const float InheritFactor = Math::Clamp(Resolved.InheritEmitterVelocity, 0.0f, 1.0f);
            const float CycleLength   = Resolved.Duration > 0.0f ? Resolved.Duration : Math::Max(Resolved.LifetimeRange.y, 0.01f);
            const float MaxLifetime   = Math::Max(Resolved.LifetimeRange.y, 0.0f);
            const uint32 TotalSteps   = PrewarmSteps + (uint32)StepDeltas.size();

            const bool   bHasGrid  = State.AllocatedGridCells > 0u;
            const bool   bHasNeighbors = State.AllocatedNeighborCells > 0u;
            const uint32 GridFlags = Item.bGridWraps ? PARTICLE_GRID_FLAG_WRAP : 0u;

            // A later step reads what the previous one wrote, and a sub-emitter reads its parent's events, so both need barriers.
            const bool bBatched = TotalSteps == 1u && Item.SourceEmitterIndex < 0 && !bHasGrid && !bHasNeighbors && ParticleCounterArena;
            TVector<FParticleSimStep>& PlannedSteps = bBatched ? ParticleBatchedSteps : ParticleSerialSteps;
            State.FrameSpawnCounter = bBatched
                ? ParticleCounterArena.Gpu + (uint64)ParticleCounterSlotsUsed++ * ParticleCounterBytes
                : State.SpawnCounterBuffer.Gpu;

            // Work past the group limit of one dispatch splits into several over the same arguments.
            auto PlanStep = [&](const FParticleSimParamsGPU& Params, FParticleSimArgs Args, uint32 Threads, RHI::GPUPtr SpawnCounter, RHI::GPUPtr EventCounts, bool bBarrierAfter,
                                RHI::FGPUAllocation Clear = {})
            {
                Args.ParamsAddr = RHI::CopyTransient(Params);
                const uint32 ArgsOffset = (uint32)ParticleSimArgsScratch.size();
                ParticleSimArgsScratch.resize(ArgsOffset + sizeof(FParticleSimArgs));
                Memory::Memcpy(ParticleSimArgsScratch.data() + ArgsOffset, &Args, sizeof(FParticleSimArgs));

                const uint32 Groups = RenderUtils::GetGroupCount(Threads, PARTICLE_SIM_GROUP_SIZE);
                for (uint32 FirstGroup = 0u; FirstGroup < Groups; FirstGroup += ParticleMaxDispatchGroups)
                {
                    const bool bFirstChunk = FirstGroup == 0u;
                    PlannedSteps.push_back(FParticleSimStep
                    {
                        .Pipeline      = SimPipeline,
                        .ArgsOffset    = ArgsOffset,
                        .SpawnCounter  = bFirstChunk ? SpawnCounter : 0,
                        .EventCounts   = bFirstChunk ? EventCounts : 0,
                        .Groups        = Math::Min(Groups - FirstGroup, ParticleMaxDispatchGroups),
                        .FirstGroup    = FirstGroup,
                        .ClearAddress  = bFirstChunk ? Clear.Gpu : 0,
                        .ClearBytes    = bFirstChunk ? Clear.Size : 0,
                        .bBarrierAfter = bBarrierAfter && FirstGroup + ParticleMaxDispatchGroups >= Groups,
                    });
                }
            };

            // Each dispatch reads the newest grid and writes the other, so a stage of N iterations ping-pongs N times.
            auto PlanGridDispatch = [&](FParticleSimParamsGPU Params, FParticleSimArgs Args, uint32 Stage, uint32 Iteration, uint32 Flags)
            {
                Params.Counts.x     = State.AllocatedGridCells;
                Params.GridInfo.z   = Stage;
                Params.GridInfo.w   = GridFlags | Flags;
                Params.GridParams.z = (float)Iteration;
                Args.GridRead       = { State.GridBuffers[State.GridCurrent] };
                Args.GridWrite      = { State.GridBuffers[State.GridCurrent ^ 1u] };
                PlanStep(Params, Args, State.AllocatedGridCells, 0, 0, true);
                State.GridCurrent ^= 1u;
            };

            // Shared by every step this frame, so the arrays cross to the GPU once.
            RHI::TGPUSpan<FVector4>     ArrayDataSpan;
            RHI::TGPUSpan<FUIntVector2> ArrayRangeSpan;
            if (!Item.ArrayRanges.empty())
            {
                ArrayRangeSpan = RHI::CopyTransientArray(Item.ArrayRanges.data(), Item.ArrayRanges.size());
                if (!Item.ArrayData.empty())
                {
                    ArrayDataSpan = RHI::CopyTransientArray(Item.ArrayData.data(), Item.ArrayData.size());
                }
            }

            FParticleSimParamsGPU LastGridParams{};
            FParticleSimArgs      LastGridArgs{};
            bool                  bGridStepped = false;

            for (uint32 StepIndex = 0; StepIndex < TotalSteps; ++StepIndex)
            {
                const bool  bPrewarm   = StepIndex < PrewarmSteps;
                const float StepDelta  = bPrewarm ? ParticlePrewarmStep : StepDeltas[StepIndex - PrewarmSteps];
                const bool  bFirstStep = StepIndex == 0;
                const bool  bLastStep  = StepIndex + 1 == TotalSteps;

                const float PrevSystemAge = State.SystemAge;
                State.TotalTime += StepDelta;
                State.SystemAge += StepDelta;

                const bool bDurationExpired = (Resolved.Duration > 0.0f) && (State.SystemAge >= Resolved.Duration);
                if (bDurationExpired && Resolved.bLooping)
                {
                    State.SystemAge = fmodf(State.SystemAge, Resolved.Duration);
                    State.bBurstPending = true;
                }

                const bool  bEmitActive = Item.bEmit && !(bDurationExpired && !Resolved.bLooping);
                const float Rate        = Resolved.SpawnRate * Item.SpawnRateMultiplier;

                uint32 SpawnCount = 0;
                if (bEmitActive && Rate > 0.0f)
                {
                    State.SpawnAccumulator += StepDelta * Rate * (1.0f - Resolved.Explosiveness);
                    SpawnCount = ToClampedCount(State.SpawnAccumulator, MaxParticles);
                    State.SpawnAccumulator = Math::Clamp(State.SpawnAccumulator - (float)SpawnCount, 0.0f, 1.0f);

                    if (Resolved.Explosiveness > 0.0f)
                    {
                        const bool bCycleStart = State.CycleTime < 0.0f || State.CycleTime + StepDelta >= CycleLength;
                        State.CycleTime = bCycleStart ? 0.0f : State.CycleTime + StepDelta;
                        if (bCycleStart)
                        {
                            SpawnCount += ToClampedCount(Math::Round(Resolved.Explosiveness * Rate * CycleLength), MaxParticles);
                        }
                    }
                }
                else
                {
                    State.SpawnAccumulator = 0.0f;
                }

                if (bEmitActive && Resolved.NumBursts > 0)
                {
                    const float CycleClock = (Resolved.Duration > 0.0f) ? Resolved.Duration : 0.0f;
                    SpawnCount += CountBurstSpawns(TSpan<const SParticleBurst>(Resolved.Bursts.data(), (size_t)Resolved.NumBursts), PrevSystemAge, State.SystemAge, CycleClock);
                }

                if (bEmitActive && Resolved.SpawnPerMeter > 0.0f)
                {
                    State.DistanceAccumulator += Math::Length(EmitterVelocity) * StepDelta * Resolved.SpawnPerMeter;
                    const uint32 ByDistance = ToClampedCount(State.DistanceAccumulator, MaxParticles);
                    State.DistanceAccumulator = Math::Clamp(State.DistanceAccumulator - (float)ByDistance, 0.0f, 1.0f);
                    SpawnCount += ByDistance;
                }

                const bool bDoBurst = bEmitActive && Item.bBurstOnSpawn && State.bBurstPending && Resolved.BurstCount > 0;
                if (bDoBurst)
                {
                    SpawnCount += Math::Min((uint32)Resolved.BurstCount, MaxParticles);
                    State.bBurstPending = false;
                }
                else if (!Item.bBurstOnSpawn)
                {
                    State.bBurstPending = false;
                }

                SpawnCount = Math::Min(SpawnCount, MaxParticles);

                // Script emits and parent events are consumed once per frame, on its first step.
                const bool bConsumesScript = bFirstStep && !Item.ScriptEmits.empty();
                const bool bConsumesEvents = bFirstStep && Parent != nullptr && Parent->EventBuffer.Gpu != 0;
                if (SpawnCount > 0 || bConsumesScript || (bConsumesEvents && State.ParentAliveTime > 0.0f))
                {
                    State.AliveTimeRemaining = Math::Max(State.AliveTimeRemaining, MaxLifetime);
                }
                State.AliveTimeRemaining = Math::Max(State.AliveTimeRemaining - StepDelta, 0.0f);
                State.ParentAliveTime    = Math::Max(State.ParentAliveTime - StepDelta, 0.0f);

                // A grid keeps simulating with no particles alive, which is how an emitter without particles runs at all.
                const bool bParticlesIdle = SpawnCount == 0 && State.AliveTimeRemaining <= 0.0f && State.ParentAliveTime <= 0.0f && !bConsumesScript;
                if (bParticlesIdle && !bHasGrid)
                {
                    continue;
                }

                bool bRibbonAdvance = false;
                if (State.AllocatedRibbonSegments > 0u)
                {
                    const float Interval = Resolved.RibbonLength / (float)State.AllocatedRibbonSegments;
                    State.RibbonTimer += StepDelta;
                    if (State.RibbonTimer >= Interval)
                    {
                        State.RibbonTimer = Math::Min(State.RibbonTimer - Interval, Interval);
                        State.RibbonHead  = (State.RibbonHead + 1u) % State.AllocatedRibbonSegments;
                        bRibbonAdvance    = true;
                    }
                }

                State.FrameSeed = (State.FrameSeed + 2654435761u) ^ (Resolved.bUseFixedSeed ? 0u : (uint32)Item.Entity);

                uint32 SimFlags = 0u;
                if (Resolved.bLooping)
                {
                    SimFlags |= PARTICLE_SIM_FLAG_LOOP;
                }
                if (State.bBurstPending)
                {
                    SimFlags |= PARTICLE_SIM_FLAG_BURST_PENDING;
                }
                if (Resolved.bLocalSpace && bFirstStep)
                {
                    SimFlags |= PARTICLE_SIM_FLAG_LOCAL_SPACE;
                }

                FParticleSimParamsGPU SimParams{};
                SimParams.EmitterPosition   = FVector4(EmitterWorld, 1.0f);
                SimParams.EmitterForward    = FVector4(EmitterForward, EmitterVelocity.x);
                SimParams.EmitterRight      = FVector4(EmitterRight,   EmitterVelocity.y);
                SimParams.EmitterUp         = FVector4(EmitterUp,      EmitterVelocity.z);
                SimParams.Counts            = FUIntVector4(MaxParticles, SpawnCount, State.FrameSeed, SimFlags);
                SimParams.Modes             = FUIntVector4((uint32)Resolved.Shape, (uint32)Resolved.VelocityMode, 0u, 0u);
                SimParams.ShapeSize         = FVector4(Resolved.ShapeSize, Math::Radians(Resolved.ShapeAngle));
                SimParams.VelocityMin       = FVector4(Resolved.VelocityMin, 0.0f);
                SimParams.VelocityMax       = FVector4(Resolved.VelocityMax, 0.0f);
                SimParams.SpeedAndLifetime  = FVector4(Resolved.SpeedRange.x, Resolved.SpeedRange.y, Resolved.LifetimeRange.x, Resolved.LifetimeRange.y);
                SimParams.Gravity           = FVector4(Resolved.Gravity, Resolved.Drag);
                SimParams.StartColor        = Resolved.StartColor;
                SimParams.EndColor          = Resolved.EndColor;
                SimParams.SizeRange         = FVector4(Resolved.StartSizeRange.x, Resolved.StartSizeRange.y, Resolved.EndSizeRange.x, Resolved.EndSizeRange.y);
                SimParams.RotationRange     = FVector4(Resolved.RotationRange.x, Resolved.RotationRange.y, Resolved.RotationSpeedRange.x, Resolved.RotationSpeedRange.y);
                SimParams.NoiseStrength     = FVector4(Resolved.NoiseStrength, Resolved.NoiseScale);
                SimParams.NoiseParams       = FVector4(Resolved.NoiseSpeed, InheritFactor, 0.0f, 0.0f);
                SimParams.Timing            = FVector4(StepDelta, State.TotalTime, State.SystemAge, EmitterScale);
                SimParams.EmitterDelta      = bFirstStep ? EmitterDelta : FMatrix4(1.0f);
                SimParams.EventParams       = FVector4(Item.RaisedEventRate, Resolved.InheritParentVelocity, Resolved.bSpawnAlongSurfaceNormal ? 1.0f : 0.0f, 0.0f);
                SimParams.EventInfo         = FUIntVector4(Item.RaisedEventMask,
                                                           bConsumesEvents ? (uint32)Resolved.SpawnEvent : PARTICLE_NO_EVENT_LIST,
                                                           Item.EmissionMeshSlot,
                                                           (uint32)Resolved.ParticlesPerEvent);
                SimParams.RibbonInfo        = FUIntVector4(State.AllocatedRibbonSegments, State.RibbonHead, bRibbonAdvance ? 1u : 0u, 0u);
                SimParams.GridInfo          = FUIntVector4(Item.GridResolution.x, Item.GridResolution.y, PARTICLE_STAGE_PARTICLES, GridFlags);
                SimParams.GridParams        = FVector4(Item.GridWorldSize.x, Item.GridWorldSize.y, 0.0f, 0.0f);
                SimParams.GridTarget        = FUIntVector4(Item.GridTargetSlot, Item.GridTargetExtent.x, Item.GridTargetExtent.y, 0u);
                SimParams.NeighborParams    = FVector4(Item.NeighborCellSize, (float)Item.NeighborResolution, (float)Item.NeighborsPerCell, 0.0f);

                FParticleSimArgs SimArgs = {};
                SimArgs.Particles    = { State.ParticleBuffer, MaxParticles };
                SimArgs.SpawnCounter = RHI::TGPUSpan<uint32>::FromAddress(State.FrameSpawnCounter, ParticleCounterBytes / sizeof(uint32));
                if (!Item.ModuleParamValues.empty())
                {
                    SimArgs.ModuleParams = RHI::CopyTransientArray(Item.ModuleParamValues.data(), Item.ModuleParamValues.size());
                }
                SimArgs.Attributes   = { State.AttributeBuffer };
                if (State.EventBuffer.Gpu != 0)
                {
                    SimArgs.OwnEventCounts = RHI::TGPUSpan<uint32>::FromAddress(State.EventBuffer.Gpu, PARTICLE_EVENT_LISTS);
                    SimArgs.OwnEvents      = RHI::TGPUSpan<FParticleEventGPU>::FromAddress(State.EventBuffer.Gpu + ParticleEventCountBytes, PARTICLE_EVENT_LISTS * PARTICLE_EVENT_CAPACITY);
                }
                if (bConsumesEvents)
                {
                    SimArgs.SourceEventCounts = RHI::TGPUSpan<uint32>::FromAddress(Parent->EventBuffer.Gpu, PARTICLE_EVENT_LISTS);
                    SimArgs.SourceEvents      = RHI::TGPUSpan<FParticleEventGPU>::FromAddress(Parent->EventBuffer.Gpu + ParticleEventCountBytes, PARTICLE_EVENT_LISTS * PARTICLE_EVENT_CAPACITY);
                }
                if (bConsumesScript)
                {
                    SimArgs.ScriptEvents = RHI::CopyTransientArray(Item.ScriptEmits.data(), Item.ScriptEmits.size());
                }
                SimArgs.RibbonHistory = { State.RibbonHistoryBuffer };
                SimArgs.Attractors    = AttractorSpan;
                SimArgs.Colliders     = ColliderSpan;
                SimArgs.Terrains      = TerrainSpan;
                SimArgs.ArrayData     = ArrayDataSpan;
                SimArgs.ArrayRanges   = ArrayRangeSpan;
                SimArgs.NeighborCounts  = { State.NeighborCounts };
                SimArgs.NeighborIndices = { State.NeighborIndices };
                if (bHasGrid)
                {
                    SimArgs.GridRead    = { State.GridBuffers[State.GridCurrent] };
                    SimArgs.GridDeposit = { State.GridDepositBuffer };
                }

                if (!bParticlesIdle)
                {
                    // The next step, a grid stage folding deposits, a sub-emitter reading these events, or next frame's event clear all touch what this wrote.
                    PlanStep(SimParams, SimArgs, MaxParticles, bBatched ? 0 : State.FrameSpawnCounter, bFirstStep ? State.EventBuffer.Gpu : 0,
                             !bLastStep || bHasGrid || bHasNeighbors || Item.RaisedEventMask != 0u || Item.SourceEmitterIndex >= 0);
                    State.bSimulatedThisFrame = true;

                    // Buckets the particles where this step left them, for the next step's neighbor lookups.
                    if (bHasNeighbors)
                    {
                        FParticleSimParamsGPU BucketParams = SimParams;
                        BucketParams.GridInfo.z = PARTICLE_STAGE_NEIGHBORS;
                        PlanStep(BucketParams, SimArgs, MaxParticles, 0, 0, true, State.NeighborCounts);
                    }
                }

                if (bHasGrid)
                {
                    // With no stage dispatched, one that matches no stage still folds the deposits in, so they never pile up.
                    uint32 Flags = PARTICLE_GRID_FLAG_ABSORB;
                    const uint32 StageCount = (uint32)Item.GridStageIterations.size();
                    for (uint32 Stage = 0u; Stage < StageCount; ++Stage)
                    {
                        const uint32 Iterations = (uint32)Math::Clamp(Item.GridStageIterations[Stage], 0, ParticleStageMaxIterations);
                        for (uint32 Iteration = 0u; Iteration < Iterations; ++Iteration)
                        {
                            PlanGridDispatch(SimParams, SimArgs, Stage + 1u, Iteration, Flags);
                            Flags = 0u;
                        }
                    }
                    if (Flags != 0u)
                    {
                        PlanGridDispatch(SimParams, SimArgs, StageCount + 1u, 0u, Flags);
                    }

                    LastGridParams = SimParams;
                    LastGridArgs   = SimArgs;
                    bGridStepped   = true;
                }
                bAnySimulated = true;
            }

            if (bGridStepped && Item.bReadBackGrid)
            {
                FParticleGridReadback& Readback = ParticleGridReadback[CurrentFrameSlot];
                FParticleGridSnapshot& Layout = Readback.Layouts.emplace_back();
                Layout.EmitterIndex = Item.EmitterIndex;
                Layout.Size         = Item.GridResolution;
                Layout.Channels     = State.AllocatedGridChannels;
                Layout.bWraps       = Item.bGridWraps;
                Layout.Origin       = EmitterWorld;
                Layout.Right        = EmitterRight;
                Layout.Forward      = EmitterForward;
                Layout.WorldSize    = Item.GridWorldSize;
                Readback.Grids.push_back(State.GridBuffers[State.GridCurrent].Gpu);
                Readback.Owners.push_back(Item.Entity);
            }

            if (bGridStepped && Item.GridTargetSlot != RHI::kInvalidHeapSlot)
            {
                const uint32 Texels = Item.GridTargetExtent.x * Item.GridTargetExtent.y;
                LastGridParams.Counts.x   = Texels;
                LastGridParams.GridInfo.z = PARTICLE_STAGE_TARGET;
                LastGridArgs.GridRead     = { State.GridBuffers[State.GridCurrent] };
                PlanStep(LastGridParams, LastGridArgs, Texels, 0, 0, false);
                bAnyGridTargetWritten = true;
            }

            TVector<FParticleCollisionSource>& CollisionSources = ParticleCollisionReadback[CurrentFrameSlot].Sources;
            if (Item.bReportCollisions && State.bSimulatedThisFrame && State.EventBuffer && CollisionSources.size() < ParticleMaxCollisionSources)
            {
                CollisionSources.push_back({ Item.Entity, Item.EmitterIndex, State.EventBuffer.Gpu });
            }
        }

        RecordParticleSimSteps(CL);

        CopyParticleCollisions(CL);
        CopyParticleGrids(CL);

        if (bAnyGridTargetWritten)
        {
            // Materials in every stage sample a grid's render target, from the next frame on.
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::PixelShader | RHI::EStageFlags::VertexShader | RHI::EStageFlags::MeshShader | RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }

        if (bAnySimulated)
        {
            // Simulated particles feed the sort pass, then the render pass VS.
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::VertexShader,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
        }
    }

    void FDefaultSceneRenderer::RecordParticleSimSteps(RHI::FCmdListH CL)
    {
        auto ClearCounters = [&](const FParticleSimStep& Step)
        {
            if (Step.SpawnCounter != 0)
            {
                RHI::CmdMemset(CL, { Step.SpawnCounter, ParticleCounterBytes }, 0u);
            }
            if (Step.EventCounts != 0)
            {
                RHI::CmdMemset(CL, { Step.EventCounts, ParticleEventCountBytes }, 0u);
            }
            if (Step.ClearAddress != 0)
            {
                RHI::CmdMemset(CL, { Step.ClearAddress, Step.ClearBytes }, 0u);
            }
            return Step.SpawnCounter != 0 || Step.EventCounts != 0 || Step.ClearAddress != 0;
        };

        auto ClearsToCompute = [&]()
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        };

        auto ComputeToNext = [&]()
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::TransferWrite);
        };

        // A run of steps on one pipeline becomes as few dispatches as the group limit allows.
        auto DispatchBatch = [&](const FParticleSimStep* Steps, uint32 Count)
        {
            uint32 First = 0u;
            while (First < Count)
            {
                ParticleSimBatchEmitters.clear();
                ParticleSimBatchGroups.clear();

                uint32 End = First;
                while (End < Count && (ParticleSimBatchGroups.empty() || ParticleSimBatchGroups.size() + Steps[End].Groups <= ParticleMaxDispatchGroups))
                {
                    const uint32 EmitterIndex = (uint32)(ParticleSimBatchEmitters.size() / sizeof(FParticleSimArgs));
                    const size_t At = ParticleSimBatchEmitters.size();
                    ParticleSimBatchEmitters.resize(At + sizeof(FParticleSimArgs));
                    Memory::Memcpy(ParticleSimBatchEmitters.data() + At, ParticleSimArgsScratch.data() + Steps[End].ArgsOffset, sizeof(FParticleSimArgs));
                    for (uint32 Group = 0u; Group < Steps[End].Groups; ++Group)
                    {
                        ParticleSimBatchGroups.push_back(FUIntVector2(EmitterIndex, Steps[End].FirstGroup + Group));
                    }
                    ++End;
                }

                const uint32 NumEmitters = (uint32)(ParticleSimBatchEmitters.size() / sizeof(FParticleSimArgs));
                const uint32 NumGroups   = (uint32)ParticleSimBatchGroups.size();
                if (NumGroups > 0u)
                {
                    const FParticleSimBatch Batch
                    {
                        .Emitters = RHI::TGPUSpan<FParticleSimArgs>(RHI::CopyTransientArray(reinterpret_cast<const FParticleSimArgs*>(ParticleSimBatchEmitters.data()), NumEmitters)),
                        .Groups   = RHI::TGPUSpan<FUIntVector2>(RHI::CopyTransientArray(ParticleSimBatchGroups.data(), NumGroups)),
                    };
                    RHI::CmdDispatch(CL, MakeArgs(Batch), Math::Min(NumGroups, ParticleMaxDispatchGroups), 1u, 1u);
                }
                First = End;
            }
        };

        // Every emitter here owns its buffers, so one barrier covers all their clears and the dispatches can overlap.
        if (!ParticleBatchedSteps.empty())
        {
            // Last frame's sorts read these counters, and the clear rewrites them.
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite);
            RHI::CmdMemset(CL, { ParticleCounterArena.Gpu, (uint64)ParticleCounterSlotsUsed * ParticleCounterBytes }, 0u);

            bool bAnyBarrierAfter = false;
            for (const FParticleSimStep& Step : ParticleBatchedSteps)
            {
                ClearCounters(Step);
                bAnyBarrierAfter |= Step.bBarrierAfter;
            }
            ClearsToCompute();

            Algo::Sort(ParticleBatchedSteps, [](const FParticleSimStep& A, const FParticleSimStep& B) { return A.Pipeline.Handle < B.Pipeline.Handle; });

            const uint32 NumBatched = (uint32)ParticleBatchedSteps.size();
            for (uint32 RunStart = 0u; RunStart < NumBatched;)
            {
                const RHI::FPipelineH Pipeline = ParticleBatchedSteps[RunStart].Pipeline;
                uint32 RunEnd = RunStart + 1u;
                while (RunEnd < NumBatched && ParticleBatchedSteps[RunEnd].Pipeline == Pipeline)
                {
                    ++RunEnd;
                }

                RHI::CmdSetPipeline(CL, Pipeline);
                DispatchBatch(ParticleBatchedSteps.data() + RunStart, RunEnd - RunStart);
                RunStart = RunEnd;
            }

            if (bAnyBarrierAfter || !ParticleSerialSteps.empty())
            {
                ComputeToNext();
            }
        }

        for (const FParticleSimStep& Step : ParticleSerialSteps)
        {
            if (ClearCounters(Step))
            {
                ClearsToCompute();
            }
            RHI::CmdSetPipeline(CL, Step.Pipeline);
            DispatchBatch(&Step, 1u);
            if (Step.bBarrierAfter)
            {
                ComputeToNext();
            }
        }

        ParticleBatchedSteps.clear();
        ParticleSerialSteps.clear();
        ParticleSimArgsScratch.clear();
    }

    void FDefaultSceneRenderer::ReadParticleCollisions()
    {
        // The frame ring waited on this slot's last submission before the render phase began, so its copies have landed.
        FParticleCollisionReadback& Readback = ParticleCollisionReadback[CurrentFrameSlot];
        const std::byte* Base = Readback.Buffer.Cpu;
        for (size_t Index = 0; Base != nullptr && Index < Readback.Sources.size(); ++Index)
        {
            const FParticleCollisionSource& Source = Readback.Sources[Index];
            const std::byte* Copy = Base + Index * ParticleCollisionReadbackStride;
            const uint32 Raised = reinterpret_cast<const uint32*>(Copy)[(uint32)EParticleEventType::Collision];
            const uint32 Count = Math::Min(Raised, (uint32)PARTICLE_EVENT_CAPACITY);
            const FParticleEventGPU* Events = reinterpret_cast<const FParticleEventGPU*>(Copy + ParticleEventCountBytes);

            TVector<SParticleCollision>& Hits = ParticleCollisionResults[Source.Entity];
            Hits.reserve(Hits.size() + Count);
            for (uint32 EventIndex = 0; EventIndex < Count; ++EventIndex)
            {
                const FParticleEventGPU& Event = Events[EventIndex];
                SParticleCollision& Hit = Hits.emplace_back();
                Hit.Position     = FVector3(Event.PositionSize);
                Hit.Size         = Event.PositionSize.w;
                Hit.Velocity     = FVector3(Event.VelocityFlags);
                Hit.Normal       = FVector3(Event.Extra);
                Hit.ImpactSpeed  = Event.Extra.w;
                Hit.EmitterIndex = Source.EmitterIndex;
            }
        }
        Readback.Sources.clear();
    }

    void FDefaultSceneRenderer::ReadParticleGrids()
    {
        // As with collisions, the frame ring already waited on this slot, so its copies have landed.
        FParticleGridReadback& Readback = ParticleGridReadback[CurrentFrameSlot];
        const float* Base = reinterpret_cast<const float*>(Readback.Buffer.Cpu);
        size_t Cursor = 0;
        for (size_t Index = 0; Base != nullptr && Index < Readback.Layouts.size(); ++Index)
        {
            FParticleGridSnapshot& Snapshot = Readback.Layouts[Index];
            const size_t Floats = (size_t)Snapshot.Size.x * Snapshot.Size.y * Snapshot.Channels;
            Snapshot.Values.assign(Base + Cursor, Base + Cursor + Floats);
            Cursor += Floats;
            ParticleGridResults[Readback.Owners[Index]].push_back(Move(Snapshot));
        }
        Readback.Layouts.clear();
        Readback.Grids.clear();
        Readback.Owners.clear();
    }

    void FDefaultSceneRenderer::CopyParticleGrids(RHI::FCmdListH CL)
    {
        FParticleGridReadback& Readback = ParticleGridReadback[CurrentFrameSlot];
        if (Readback.Layouts.empty())
        {
            return;
        }

        // Grids past the budget drop out, and those emitters keep their previous copy.
        uint64 Bytes = 0;
        size_t Kept = 0;
        for (; Kept < Readback.Layouts.size(); ++Kept)
        {
            const FParticleGridSnapshot& Layout = Readback.Layouts[Kept];
            const uint64 GridBytes = (uint64)Layout.Size.x * Layout.Size.y * Layout.Channels * sizeof(float);
            if (Bytes + GridBytes > ParticleGridReadbackBudget)
            {
                break;
            }
            Bytes += GridBytes;
        }
        Readback.Layouts.resize(Kept);
        Readback.Grids.resize(Kept);
        Readback.Owners.resize(Kept);
        if (Bytes == 0)
        {
            return;
        }

        if (Readback.Buffer.Size < Bytes)
        {
            if (Readback.Buffer)
            {
                DeferFree(Readback.Buffer);
            }
            // Rounded up to a whole megabyte, so a grid that grows a little does not reallocate every frame.
            Readback.Buffer = RHI::Malloc(Math::AlignUp(Bytes, 1ull << 20), RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            if (!Readback.Buffer)
            {
                Readback.Layouts.clear();
                Readback.Grids.clear();
                Readback.Owners.clear();
                return;
            }
            RHI::SetDebugName(Readback.Buffer.Gpu, "Readback.ParticleGrids");
        }

        TVector<RHI::FBufferCopy> Copies;
        uint64 Offset = 0;
        for (size_t Index = 0; Index < Readback.Layouts.size(); ++Index)
        {
            const FParticleGridSnapshot& Layout = Readback.Layouts[Index];
            const uint64 GridBytes = (uint64)Layout.Size.x * Layout.Size.y * Layout.Channels * sizeof(float);
            Copies.push_back(RHI::FBufferCopy(Readback.Buffer.Sub(Offset, GridBytes), RHI::FGPURange{ Readback.Grids[Index], GridBytes }));
            Offset += GridBytes;
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead);
        RHI::CmdMemcpyBatch(CL, TSpan<const RHI::FBufferCopy>(Copies.data(), Copies.size()));

        // The next frame's stages write these grids, and the CPU reads the copy once this slot comes round.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::Host,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::HostRead);
    }

    void FDefaultSceneRenderer::CopyParticleCollisions(RHI::FCmdListH CL)
    {
        FParticleCollisionReadback& Readback = ParticleCollisionReadback[CurrentFrameSlot];
        if (Readback.Sources.empty())
        {
            return;
        }

        if (!Readback.Buffer)
        {
            Readback.Buffer = RHI::Malloc(ParticleMaxCollisionSources * ParticleCollisionReadbackStride, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
            if (!Readback.Buffer)
            {
                Readback.Sources.clear();
                return;
            }
            RHI::SetDebugName(Readback.Buffer.Gpu, "Readback.ParticleCollisions");
        }

        TFixedVector<RHI::FBufferCopy, ParticleMaxCollisionSources * 2> Copies;
        for (size_t Index = 0; Index < Readback.Sources.size(); ++Index)
        {
            const uint64 Offset = Index * ParticleCollisionReadbackStride;
            const RHI::GPUPtr Events = Readback.Sources[Index].EventBuffer;
            Copies.push_back(RHI::FBufferCopy(Readback.Buffer.Sub(Offset, ParticleEventCountBytes), RHI::FGPURange{ Events, ParticleEventCountBytes }));
            Copies.push_back(RHI::FBufferCopy(Readback.Buffer.Sub(Offset + ParticleEventCountBytes, ParticleCollisionListBytes),
                RHI::FGPURange{ Events + ParticleCollisionListOffset, ParticleCollisionListBytes }));
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead);
        RHI::CmdMemcpyBatch(CL, TSpan<const RHI::FBufferCopy>(Copies.data(), Copies.size()));

        // Next frame clears these counts, and the CPU reads the copy once this slot comes round.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Transfer | RHI::EStageFlags::Compute | RHI::EStageFlags::Host,
            RHI::EAccessFlags::TransferWrite | RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::HostRead);
    }

    // ParticleSortCompact packs a slot index into the low bits of every sort key.
    static_assert((1u << PARTICLE_SORT_INDEX_BITS) == PARTICLE_SORT_CAPACITY,
                  "The sort key's index field must address exactly the sortable capacity.");
    static_assert((1u << PARTICLE_GLOBAL_SORT_INDEX_BITS) == PARTICLE_GLOBAL_SORT_CAPACITY,
                  "The multi-pass sort key's index field must address exactly its capacity.");

    // Stages of ParticleSortGlobal.slang.
    enum class EParticleGlobalSortStage : uint32
    {
        Keys,
        LocalSort,
        GlobalStep,
        LocalMerge,
        Compact,
    };

    void FDefaultSceneRenderer::SortParticlesGlobal(RHI::FCmdListH CL, FParticleGPUState& State, uint32 SortMode, uint32 VertsPerParticle)
    {
        static const FShaderH GlobalSortShader = FShaderLibrary::Get("ParticleSortGlobal.slang");
        const FShaderEntry* Entry = FShaderLibrary::Resolve(GlobalSortShader);
        if (Entry == nullptr || !Entry->IsValid() || !State.SortKeyBuffer)
        {
            return;
        }

        // Mirrors FParticleGlobalSortArgs in ParticleSortGlobal.slang.
        struct FParticleGlobalSortArgs
        {
            RHI::TGPUSpan<FGPUParticle>                 Particles;
            RHI::TGPUSpan<uint32>                       Keys;
            RHI::TGPUSpan<uint32>                       OutIndices;
            RHI::TGPUSpan<RHI::FDrawIndirectArguments>  OutDrawArgs;
            RHI::TGPUSpan<uint32>                       Counters;
            uint32 SortCount;
            uint32 Stage;
            uint32 K;
            uint32 J;
            uint32 SortMode;
            uint32 VertsPerParticle;
        };
        static_assert(sizeof(FParticleGlobalSortArgs) == 104, "FParticleGlobalSortArgs must match ParticleSortGlobal.slang.");

        FParticleGlobalSortArgs Args = {};
        Args.Particles        = { State.ParticleBuffer, State.AllocatedMax };
        Args.Keys             = { State.SortKeyBuffer };
        Args.OutIndices       = { State.SortIndexBuffer };
        Args.OutDrawArgs      = { State.SortDrawArgsBuffer };
        Args.Counters         = RHI::TGPUSpan<uint32>::FromAddress(State.FrameSpawnCounter, ParticleCounterBytes / sizeof(uint32));
        Args.SortCount        = State.SortCount;
        Args.SortMode         = SortMode;
        Args.VertsPerParticle = VertsPerParticle;

        const uint32 ElementGroups = RenderUtils::GetGroupCount(State.SortCount, (uint32)PARTICLE_SORT_THREADS);
        const uint32 BlockGroups   = State.SortCount / PARTICLE_SORT_CAPACITY;

        auto Dispatch = [&](EParticleGlobalSortStage Stage, uint32 Groups, uint32 K, uint32 J)
        {
            Args.Stage = (uint32)Stage;
            Args.K     = K;
            Args.J     = J;
            RHI::CmdDispatch(CL, MakeArgs(Args), Groups, 1u, 1u);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        };

        RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(GlobalSortShader));
        Dispatch(EParticleGlobalSortStage::Keys, ElementGroups, 0u, 0u);
        Dispatch(EParticleGlobalSortStage::LocalSort, BlockGroups, 0u, 0u);
        for (uint32 K = PARTICLE_SORT_CAPACITY * 2u; K <= State.SortCount; K <<= 1u)
        {
            for (uint32 J = K >> 1u; J >= PARTICLE_SORT_CAPACITY; J >>= 1u)
            {
                Dispatch(EParticleGlobalSortStage::GlobalStep, ElementGroups, K, J);
            }
            Dispatch(EParticleGlobalSortStage::LocalMerge, BlockGroups, K, 0u);
        }
        Dispatch(EParticleGlobalSortStage::Compact, ElementGroups, 0u, 0u);
    }

    void FDefaultSceneRenderer::ParticleSortPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Particle Sort", tracy::Color::Orange);

        const FFrameData& Frame = *RenderFrame;

        static const FShaderH SortShader = FShaderLibrary::Get("ParticleSortCompact.slang");
        const FShaderEntry* SortEntry = FShaderLibrary::Resolve(SortShader);
        if (SortEntry == nullptr || !SortEntry->IsValid())
        {
            return;
        }

        // Mirrors FParticleSortEmitter in ParticleSortCompact.slang, one per workgroup of the single dispatch below.
        struct FParticleSortEmitter
        {
            RHI::TGPUSpan<FGPUParticle> Particles;
            RHI::TGPUSpan<uint32>       OutIndices;
            RHI::TGPUSpan<RHI::FDrawIndirectArguments> OutDrawArgs;
            uint32 SortCount;
            uint32 SortMode;
            uint32 VertsPerParticle;
            uint32 _Pad0;
        };
        static_assert(sizeof(FParticleSortEmitter) == 64, "FParticleSortEmitter must match ParticleSortCompact.slang.");

        ParticleSortScratch.clear();
        bool bAnySorted = false;

        for (const FFrameData::FParticleExtract& Item : Frame.Extracts.ParticleExtracts)
        {
            if (!Item.bReady || !IsParticleDrawable(Item) || IsParticleEmitterCulled(Item, true))
            {
                continue;
            }

            auto ParticleStateIt = ParticleGPUStates.find(Item.Entity);
            if (ParticleStateIt == ParticleGPUStates.end()
                || Item.EmitterIndex >= (int32)ParticleStateIt->second.size())
            {
                continue;
            }

            FParticleGPUState& State = ParticleStateIt->second[(size_t)Item.EmitterIndex];
            if (!State.bSimulatedThisFrame || State.SortCount == 0 || !State.SortIndexBuffer)
            {
                continue;
            }

            const uint32 SortMode         = (uint32)Item.Resolved.SortMode;
            const uint32 VertsPerParticle = ParticleVertsPerParticle(Item);
            bAnySorted = true;

            if (State.SortCount > PARTICLE_SORT_CAPACITY)
            {
                SortParticlesGlobal(CL, State, SortMode, VertsPerParticle);
                continue;
            }

            const FParticleSortEmitter Emitter
            {
                .Particles        = { State.ParticleBuffer, State.AllocatedMax },
                .OutIndices       = { State.SortIndexBuffer },
                .OutDrawArgs      = { State.SortDrawArgsBuffer },
                .SortCount        = State.SortCount,
                .SortMode         = SortMode,
                .VertsPerParticle = VertsPerParticle,
                ._Pad0            = 0u,
            };
            const size_t At = ParticleSortScratch.size();
            ParticleSortScratch.resize(At + sizeof(FParticleSortEmitter));
            Memory::Memcpy(ParticleSortScratch.data() + At, &Emitter, sizeof(FParticleSortEmitter));
        }

        // One workgroup per emitter, split only where a dispatch would pass the group count limit.
        const uint32 NumCompact = (uint32)(ParticleSortScratch.size() / sizeof(FParticleSortEmitter));
        if (NumCompact > 0u)
        {
            constexpr uint32 kMaxGroups = 65535u;
            const FParticleSortEmitter* Emitters = reinterpret_cast<const FParticleSortEmitter*>(ParticleSortScratch.data());
            const RHI::TGPUSpan<FParticleSortEmitter> Uploaded(RHI::CopyTransientArray(Emitters, NumCompact));

            struct FParticleSortArgs
            {
                RHI::TGPUSpan<FParticleSortEmitter> Emitters;
            };
            static_assert(sizeof(FParticleSortArgs) == 16, "FParticleSortArgs must match ParticleSortCompact.slang.");

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(SortShader));
            for (uint32 First = 0u; First < NumCompact; First += kMaxGroups)
            {
                const uint32 Count = Math::Min(NumCompact - First, kMaxGroups);
                const FParticleSortArgs SortArgs{ RHI::TGPUSpan<FParticleSortEmitter>::FromAddress(Uploaded.Address + (uint64)First * sizeof(FParticleSortEmitter), Count) };
                RHI::CmdDispatch(CL, MakeArgs(SortArgs), Count, 1u, 1u);
            }
        }

        if (bAnySorted)
        {
            // The compacted draw reads the sorted indices in its VS and its count as indirect arguments.
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::VertexShader | RHI::EStageFlags::IndirectArguments,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead | RHI::EAccessFlags::IndexRead);
        }
    }

    static RHI::FBlendDesc MakeParticleBlendFactors(EParticleBlendMode Mode)
    {
        RHI::FBlendDesc Blend;
        Blend.bBlendEnable = true;
        Blend.ColorOp      = RHI::EBlend::Add;
        Blend.AlphaOp      = RHI::EBlend::Add;

        switch (Mode)
        {
        case EParticleBlendMode::Additive:
            Blend.SrcColorFactor = RHI::EFactor::SrcAlpha;
            Blend.DstColorFactor = RHI::EFactor::One;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::One;
            break;

        case EParticleBlendMode::PreMultiplied:
            Blend.SrcColorFactor = RHI::EFactor::One;
            Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;
            break;

        case EParticleBlendMode::Multiply:
            Blend.SrcColorFactor = RHI::EFactor::DstColor;
            Blend.DstColorFactor = RHI::EFactor::Zero;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::Zero;
            break;

        case EParticleBlendMode::Alpha:
        default:
            Blend.SrcColorFactor = RHI::EFactor::SrcAlpha;
            Blend.DstColorFactor = RHI::EFactor::OneMinusSrcAlpha;
            Blend.SrcAlphaFactor = RHI::EFactor::One;
            Blend.DstAlphaFactor = RHI::EFactor::OneMinusSrcAlpha;
            break;
        }
        return Blend;
    }

    // A Particle material carries its own blend, so the emitter's EParticleBlendMode is not consulted.
    // The built-in pixel stage premultiplies alpha and additive itself, so only multiply needs a blend state of its own.
    static RHI::FBlendDesc MakeParticleBlend(EParticleBlendMode Mode)
    {
        return MakeParticleBlendFactors(Mode == EParticleBlendMode::Multiply ? EParticleBlendMode::Multiply : EParticleBlendMode::PreMultiplied);
    }

    static RHI::FBlendDesc MakeParticleMaterialBlend(EBlendMode Mode)
    {
        switch (Mode)
        {
        case EBlendMode::Additive:       return MakeParticleBlendFactors(EParticleBlendMode::Additive);
        case EBlendMode::Modulate:       return MakeParticleBlendFactors(EParticleBlendMode::Multiply);
        case EBlendMode::AlphaComposite: return MakeParticleBlendFactors(EParticleBlendMode::PreMultiplied);
        case EBlendMode::Translucent:    return MakeParticleBlendFactors(EParticleBlendMode::Alpha);

        // Opaque and Masked write coverage rather than blending it; the pixel stage clips instead.
        default:
            return RHI::FBlendDesc{};
        }
    }

    static FShaderH ParticleVertexShaderFor(EParticleRenderMode Mode)
    {
        static const FShaderH SpriteVertexShader = FShaderLibrary::Get("ParticleVertex.slang");
        static const FShaderH MeshVertexShader   = FShaderLibrary::Get("ParticleMeshVertex.slang");
        static const FShaderH RibbonVertexShader = FShaderLibrary::Get("ParticleRibbonVertex.slang");
        switch (Mode)
        {
        case EParticleRenderMode::Mesh:   return MeshVertexShader;
        case EParticleRenderMode::Ribbon: return RibbonVertexShader;
        default:                          return SpriteVertexShader;
        }
    }

    // Everything the three particle vertex stages read, before the per-pass fields are set.
    static FParticlePushConstants MakeParticlePushConstants(const FParticleExtract& Item, const FParticleGPUState& State, bool bMaterial)
    {
        const FResolvedParticleParams& Resolved = Item.Resolved;

        FParticlePushConstants PC = {};
        PC.Particles            = { State.ParticleBuffer, State.AllocatedMax };
        PC.TextureIndex         = Item.TextureIndex;
        PC.FacingMode           = (uint32)Resolved.FacingMode;
        PC.Tint                 = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
        PC.VelocityStretch      = Resolved.VelocityStretch;
        PC.SubUVColumns         = (uint32)Math::Max(Resolved.SubUVColumns, 1);
        PC.SubUVRows            = (uint32)Math::Max(Resolved.SubUVRows, 1);
        PC.AttrFloats           = Math::Max(Item.AttributeFloatCount, 1u);
        PC.Attributes           = { State.AttributeBuffer };
        PC.AttrSlotSizeScaleX   = Item.RenderAttrSlots[ParticleRenderAttribute::SizeScaleX];
        PC.AttrSlotSizeScaleY   = Item.RenderAttrSlots[ParticleRenderAttribute::SizeScaleY];
        PC.AttrSlotPrevPosX     = Item.RenderAttrSlots[ParticleRenderAttribute::PrevPosX];
        PC.AttrSlotPrevPosY     = Item.RenderAttrSlots[ParticleRenderAttribute::PrevPosY];
        PC.AttrSlotPrevPosZ     = Item.RenderAttrSlots[ParticleRenderAttribute::PrevPosZ];
        PC.MaterialIndex        = bMaterial ? (uint32)Item.MaterialIndex : 0u;
        PC.RenderFlags          = (uint32)Resolved.BlendMode | (Resolved.bLit ? PARTICLE_RENDER_FLAG_LIT : 0u);
        PC.RibbonHistory        = { State.RibbonHistoryBuffer };
        PC.RenderMode           = (uint32)Resolved.RenderMode;
        PC.VertsPerParticle     = ParticleVertsPerParticle(Item);
        PC.MeshletHeaderSlot    = Item.MeshletHeaderSlot;
        PC.bAlignMeshToVelocity = Resolved.bAlignMeshToVelocity ? 1u : 0u;
        PC.RibbonSegments       = State.AllocatedRibbonSegments;
        PC.RibbonHead           = State.RibbonHead;
        PC.RibbonTailWidth      = Resolved.RibbonTailWidth;
        PC.ExtrapolateTime      = (Resolved.FixedFPS > 0.0f && Resolved.bInterpolate) ? State.FixedStepRemainder : 0.0f;
        PC.FlipbookMode         = (uint32)Resolved.FlipbookMode;
        PC.FlipbookFPS          = Resolved.FlipbookFPS;
        PC.bRandomStartFrame    = Resolved.bRandomStartFrame ? 1u : 0u;
        PC.ShadowDataIndex      = Constants::kIndexNone;
        return PC;
    }

    void FDefaultSceneRenderer::ParticleRenderPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Particle Render", tracy::Color::OrangeRed);

        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (Frame.Extracts.ParticleExtracts.empty())
        {
            return;
        }

        static const FShaderH SpritePixelShader = FShaderLibrary::Get("ParticlePixel.slang");
        static const FShaderH DrawArgsShader    = FShaderLibrary::Get("ParticleDrawArgs.slang");
        if (!SpritePixelShader || !DrawArgsShader)
        {
            return;
        }

        const FSceneImage& HDR   = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Depth = GetNamedImage(ENamedImage::DepthAttachment);

        const bool bHDRWasWritten = !DrawCommands.empty() || FrameFlags.bHasEnvironment
            || !Frame.Extracts.TerrainExtracts.empty() || !Frame.Primitives.SolidBatches.empty()
            || !Frame.Primitives.LineBatches.empty() || bSceneColorClearedForCallbacks;

        RHI::FRenderAttachment Color;
        Color.Texture  = HDR.Texture;
        Color.LoadOp   = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Color.StoreOp  = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments         = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture  = Depth.Texture;
        // Meshes and terrain both lay depth down before this, so only a frame with neither may clear it.
        const bool bClearsDepth = DrawCommands.empty() && Frame.Extracts.TerrainExtracts.empty();
        Pass.DepthAttachment.LoadOp   = bClearsDepth ? RHI::ELoadOp::Clear : RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        Pass.DepthAttachment.Color[0] = 0.0f;
        Pass.RenderArea               = HDR.GetExtent();

        // Only the first pass may clear; a reopened pass continues from what the previous one stored.
        auto OpenParticlePass = [&]()
        {
            RHI::CmdBeginRenderPass(CL, Pass);
            SetViewportScissor(CL, HDR.GetExtent());
            RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
            Color.LoadOp                = RHI::ELoadOp::Load;
            Pass.DepthAttachment.LoadOp = RHI::ELoadOp::Load;
        };

        // Soft particles sample the depth attachment, so a pass that also writes it would be a feedback loop.
        auto IsolateDepthWriter = [&]()
        {
            RHI::CmdEndRenderPass(CL);
            Barriers::RasterToRead(CL);
            OpenParticlePass();
        };

        // Planned before anything is recorded, since the arguments are gathered by a dispatch that cannot run inside the pass.
        ParticleDraws.clear();
        ParticleDrawConstants.clear();
        ParticleDrawArgSources.clear();

        // Neighboring built-in emitters usually resolve to the same pipeline, which skips hashing a key for each.
        FShaderH        LastBuiltinVS;
        bool            bLastBuiltinMultiply = false;
        RHI::FPipelineH LastBuiltinPipeline;

        for (const FFrameData::FParticleExtract& Item : Frame.Extracts.ParticleExtracts)
        {
            if (!Item.bReady || !IsParticleDrawable(Item) || IsParticleEmitterCulled(Item, true))
            {
                continue;
            }

            // GPUState is render-owned; the snapshot supplies everything else.
            auto ParticleStateIt = ParticleGPUStates.find(Item.Entity);
            if (ParticleStateIt == ParticleGPUStates.end()
                || Item.EmitterIndex >= (int32)ParticleStateIt->second.size())
            {
                continue;
            }
            FParticleGPUState& State = ParticleStateIt->second[(size_t)Item.EmitterIndex];
            if (!State.ParticleBuffer || !State.bSimulatedThisFrame)
            {
                continue;
            }

            const bool bSorted = (State.SortCount > 0) && State.SortIndexBuffer && State.SortDrawArgsBuffer;

            const FResolvedParticleParams& Resolved = Item.Resolved;
            // A Particle material shades sprites only; meshes and ribbons keep the built-in stages.
            const bool bMaterial = (Item.MaterialIndex >= 0) && Resolved.RenderMode == EParticleRenderMode::Sprite;

            RHI::FPipelineH Pipeline;
            const FShaderH BuiltinVS = ParticleVertexShaderFor(Resolved.RenderMode);
            const bool bMultiply = Resolved.BlendMode == EParticleBlendMode::Multiply;
            if (!bMaterial && LastBuiltinPipeline && BuiltinVS == LastBuiltinVS && bMultiply == bLastBuiltinMultiply)
            {
                Pipeline = LastBuiltinPipeline;
            }
            else
            {
                FGraphicsPipelineKey Key;
                Key.VS          = bMaterial ? Item.MaterialVertexShader : BuiltinVS;
                Key.PS          = bMaterial ? Item.MaterialPixelShader  : SpritePixelShader;
                Key.DepthFormat = EFormat::D32;
                Key.ColorTargets.push_back({ HDR.Desc.Format, bMaterial
                    ? MakeParticleMaterialBlend(Item.MaterialBlendMode)
                    : MakeParticleBlend(Resolved.BlendMode) });
                Pipeline = bMaterial ? FindPipeline(Key) : GetOrCreatePipeline(Key);
                if (!bMaterial)
                {
                    LastBuiltinVS        = BuiltinVS;
                    bLastBuiltinMultiply = bMultiply;
                    LastBuiltinPipeline  = Pipeline;
                }
            }
            if (!Pipeline)
            {
                continue;
            }

            const bool bWriteDepth = bMaterial ? Item.bMaterialWritesDepth : Resolved.bWriteDepth;

            FParticlePushConstants PC = MakeParticlePushConstants(Item, State, bMaterial);
            PC.bSorted            = bSorted ? 1u : 0u;
            PC.SortedIndices      = { State.SortIndexBuffer };
            PC.SoftFadeDistance   = (bWriteDepth || bClearsDepth) ? 0.0f : Resolved.SoftFadeDistance;
            PC.CameraFadeDistance = Resolved.CameraFadeDistance;
            AppendBytes(ParticleDrawConstants, PC);

            FParticleDrawArgsSource Source = {};
            if (bSorted)
            {
                Source.SortedArgs = RHI::TGPUSpan<RHI::FDrawIndirectArguments>::FromAddress(State.SortDrawArgsBuffer.Gpu, 1u);
            }
            else
            {
                Source.VertexCount = PC.VertsPerParticle * State.AllocatedMax;
            }
            AppendBytes(ParticleDrawArgSources, Source);

            ParticleDraws.push_back({ Pipeline, bWriteDepth });
        }

        const uint32 NumDraws = (uint32)ParticleDraws.size();
        if (NumDraws > 0u)
        {
            if (NumDraws > ParticleDrawArgsArenaSlots)
            {
                if (ParticleDrawArgsArena)
                {
                    DeferFree(ParticleDrawArgsArena);
                }
                ParticleDrawArgsArenaSlots = Math::Max(NumDraws + NumDraws / 2u, 256u);
                ParticleDrawArgsArena = RHI::Malloc((uint64)ParticleDrawArgsArenaSlots * sizeof(RHI::FDrawIndirectArguments), RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
                RHI::SetDebugName(ParticleDrawArgsArena.Gpu, "Particles.DrawArgs");
            }

            // The sorts wrote the compacted counts, and last frame's draws may still be reading the arena this rewrites.
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute | RHI::EStageFlags::IndirectArguments,
                RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead,
                RHI::EStageFlags::Compute,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

            const FParticleDrawArgsPass GatherArgs
            {
                .Sources = RHI::TGPUSpan<FParticleDrawArgsSource>(RHI::CopyTransientArray(reinterpret_cast<const FParticleDrawArgsSource*>(ParticleDrawArgSources.data()), NumDraws)),
                .OutArgs = RHI::TGPUSpan<RHI::FDrawIndirectArguments>::FromAddress(ParticleDrawArgsArena.Gpu, NumDraws),
            };
            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(DrawArgsShader));
            RHI::CmdDispatch(CL, MakeArgs(GatherArgs), RenderUtils::GetGroupCount(NumDraws, 64u), 1u, 1u);

            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::IndirectArguments, RHI::EAccessFlags::IndirectRead);
        }

        OpenParticlePass();

        const RHI::GPUPtr DrawsBase = NumDraws > 0u
            ? RHI::CopyTransientArray(reinterpret_cast<const FParticlePushConstants*>(ParticleDrawConstants.data()), NumDraws).Address
            : 0;

        RHI::FPipelineH BoundPipeline;
        int32 BoundDepthWrite = -1;
        for (uint32 First = 0u; First < NumDraws;)
        {
            const FParticleDraw& Draw = ParticleDraws[First];

            // A depth writer gets a pass of its own, so it never batches with its neighbors.
            uint32 End = First + 1u;
            while (!Draw.bWriteDepth && End < NumDraws && ParticleDraws[End].Pipeline == Draw.Pipeline && !ParticleDraws[End].bWriteDepth)
            {
                ++End;
            }

            if (Draw.bWriteDepth)
            {
                // Reopening the pass drops the bound state, so the next draw binds again.
                IsolateDepthWriter();
                BoundPipeline   = {};
                BoundDepthWrite = -1;
            }
            if (BoundDepthWrite != (int32)Draw.bWriteDepth)
            {
                RHI::FDepthStencilDesc DepthDesc;
                DepthDesc.DepthMode = Draw.bWriteDepth
                    ? (RHI::EDepthFlags::Read | RHI::EDepthFlags::Write)
                    : RHI::EDepthFlags::Read;
                DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
                RHI::CmdSetDepthStencil(CL, DepthDesc);
                BoundDepthWrite = (int32)Draw.bWriteDepth;
            }
            if (Draw.Pipeline != BoundPipeline)
            {
                RHI::CmdSetPipeline(CL, Draw.Pipeline);
                BoundPipeline = Draw.Pipeline;
            }

            const uint32 Count = End - First;
            const FParticleDrawBatch Batch
            {
                .Draws = RHI::TGPUSpan<FParticlePushConstants>::FromAddress(DrawsBase + (uint64)First * sizeof(FParticlePushConstants), Count),
            };
            const RHI::FGPURange Args{ ParticleDrawArgsArena.Gpu + (uint64)First * sizeof(RHI::FDrawIndirectArguments), (uint64)Count * sizeof(RHI::FDrawIndirectArguments) };
            RHI::CmdDrawIndirect(CL, MakeArgs(Batch), Args, Count, sizeof(RHI::FDrawIndirectArguments));

            if (Draw.bWriteDepth)
            {
                IsolateDepthWriter();
                BoundPipeline   = {};
                BoundDepthWrite = -1;
            }
            First = End;
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }

    void FDefaultSceneRenderer::ParticleShadowCasters(RHI::FCmdListH CL, int32 SunShadowDataIndex)
    {
        const FFrameData& Frame = *RenderFrame;

        static const FShaderH ShadowPixelShader = FShaderLibrary::Get("ParticleShadowPixel.slang");
        if (!ShadowPixelShader)
        {
            return;
        }

        for (const FFrameData::FParticleExtract& Item : Frame.Extracts.ParticleExtracts)
        {
            if (!Item.bReady || !Item.Resolved.bCastShadows || !IsParticleDrawable(Item) || IsParticleEmitterCulled(Item, false))
            {
                continue;
            }

            // The cascades render before this frame's simulation, so they show the particles as last simulated.
            auto ParticleStateIt = ParticleGPUStates.find(Item.Entity);
            if (ParticleStateIt == ParticleGPUStates.end() || Item.EmitterIndex >= (int32)ParticleStateIt->second.size())
            {
                continue;
            }
            const FParticleGPUState& State = ParticleStateIt->second[(size_t)Item.EmitterIndex];
            if (!State.ParticleBuffer || !State.bSimulatedThisFrame)
            {
                continue;
            }

            FGraphicsPipelineKey Key;
            Key.VS          = ParticleVertexShaderFor(Item.Resolved.RenderMode);
            Key.PS          = ShadowPixelShader;
            Key.DepthFormat = EFormat::D32;
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FParticlePushConstants PC = MakeParticlePushConstants(Item, State, false);
            PC.bShadowPass     = 1u;
            PC.ShadowDataIndex = SunShadowDataIndex;

            for (uint32 Cascade = 0; Cascade < RenderFrame->Views.NumNearCascadeViews; ++Cascade)
            {
                const int32 TileX = GCSMCascadeOriginX((int)Cascade);
                const int32 TileY = GCSMCascadeOriginY((int)Cascade);
                const int32 TileW = GCSMCascadeSize;
                const RHI::FRect TileRect{ TileX, TileX + TileW, TileY, TileY + TileW };
                RHI::CmdSetViewport(CL, TileRect);
                RHI::CmdSetScissor(CL, TileRect);

                PC.ShadowCascade = Cascade;
                const FParticleDrawBatch Batch{ .Draws = RHI::TGPUSpan<FParticlePushConstants>(RHI::CopyTransientArray(&PC, 1u)) };
                RHI::CmdDraw(CL, MakeArgs(Batch), PC.VertsPerParticle * State.AllocatedMax, 1u, 0u, 0u);
            }
        }
    }
}
