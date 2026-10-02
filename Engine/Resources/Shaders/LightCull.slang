#pragma once
#include "Includes/SceneGlobals.slang"

#define LOCAL_SIZE (CLUSTER_CULL_BLOCK_X * CLUSTER_CULL_BLOCK_Y * CLUSTER_CULL_BLOCK_Z)
#define MAX_WAVES_PER_GROUP (LOCAL_SIZE / 4)
#define BATCH_WORDS (LOCAL_SIZE / 32)

#if LOCAL_SIZE != 128
#error "Each batch of LOCAL_SIZE lights fills the four mask words held in a uint4."
#endif

bool SphereAABBIntersection(float3 Center, float Radius, float3 AABBMin, float3 AABBMax)
{
    float3 ClosestPoint    = clamp(Center, AABBMin, AABBMax);
    float3 Delta           = ClosestPoint - Center;
    float  DistanceSquared = dot(Delta, Delta);
    return DistanceSquared <= Radius * Radius;
}

// The batch's lights that touch this block, which each cluster then tests on its own.
groupshared float4 gLightViewPosRadius[LOCAL_SIZE];
groupshared uint   gLightIndex[LOCAL_SIZE];
groupshared uint   gWaveKept[MAX_WAVES_PER_GROUP];
groupshared float3 gWaveMin[MAX_WAVES_PER_GROUP];
groupshared float3 gWaveMax[MAX_WAVES_PER_GROUP];

