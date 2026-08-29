#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/XeGTAO.slang"

// Edge-aware blur driven by the packed edges the main pass wrote, two horizontal pixels per thread.

struct FPushConstants
{
    uint2 ViewportSize;
    uint  AOTermIndex;
    uint  EdgesIndex;
    uint  OutputUAV;
    uint  FinalApply;
    float DenoiseBlurBeta;
    float Intensity;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

void AddSample(float Value, float Weight, inout float Sum, inout float SumWeight)
{
    Sum       += Weight * Value;
    SumWeight += Weight;
}

void StoreOutput(uint2 PixCoord, float Value, bool bFinalApply)
{
    float Result = Value;
    if (bFinalApply)
    {
        Result = saturate(Result * XE_GTAO_OCCLUSION_TERM_SCALE);
        Result = saturate(1.0 - (1.0 - Result) * Pass().Intensity);
    }
    gRWTextures2D[Pass().OutputUAV][PixCoord] = float4(Result, 0.0, 0.0, 0.0);
}

[shader("compute")]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 ViewportSize = Pass().ViewportSize;
    const uint2 PixCoordBase = DTid.xy * uint2(2, 1);

    if (PixCoordBase.y >= ViewportSize.y || PixCoordBase.x >= ViewportSize.x)
    {
        return;
    }

    const bool  bFinalApply = Pass().FinalApply != 0u;
    const float BlurAmount  = bFinalApply ? Pass().DenoiseBlurBeta : (Pass().DenoiseBlurBeta / 5.0);
    const float DiagWeight  = 0.85 * 0.5;

    const float2 PixelSize    = 1.0 / float2(ViewportSize);
    const float2 GatherCenter = float2(PixCoordBase) * PixelSize;

    Texture2D    Edges  = gTextures2D[Pass().EdgesIndex];
    Texture2D    AOTerm = gTextures2D[Pass().AOTermIndex];
    SamplerState Smp    = gSamplers[SAMPLER_POINT_CLAMP];

    const float4 EdgesQ0 = Edges.GatherRed(Smp, GatherCenter, int2(0, 0));
    const float4 EdgesQ1 = Edges.GatherRed(Smp, GatherCenter, int2(2, 0));
    const float4 EdgesQ2 = Edges.GatherRed(Smp, GatherCenter, int2(1, 2));

    const float4 VisQ0 = AOTerm.GatherRed(Smp, GatherCenter, int2(0, 0));
    const float4 VisQ1 = AOTerm.GatherRed(Smp, GatherCenter, int2(2, 0));
    const float4 VisQ2 = AOTerm.GatherRed(Smp, GatherCenter, int2(0, 2));
    const float4 VisQ3 = AOTerm.GatherRed(Smp, GatherCenter, int2(2, 2));

    [unroll]
    for (int Side = 0; Side < 2; ++Side)
    {
        const uint2 PixCoord = uint2(PixCoordBase.x + uint(Side), PixCoordBase.y);
        if (PixCoord.x >= ViewportSize.x)
        {
            continue;
        }

        const float4 EdgesL = XeGTAO_UnpackEdges((Side == 0) ? EdgesQ0.x : EdgesQ0.y);
        const float4 EdgesT = XeGTAO_UnpackEdges((Side == 0) ? EdgesQ0.z : EdgesQ1.w);
        const float4 EdgesR = XeGTAO_UnpackEdges((Side == 0) ? EdgesQ1.x : EdgesQ1.y);
        const float4 EdgesB = XeGTAO_UnpackEdges((Side == 0) ? EdgesQ2.w : EdgesQ2.z);

        float4 EdgesC = XeGTAO_UnpackEdges((Side == 0) ? EdgesQ0.y : EdgesQ1.x);

        // Edge detection is not symmetric across a pair, so folding in the neighbor's opposite edge sharpens it.
        EdgesC *= float4(EdgesL.y, EdgesR.x, EdgesT.w, EdgesB.z);

        // Letting a little occlusion cross a heavily walled pixel cuts both spatial and temporal aliasing.
        const float LeakThreshold = 2.5;
        const float LeakStrength  = 0.5;
        const float Edginess      = (saturate(4.0 - LeakThreshold - dot(EdgesC, 1.0.xxxx)) / (4.0 - LeakThreshold)) * LeakStrength;
        EdgesC = saturate(EdgesC + Edginess);

        const float WeightTL = DiagWeight * (EdgesC.x * EdgesL.z + EdgesC.z * EdgesT.x);
        const float WeightTR = DiagWeight * (EdgesC.z * EdgesT.y + EdgesC.y * EdgesR.z);
        const float WeightBL = DiagWeight * (EdgesC.w * EdgesB.x + EdgesC.x * EdgesL.w);
        const float WeightBR = DiagWeight * (EdgesC.y * EdgesR.w + EdgesC.w * EdgesB.y);

        const float ValueC  = (Side == 0) ? VisQ0[1] : VisQ1[0];
        const float ValueL  = (Side == 0) ? VisQ0[0] : VisQ0[1];
        const float ValueT  = (Side == 0) ? VisQ0[2] : VisQ1[3];
        const float ValueR  = (Side == 0) ? VisQ1[0] : VisQ1[1];
        const float ValueB  = (Side == 0) ? VisQ2[2] : VisQ3[3];
        const float ValueTL = (Side == 0) ? VisQ0[3] : VisQ0[2];
        const float ValueBR = (Side == 0) ? VisQ3[3] : VisQ3[2];
        const float ValueTR = (Side == 0) ? VisQ1[3] : VisQ1[2];
        const float ValueBL = (Side == 0) ? VisQ2[3] : VisQ2[2];

        float SumWeight = BlurAmount;
        float Sum       = ValueC * SumWeight;

        AddSample(ValueL, EdgesC.x, Sum, SumWeight);
        AddSample(ValueR, EdgesC.y, Sum, SumWeight);
        AddSample(ValueT, EdgesC.z, Sum, SumWeight);
        AddSample(ValueB, EdgesC.w, Sum, SumWeight);

        AddSample(ValueTL, WeightTL, Sum, SumWeight);
        AddSample(ValueTR, WeightTR, Sum, SumWeight);
        AddSample(ValueBL, WeightBL, Sum, SumWeight);
        AddSample(ValueBR, WeightBR, Sum, SumWeight);

        StoreOutput(PixCoord, Sum / max(SumWeight, 1e-6), bFinalApply);
    }
}
