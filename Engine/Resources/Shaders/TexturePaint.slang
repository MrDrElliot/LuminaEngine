#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"

// Stamps a soft radial brush into a bindless UAV; the CPU dispatches only over the brush's pixel
// rect [RectMin, RectMax). An optional grayscale brush mask (white = full coverage) modulates falloff.

struct FPushConstants
{
    uint    TargetIndex;    // bindless RWTexture2D<float4> slot to write
    int     BrushIndex;     // bindless Texture2D slot of a grayscale brush mask; < 0 = solid radial
    uint2   TargetSize;     // full image size in pixels
    uint2   RectMin;        // dispatch covers pixels [RectMin, RectMax)
    uint2   RectMax;
    float2  CenterPx;       // brush center, in pixels
    float   RadiusPx;       // brush radius, in pixels
    float   Strength;       // 0..1 opacity at the brush center
    float   Hardness;       // edge falloff exponent; > 1 sharpens the edge
    // RGBA as scalars so the CPU struct's offsets match under any block layout.
    float   ColorR;
    float   ColorG;
    float   ColorB;
    float   ColorA;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    uint2 Px = Pass().RectMin + DTid.xy;
    if (Px.x >= Pass().RectMax.x || Px.y >= Pass().RectMax.y)   return;
    if (Px.x >= Pass().TargetSize.x || Px.y >= Pass().TargetSize.y) return;

    float2 Center = float2(Px) + 0.5f;
    float  Dist   = length(Center - Pass().CenterPx);
    if (Dist > Pass().RadiusPx)
    {
        return;
    }

    float Falloff = saturate(1.0f - Dist / max(Pass().RadiusPx, 1e-4f));
    float Mask    = pow(Falloff, max(Pass().Hardness, 1e-3f));

    if (Pass().BrushIndex >= 0)
    {
        // Map the brush rect onto [0,1]^2 and sample the mask's red channel as coverage.
        float2 BrushUV = (Center - (Pass().CenterPx - Pass().RadiusPx)) / (2.0f * Pass().RadiusPx);
        Mask *= gTextures2D[Pass().BrushIndex].SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], BrushUV, 0.0f).r;
    }

    float  Alpha = saturate(Mask * Pass().Strength);
    float4 Color = float4(Pass().ColorR, Pass().ColorG, Pass().ColorB, Pass().ColorA);
    float4 Dst   = gRWTextures2D[Pass().TargetIndex][Px];
    gRWTextures2D[Pass().TargetIndex][Px] = lerp(Dst, Color, Alpha);
}
