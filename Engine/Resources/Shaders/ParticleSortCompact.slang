#include "Includes/ParticleSimCommon.slang"
#include "Shared/SharedConstants.h"

// Orders one emitter back-to-front and compacts it into an indirect draw.

struct FParticleSortArgs
{
    FGPUParticle*           Particles;
    uint*                   OutIndices;
    FDrawIndirectArguments* OutDrawArgs;
    uint                    ParticleCount;
    // ParticleCount rounded up to the power of two bitonic needs, clamped to the groupshared array.
    uint                    SortCount;
};
FParticleSortArgs* Pass() { return GetPassArgs<FParticleSortArgs>(); }

// Above every live key, so dead slots sink past LiveCount and the sort compacts as it orders.
static const uint kDeadKey = ~0u;

groupshared uint gKeys[PARTICLE_SORT_CAPACITY];
groupshared uint gLiveCount;

// Reciprocal distance, so far particles take the small keys an ascending sort emits first.
uint ParticleDepthKey(float3 WorldPos, float3 CameraPos)
{
    const float Nearness = 1.0 / (1.0 + length(WorldPos - CameraPos));
    const uint  MaxKey   = (1u << PARTICLE_SORT_DEPTH_BITS) - 1u;
    // Held one below kDeadKey, which a particle sitting on the camera would otherwise reach.
    return min((uint)(saturate(Nearness) * (float)MaxKey), MaxKey - 1u);
}

// RULE. No lane may return early, or the barriers below go non-uniform and AMD hangs.
[shader("compute")]
[numthreads(PARTICLE_SORT_THREADS, 1, 1)]
void ComputeMain(uint Lane : SV_GroupIndex)
{
    const uint SortCount     = min(Pass().SortCount, PARTICLE_SORT_CAPACITY);
    const uint ParticleCount = min(Pass().ParticleCount, SortCount);
    const float3 CameraPos   = GetCameraPosition();

    if (Lane == 0u)
    {
        gLiveCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint LocalLive = 0u;
    for (uint i = Lane; i < SortCount; i += PARTICLE_SORT_THREADS)
    {
        uint Key = kDeadKey;
        if (i < ParticleCount)
        {
            const FGPUParticle P = Pass().Particles[i];

            // Must match the liveness test in ParticleVertex.slang, which draws what this counts.
            if (P.Lifetime > 0.0 && P.Age < P.Lifetime && P.Size > 0.0)
            {
                Key = (ParticleDepthKey(P.Position, CameraPos) << PARTICLE_SORT_INDEX_BITS) | i;
                ++LocalLive;
            }
        }
        gKeys[i] = Key;
    }

    if (LocalLive != 0u)
    {
        InterlockedAdd(gLiveCount, LocalLive);
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
                    const uint A = gKeys[i];
                    const uint B = gKeys[Partner];
                    const bool bAscending = ((i & k) == 0u);
                    if ((A > B) == bAscending)
                    {
                        gKeys[i]       = B;
                        gKeys[Partner] = A;
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    // Every key carries its own index, so no two tie and a static emitter cannot reshuffle.
    const uint LiveCount = gLiveCount;

    for (uint i = Lane; i < LiveCount; i += PARTICLE_SORT_THREADS)
    {
        Pass().OutIndices[i] = gKeys[i] & PARTICLE_SORT_INDEX_MASK;
    }

    if (Lane == 0u)
    {
        FDrawIndirectArguments Args;
        Args.VertexCount   = LiveCount * 6u;
        Args.InstanceCount = 1u;
        Args.VertexOffset  = 0u;
        Args.FirstInstance = 0u;
        Pass().OutDrawArgs[0] = Args;
    }
}
