// Skeleton compiled by FParticleCompiler for module-stack particle systems.
// Data layout + RNG/noise/emitter-shape helpers live in Includes/ParticleSimCommon.slang, shared
// with the data-driven default sim (ParticleSimulate.slang).

#include "Includes/ParticleSimCommon.slang"

// Module-stack function bodies (filled in by FParticleCompiler).
void SpawnParticleGraph(inout FGPUParticle P, inout uint Seed, float TotalTime, uint SpawnIndex)
{
$PARTICLE_SPAWN_FUNC
}

void UpdateParticleGraph(inout FGPUParticle P, float DeltaTime, float TotalTime)
{
    float LifeRatio = saturate(P.Age / max(P.Lifetime, 0.0001));
$PARTICLE_UPDATE_FUNC
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint Index = DispatchThreadID.x;
    if (Index >= SimParams().Counts.x) return;

    FGPUParticle P = Particles()[Index];

    bool bAlive = (P.Lifetime > 0.0) && (P.Age < P.Lifetime);

    if (!bAlive)
    {
        uint myAlloc;
        InterlockedAdd(SpawnCounter()[0], 1u, myAlloc);
        if (myAlloc < SimParams().Counts.y)
        {
            // Seed defaults; spawn modules overwrite what they care about.
            uint Seed = PCGHash(Index ^ SimParams().Counts.z);

            P.Position      = SimParams().EmitterPosition.xyz;
            P.Velocity      = float3(0.0, 0.0, 0.0);
            P.Lifetime      = 1.0;
            P.Color         = float4(1.0, 1.0, 1.0, 1.0);
            P.Size          = 0.1;
            P.Age           = 0.0;
            P.Rotation      = 0.0;
            P.RotationSpeed = 0.0;
            P.Seed          = RandUnit(Seed);

            SpawnParticleGraph(P, Seed, SimParams().Timing.y, Index);

            P.Lifetime = max(P.Lifetime, 0.01);

            // Inherit a fraction of the emitter's own motion.
            float3 EmitterVelocity = float3(SimParams().EmitterForward.w,
                                            SimParams().EmitterRight.w,
                                            SimParams().EmitterUp.w);
            P.Velocity += EmitterVelocity * SimParams().NoiseParams.y;
        }
        else
        {
            P.Lifetime = 0.0;
            P.Age      = 0.0;
            P.Color    = float4(0, 0, 0, 0);
            P.Size     = 0.0;
        }
    }
    else
    {
        P.Age += SimParams().Timing.x;
        if (P.Age >= P.Lifetime)
        {
            P.Lifetime = 0.0;
            P.Age      = 0.0;
            P.Color    = float4(0, 0, 0, 0);
            P.Size     = 0.0;
        }
        else
        {
            UpdateParticleGraph(P, SimParams().Timing.x, SimParams().Timing.y);
        }
    }

    Particles()[Index] = P;
}
