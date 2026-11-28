#pragma once
#include "Includes/SceneGlobals.slang"

#define BINS_PER_THREAD (LIGHT_ZBIN_COUNT / LIGHT_BIN_SCAN_GROUP_SIZE)
#define ZBIN_WORDS_PER_THREAD (2u * BINS_PER_THREAD)

// Sixteen lanes is the narrowest wave a GPU runs, which bounds how many waves the group holds.
groupshared uint gsWaveTotals[LIGHT_BIN_SCAN_GROUP_SIZE / 16u];

// One group turns the bin counts into each bin's first sorted slot, resets what the next frame accumulates and sizes the dispatches after it.
[shader("compute")]
[numthreads(LIGHT_BIN_SCAN_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 LocalID : SV_GroupThreadID)
{
    const uint Thread = LocalID.x;
    const uint First  = Thread * BINS_PER_THREAD;

    uint Counts[BINS_PER_THREAD];
    uint Sum = 0u;
    [unroll]
    for (uint Index = 0u; Index < BINS_PER_THREAD; ++Index)
    {
        Counts[Index] = LightGrid().At(LIGHT_GRID_BIN_COUNTS + First + Index)[0];
        Sum += Counts[Index];
    }

    const uint WavePrefix = WavePrefixSum(Sum);
    const uint WaveIndex  = Thread / WaveGetLaneCount();
    if (WaveGetLaneIndex() == WaveGetLaneCount() - 1u)
    {
        gsWaveTotals[WaveIndex] = WavePrefix + Sum;
    }
    GroupMemoryBarrierWithGroupSync();

    uint Before = 0u;
    for (uint Wave = 0u; Wave < WaveIndex; ++Wave)
    {
        Before += gsWaveTotals[Wave];
    }

    // Slot 0 belongs to the sun, so the depth-sorted locals start at 1.
    uint Start = 1u + Before + WavePrefix;
    [unroll]
    for (uint Index = 0u; Index < BINS_PER_THREAD; ++Index)
    {
        LightGrid().At(LIGHT_GRID_BIN_STARTS + First + Index)[0] = Start;
        LightGrid().At(LIGHT_GRID_BIN_COUNTS + First + Index)[0] = 0u;
        Start += Counts[Index];
    }

    // An empty bin reads as a first index past its last, which the scatter's minimums pull down.
    [unroll]
    for (uint Index = 0u; Index < ZBIN_WORDS_PER_THREAD; ++Index)
    {
        LightGrid().At(LIGHT_GRID_ZBINS + Thread * ZBIN_WORDS_PER_THREAD + Index)[0] = 0xFFFFFFFFu;
    }

    if (Thread == 0u)
    {
        const uint Visible   = LightGrid().At(LIGHT_GRID_COUNTER)[0];
        const uint NumLights = 1u + Visible;
        const uint MaskWords = (NumLights + 31u) / 32u;
        const uint Tiles     = Scene().GridSize.x * Scene().GridSize.y;
        LightGrid().At(LIGHT_GRID_COUNTER)[0]          = 0u;
        LightGrid().At(LIGHT_GRID_NUM_LIGHTS)[0]       = NumLights;
        LightGrid().At(LIGHT_GRID_NUM_VISIBLE)[0]      = Visible;
        LightGrid().At(LIGHT_GRID_MASK_WORDS)[0]       = MaskWords;
        LightGrid().At(LIGHT_GRID_CLEAR_ARGS)[0]       = (Tiles * MaskWords + LIGHT_MASK_CLEAR_GROUP_SIZE - 1u) / LIGHT_MASK_CLEAR_GROUP_SIZE;
        LightGrid().At(LIGHT_GRID_CLEAR_ARGS + 1u)[0]  = 1u;
        LightGrid().At(LIGHT_GRID_CLEAR_ARGS + 2u)[0]  = 1u;
        LightGrid().At(LIGHT_GRID_SCATTER_ARGS)[0]     = (Visible + LIGHT_CULL_GROUP_SIZE - 1u) / LIGHT_CULL_GROUP_SIZE;
        LightGrid().At(LIGHT_GRID_SCATTER_ARGS + 1u)[0] = 1u;
        LightGrid().At(LIGHT_GRID_SCATTER_ARGS + 2u)[0] = 1u;

        FLight Sun = (FLight)0;
        if (LightData().bHasSun != 0u && LightData().Lights.Count > 0u)
        {
            Sun = LightData().Lights.GetAligned(0u);
        }
        if (ViewLights().Contains(0u))
        {
            ViewLights().At(0u)[0] = Sun;
        }
    }
}
