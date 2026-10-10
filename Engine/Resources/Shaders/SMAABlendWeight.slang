#pragma once

#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"

// Edges and both LUTs come through the bindless table by push-constant index (matches FSMAAPushConstants).
struct FPushConstants
{
    float4     RTMetrics;
    float      EdgeThreshold;
    float      DebugMode;
    FTexture2D Edges;
    FTexture2D AreaLUT;
    FTexture2D SearchLUT;
    float      MaxSearchSteps;     // driven by ESMAAQuality, not a compile-time preset
    float      MaxSearchStepsDiag;
    uint       _Pad2;
    float4     SubsampleIndices;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define SMAA_RT_METRICS (Pass().RTMetrics)

// Defined ahead of the include so SMAA.slang's #ifndef defaults are skipped.
#define SMAA_MAX_SEARCH_STEPS      (Pass().MaxSearchSteps)
#define SMAA_MAX_SEARCH_STEPS_DIAG (Pass().MaxSearchStepsDiag)

#include "Includes/SMAA.slang"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

[shader("fragment")]
float4 FragmentMain(VSOutput Input) : SV_Target0
{
    // The search LUT is integer-coded but the technique relies on bilinear filtering, so one linear-clamp sampler serves all three.
    FSampler LinearClamp = Samplers::LinearClamp;

    SMAATex edgesTex;  edgesTex.Tex  = Pass().Edges; edgesTex.Smp  = LinearClamp;
    SMAATex areaTex;   areaTex.Tex   = Pass().AreaLUT; areaTex.Smp   = LinearClamp;
    SMAATex searchTex; searchTex.Tex = Pass().SearchLUT; searchTex.Smp = LinearClamp;

    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationOffsets(Input.UV, pixcoord, offset);

    return SMAABlendingWeightCalculationPS(
        Input.UV,
        pixcoord,
        offset,
        edgesTex,
        areaTex,
        searchTex,
        Pass().SubsampleIndices);
}
