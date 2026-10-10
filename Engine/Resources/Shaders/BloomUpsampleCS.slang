#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// 3x3 tent upsample (Jimenez 2014) additively accumulated into the larger mip, one dispatch per step.

#define TILE_WIDTH  8
#define TILE_HEIGHT 8

struct FPushConstants
{
    float2     SrcTexelSize;
    float      Radius;
    FTexture2D Src;

    uint2        DstSize;
    FRWTexture2D Dst;
    float        SrcMip;

    float  Scatter;
    uint   _Pad0;
    uint   _Pad1;
    uint   _Pad2;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(TILE_WIDTH, TILE_HEIGHT, 1)]
void ComputeMain(uint3 DispatchID : SV_DispatchThreadID)
{
    const int2 DstP = int2(DispatchID.xy);
    if (any(uint2(DstP) >= Pass().DstSize))
    {
        return;
    }

    const float2 UV = (float2(DstP) + 0.5) / float2(Pass().DstSize);
    const float2 T  = Pass().SrcTexelSize * Pass().Radius;

    FSampler   Smp = Samplers::LinearClamp;
    FTexture2D Src = Pass().Src;

    float3 a = Src.SampleLevel(Smp, UV + T * float2(-1.0, -1.0), Pass().SrcMip).rgb;
    float3 b = Src.SampleLevel(Smp, UV + T * float2( 0.0, -1.0), Pass().SrcMip).rgb;
    float3 c = Src.SampleLevel(Smp, UV + T * float2( 1.0, -1.0), Pass().SrcMip).rgb;

    float3 d = Src.SampleLevel(Smp, UV + T * float2(-1.0,  0.0), Pass().SrcMip).rgb;
    float3 e = Src.SampleLevel(Smp, UV + T * float2( 0.0,  0.0), Pass().SrcMip).rgb;
    float3 f = Src.SampleLevel(Smp, UV + T * float2( 1.0,  0.0), Pass().SrcMip).rgb;

    float3 g = Src.SampleLevel(Smp, UV + T * float2(-1.0,  1.0), Pass().SrcMip).rgb;
    float3 h = Src.SampleLevel(Smp, UV + T * float2( 0.0,  1.0), Pass().SrcMip).rgb;
    float3 i = Src.SampleLevel(Smp, UV + T * float2( 1.0,  1.0), Pass().SrcMip).rgb;

    float3 Tent =  e * 4.0;
    Tent       += (b + d + f + h) * 2.0;
    Tent       += (a + c + g + i);
    Tent       *= (1.0 / 16.0);

    // Scatter below 1 makes each coarser octave contribute geometrically less, which sets the halo's reach.
    float3 Prev = Pass().Dst[DstP].rgb;
    Pass().Dst[DstP] = float4(Prev + Tent * Pass().Scatter, 0);
}
