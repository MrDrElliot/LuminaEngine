#pragma once

#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"

// Matches FSMAAPushConstants on the C++ side.
struct FPushConstants
{
    float4     RTMetrics;
    float      EdgeThreshold;
    float      DebugMode;
    FTexture2D Color;
    uint       _Unused0;
    uint       _Unused1;
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
    SMAATex colorTex;
    colorTex.Tex = Pass().Color;
    colorTex.Smp = Samplers::PointClamp;

    float4 offset[3];
    SMAAEdgeDetectionOffsets(Input.UV, offset);

    float2 edges = SMAALumaEdgeDetectionPS(Input.UV, offset, colorTex, Pass().EdgeThreshold);
    return float4(edges, 0.0, 1.0);
}
