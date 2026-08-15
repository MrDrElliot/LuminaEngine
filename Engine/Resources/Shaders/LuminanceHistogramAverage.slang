#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"

// Weighted mean of the histogram between two percentiles, then the eye-adaptation feedback.

#define HISTOGRAM_BINS 256

struct FPushConstants
{
    uint*  Histogram;       // device address of the 256-uint bin array
    uint   AdaptUAV;        // bindless UAV of the 1x1 adapted-luminance image
    float  MinLogLum;

    float  LogLumRange;
    float  LowPercent;      // fraction of pixels rejected from the dark tail
    float  HighPercent;     // cumulative fraction above which pixels are rejected
    float  DeltaTime;

    float  AdaptationSpeed;
    float  _Pad;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

groupshared uint gBins[HISTOGRAM_BINS];

float BinLogLuminance(uint Bin)
{
    return Pass().MinLogLum + ((float(Bin) + 0.5) / float(HISTOGRAM_BINS)) * Pass().LogLumRange;
}

[shader("compute")]
[numthreads(HISTOGRAM_BINS, 1, 1)]
void ComputeMain(uint GroupIndex : SV_GroupIndex)
{
    gBins[GroupIndex] = Pass().Histogram[GroupIndex];
    GroupMemoryBarrierWithGroupSync();

    // Serial in one lane; finding the percentile cuts in parallel would need a full prefix sum.
    if (GroupIndex != 0u)
    {
        return;
    }

    float Total = 0.0;
    for (uint t = 0u; t < HISTOGRAM_BINS; ++t)
    {
        Total += float(gBins[t]);
    }

    float MidLogLum = Pass().MinLogLum + Pass().LogLumRange * 0.5;
    float TargetLum = exp2(MidLogLum);

    if (Total > 0.0)
    {
        float LowThresh  = Total * Pass().LowPercent;
        float HighThresh = Total * Pass().HighPercent;

        float SumWeighted = 0.0;
        float SumWeight   = 0.0;
        float Running     = 0.0;

        for (uint i = 0u; i < HISTOGRAM_BINS; ++i)
        {
            float BinStart = Running;
            float BinEnd   = Running + float(gBins[i]);
            Running = BinEnd;

            // Partial overlap, so a percentile cut landing mid-bin does not quantize the result.
            float Weight = max(min(BinEnd, HighThresh) - max(BinStart, LowThresh), 0.0);
            if (Weight <= 0.0)
            {
                continue;
            }

            SumWeighted += Weight * BinLogLuminance(i);
            SumWeight   += Weight;
        }

        if (SumWeight > 0.0)
        {
            TargetLum = exp2(SumWeighted / SumWeight);
        }
    }

    float PrevLum = gRWTextures2D[Pass().AdaptUAV][int2(0, 0)].r;
    // The range test also catches NaN/Inf, since any NaN comparison is false.
    if (!(PrevLum > 1e-6 && PrevLum < 1e6))
    {
        PrevLum = TargetLum;
    }

    float Blend      = 1.0 - exp(-Pass().DeltaTime * Pass().AdaptationSpeed);
    float AdaptedLum = PrevLum + (TargetLum - PrevLum) * Blend;

    gRWTextures2D[Pass().AdaptUAV][int2(0, 0)] = float4(AdaptedLum, 0.0, 0.0, 0.0);
}
