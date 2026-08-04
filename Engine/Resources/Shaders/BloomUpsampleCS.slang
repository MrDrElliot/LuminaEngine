#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"

// Bloom upsample: 3x3 tent filter (Jimenez 2014) from the smaller mip, additively accumulated into
// the larger one. One dispatch per step. Bindless source (SrcIndex@SrcMip) + read-modify-write DstUAV.

#define TILE_WIDTH  8
#define TILE_HEIGHT 8

struct FPushConstants
{
    float2 SrcTexelSize;
    float  Radius;
    uint   SrcIndex;        // bindless SRV: source image (all mips)

    uint2  DstSize;
    uint   DstUAV;          // bindless RW: destination mip
    float  SrcMip;          // mip level to sample on the source SRV

    float  Scatter;         // weight on the accumulated coarser mips; <1 = geometric halo falloff
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

    // Destination mip UV: each dest pixel center maps to the same UV on the
    // smaller source thanks to the 1:2 ratio.
    const float2 UV = (float2(DstP) + 0.5) / float2(Pass().DstSize);
    const float2 T  = Pass().SrcTexelSize * Pass().Radius;

    // 3x3 tent. Bilinear samples make this 9 taps without manual corner
    // fetches; the source filter mode is linear-clamp.
    SamplerState Smp = gSamplers[SAMPLER_LINEAR_CLAMP];
    Texture2D    Src = gTextures2D[Pass().SrcIndex];

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

    // Progressive accumulation: dest holds its own downsample plus everything already gathered
    // from deeper mips; Scatter < 1 makes each coarser octave contribute geometrically less, which
    // controls how far the wide halo reaches without re-tuning intensity.
    float3 Prev = gRWTextures2D[Pass().DstUAV][DstP].rgb;
    gRWTextures2D[Pass().DstUAV][DstP] = float4(Prev + Tent * Pass().Scatter, 0);
}
