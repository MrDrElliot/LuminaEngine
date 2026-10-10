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
groupshared float4 gsLightViewPosRadius[LOCAL_SIZE];
groupshared uint   gsLightIndex[LOCAL_SIZE];
groupshared uint   gsWaveKept[MAX_WAVES_PER_GROUP];
groupshared float3 gsWaveMin[MAX_WAVES_PER_GROUP];
groupshared float3 gsWaveMax[MAX_WAVES_PER_GROUP];

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
    const bool bValidCluster = all(Coord < Grid) && Clusters().Contains(Index) && ClusterWordRanges().Contains(Index);

    float3 ClusterMin = bValidCluster ? loadAligned<16>(&Clusters().At(Index).MinPoint).xyz : float3(3.0e38);
    float3 ClusterMax = bValidCluster ? loadAligned<16>(&Clusters().At(Index).MaxPoint).xyz : float3(-3.0e38);

    // The block's bounds, reduced per wave and then across the waves.
    const uint WaveSize  = WaveGetLaneCount();
    const uint WaveIndex = Local / WaveSize;
    const uint NumWaves  = (LOCAL_SIZE + WaveSize - 1) / WaveSize;
    const float3 WaveMinBounds = WaveActiveMin(ClusterMin);
    const float3 WaveMaxBounds = WaveActiveMax(ClusterMax);
    if (WaveIsFirstLane())
    {
        gsWaveMin[WaveIndex] = WaveMinBounds;
        gsWaveMax[WaveIndex] = WaveMaxBounds;
    }
    GroupMemoryBarrierWithGroupSync();

    float3 BlockMin = gsWaveMin[0];
    float3 BlockMax = gsWaveMax[0];
    for (uint w = 1; w < NumWaves; ++w)
    {
        BlockMin = min(BlockMin, gsWaveMin[w]);
        BlockMax = max(BlockMax, gsWaveMax[w]);
    }

    const uint LightCount = GetNumLights();
    const uint MaskWords  = GetClusterMaskWords();

    // The directional light is always at index 0 and applies globally.
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
            const float4 PositionRadius = loadAligned<16>(reinterpret<float4*>(LightData().Lights.At(LoadIndex)));
            ViewPosRadius = float4(mul(View, float4(PositionRadius.xyz, 1.0)).xyz, PositionRadius.w);
            bKeep = SphereAABBIntersection(ViewPosRadius.xyz, PositionRadius.w, BlockMin, BlockMax);
        }

        const uint WaveKept   = WaveActiveCountBits(bKeep);
        const uint WaveOffset = WavePrefixCountBits(bKeep);
        if (WaveIsFirstLane())
        {
            gsWaveKept[WaveIndex] = WaveKept;
        }
        GroupMemoryBarrierWithGroupSync();

        uint Offset = 0;
        uint Kept   = 0;
        for (uint w = 0; w < NumWaves; ++w)
        {
            Offset += (w < WaveIndex) ? gsWaveKept[w] : 0u;
            Kept   += gsWaveKept[w];
        }
        if (bKeep)
        {
            gsLightViewPosRadius[Offset + WaveOffset] = ViewPosRadius;
            gsLightIndex[Offset + WaveOffset]         = LoadIndex;
        }
        GroupMemoryBarrierWithGroupSync();

        if (bValidCluster)
        {
            uint4 Words = uint4(0u, 0u, 0u, 0u);
            for (uint j = 0; j < Kept; ++j)
            {
                const float4 LightVP = gsLightViewPosRadius[j];
                if (SphereAABBIntersection(LightVP.xyz, LightVP.w, ClusterMin, ClusterMax))
                {
                    const uint InBatch = gsLightIndex[j] - Base;
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
                    ClusterLightMasks().At(Index * MaskWords + Word)[0] = Words[k];
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
        const uint First = (FirstWord == 0xFFFFFFFFu) ? 0u : FirstWord;
        ClusterWordRanges().At(Index)[0] = First | (EndWord << 16u);
    }
}
