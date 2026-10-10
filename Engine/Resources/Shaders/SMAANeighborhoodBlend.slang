#pragma once

#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"

// Matches FSMAAPushConstants.
struct FPushConstants
{
    float4     RTMetrics;
    float      EdgeThreshold;
    float      DebugMode;
    FTexture2D Color;
    FTexture2D BlendWeights;
    uint       _Unused0;
    uint       _Pad0;
    uint       _Pad1;
    uint       _Pad2;
    float4     SubsampleIndices;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define SMAA_RT_METRICS (Pass().RTMetrics)
#include "Includes/SMAA.slang"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

[shader("fragment")]
float4 FragmentMain(VSOutput Input) : SV_Target0
{
    FSampler LinearClamp = Samplers::LinearClamp;

    SMAATex colorTex; colorTex.Tex = Pass().Color; colorTex.Smp = LinearClamp;
    SMAATex blendTex; blendTex.Tex = Pass().BlendWeights; blendTex.Smp = LinearClamp;

    if (Pass().DebugMode > 0.5)
    {
        float4 bw = blendTex.Sample(Input.UV);
        return float4(bw.rgb, 1.0);
    }

    float4 offset;
    SMAANeighborhoodBlendingOffsets(Input.UV, offset);

    return SMAANeighborhoodBlendingPS(Input.UV, offset, colorTex, blendTex);
}
