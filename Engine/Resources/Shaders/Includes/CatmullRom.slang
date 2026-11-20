#pragma once
#include "SceneGlobals.slang"

// Nine Catmull-Rom taps folded into five bilinear ones, after Jimenez's filmic SMAA presentation.
float3 SampleCatmullRom(FTexture2D Source, FSampler Linear, float2 UV, float2 SourceSize)
{
    const float2 InvSize   = 1.0 / SourceSize;
    const float2 SamplePos = UV * SourceSize;
    const float2 Center    = floor(SamplePos - 0.5) + 0.5;
    const float2 F         = SamplePos - Center;

    const float2 W0 = F * (-0.5 + F * (1.0 - 0.5 * F));
    const float2 W1 = 1.0 + F * F * (-2.5 + 1.5 * F);
    const float2 W2 = F * (0.5 + F * (2.0 - 1.5 * F));
    const float2 W3 = F * F * (-0.5 + 0.5 * F);

    const float2 W12      = W1 + W2;
    const float2 Offset12 = W2 / W12;

    const float2 UV0  = (Center - 1.0) * InvSize;
    const float2 UV3  = (Center + 2.0) * InvSize;
    const float2 UV12 = (Center + Offset12) * InvSize;

    float3 Result = float3(0.0, 0.0, 0.0);
    float  Weight = 0.0;

    Result += Source.SampleLevel(Linear, float2(UV12.x, UV0.y),  0.0).rgb * (W12.x * W0.y);
    Result += Source.SampleLevel(Linear, float2(UV0.x,  UV12.y), 0.0).rgb * (W0.x  * W12.y);
    Result += Source.SampleLevel(Linear, float2(UV12.x, UV12.y), 0.0).rgb * (W12.x * W12.y);
    Result += Source.SampleLevel(Linear, float2(UV3.x,  UV12.y), 0.0).rgb * (W3.x  * W12.y);
    Result += Source.SampleLevel(Linear, float2(UV12.x, UV3.y),  0.0).rgb * (W12.x * W3.y);
    Weight += W12.x * W0.y + W0.x * W12.y + W12.x * W12.y + W3.x * W12.y + W12.x * W3.y;

    return Result / max(Weight, 1e-5);
}
