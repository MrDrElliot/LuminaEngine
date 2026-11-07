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
    // Invalid unless depth edges are on.
    FTexture2D Depth;
    uint       _Unused1;
    uint       _Pad0;
    uint       _Pad1;
    uint       _Pad2;
    float4     SubsampleIndices;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define SMAA_RT_METRICS (Pass().RTMetrics)
#include "Includes/SMAA.slang"

// Relative step in linear depth between neighbors that marks a silhouette.
static const float kDepthEdgeRatio = 0.05;
// The share of the luma threshold a silhouette removes, so low-contrast outlines still count as edges.
static const float kDepthEdgeStrength = 0.75;

// SMAA predication, with depth instead of a separate predication buffer, per left and top edge.
float2 DepthPredicatedThreshold(float2 UV, float4 Offset[3], float Threshold)
{
    SMAATex DepthTex;
    DepthTex.Tex = Pass().Depth;
    DepthTex.Smp = Samplers::PointClamp;

    const float  Center    = LinearizeSceneDepth(DepthTex.SampleLevel(UV, 0.0).r);
    const float2 Neighbors = float2(LinearizeSceneDepth(DepthTex.SampleLevel(Offset[0].xy, 0.0).r),
                                    LinearizeSceneDepth(DepthTex.SampleLevel(Offset[0].zw, 0.0).r));
    const float2 Ratio     = abs(Neighbors - Center) / max(min(Neighbors, Center), 1e-4);
    return Threshold * (1.0 - kDepthEdgeStrength * step(kDepthEdgeRatio, Ratio));
}

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

    const float2 Threshold = Pass().Depth.IsValid() ? DepthPredicatedThreshold(Input.UV, offset, Pass().EdgeThreshold)
                                                    : float2(Pass().EdgeThreshold);
    float2 edges = SMAALumaEdgeDetectionPS(Input.UV, offset, colorTex, Threshold);
    return float4(edges, 0.0, 1.0);
}
