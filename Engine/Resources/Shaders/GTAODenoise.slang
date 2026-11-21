#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/XeGTAO.slang"

// Edge-aware blur of visibility and bent normal lean, driven by the packed edges the main pass wrote, two horizontal pixels per thread.

struct FPushConstants
{
    uint2        ViewportSize;
    FTexture2D   AOTerm;
    FTexture2D   Edges;
    FRWTexture2D Output;
    uint         FinalApply;
    float        DenoiseBlurBeta;
    float        Intensity;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

void AddSample(float4 Value, float Weight, inout float4 Sum, inout float SumWeight)
{
    Sum       += Weight * Value;
    SumWeight += Weight;
}

// The lean in gba is stored linearly, so a weighted mean of it needs no decode.
void StoreOutput(uint2 PixCoord, float4 Value, bool bFinalApply)
{
    float4 Result = Value;
    if (bFinalApply)
    {
        Result.r = saturate(Result.r * XE_GTAO_OCCLUSION_TERM_SCALE);
        Result.r = saturate(1.0 - (1.0 - Result.r) * Pass().Intensity);
    }
    Pass().Output[PixCoord] = Result;
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

    FTexture2D Edges  = Pass().Edges;
    FTexture2D AOTerm = Pass().AOTerm;
    FSampler   Smp    = Samplers::PointClamp;

    const float4 EdgesQ0 = Edges.GatherRed(Smp, GatherCenter, int2(0, 0));
    const float4 EdgesQ1 = Edges.GatherRed(Smp, GatherCenter, int2(2, 0));
    const float4 EdgesQ2 = Edges.GatherRed(Smp, GatherCenter, int2(1, 2));

    // Rows above, at and below the pair, columns from one left of it to one right of it, clamped like the edge gathers.
    float4 Terms[3][4];
    const int2 MaxTap = int2(ViewportSize) - 1;
    [unroll]
    for (int Row = 0; Row < 3; ++Row)
    {
        [unroll]
        for (int Column = 0; Column < 4; ++Column)
        {
            const int2 Tap = clamp(int2(PixCoordBase) + int2(Column - 1, Row - 1), int2(0, 0), MaxTap);
            Terms[Row][Column] = AOTerm.Load(int3(Tap, 0));
        }
    }

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

        const int Center = Side + 1;

        float  SumWeight = BlurAmount;
        float4 Sum       = Terms[1][Center] * SumWeight;

        AddSample(Terms[1][Center - 1], EdgesC.x, Sum, SumWeight);
        AddSample(Terms[1][Center + 1], EdgesC.y, Sum, SumWeight);
        AddSample(Terms[0][Center],     EdgesC.z, Sum, SumWeight);
        AddSample(Terms[2][Center],     EdgesC.w, Sum, SumWeight);

        AddSample(Terms[0][Center - 1], WeightTL, Sum, SumWeight);
        AddSample(Terms[0][Center + 1], WeightTR, Sum, SumWeight);
        AddSample(Terms[2][Center - 1], WeightBL, Sum, SumWeight);
        AddSample(Terms[2][Center + 1], WeightBR, Sum, SumWeight);

        StoreOutput(PixCoord, Sum / max(SumWeight, 1e-6), bFinalApply);
    }
}
