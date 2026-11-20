#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/CatmullRom.slang"

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
