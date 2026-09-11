#include "RuntimePCH.h"
#include "SceneRendererInternal.h"

namespace Lumina
{
    void FDefaultSceneRenderer::ParticleSimulatePass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Particle Simulate", tracy::Color::Orange);

        const FFrameData& Frame = *RenderFrame;
        const float DeltaTime   = Frame.CachedWorldDeltaTime;
        
        if (!ParticleGPUStates.empty())
        {
            const TVector<ECS::FEntity>& Live = Frame.Extracts.LiveParticleEntities;
            auto IsLive = [&](ECS::FEntity E)
            {
                return Algo::Contains(Live, E);
            };

            for (auto It = ParticleGPUStates.begin(); It != ParticleGPUStates.end();)
            {
                if (!IsLive(It->first))
                {
                    for (FParticleGPUState& Dead : It->second)
                    {
                        if (Dead.ParticleBuffer)     { DeferFree(Dead.ParticleBuffer); }
                        if (Dead.SpawnCounterBuffer) { DeferFree(Dead.SpawnCounterBuffer); }
                        if (Dead.AttributeBuffer)    { DeferFree(Dead.AttributeBuffer); }
                        if (Dead.SortIndexBuffer)    { DeferFree(Dead.SortIndexBuffer); }
                        if (Dead.SortDrawArgsBuffer) { DeferFree(Dead.SortDrawArgsBuffer); }
                    }
                    It = ParticleGPUStates.erase(It);
                }
                else
                {
                    ++It;
                }
            }
        }

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
                    FParticleGPUState& Dropped = EntityStates[(size_t)Stale];
                    if (Dropped.ParticleBuffer)     { DeferFree(Dropped.ParticleBuffer); }
                    if (Dropped.SpawnCounterBuffer) { DeferFree(Dropped.SpawnCounterBuffer); }
                    if (Dropped.AttributeBuffer)    { DeferFree(Dropped.AttributeBuffer); }
                    if (Dropped.SortIndexBuffer)    { DeferFree(Dropped.SortIndexBuffer); }
                    if (Dropped.SortDrawArgsBuffer) { DeferFree(Dropped.SortDrawArgsBuffer); }
                }
                EntityStates.resize((size_t)Math::Max(Item.EmitterCount, 1));
            }
            FParticleGPUState& State = EntityStates[(size_t)Item.EmitterIndex];

            // An emitter that reaches none of the dispatches below is fully dead, and draws nothing.
            State.bSimulatedThisFrame = false;

            const bool bNeedsAlloc = (State.ParticleBuffer.Gpu == 0)
                                  || (State.AllocatedMax != MaxParticles)
                                  || (State.AllocatedAttributeFloats != Item.AttributeFloatCount);
            if (bNeedsAlloc)
            {
                if (State.ParticleBuffer)     { DeferFree(State.ParticleBuffer); }
                if (State.SpawnCounterBuffer) { DeferFree(State.SpawnCounterBuffer); }
                if (State.AttributeBuffer)    { DeferFree(State.AttributeBuffer); }
                if (State.SortIndexBuffer)    { DeferFree(State.SortIndexBuffer); }
                if (State.SortDrawArgsBuffer) { DeferFree(State.SortDrawArgsBuffer); }
                State.SortIndexBuffer    = {};
                State.SortDrawArgsBuffer = {};

                State.ParticleBufferSize = (uint64)MaxParticles * sizeof(FGPUParticle);
                State.ParticleBuffer     = RHI::Malloc(State.ParticleBufferSize, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
                State.SpawnCounterBuffer = RHI::Malloc(sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
                RHI::SetDebugName(State.ParticleBuffer.Gpu,     "Particles.Particles");
                RHI::SetDebugName(State.SpawnCounterBuffer.Gpu, "Particles.SpawnCounter");

                State.AttributeBufferSize = (uint64)MaxParticles * (uint64)Item.AttributeFloatCount * sizeof(float);
                State.AttributeBuffer     = RHI::Malloc(State.AttributeBufferSize, RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
                RHI::SetDebugName(State.AttributeBuffer.Gpu,    "Particles.Attributes");

                // Bitonic needs a power-of-two span, and one workgroup can only hold so much of it.
                State.SortCount = (MaxParticles <= PARTICLE_SORT_CAPACITY)
                    ? (uint32)Math::NextPowerOfTwo((int32)MaxParticles)
                    : 0u;
                if (State.SortCount > 0)
                {
                    State.SortIndexBuffer    = RHI::Malloc((uint64)MaxParticles * sizeof(uint32), RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
                    State.SortDrawArgsBuffer = RHI::Malloc(sizeof(RHI::FDrawIndirectArguments),   RHI::kDefaultAlign, RHI::EMemoryType::GPUOnly);
                    RHI::SetDebugName(State.SortIndexBuffer.Gpu,    "Particles.SortIndices");
                    RHI::SetDebugName(State.SortDrawArgsBuffer.Gpu, "Particles.SortDrawArgs");

                    // A sort that never runs must draw nothing rather than an uninitialized count.
                    RHI::CmdMemset(CL, { State.SortDrawArgsBuffer.Gpu, sizeof(RHI::FDrawIndirectArguments) }, 0u);
                }

                // Zero-fill the particle buffer so all entries start dead.
                RHI::CmdMemset(CL, { State.ParticleBuffer.Gpu, State.ParticleBufferSize }, 0u);
                RHI::CmdMemset(CL, { State.AttributeBuffer.Gpu, State.AttributeBufferSize }, 0u);

                State.AllocatedMax             = MaxParticles;
                State.AllocatedAttributeFloats = Item.AttributeFloatCount;
                State.SpawnAccumulator  = 0.0f;
                State.SystemAge         = 0.0f;
                State.bBurstPending     = true;
            }

            // Apply the extract-phase Activate()/Deactivate() intents to the render-owned sim state.
            if (Item.bForceReset)
            {
                RHI::CmdMemset(CL, { State.ParticleBuffer.Gpu, State.ParticleBufferSize }, 0u);
                State.AliveTimeRemaining = 0.0f;
                State.SpawnAccumulator   = 0.0f;
                State.SystemAge          = 0.0f;
            }
            if (Item.bForceBurst)
            {
                State.bBurstPending = true;
            }

            const float ScaledDelta = DeltaTime * Item.TimeScale;
            State.TotalTime += DeltaTime;
            State.SystemAge += ScaledDelta;

            const bool bDurationExpired = (Resolved.Duration > 0.0f) && (State.SystemAge >= Resolved.Duration);
            if (bDurationExpired)
            {
                if (Resolved.bLooping)
                {
                    State.SystemAge = fmodf(State.SystemAge, Resolved.Duration);
                    State.bBurstPending = true;
                }
            }

            const bool bEmitActive = Item.bEmit && !(bDurationExpired && !Resolved.bLooping);

            uint32 SpawnCount = 0;
            if (bEmitActive && Resolved.SpawnRate > 0.0f && Item.SpawnRateMultiplier > 0.0f)
            {
                State.SpawnAccumulator += DeltaTime * Resolved.SpawnRate * Item.SpawnRateMultiplier;
                SpawnCount = (uint32)State.SpawnAccumulator;
                State.SpawnAccumulator -= (float)SpawnCount;
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

            const float MaxLifetime = Math::Max(Resolved.LifetimeRange.y, 0.0f);
            if (SpawnCount > 0)
            {
                State.AliveTimeRemaining = Math::Max(State.AliveTimeRemaining, MaxLifetime);
            }
            State.AliveTimeRemaining = Math::Max(State.AliveTimeRemaining - ScaledDelta, 0.0f);

            if (SpawnCount == 0 && State.AliveTimeRemaining <= 0.0f)
            {
                continue;
            }

            RHI::CmdMemset(CL, { State.SpawnCounterBuffer.Gpu, sizeof(uint32) }, 0u);

            const FMatrix4 WorldMat = Item.WorldMatrix;
            const FVector3 EmitterWorld = FVector3(WorldMat * FVector4(Item.EmitterOffset, 1.0f));
            const FVector3 EmitterRight   = Math::Normalize(FVector3(WorldMat[0]));
            const FVector3 EmitterUp      = Math::Normalize(FVector3(WorldMat[1]));
            const FVector3 EmitterForward = Math::Normalize(FVector3(WorldMat[2]));
            
            FVector3 EmitterVelocity(0.0f);
            if (State.bHasPrevPosition && DeltaTime > 0.0f)
            {
                EmitterVelocity = (EmitterWorld - State.PrevEmitterPosition) / DeltaTime;
            }
            State.PrevEmitterPosition = EmitterWorld;
            State.bHasPrevPosition    = true;

            const float InheritFactor = Math::Clamp(Resolved.InheritEmitterVelocity, 0.0f, 1.0f);

            State.FrameSeed = (State.FrameSeed + 2654435761u) ^ (uint32)Item.Entity;

            uint32 SimFlags = 0u;
            if (Resolved.bLooping)
            {
                SimFlags |= PARTICLE_SIM_FLAG_LOOP;
            }
            if (State.bBurstPending)
            {
                SimFlags |= PARTICLE_SIM_FLAG_BURST_PENDING;
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
            SimParams.Timing            = FVector4(ScaledDelta, State.TotalTime, State.SystemAge, 0.0f);

            static const FShaderH DefaultSimShader = FShaderLibrary::Get("ParticleSimulate.slang");
            FShaderH ComputeShader = Item.bUsesCustomShader ? Item.CustomComputeShader : DefaultSimShader;

            const FShaderEntry* SimEntry = FShaderLibrary::Resolve(ComputeShader);
            if (SimEntry == nullptr || !SimEntry->IsValid())
            {
                continue;
            }

            // Buffer fills (zero/reset/counter) must land before the sim reads them.
            RHI::CmdBarrier(CL, RHI::EStageFlags::Transfer, RHI::EStageFlags::Compute);

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(ComputeShader));

            // Mirrors FParticleSimArgs in ParticleSimulate.slang, everything by device address.
            struct FParticleSimArgs
            {
                uint64 ParamsAddr;
                RHI::TGPUSpan<FGPUParticle> Particles;
                RHI::TGPUSpan<uint32>       SpawnCounter;
                RHI::TGPUSpan<FVector4>     ModuleParams;
                RHI::TGPUSpan<float>        Attributes;
            };
            static_assert(sizeof(FParticleSimArgs) == 72, "FParticleSimArgs must match ParticleSimCommon.slang.");

            FParticleSimArgs SimArgs = {};
            SimArgs.ParamsAddr   = RHI::CopyTransient(SimParams);
            SimArgs.Particles    = { State.ParticleBuffer, MaxParticles };
            SimArgs.SpawnCounter = { State.SpawnCounterBuffer };
            if (!Item.ModuleParamValues.empty())
            {
                SimArgs.ModuleParams = RHI::CopyTransientArray(Item.ModuleParamValues.data(),
                                                               Item.ModuleParamValues.size());
            }
            SimArgs.Attributes   = { State.AttributeBuffer };

            RHI::CmdDispatch(CL, MakeArgs(SimArgs), RenderUtils::GetGroupCount(MaxParticles, 64u), 1u, 1u);
            bAnySimulated = true;
            State.bSimulatedThisFrame = true;
        }

        if (bAnySimulated)
        {
            // Simulated particles feed the sort pass, then the render pass VS.
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::Compute | RHI::EStageFlags::VertexShader);
        }
    }

    // ParticleSortCompact packs a slot index into the low bits of every sort key.
    static_assert((1u << PARTICLE_SORT_INDEX_BITS) == PARTICLE_SORT_CAPACITY,
                  "The sort key's index field must address exactly the sortable capacity.");

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
            if (!Item.bReady)
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

            RHI::CmdSetPipeline(CL, GetOrCreateComputePipeline(SortShader));
            bAnySorted = true;

            // Mirrors FParticleSortArgs in ParticleSortCompact.slang, pointers first to stay 8-aligned.
            struct FParticleSortArgs
            {
                RHI::TGPUSpan<FGPUParticle> Particles;
                RHI::TGPUSpan<uint32>       OutIndices;
                RHI::TGPUSpan<RHI::FDrawIndirectArguments> OutDrawArgs;
                uint32 SortCount;
                uint32 _Pad0;
            };
            static_assert(sizeof(FParticleSortArgs) == 56, "FParticleSortArgs must match ParticleSortCompact.slang.");

            FParticleSortArgs SortArgs = {};
            SortArgs.Particles  = { State.ParticleBuffer, State.AllocatedMax };
            SortArgs.OutIndices = { State.SortIndexBuffer };
            SortArgs.OutDrawArgs = { State.SortDrawArgsBuffer };
            SortArgs.SortCount  = State.SortCount;

            RHI::CmdDispatch(CL, MakeArgs(SortArgs), 1u, 1u, 1u);
        }

        if (bAnySorted)
        {
            // The compacted draw reads the sorted indices in its VS and its count as indirect arguments.
            RHI::CmdBarrier(CL, RHI::EStageFlags::Compute, RHI::EStageFlags::VertexShader | RHI::EStageFlags::IndirectArguments);
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

    void FDefaultSceneRenderer::ParticleRenderPass(RHI::FCmdListH CL)
    {
        LUMINA_PROFILE_SECTION_COLORED("Particle Render", tracy::Color::OrangeRed);

        const FFrameData& Frame = *RenderFrame;
        const auto& DrawCommands = Frame.Geometry.DrawCommands;

        if (Frame.Extracts.ParticleExtracts.empty())
        {
            return;
        }

        static const FShaderH SpriteVertexShader = FShaderLibrary::Get("ParticleVertex.slang");
        static const FShaderH SpritePixelShader  = FShaderLibrary::Get("ParticlePixel.slang");
        if (!SpriteVertexShader || !SpritePixelShader)
        {
            return;
        }

        const FSceneImage& HDR   = GetNamedImage(ENamedImage::HDR);
        const FSceneImage& Depth = GetNamedImage(ENamedImage::DepthAttachment);

        const bool bHDRWasWritten = !DrawCommands.empty() || FrameFlags.bHasEnvironment
            || !Frame.Extracts.TerrainExtracts.empty() || !Frame.Primitives.SolidBatches.empty()
            || !Frame.Primitives.LineBatches.empty();

        RHI::FRenderAttachment Color;
        Color.Texture  = HDR.Texture;
        Color.LoadOp   = bHDRWasWritten ? RHI::ELoadOp::Load : RHI::ELoadOp::Clear;
        Color.StoreOp  = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments         = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.DepthAttachment.Texture  = Depth.Texture;
        Pass.DepthAttachment.LoadOp   = DrawCommands.empty() ? RHI::ELoadOp::Clear : RHI::ELoadOp::Load;
        Pass.DepthAttachment.StoreOp  = RHI::EStoreOp::Store;
        Pass.DepthAttachment.Color[0] = 0.0f;
        Pass.RenderArea               = HDR.GetExtent();

        RHI::CmdBeginRenderPass(CL, Pass);
        SetViewportScissor(CL, HDR.GetExtent());
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);

        struct FParticlePushConstants
        {
            RHI::TGPUSpan<FGPUParticle> Particles;
            uint32   TextureIndex;
            uint32   FacingMode;
            FVector4 Tint;
            float    VelocityStretch;
            uint32   SubUVColumns;
            uint32   SubUVRows;
            uint32   AttrFloats;        // floats per particle in the attribute buffer
            RHI::TGPUSpan<float> Attributes;   // empty when the emitter declared none
            int32    AttrSlotSizeScaleX; // -1 when the stack did not declare it
            int32    AttrSlotSizeScaleY;
            int32    AttrSlotPrevPosX;
            int32    AttrSlotPrevPosY;
            int32    AttrSlotPrevPosZ;
            uint32   MaterialIndex;      // Materials() slot; read only by the Particle material stages
            uint32   bSorted;            // 0 draws unsorted at full capacity, and SortedIndices is not read
            RHI::TGPUSpan<uint32> SortedIndices;
        };
        static_assert(sizeof(FParticlePushConstants) == 120, "FParticlePushConstants must match the slang pass block.");

        for (const FFrameData::FParticleExtract& Item : Frame.Extracts.ParticleExtracts)
        {
            if (!Item.bReady)
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
            const bool bMaterial = (Item.MaterialIndex >= 0);

            RHI::FDepthStencilDesc DepthDesc;
            const bool bWriteDepth = bMaterial ? Item.bMaterialWritesDepth : Resolved.bWriteDepth;
            DepthDesc.DepthMode = bWriteDepth
                ? (RHI::EDepthFlags::Read | RHI::EDepthFlags::Write)
                : RHI::EDepthFlags::Read;
            DepthDesc.DepthTest = RHI::EOp::GreaterEqual;
            RHI::CmdSetDepthStencil(CL, (DepthDesc));

            FGraphicsPipelineKey Key;
            Key.VS          = bMaterial ? Item.MaterialVertexShader : SpriteVertexShader;
            Key.PS          = bMaterial ? Item.MaterialPixelShader  : SpritePixelShader;
            Key.DepthFormat = EFormat::D32;
            Key.ColorTargets.push_back({ HDR.Desc.Format, bMaterial
                ? MakeParticleMaterialBlend(Item.MaterialBlendMode)
                : MakeParticleBlend(Resolved.BlendMode) });
            RHI::CmdSetPipeline(CL, GetOrCreatePipeline(Key));

            FParticlePushConstants PC = {};
            PC.Particles         = { State.ParticleBuffer, State.AllocatedMax };
            PC.TextureIndex      = Item.TextureIndex;
            PC.FacingMode        = (uint32)Resolved.FacingMode;
            PC.Tint              = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
            PC.VelocityStretch   = Resolved.VelocityStretch;
            PC.SubUVColumns      = (uint32)Math::Max(Resolved.SubUVColumns, 1);
            PC.SubUVRows         = (uint32)Math::Max(Resolved.SubUVRows, 1);
            PC.AttrFloats        = Math::Max(Item.AttributeFloatCount, 1u);
            PC.Attributes        = { State.AttributeBuffer };
            PC.AttrSlotSizeScaleX = Item.RenderAttrSlots[ParticleRenderAttribute::SizeScaleX];
            PC.AttrSlotSizeScaleY = Item.RenderAttrSlots[ParticleRenderAttribute::SizeScaleY];
            PC.AttrSlotPrevPosX   = Item.RenderAttrSlots[ParticleRenderAttribute::PrevPosX];
            PC.AttrSlotPrevPosY   = Item.RenderAttrSlots[ParticleRenderAttribute::PrevPosY];
            PC.AttrSlotPrevPosZ   = Item.RenderAttrSlots[ParticleRenderAttribute::PrevPosZ];
            PC.MaterialIndex      = bMaterial ? (uint32)Item.MaterialIndex : 0u;
            PC.bSorted            = bSorted ? 1u : 0u;
            PC.SortedIndices      = { State.SortIndexBuffer };

            if (bSorted)
            {
                RHI::CmdDrawIndirect(CL, MakeArgs(PC), State.SortDrawArgsBuffer, 1u, sizeof(RHI::FDrawIndirectArguments));
            }
            else
            {
                RHI::CmdDraw(CL, MakeArgs(PC), 6u * State.AllocatedMax, 1u, 0u, 0u);
            }
        }

        RHI::CmdEndRenderPass(CL);
        Barriers::RasterToRead(CL);
    }
}
