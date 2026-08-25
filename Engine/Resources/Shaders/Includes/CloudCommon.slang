#pragma once
#include "GlobalRHI.slang"

// Shared so the lit clouds and the shadow they cast can never describe different weather.
struct FCloudMedium
{
    uint   NoiseIndex;
    float  ShapeScale;
    float  DetailScale;
    float  DetailStrength;

    float  Billow;
    float  Coverage;
    float  Density;
    float  LayerBottom;

    float  LayerTop;
    float  _Pad0;
    float2 WindOffset;

    float2 DetailWindOffset;
    float2 _Pad1;
};

float Cloud_Remap(float V, float InMin, float InMax, float OutMin, float OutMax)
{
    return OutMin + (V - InMin) * (OutMax - OutMin) / max(InMax - InMin, 1e-5);
}

// Thin bases should not lose their vertical shape the way a full billow profile would.
float Cloud_HeightGradient(float H, float Billow)
{
    float Base = saturate(Cloud_Remap(H, 0.0, 0.15, 0.0, 1.0));
    float Top  = saturate(Cloud_Remap(H, 0.45, 1.0, 1.0, 0.0));
    float Flat = Base * Top;

    float Round = saturate(Cloud_Remap(H, 0.0, 0.25, 0.0, 1.0))
                * saturate(Cloud_Remap(H, 0.7, 1.0, 1.0, 0.0));
    return lerp(Flat, Round, Billow);
}

float Cloud_HeightFraction(FCloudMedium M, float Y)
{
    return saturate((Y - M.LayerBottom) / max(M.LayerTop - M.LayerBottom, 1.0));
}

float Cloud_SampleDensityM(FCloudMedium M, float3 WorldPos, float HeightFrac, bool bCheap)
{
    float3 ShapeUVW = (WorldPos + float3(M.WindOffset.x, 0.0, M.WindOffset.y)) / max(M.ShapeScale, 1.0);

    float4 Shape = SampleTexture3D(M.NoiseIndex, SAMPLER_LINEAR_WRAP, ShapeUVW);

    // The Worley octaves ride on the base as a noise floor, which is what carves the billows.
    float LowFreq = Shape.g * 0.625 + Shape.b * 0.25 + Shape.a * 0.125;
    float Base    = saturate(Cloud_Remap(Shape.r, LowFreq - 1.0, 1.0, 0.0, 1.0));

    Base *= Cloud_HeightGradient(HeightFrac, M.Billow);

    // Coverage as the remap floor erodes inward, so puffs shrink rather than uniformly fading out.
    float Cov     = saturate(M.Coverage);
    float Density = saturate(Cloud_Remap(Base, 1.0 - Cov, 1.0, 0.0, 1.0));
    if (Density <= 0.0 || bCheap)
    {
        return Density * M.Density;
    }

    float3 DetailUVW = (WorldPos + float3(M.DetailWindOffset.x, 0.0, M.DetailWindOffset.y))
                     / max(M.DetailScale, 1.0);

    float4 Detail    = SampleTexture3D(M.NoiseIndex, SAMPLER_LINEAR_WRAP, DetailUVW);
    float  DetailFBM = Detail.g * 0.625 + Detail.b * 0.25 + Detail.a * 0.125;

    // Wispy at the base and billowed aloft, which is the direction real erosion runs.
    float Erosion = lerp(1.0 - DetailFBM, DetailFBM, saturate(HeightFrac * 2.0));
    Density = saturate(Cloud_Remap(Density, Erosion * M.DetailStrength, 1.0, 0.0, 1.0));

    return Density * M.Density;
}

// World XZ to cloud-shadow UV. Center is the snapped camera position, HalfExtent the map's reach.
float2 CloudShadow_UV(float3 WorldPos, float2 Center, float HalfExtent)
{
    return (WorldPos.xz - Center) / max(2.0 * HalfExtent, 1e-3) + 0.5;
}

// Sun transmittance through the cloud layer above a point, 1 when no map was built this frame.
float CloudShadow_Sample(uint MapIndex, float3 WorldPos, float2 Center, float HalfExtent)
{
    if (MapIndex == 0xFFFFFFFFu)
    {
        return 1.0;
    }
    float2 UV = CloudShadow_UV(WorldPos, Center, HalfExtent);
    if (any(UV < 0.0) || any(UV > 1.0))
    {
        return 1.0;
    }
    return SampleTexture2DLevel(MapIndex, SAMPLER_LINEAR_CLAMP, UV, 0.0).r;
}