// One thread per cluster writes its light bitmask, so no list, pool or atomic is needed.
[shader("compute")]
[numthreads(LOCAL_SIZE, 1, 1)]
void ComputeMain(uint3 WorkGroupID : SV_GroupID, uint3 LocalID : SV_GroupThreadID)
{
    const uint3 Grid   = Scene().GridSize.xyz;
    const uint3 Blocks = (Grid + uint3(CLUSTER_CULL_BLOCK_X, CLUSTER_CULL_BLOCK_Y, CLUSTER_CULL_BLOCK_Z) - 1)
                       / uint3(CLUSTER_CULL_BLOCK_X, CLUSTER_CULL_BLOCK_Y, CLUSTER_CULL_BLOCK_Z);
    const uint  Block  = WorkGroupID.x;

    // Group-uniform, so the whole group leaves before any barrier.
    if (Block >= Blocks.x * Blocks.y * Blocks.z)
    {
        return;
    }

    const uint  Local = LocalID.x;
    const uint3 BlockCoord = uint3(Block % Blocks.x, (Block / Blocks.x) % Blocks.y, Block / (Blocks.x * Blocks.y));
    const uint3 InBlock    = uint3(Local % CLUSTER_CULL_BLOCK_X,
                                   (Local / CLUSTER_CULL_BLOCK_X) % CLUSTER_CULL_BLOCK_Y,
                                   Local / (CLUSTER_CULL_BLOCK_X * CLUSTER_CULL_BLOCK_Y));
    const uint3 Coord = BlockCoord * uint3(CLUSTER_CULL_BLOCK_X, CLUSTER_CULL_BLOCK_Y, CLUSTER_CULL_BLOCK_Z) + InBlock;

    // Threads past the grid edge still join every barrier, they just never test or write.
    const uint Index         = Coord.x + Coord.y * Grid.x + Coord.z * Grid.x * Grid.y;
    const bool bValidCluster = all(Coord < Grid) && SpanContains(Clusters(), Index);

    float3 ClusterMin = bValidCluster ? loadAligned<16>(&SpanAt(Clusters(), Index).MinPoint).xyz : float3(3.0e38);
    float3 ClusterMax = bValidCluster ? loadAligned<16>(&SpanAt(Clusters(), Index).MaxPoint).xyz : float3(-3.0e38);

    // The block's bounds, reduced per wave and then across the waves.
    const uint WaveSize  = WaveGetLaneCount();
    const uint WaveIndex = Local / WaveSize;
    const uint NumWaves  = (LOCAL_SIZE + WaveSize - 1) / WaveSize;
    const float3 WaveMinBounds = WaveActiveMin(ClusterMin);
    const float3 WaveMaxBounds = WaveActiveMax(ClusterMax);
    if (WaveIsFirstLane())
    {
        gWaveMin[WaveIndex] = WaveMinBounds;
        gWaveMax[WaveIndex] = WaveMaxBounds;
    }
    GroupMemoryBarrierWithGroupSync();

    float3 BlockMin = gWaveMin[0];
    float3 BlockMax = gWaveMax[0];
    for (uint w = 1; w < NumWaves; ++w)
    {
        BlockMin = min(BlockMin, gWaveMin[w]);
        BlockMax = max(BlockMax, gWaveMax[w]);
    }

    const uint LightCount = GetNumLights();
    const uint MaskWords  = GetClusterMaskWords();

    // Directional light is always at index 0; skip since it applies globally.
    const uint StartIndex = (LightCount > 0 && HasFlag(GetLightAt(0).Flags, ELightFlags::Directional)) ? 1u : 0u;

    const float4x4 View = GetCameraView();

    uint FirstWord = 0xFFFFFFFFu;
    uint EndWord   = 0u;

    // LightCount is group-uniform, so every thread runs the same iterations and reaches every barrier.
    for (uint Base = 0; Base < LightCount; Base += LOCAL_SIZE)
    {
        const uint LoadIndex = Base + Local;
        float4 ViewPosRadius = float4(0.0, 0.0, 0.0, -1.0);
        bool bKeep = false;
        if (LoadIndex < LightCount && LoadIndex >= StartIndex)
        {
            const FLight Loaded = GetLightAt(LoadIndex);
            ViewPosRadius = float4(mul(View, float4(Loaded.Position, 1.0)).xyz, Loaded.Radius);
            bKeep = SphereAABBIntersection(ViewPosRadius.xyz, Loaded.Radius, BlockMin, BlockMax);
        }

        const uint WaveKept   = WaveActiveCountBits(bKeep);
        const uint WaveOffset = WavePrefixCountBits(bKeep);
        if (WaveIsFirstLane())
        {
            gWaveKept[WaveIndex] = WaveKept;
        }
        GroupMemoryBarrierWithGroupSync();

        uint Offset = 0;
        uint Kept   = 0;
        for (uint w = 0; w < NumWaves; ++w)
        {
            Offset += (w < WaveIndex) ? gWaveKept[w] : 0u;
            Kept   += gWaveKept[w];
        }
        if (bKeep)
        {
            gLightViewPosRadius[Offset + WaveOffset] = ViewPosRadius;
            gLightIndex[Offset + WaveOffset]         = LoadIndex;
        }
        GroupMemoryBarrierWithGroupSync();

        if (bValidCluster)
        {
            uint4 Words = uint4(0u, 0u, 0u, 0u);
            for (uint j = 0; j < Kept; ++j)
            {
                const float4 LightVP = gLightViewPosRadius[j];
                if (SphereAABBIntersection(LightVP.xyz, LightVP.w, ClusterMin, ClusterMax))
                {
                    const uint InBatch = gLightIndex[j] - Base;
                    const uint Bit     = 1u << (InBatch & 31u);
                    const uint Slot    = InBatch >> 5u;
                    Words |= uint4(Slot == 0u ? Bit : 0u, Slot == 1u ? Bit : 0u, Slot == 2u ? Bit : 0u, Slot == 3u ? Bit : 0u);
                }
            }

            // Every word the batch covers is written, so empty ones are cleared without a separate pass.
            [unroll]
            for (uint k = 0; k < BATCH_WORDS; ++k)
            {
                const uint Word = Base / 32u + k;
                if (Word < MaskWords)
                {
                    SpanAt(ClusterLightMasks(), Index * MaskWords + Word)[0] = Words[k];
                    if (Words[k] != 0u)
                    {
                        FirstWord = min(FirstWord, Word);
                        EndWord   = Word + 1u;
                    }
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (bValidCluster)
    {
        SpanAt(Clusters(), Index).FirstWord = (FirstWord == 0xFFFFFFFFu) ? 0u : FirstWord;
        SpanAt(Clusters(), Index).EndWord   = EndWord;
    }
}
