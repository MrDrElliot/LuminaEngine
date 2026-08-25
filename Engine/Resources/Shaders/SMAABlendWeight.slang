#pragma once

#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"

// SMAA pass 2 (blend weight). No pass-local binding set: edges / area-LUT / search-LUT are read
// through the bindless table via indices in the push constant (matches FSMAAPushConstants).
struct FPushConstants
{
    float4 RTMetrics;
    float  EdgeThreshold;
    float  DebugMode;
    uint   TexIndex0;   // edges
    uint   TexIndex1;   // area LUT
    uint   TexIndex2;   // search LUT
    float  MaxSearchSteps;     // driven by ESMAAQuality, not a compile-time preset
    float  MaxSearchStepsDiag;
    uint   _Pad2;
    float4 SubsampleIndices;
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
    // Edges/area/search all want bilinear filtering (the search LUT is integer-coded but the
    // technique relies on the filter); single linear-clamp sampler for all three.
    SamplerState LinearClamp = gSamplers[SAMPLER_LINEAR_CLAMP];

    SMAATex edgesTex;  edgesTex.Tex  = gTextures2D[Pass().TexIndex0]; edgesTex.Smp  = LinearClamp;
    SMAATex areaTex;   areaTex.Tex   = gTextures2D[Pass().TexIndex1]; areaTex.Smp   = LinearClamp;
    SMAATex searchTex; searchTex.Tex = gTextures2D[Pass().TexIndex2]; searchTex.Smp = LinearClamp;

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
