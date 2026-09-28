// Skeleton FParticleCompiler fills with a module stack, sharing its layout and helpers with ParticleSimulate.slang.

#include "Includes/ParticleSimCommon.slang"

// Floats per particle in the attribute buffer, at least one so the indexing stays well formed.
static const uint PARTICLE_ATTR_FLOATS = $PARTICLE_ATTR_COUNT;

// Where a dead slot's new particle came from, so the spawn knows its origin and what to override.
static const uint SPAWN_NONE    = 0u;
static const uint SPAWN_EMITTER = 1u;
static const uint SPAWN_SCRIPT  = 2u;
static const uint SPAWN_EVENT   = 3u;

// SpawnOrigin is the emitter, or the parent particle a sub-emitter spawns from.
void SpawnParticleGraph(inout FGPUParticle P, inout uint Seed, float TotalTime, uint Index, float3 SpawnOrigin)
{
$PARTICLE_SPAWN_FUNC
}

void UpdateParticleGraph(inout FGPUParticle P, float DeltaTime, float TotalTime, uint Index)
{
    float LifeRatio = saturate(P.Age / max(P.Lifetime, 0.0001));
$PARTICLE_UPDATE_FUNC
}

// The emitter's own rate and burst first, then script calls, then the parent's events, each claimed by one dead slot.
uint ClaimSpawn(out FParticleEvent Source)
{
    Source = (FParticleEvent)0;

    uint Claim;
    InterlockedAdd(SpanAt(SpawnCounter(), 0u)[0], 1u, Claim);
    if (Claim < SimParams().Counts.y)
    {
        return SPAWN_EMITTER;
    }

    const uint ScriptCount = SimArgs().ScriptEvents.Count;
    if (ScriptCount != 0u)
    {
        InterlockedAdd(SpanAt(SpawnCounter(), 1u)[0], 1u, Claim);
        if (Claim < ScriptCount)
        {
            Source = SpanGet(&SimArgs().ScriptEvents, Claim);
            return SPAWN_SCRIPT;
        }
    }

    const uint List = SimParams().EventInfo.y;
    if (List < PARTICLE_EVENT_LISTS && SimArgs().SourceEventCounts.Count > List)
    {
        const uint PerEvent = max(SimParams().EventInfo.w, 1u);
        const uint Events   = min(SpanGet(&SimArgs().SourceEventCounts, List), PARTICLE_EVENT_CAPACITY);
        InterlockedAdd(SpanAt(SpawnCounter(), 2u)[0], 1u, Claim);
        if (Claim < Events * PerEvent)
        {
            Source = SpanGet(&SimArgs().SourceEvents, List * PARTICLE_EVENT_CAPACITY + Claim / PerEvent);
            return SPAWN_EVENT;
        }
    }

    return SPAWN_NONE;
}

FGPUParticle SpawnParticle(uint Index, uint Origin, FParticleEvent Source)
{
    uint Seed = PCGHash(Index ^ SimParams().Counts.z);
    const float3 SpawnOrigin = (Origin == SPAWN_EMITTER) ? SimParams().EmitterPosition.xyz : Source.PositionSize.xyz;

    FGPUParticle P;
    P.Position      = SpawnOrigin;
    P.Velocity      = float3(0.0, 0.0, 0.0);
    P.Lifetime      = 1.0;
    P.Color         = float4(1.0, 1.0, 1.0, 1.0);
    P.Size          = 0.1;
    P.Age           = 0.0;
    P.Rotation      = 0.0;
    P.RotationSpeed = 0.0;
    P.Seed          = RandUnit(Seed);

    // Seeded before the stack, so a module reading an attribute nobody wrote gets its declared default.
$PARTICLE_ATTR_DEFAULTS
    SpawnParticleGraph(P, Seed, SimParams().Timing.y, Index, SpawnOrigin);

    P.Lifetime = max(P.Lifetime, 0.01);

    if (Origin == SPAWN_EVENT)
    {
        P.Velocity += Source.VelocityFlags.xyz * SimParams().EventParams.y;

        // Only a direction for SurfaceAligned facing to lie across, far too slow to carry the particle anywhere.
        if (SimParams().EventParams.z > 0.0 && dot(Source.Extra.xyz, Source.Extra.xyz) > 0.5)
        {
            P.Velocity = Source.Extra.xyz * PARTICLE_SURFACE_NORMAL_SPEED;
        }
    }
    else
    {
        const float3 EmitterVelocity = float3(SimParams().EmitterForward.w, SimParams().EmitterRight.w, SimParams().EmitterUp.w);
        P.Velocity += EmitterVelocity * SimParams().NoiseParams.y;
    }

    if (Origin == SPAWN_SCRIPT)
    {
        const uint Flags = asuint(Source.VelocityFlags.w);
        if ((Flags & PARTICLE_EMIT_POSITION) != 0u) { P.Position = Source.PositionSize.xyz; }
        if ((Flags & PARTICLE_EMIT_VELOCITY) != 0u) { P.Velocity = Source.VelocityFlags.xyz; }
        if ((Flags & PARTICLE_EMIT_COLOR)    != 0u) { P.Color    = Source.Extra; }
        if ((Flags & PARTICLE_EMIT_SIZE)     != 0u) { P.Size     = Source.PositionSize.w; }
    }

    InitializeRibbon(Index, P.Position);
    return P;
}

void KillParticle(inout FGPUParticle P)
{
    P.Lifetime = 0.0;
    P.Age      = 0.0;
    P.Color    = float4(0, 0, 0, 0);
    P.Size     = 0.0;
}

// Counts how many 1/Rate boundaries this step's aging crossed.
void RaiseContinuousEvents(FGPUParticle P, float DeltaTime)
{
    const float Rate = SimParams().EventParams.x;
    if ((SimParams().EventInfo.x & (1u << PARTICLE_EVENT_CONTINUOUS)) == 0u || Rate <= 0.0)
    {
        return;
    }

    const uint Crossed = (uint)(floor(P.Age * Rate) - floor(max(P.Age - DeltaTime, 0.0) * Rate));
    for (uint Event = 0u; Event < min(Crossed, 4u); ++Event)
    {
        RaiseParticleEvent(PARTICLE_EVENT_CONTINUOUS, P);
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint Index = DispatchThreadID.x;
    if (Index >= SimParams().Counts.x) return;

    FGPUParticle P = SpanGet(Particles(), Index);

    bool bAlive = (P.Lifetime > 0.0) && (P.Age < P.Lifetime);

    if (!bAlive)
    {
        FParticleEvent Source;
        const uint Origin = ClaimSpawn(Source);
        if (Origin != SPAWN_NONE)
        {
            P = SpawnParticle(Index, Origin, Source);
        }
        else
        {
            KillParticle(P);
        }
    }
    else
    {
        ApplyLocalSpace(Index, P);

        P.Age += SimParams().Timing.x;
        if (P.Age >= P.Lifetime)
        {
            RaiseParticleEvent(PARTICLE_EVENT_DEATH, P);
            KillParticle(P);
        }
        else
        {
            UpdateParticleGraph(P, SimParams().Timing.x, SimParams().Timing.y, Index);
            RaiseContinuousEvents(P, SimParams().Timing.x);
            RecordRibbon(Index, P.Position);
        }
    }

    SpanSet(Particles(), Index, P);
}
