#include "Includes/ParticleSimCommon.slang"
#include "Shared/SharedConstants.h"

// Bitonic sort for emitters past one workgroup, sorting blocks in groupshared and merging them in memory.

struct FParticleGlobalSortArgs
{
    GPUSpan<FGPUParticle>           Particles;
    GPUSpan<uint>                   Keys;
    GPUSpan<uint>                   OutIndices;
    GPUSpan<FDrawIndirectArguments> OutDrawArgs;
    // The sim's spawn counters, whose last slot this pass uses to count live particles.
    GPUSpan<uint>                   Counters;
    uint                            SortCount;
    uint                            Stage;
    uint                            K;
    uint                            J;
    uint                            SortMode;
    uint                            VertsPerParticle;
};
FParticleGlobalSortArgs* Pass() { return GetPassArgs<FParticleGlobalSortArgs>(); }

// Must match EParticleGlobalSortStage.
static const uint STAGE_KEYS        = 0u;
static const uint STAGE_LOCAL_SORT  = 1u;
static const uint STAGE_GLOBAL_STEP = 2u;
static const uint STAGE_LOCAL_MERGE = 3u;
static const uint STAGE_COMPACT     = 4u;

static const uint kDeadKey      = ~0u;
static const uint kLiveCounter  = 3u;

groupshared uint gKeys[PARTICLE_SORT_CAPACITY];

void CompareSwap(uint Local, uint Partner, bool bAscending)
{
    const uint A = gKeys[Local];
    const uint B = gKeys[Partner];
    if ((A > B) == bAscending)
    {
        gKeys[Local]   = B;
        gKeys[Partner] = A;
    }
}

// Every stage is uniform across a dispatch, so the early returns below never split a barrier.
[shader("compute")]
[numthreads(PARTICLE_SORT_THREADS, 1, 1)]
void ComputeMain(uint3 GroupID : SV_GroupID, uint3 DispatchID : SV_DispatchThreadID, uint Lane : SV_GroupIndex)
{
    const uint Stage     = Pass().Stage;
    const uint SortCount = Pass().SortCount;
    const uint Index     = DispatchID.x;

    if (Stage == STAGE_KEYS)
    {
        if (Index < SortCount)
        {
            uint Key = kDeadKey;
            if (Index < Pass().Particles.Count)
            {
                const FGPUParticle P = SpanGet(&Pass().Particles, Index);
                if (P.Lifetime > 0.0 && P.Age < P.Lifetime && P.Size > 0.0)
                {
                    const uint MaxKey = (1u << PARTICLE_GLOBAL_SORT_KEY_BITS) - 2u;
                    const uint Value  = (uint)(saturate(ParticleSortValue(P, GetCameraPosition(), Pass().SortMode)) * (float)MaxKey);
                    Key = (Value << PARTICLE_GLOBAL_SORT_INDEX_BITS) | Index;
                    InterlockedAdd(SpanAt(&Pass().Counters, kLiveCounter)[0], 1u);
                }
            }
            SpanSet(&Pass().Keys, Index, Key);
        }
        return;
    }

    if (Stage == STAGE_GLOBAL_STEP)
    {
        const uint Partner = Index ^ Pass().J;
        if (Index < SortCount && Partner > Index && Partner < SortCount)
        {
            const uint A = SpanGet(&Pass().Keys, Index);
            const uint B = SpanGet(&Pass().Keys, Partner);
            const bool bAscending = (Index & Pass().K) == 0u;
            if ((A > B) == bAscending)
            {
                SpanSet(&Pass().Keys, Index, B);
                SpanSet(&Pass().Keys, Partner, A);
            }
        }
        return;
    }

    if (Stage == STAGE_COMPACT)
    {
        const uint LiveCount = SpanGet(&Pass().Counters, kLiveCounter);
        if (Index < LiveCount)
        {
            SpanSet(&Pass().OutIndices, Index, SpanGet(&Pass().Keys, Index) & PARTICLE_GLOBAL_SORT_INDEX_MASK);
        }
        if (Index == 0u)
        {
            FDrawIndirectArguments Args;
            Args.VertexCount   = LiveCount * Pass().VertsPerParticle;
            Args.InstanceCount = 1u;
            Args.VertexOffset  = 0u;
            Args.FirstInstance = 0u;
            SpanSet(&Pass().OutDrawArgs, 0u, Args);
        }
        return;
    }

    const uint Base = GroupID.x * PARTICLE_SORT_CAPACITY;
    for (uint i = Lane; i < PARTICLE_SORT_CAPACITY; i += PARTICLE_SORT_THREADS)
    {
        gKeys[i] = SpanGet(&Pass().Keys, Base + i);
    }
    GroupMemoryBarrierWithGroupSync();

    if (Stage == STAGE_LOCAL_SORT)
    {
        for (uint k = 2u; k <= PARTICLE_SORT_CAPACITY; k <<= 1u)
        {
            for (uint j = k >> 1u; j > 0u; j >>= 1u)
            {
                for (uint i = Lane; i < PARTICLE_SORT_CAPACITY; i += PARTICLE_SORT_THREADS)
                {
                    const uint Partner = i ^ j;
                    if (Partner > i)
                    {
                        CompareSwap(i, Partner, ((Base + i) & k) == 0u);
                    }
                }
                GroupMemoryBarrierWithGroupSync();
            }
        }
    }
    else
    {
        for (uint j = PARTICLE_SORT_CAPACITY >> 1u; j > 0u; j >>= 1u)
        {
            for (uint i = Lane; i < PARTICLE_SORT_CAPACITY; i += PARTICLE_SORT_THREADS)
            {
                const uint Partner = i ^ j;
                if (Partner > i)
                {
                    CompareSwap(i, Partner, ((Base + i) & Pass().K) == 0u);
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    for (uint i = Lane; i < PARTICLE_SORT_CAPACITY; i += PARTICLE_SORT_THREADS)
    {
        SpanSet(&Pass().Keys, Base + i, gKeys[i]);
    }
}
