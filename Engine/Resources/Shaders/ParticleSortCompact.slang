#include "Includes/ParticleSimCommon.slang"
#include "Shared/SharedConstants.h"

// Orders one emitter back-to-front and compacts it into an indirect draw.

struct FParticleSortArgs
{
    GPUSpan<FGPUParticle>           Particles;
    GPUSpan<uint>                   OutIndices;
    GPUSpan<FDrawIndirectArguments> OutDrawArgs;
    // Particles rounded up to the power of two bitonic needs, clamped to the groupshared array.
    uint                            SortCount;
    uint                            SortMode;
    uint                            VertsPerParticle;
    uint                            _Pad0;
};
FParticleSortArgs* Pass() { return GetPassArgs<FParticleSortArgs>(); }

// Above every live key, so dead slots sink past LiveCount and the sort compacts as it orders.
static const uint kDeadKey = ~0u;

groupshared uint gsKeys[PARTICLE_SORT_CAPACITY];
groupshared uint gsLiveCount;

// Held one below kDeadKey, which a particle sitting on the camera would otherwise reach.
uint ParticleDepthKey(FGPUParticle P, float3 CameraPos)
{
    const uint MaxKey = (1u << PARTICLE_SORT_DEPTH_BITS) - 1u;
    return min((uint)(saturate(ParticleSortValue(P, CameraPos, Pass().SortMode)) * (float)MaxKey), MaxKey - 1u);
}

// No lane may return early, or the barriers below go non-uniform and AMD hangs.
[shader("compute")]
[numthreads(PARTICLE_SORT_THREADS, 1, 1)]
void ComputeMain(uint Lane : SV_GroupIndex)
{
    const uint SortCount     = min(Pass().SortCount, PARTICLE_SORT_CAPACITY);
    const uint ParticleCount = min(Pass().Particles.Count, SortCount);
    const float3 CameraPos   = GetCameraPosition();

    if (Lane == 0u)
    {
        gsLiveCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint LocalLive = 0u;
    for (uint i = Lane; i < SortCount; i += PARTICLE_SORT_THREADS)
    {
        uint Key = kDeadKey;
        if (i < ParticleCount)
        {
            const FGPUParticle P = Pass().Particles.Get(i);

            // Must match the liveness test in ParticleVertex.slang, which draws what this counts.
            if (P.Lifetime > 0.0 && P.Age < P.Lifetime && P.Size > 0.0)
            {
                Key = (ParticleDepthKey(P, CameraPos) << PARTICLE_SORT_INDEX_BITS) | i;
                ++LocalLive;
            }
        }
        gsKeys[i] = Key;
    }

    if (LocalLive != 0u)
    {
        InterlockedAdd(gsLiveCount, LocalLive);
    }
    GroupMemoryBarrierWithGroupSync();

    // Each XOR pair is owned by its lower index, so the two writes inside a step never race.
    for (uint k = 2u; k <= SortCount; k <<= 1u)
    {
        for (uint j = k >> 1u; j > 0u; j >>= 1u)
        {
            for (uint i = Lane; i < SortCount; i += PARTICLE_SORT_THREADS)
            {
                const uint Partner = i ^ j;
                if (Partner > i)
                {
                    const uint A = gsKeys[i];
                    const uint B = gsKeys[Partner];
                    const bool bAscending = ((i & k) == 0u);
                    if ((A > B) == bAscending)
                    {
                        gsKeys[i]       = B;
                        gsKeys[Partner] = A;
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    // Every key carries its own index, so no two tie and a static emitter cannot reshuffle.
    const uint LiveCount = gsLiveCount;

    for (uint i = Lane; i < LiveCount; i += PARTICLE_SORT_THREADS)
    {
        Pass().OutIndices.Set(i, gsKeys[i] & PARTICLE_SORT_INDEX_MASK);
    }

    if (Lane == 0u)
    {
        FDrawIndirectArguments Args;
        Args.VertexCount   = LiveCount * Pass().VertsPerParticle;
        Args.InstanceCount = 1u;
        Args.VertexOffset  = 0u;
        Args.FirstInstance = 0u;
        Pass().OutDrawArgs.Set(0u, Args);
    }
}
