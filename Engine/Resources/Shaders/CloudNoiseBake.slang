#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"

// Stands in for Nubis' baked volume: R = Perlin-Worley base, GBA = Worley erosion octaves.

struct FPushConstants
{
    uint  NoiseUAV;   // bindless 3D UAV, RGBA8
    uint  _Pad0;
    uint  _Pad1;
    uint  _Pad2;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

float CloudHash1(float3 P)
{
    return frac(sin(dot(P, float3(127.1, 311.7, 74.7))) * 43758.5453);
}

float3 CloudHash3(float3 P)
{
    return float3(CloudHash1(P), CloudHash1(P + 19.19), CloudHash1(P + 43.71));
}

// Wrapping the cell index is what makes the volume tile; without it the layer seams at every repeat.
float3 CloudCellPoint(float3 Cell, float Freq)
{
    float3 Wrapped = Cell - floor(Cell / Freq) * Freq;
    return CloudHash3(Wrapped);
}

// Inverted so cell centres are dense and the boundaries read as the gaps between puffs.
float CloudWorley(float3 UVW, float Freq)
{
    float3 P = UVW * Freq;
    float3 I = floor(P);
    float3 F = P - I;

    float MinDistSq = 1e9;
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int z = -1; z <= 1; ++z)
            {
                float3 Offset = float3(x, y, z);
                float3 Point  = CloudCellPoint(I + Offset, Freq);
                float3 D      = Offset + Point - F;
                MinDistSq = min(MinDistSq, dot(D, D));
            }
        }
    }
    return saturate(1.0 - sqrt(MinDistSq));
}

float CloudWorleyFBM(float3 UVW, float Freq)
{
    return CloudWorley(UVW, Freq)        * 0.625
         + CloudWorley(UVW, Freq * 2.0)  * 0.25
         + CloudWorley(UVW, Freq * 4.0)  * 0.125;
}

// Tileable gradient noise: the corner hash wraps on Freq for the same reason Worley's does.
float CloudGradient(float3 UVW, float Freq)
{
    float3 P = UVW * Freq;
    float3 I = floor(P);
    float3 F = P - I;
    float3 W = F * F * (3.0 - 2.0 * F);

    float Result = 0.0;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float3 Corner  = float3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
        float3 Wrapped = I + Corner;
        Wrapped -= floor(Wrapped / Freq) * Freq;

        float3 Grad  = CloudHash3(Wrapped) * 2.0 - 1.0;
        float  Dot   = dot(Grad, F - Corner);
        float3 Blend = lerp(1.0 - W, W, Corner);
        Result += Dot * Blend.x * Blend.y * Blend.z;
    }
    return saturate(Result * 0.5 + 0.5);
}

float CloudPerlinFBM(float3 UVW, float Freq)
{
    return CloudGradient(UVW, Freq)       * 0.5333
         + CloudGradient(UVW, Freq * 2.0) * 0.2667
         + CloudGradient(UVW, Freq * 4.0) * 0.1333
         + CloudGradient(UVW, Freq * 8.0) * 0.0667;
}

float CloudRemap(float V, float InMin, float InMax, float OutMin, float OutMax)
{
    return OutMin + (V - InMin) * (OutMax - OutMin) / max(InMax - InMin, 1e-5);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    RWTexture3D<float4> uNoise = gRWTextures3D[Pass().NoiseUAV];

    uint W, H, D;
    uNoise.GetDimensions(W, H, D);
    if (DTid.x >= W || DTid.y >= H || DTid.z >= D)
    {
        return;
    }

    float3 UVW = (float3(DTid) + 0.5) / float3(W, H, D);

    float Perlin = CloudPerlinFBM(UVW, 4.0);
    float Worley = CloudWorleyFBM(UVW, 4.0);

    // Worley as Perlin's noise floor turns smooth blobs into the connected billows a cloud base has.
    float PerlinWorley = saturate(CloudRemap(Perlin, Worley - 1.0, 1.0, 0.0, 1.0));

    uNoise[DTid] = float4(PerlinWorley,
                          CloudWorleyFBM(UVW, 8.0),
                          CloudWorleyFBM(UVW, 16.0),
                          CloudWorleyFBM(UVW, 32.0));
}
