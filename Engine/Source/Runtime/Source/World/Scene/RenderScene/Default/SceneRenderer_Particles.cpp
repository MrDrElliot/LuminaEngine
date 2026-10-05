#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

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
        return Item.Resolved.RenderMode != EParticleRenderMode::Mesh || Item.MeshletHeaderSlot != 0u;
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
        ReleaseParticleBuffer(State.ParticleBuffer, State.ParticleBufferSize);
        ReleaseParticleBuffer(State.SpawnCounterBuffer, ParticleCounterBytes);
        ReleaseParticleBuffer(State.AttributeBuffer, State.AttributeBufferSize);
        ReleaseParticleBuffer(State.SortIndexBuffer, State.SortIndexBufferSize);
        ReleaseParticleBuffer(State.SortDrawArgsBuffer, sizeof(RHI::FDrawIndirectArguments));
        ReleaseParticleBuffer(State.SortKeyBuffer, State.SortKeyBufferSize);
        ReleaseParticleBuffer(State.EventBuffer, State.EventBufferSize);
        ReleaseParticleBuffer(State.RibbonHistoryBuffer, State.RibbonHistorySize);
        State.AllocatedMax = 0;
        State.SortCount    = 0;
    }

    void FDefaultSceneRenderer::EnsureParticleBuffers(RHI::FCmdListH CL, const FFrameData::FParticleExtract& Item, FParticleGPUState& State)
    {
        const FResolvedParticleParams& Resolved = Item.Resolved;
        const uint32 MaxParticles    = (uint32)Resolved.MaxParticles;
        const uint32 RibbonSegments  = Resolved.RenderMode == EParticleRenderMode::Ribbon ? (uint32)Resolved.RibbonSegments : 0u;
        const bool   bWantsEvents    = Item.RaisedEventMask != 0u;
        const bool   bWantsSort      = Resolved.SortMode != EParticleSortMode::None && MaxParticles <= PARTICLE_GLOBAL_SORT_CAPACITY;

        const bool bNeedsAlloc = (State.ParticleBuffer.Gpu == 0)
                              || (State.AllocatedMax != MaxParticles)
                              || (State.AllocatedAttributeFloats != Item.AttributeFloatCount)
                              || (State.AllocatedRibbonSegments != RibbonSegments)
                              || ((State.EventBuffer.Gpu != 0) != bWantsEvents)
                              || ((State.SortCount > 0) != bWantsSort);
        if (!bNeedsAlloc)
        {
            return;
        }

        ReleaseParticleState(State);

        // A pooled buffer may still be read by an earlier frame's draw, so its clear waits for those reads.
        if (!bParticlePoolReuseBarrierIssued)
        {
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute | RHI::EStageFlags::VertexShader | RHI::EStageFlags::PixelShader | RHI::EStageFlags::IndirectArguments,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndirectRead,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite);
            bParticlePoolReuseBarrierIssued = true;
        }

        State.ParticleBufferSize  = (uint64)MaxParticles * sizeof(FGPUParticle);
        State.ParticleBuffer      = AcquireParticleBuffer(State.ParticleBufferSize, "Particles.Particles");
        State.SpawnCounterBuffer  = AcquireParticleBuffer(ParticleCounterBytes, "Particles.SpawnCounter");
        State.AttributeBufferSize = (uint64)MaxParticles * (uint64)Item.AttributeFloatCount * sizeof(float);
        State.AttributeBuffer     = AcquireParticleBuffer(State.AttributeBufferSize, "Particles.Attributes");

        RHI::CmdMemset(CL, { State.ParticleBuffer.Gpu, State.ParticleBufferSize }, 0u);
        RHI::CmdMemset(CL, { State.AttributeBuffer.Gpu, State.AttributeBufferSize }, 0u);
        RHI::CmdMemset(CL, { State.SpawnCounterBuffer.Gpu, ParticleCounterBytes }, 0u);

        if (RibbonSegments > 0u)
        {
            State.RibbonHistorySize   = (uint64)MaxParticles * RibbonSegments * sizeof(FVector4);
            State.RibbonHistoryBuffer = AcquireParticleBuffer(State.RibbonHistorySize, "Particles.RibbonHistory");
        }

        if (bWantsEvents)
        {
            State.EventBufferSize = ParticleEventBufferSize;
            State.EventBuffer     = AcquireParticleBuffer(State.EventBufferSize, "Particles.Events");
            RHI::CmdMemset(CL, { State.EventBuffer.Gpu, ParticleEventCountBytes }, 0u);
        }

        if (bWantsSort)
        {
            State.SortCount           = (uint32)Math::NextPowerOfTwo((int32)MaxParticles);
            State.SortIndexBufferSize = (uint64)MaxParticles * sizeof(uint32);
            State.SortIndexBuffer     = AcquireParticleBuffer(State.SortIndexBufferSize, "Particles.SortIndices");
            State.SortDrawArgsBuffer  = AcquireParticleBuffer(sizeof(RHI::FDrawIndirectArguments), "Particles.SortDrawArgs");

            // A sort that never runs must draw nothing rather than an uninitialized count.
            RHI::CmdMemset(CL, { State.SortDrawArgsBuffer.Gpu, sizeof(RHI::FDrawIndirectArguments) }, 0u);

            if (State.SortCount > PARTICLE_SORT_CAPACITY)
            {
                State.SortKeyBufferSize = (uint64)State.SortCount * sizeof(uint32);
                State.SortKeyBuffer     = AcquireParticleBuffer(State.SortKeyBufferSize, "Particles.SortKeys");
            }
        }

        State.AllocatedMax             = MaxParticles;
        State.AllocatedAttributeFloats = Item.AttributeFloatCount;
        State.AllocatedRibbonSegments  = RibbonSegments;
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

        ReadParticleCollisions();

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

        bool bAnySimulated = false;

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

            TVector<FParticleGPUState>& EntityStates = ParticleGPUStates[Item.Entity];
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

            EnsureParticleBuffers(CL, Item, State);

            if (Item.bForceReset)
            {
                RHI::CmdMemset(CL, { State.ParticleBuffer.Gpu, State.ParticleBufferSize }, 0u);
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
            const FShaderEntry* SimEntry = FShaderLibrary::Resolve(ComputeShader);
            if (SimEntry == nullptr || !SimEntry->IsValid())
            {
                continue;
            }

            // Steps are either one per frame or a whole number of fixed steps, preceded by any prewarm.
            const float FrameDelta = DeltaTime * Item.TimeScale;
            TFixedVector<float, 8> StepDeltas;
            uint32 PrewarmSteps = 0;
            if (State.bPrewarmPending && Resolved.PrewarmTime > 0.0f)
            {
                PrewarmSteps = Math::Min((uint32)Math::Ceil(Resolved.PrewarmTime / ParticlePrewarmStep), ParticleMaxPrewarmSteps);
            }
            State.bPrewarmPending = false;

            if (Resolved.FixedFPS > 0.0f)
            {
                const float Step = 1.0f / Resolved.FixedFPS;
                State.FixedStepRemainder += FrameDelta;
                const uint32 Steps = Math::Min((uint32)(State.FixedStepRemainder / Step), ParticleMaxFixedSteps);
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

            const FMatrix4 EmitterDelta = State.bHasPrevMatrix ? WorldMat * Math::Inverse(State.PrevEmitterMatrix) : FMatrix4(1.0f);
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

            for (uint32 StepIndex = 0; StepIndex < TotalSteps; ++StepIndex)
            {
                const bool  bPrewarm   = StepIndex < PrewarmSteps;
                const float StepDelta  = bPrewarm ? ParticlePrewarmStep : StepDeltas[StepIndex - PrewarmSteps];
                const bool  bFirstStep = StepIndex == 0;
                const bool  bLastStep  = StepIndex + 1 == TotalSteps;

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
                    SpawnCount = (uint32)State.SpawnAccumulator;
                    State.SpawnAccumulator -= (float)SpawnCount;

                    if (Resolved.Explosiveness > 0.0f)
                    {
                        const bool bCycleStart = State.CycleTime < 0.0f || State.CycleTime + StepDelta >= CycleLength;
                        State.CycleTime = bCycleStart ? 0.0f : State.CycleTime + StepDelta;
                        if (bCycleStart)
                        {
                            SpawnCount += (uint32)Math::Round(Resolved.Explosiveness * Rate * CycleLength);
                        }
                    }
                }
                else
                {
                    State.SpawnAccumulator = 0.0f;
                }

                const bool bDoBurst = bEmitActive && Item.bBurstOnSpawn && State.bBurstPending && Resolved.BurstCount > 0;
                if (bDoBurst)
                {
                    SpawnCount += (uint32)Resolved.BurstCount;
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

                if (SpawnCount == 0 && State.AliveTimeRemaining <= 0.0f && State.ParentAliveTime <= 0.0f && !bConsumesScript)
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

                RHI::CmdMemset(CL, { State.SpawnCounterBuffer.Gpu, ParticleCounterBytes }, 0u);
                if (bFirstStep && State.EventBuffer.Gpu != 0)
                {
                    RHI::CmdMemset(CL, { State.EventBuffer.Gpu, ParticleEventCountBytes }, 0u);
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

                // Buffer fills (zero/reset/counter) must land before the sim reads them.
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                    RHI::EStageFlags::Compute,
                    RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);

                RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

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
                };
                static_assert(sizeof(FParticleSimArgs) == 216, "FParticleSimArgs must match ParticleSimCommon.slang.");

                FParticleSimArgs SimArgs = {};
                SimArgs.ParamsAddr   = RHI::CopyTransient(SimParams);
                SimArgs.Particles    = { State.ParticleBuffer, MaxParticles };
                SimArgs.SpawnCounter = { State.SpawnCounterBuffer };
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

                RHI::CmdDispatch(CL, MakeArgs(SimArgs), RenderUtils::GetGroupCount(MaxParticles, 64u), 1u, 1u);
                bAnySimulated = true;
                State.bSimulatedThisFrame = true;

                // The next step, a sub-emitter reading these events, or next frame's event clear all touch what this wrote.
                if (!bLastStep || Item.RaisedEventMask != 0u || Item.SourceEmitterIndex >= 0)
                {
                    RHI::CmdBarrier(CL,
                        RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite,
                        RHI::EStageFlags::Compute | RHI::EStageFlags::Transfer,
                        RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::TransferWrite);
                }
            }

            TVector<FParticleCollisionSource>& CollisionSources = ParticleCollisionReadback[CurrentFrameSlot].Sources;
            if (Item.bReportCollisions && State.bSimulatedThisFrame && State.EventBuffer && CollisionSources.size() < ParticleMaxCollisionSources)
            {
                CollisionSources.push_back({ Item.Entity, Item.EmitterIndex, State.EventBuffer.Gpu });
            }
        }

        CopyParticleCollisions(CL);

        if (bAnySimulated)
        {
            // Simulated particles feed the sort pass, then the render pass VS.
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Compute, RHI::EAccessFlags::ShaderWrite,
                RHI::EStageFlags::Compute | RHI::EStageFlags::VertexShader,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
        }
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

            TVector<FParticleCollision>& Hits = ParticleCollisionResults[Source.Entity];
            Hits.reserve(Hits.size() + Count);
            for (uint32 EventIndex = 0; EventIndex < Count; ++EventIndex)
            {
                const FParticleEventGPU& Event = Events[EventIndex];
                FParticleCollision& Hit = Hits.emplace_back();
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
        Args.Counters         = { State.SpawnCounterBuffer };
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

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(SortShader));

            // Mirrors FParticleSortArgs in ParticleSortCompact.slang, pointers first to stay 8-aligned.
            struct FParticleSortArgs
            {
                RHI::TGPUSpan<FGPUParticle> Particles;
                RHI::TGPUSpan<uint32>       OutIndices;
                RHI::TGPUSpan<RHI::FDrawIndirectArguments> OutDrawArgs;
                uint32 SortCount;
                uint32 SortMode;
                uint32 VertsPerParticle;
                uint32 _Pad0;
            };
            static_assert(sizeof(FParticleSortArgs) == 64, "FParticleSortArgs must match ParticleSortCompact.slang.");

            FParticleSortArgs SortArgs = {};
            SortArgs.Particles        = { State.ParticleBuffer, State.AllocatedMax };
            SortArgs.OutIndices       = { State.SortIndexBuffer };
            SortArgs.OutDrawArgs      = { State.SortDrawArgsBuffer };
            SortArgs.SortCount        = State.SortCount;
            SortArgs.SortMode         = SortMode;
            SortArgs.VertsPerParticle = VertsPerParticle;

            RHI::CmdDispatch(CL, MakeArgs(SortArgs), 1u, 1u, 1u);
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

    static RHI::FBlendDesc MakeParticleBlend(EParticleBlendMode Mode)
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
    static RHI::FBlendDesc MakeParticleMaterialBlend(EBlendMode Mode)
    {
        switch (Mode)
        {
        case EBlendMode::Additive:       return MakeParticleBlend(EParticleBlendMode::Additive);
        case EBlendMode::Modulate:       return MakeParticleBlend(EParticleBlendMode::Multiply);
        case EBlendMode::AlphaComposite: return MakeParticleBlend(EParticleBlendMode::PreMultiplied);
        case EBlendMode::Translucent:    return MakeParticleBlend(EParticleBlendMode::Alpha);

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
        PC.ShadowDataIndex      = INDEX_NONE;
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
        if (!SpritePixelShader)
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
        const bool bClearsDepth = DrawCommands.empty();
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

        OpenParticlePass();

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

            RHI::FDepthStencilDesc DepthDesc;
            const bool bWriteDepth = bMaterial ? Item.bMaterialWritesDepth : Resolved.bWriteDepth;
            if (bWriteDepth)
            {
                IsolateDepthWriter();
            }
            DepthDesc.DepthMode = bWriteDepth
                ? (RHI::EDepthFlags::Read | RHI::EDepthFlags::Write)
                : RHI::EDepthFlags::Read;
            DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
            RHI::CmdSetDepthStencil(CL, (DepthDesc));

            FGraphicsPipelineKey Key;
            Key.VS          = bMaterial ? Item.MaterialVertexShader : ParticleVertexShaderFor(Resolved.RenderMode);
            Key.PS          = bMaterial ? Item.MaterialPixelShader  : SpritePixelShader;
            Key.DepthFormat = EFormat::D32;
            Key.ColorTargets.push_back({ HDR.Desc.Format, bMaterial
                ? MakeParticleMaterialBlend(Item.MaterialBlendMode)
                : MakeParticleBlend(Resolved.BlendMode) });
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FParticlePushConstants PC = MakeParticlePushConstants(Item, State, bMaterial);
            PC.bSorted          = bSorted ? 1u : 0u;
            PC.SortedIndices    = { State.SortIndexBuffer };
            PC.SoftFadeDistance = (bWriteDepth || bClearsDepth) ? 0.0f : Resolved.SoftFadeDistance;
            PC.CameraFadeDistance = Resolved.CameraFadeDistance;

            if (bSorted)
            {
                RHI::CmdDrawIndirect(CL, MakeArgs(PC), State.SortDrawArgsBuffer, 1u, sizeof(RHI::FDrawIndirectArguments));
            }
            else
            {
                RHI::CmdDraw(CL, MakeArgs(PC), PC.VertsPerParticle * State.AllocatedMax, 1u, 0u, 0u);
            }

            if (bWriteDepth)
            {
                IsolateDepthWriter();
            }
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

            for (uint32 Cascade = 0; Cascade < (uint32)NumCascades; ++Cascade)
            {
                const int32 TileX = GCSMCascadeOriginX[Cascade];
                const int32 TileY = GCSMCascadeOriginY[Cascade];
                const int32 TileW = GCSMCascadeSizes[Cascade];
                const RHI::FRect TileRect{ TileX, TileX + TileW, TileY, TileY + TileW };
                RHI::CmdSetViewport(CL, TileRect);
                RHI::CmdSetScissor(CL, TileRect);

                PC.ShadowCascade = Cascade;
                RHI::CmdDraw(CL, MakeArgs(PC), PC.VertsPerParticle * State.AllocatedMax, 1u, 0u, 0u);
            }
        }
    }
}
