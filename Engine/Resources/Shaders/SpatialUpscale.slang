#pragma once
#include "Includes/SceneGlobals.slang"

// The built-in upscale from render to display resolution, a Catmull-Rom filter held inside the source texels around each pixel.

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

struct FData
{
    FTexture2D Source;
    uint       SourceWidth;
    uint       SourceHeight;
    float      Sharpness;
};

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

[shader("fragment")]
float4 FragmentMain(FragmentInput Input) : SV_Target0
{
    FData* Data = GetPassArgs<FData>();

    FTexture2D Source = Data.Source;
    FSampler   Linear = Samplers::LinearClamp;

    const float2 SourceSize = float2(Data.SourceWidth, Data.SourceHeight);

    const float3 Bicubic  = SampleCatmullRom(Source, Linear, Input.UV, SourceSize);
    const float3 Bilinear = Source.SampleLevel(Linear, Input.UV, 0.0).rgb;

    // Pushing away from the bilinear result is an unsharp mask whose blur is the upscale's own.
    float3 Color = Bicubic + (Bicubic - Bilinear) * (Data.Sharpness * 2.0);

    // The four texels the pixel falls between bound it, which stops the negative lobes ringing on HDR edges.
    const int2 MaxTexel = int2(SourceSize) - int2(1, 1);
    const int2 Base     = int2(floor(Input.UV * SourceSize - 0.5));
    const float3 A = Source.Load(int3(clamp(Base,               int2(0, 0), MaxTexel), 0)).rgb;
    const float3 B = Source.Load(int3(clamp(Base + int2(1, 0),  int2(0, 0), MaxTexel), 0)).rgb;
    const float3 C = Source.Load(int3(clamp(Base + int2(0, 1),  int2(0, 0), MaxTexel), 0)).rgb;
    const float3 D = Source.Load(int3(clamp(Base + int2(1, 1),  int2(0, 0), MaxTexel), 0)).rgb;
    Color = clamp(Color, min(min(A, B), min(C, D)), max(max(A, B), max(C, D)));

    return float4(max(Color, float3(0.0, 0.0, 0.0)), 1.0);
}
