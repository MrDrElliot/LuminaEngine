#include "Includes/ParticleSimCommon.slang"

// Data-driven particle simulation: one shader for all effects, driven by FParticleSimParams.
// Data layout + RNG/noise/emitter-shape helpers live in Includes/ParticleSimCommon.slang, shared
// with the module-stack template (Particles/ParticleSimulateTemplate.slang).

float3 ComputeInitialVelocity(inout uint state, uint mode, uint shape, float3 spawnOffset,
                              float4 velMin, float4 velMax, float2 speedRange,
                              float coneAngle, float3 forward, float3 right, float3 up)
{
    if (mode == VELOCITY_MODE_RADIAL)
    {
        float3 dir;
        float len = length(spawnOffset);
        if (len > 1e-5)
        {
            dir = spawnOffset / len;
        }
        else
        {
            // Degenerate (point shape): sample a random direction, biased by cone if applicable.
            if (shape == EMITTER_SHAPE_CONE)
            {
                dir = RandDirectionInCone(state, forward, right, up, coneAngle);
            }
            else
            {
                dir = RandOnUnitSphere(state);
            }
        }
        float speed = lerp(speedRange.x, speedRange.y, RandUnit(state));
        return dir * speed;
    }

    // Explicit: independent per-axis lerp.
    float3 r = float3(RandUnit(state), RandUnit(state), RandUnit(state));
    return lerp(velMin.xyz, velMax.xyz, r);
}

void SpawnParticle(uint Index, inout FGPUParticle P)
{
    uint seed = WangHash(Index * 73856093u ^ SimParams().Counts.z);

    uint shape = SimParams().Modes.x;
    uint velMode = SimParams().Modes.y;

    float3 fwd   = SimParams().EmitterForward.xyz;
    float3 rgt   = SimParams().EmitterRight.xyz;
    float3 up_   = SimParams().EmitterUp.xyz;

    float3 spawnOffset = SampleEmitterShape(seed, shape,
                                            SimParams().ShapeSize.xyz, SimParams().ShapeSize.w,
                                            fwd, rgt, up_);

    float3 spawnPos = SimParams().EmitterPosition.xyz + spawnOffset;

    float3 vel = ComputeInitialVelocity(seed, velMode, shape, spawnOffset,
                                        SimParams().VelocityMin, SimParams().VelocityMax,
                                        SimParams().SpeedAndLifetime.xy,
                                        SimParams().ShapeSize.w,
                                        fwd, rgt, up_);

    float lifetime = lerp(SimParams().SpeedAndLifetime.z, SimParams().SpeedAndLifetime.w, RandUnit(seed));
    float size     = lerp(SimParams().SizeRange.x,        SimParams().SizeRange.y,        RandUnit(seed));
    float rotation = lerp(SimParams().RotationRange.x,    SimParams().RotationRange.y,    RandUnit(seed));
    float rotSpeed = lerp(SimParams().RotationRange.z,    SimParams().RotationRange.w,    RandUnit(seed));
    float particleSeed = RandUnit(seed);

    // Emitter velocity inheritance: EmitterVelocity is packed in the .w components
    // of the emitter basis vectors; NoiseParams.y is the 0-1 inheritance factor.
    float3 EmitterVelocity = float3(SimParams().EmitterForward.w,
                                    SimParams().EmitterRight.w,
                                    SimParams().EmitterUp.w);
    vel += EmitterVelocity * SimParams().NoiseParams.y;

    P.Position      = spawnPos;
    P.Age           = 0.0;
    P.Velocity      = vel;
    P.Lifetime      = max(lifetime, 0.01);
    P.Color         = SimParams().StartColor;
    P.Size          = size;
    P.Rotation      = radians(rotation);
    P.RotationSpeed = radians(rotSpeed);
    P.Seed          = particleSeed;
}

void SimulateParticle(inout FGPUParticle P, float DeltaTime, float TotalTime)
{
    P.Velocity += SimParams().Gravity.xyz * DeltaTime;

    float3 noiseStrength = SimParams().NoiseStrength.xyz;
    if (any(noiseStrength > 0.0))
    {
        float3 turbulence = CurlishNoise(P.Position,
                                         TotalTime,
                                         SimParams().NoiseStrength.w,
                                         SimParams().NoiseParams.x);
        P.Velocity += turbulence * noiseStrength * DeltaTime;
    }

    // Exponential drag (1st order). Keeps behavior framerate-independent.
    float drag = SimParams().Gravity.w;
    if (drag > 0.0)
    {
        P.Velocity *= exp(-drag * DeltaTime);
    }

    P.Position += P.Velocity * DeltaTime;
    P.Rotation += P.RotationSpeed * DeltaTime;

    // Color over life
    float lifeT = saturate(P.Age / P.Lifetime);
    P.Color = lerp(SimParams().StartColor, SimParams().EndColor, lifeT);

    // Size over life (random end per-particle but deterministic via Seed)
    float endSize = lerp(SimParams().SizeRange.z, SimParams().SizeRange.w, P.Seed);
    float startSize = lerp(SimParams().SizeRange.x, SimParams().SizeRange.y, P.Seed);
    P.Size = lerp(startSize, endSize, lifeT);
}

// 64 threads per group matches wavefront widths on AMD/NVIDIA.
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint Index = DispatchThreadID.x;
    uint MaxParticles = SimParams().Counts.x;
    if (Index >= MaxParticles)
    {
        return;
    }

    FGPUParticle P = Particles()[Index];

    bool bAlive = (P.Lifetime > 0.0) && (P.Age < P.Lifetime);

    if (!bAlive)
    {
        // Race for a spawn slot: first SpawnCount dead particles (in thread order) spawn this frame.
        uint myAlloc;
        InterlockedAdd(SpawnCounter()[0], 1u, myAlloc);
        if (myAlloc < SimParams().Counts.y)
        {
            SpawnParticle(Index, P);
        }
        else
        {
            // Keep particle flagged dead
            P.Lifetime = 0.0;
            P.Age      = 0.0;
            P.Color    = float4(0, 0, 0, 0);
            P.Size     = 0.0;
        }
    }
    else
    {
        float dt = SimParams().Timing.x;
        P.Age += dt;
        if (P.Age >= P.Lifetime)
        {
            P.Lifetime = 0.0;
            P.Age      = 0.0;
            P.Color    = float4(0, 0, 0, 0);
            P.Size     = 0.0;
        }
        else
        {
            SimulateParticle(P, dt, SimParams().Timing.y);
        }
    }

    Particles()[Index] = P;
}
