#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

struct FPushConstants
{
    FRWTexture2D Target;
    int          BrushIndex;   // Grayscale brush mask, negative for a solid radial
    uint2        TargetSize;
    uint2        RectMin;      // Dispatch covers pixels [RectMin, RectMax)
    uint2        RectMax;
    float2       CenterPx;
    float        RadiusPx;
    float        Strength;     // 0..1 opacity at the brush center
    float        Hardness;     // Edge falloff exponent, above 1 sharpens the edge
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
        float2 BrushUV = (Center - (Pass().CenterPx - Pass().RadiusPx)) / (2.0f * Pass().RadiusPx);
        Mask *= FTexture2D(Pass().BrushIndex).SampleLevel(Samplers::LinearClamp, BrushUV, 0.0f).r;
    }

    float  Alpha = saturate(Mask * Pass().Strength);
    float4 Color = float4(Pass().ColorR, Pass().ColorG, Pass().ColorB, Pass().ColorA);
    float4 Dst   = Pass().Target[Px];
    Pass().Target[Px] = lerp(Dst, Color, Alpha);
}
