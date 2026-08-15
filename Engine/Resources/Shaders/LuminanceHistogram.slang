#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"

// LDS accumulation first, so the global buffer sees 256 atomics per group rather than one per pixel.

#define HISTOGRAM_BINS 256
#define TILE_DIM       16

struct FPushConstants
{
    uint*  Histogram;       // device address of the 256-uint bin array
    uint   HDRIndex;        // bindless SRV of HDR scene color
    uint   _Pad0;

    uint2  HDRSize;
    float  MinLogLum;       // log2 luminance mapped to bin 0
    float  InvLogLumRange;  // 1 / (MaxLogLum - MinLogLum)
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

groupshared uint gBins[HISTOGRAM_BINS];

uint LuminanceToBin(float Luma, float MinLogLum, float InvLogLumRange)
{
    // Bin 0 absorbs the range floor and true black, where log2 diverges.
    if (Luma < 1e-8)
    {
        return 0u;
    }

    float T = saturate((log2(Luma) - MinLogLum) * InvLogLumRange);
    return (uint)(T * float(HISTOGRAM_BINS - 1) + 0.5);
}

[shader("compute")]
[numthreads(TILE_DIM, TILE_DIM, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID, uint GroupIndex : SV_GroupIndex)
{
    gBins[GroupIndex] = 0u;
    GroupMemoryBarrierWithGroupSync();

    uint2 Size = Pass().HDRSize;
    if (DTid.x < Size.x && DTid.y < Size.y)
    {
        float3 Color = SanitizeRadiance(LoadTexture2D(Pass().HDRIndex, int2(DTid.xy)).rgb);
        float  Luma  = max(Luminance(Color), 0.0);

        InterlockedAdd(gBins[LuminanceToBin(Luma, Pass().MinLogLum, Pass().InvLogLumRange)], 1u);
    }

    GroupMemoryBarrierWithGroupSync();

    // TILE_DIM^2 == HISTOGRAM_BINS, so each thread owns exactly one bin.
    if (gBins[GroupIndex] > 0u)
    {
        InterlockedAdd(Pass().Histogram[GroupIndex], gBins[GroupIndex]);
    }
}
