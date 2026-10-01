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

// Extinction per meter at full density, near real cumulus, so thin wisps stay translucent over a long march.
static const float kCloudExtinctionPerMeter = 0.04;

// The remapped base shape sits almost entirely in this band, so Coverage spans it rather than all of 0 to 1.
static const float kCoverageClearFloor    = 0.88;
static const float kCoverageOvercastFloor = 0.6;

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

// The weather layer tiles at an irrational multiple of the shape tile, so the two repeats never line up.
static const float kWeatherScaleRatio = 4.37;

// Turning the weather layer against the shape tile keeps the two lattices from sharing an axis.
static const float2x2 kWeatherRotation = float2x2(0.8572, -0.5150, 0.5150, 0.8572);

// How far the weather layer drags the base shape, as a fraction of one shape tile.
static const float kWeatherWarp = 0.3;

struct FCloudWeather
{
    float  Coverage;
    float  Billow;
    float2 Warp;
};

// Plays the part of Unreal's cloud layout map and Nubis' weather map, read from the shared noise volume instead of a texture.
FCloudWeather Cloud_SampleWeather(FCloudMedium M, float3 WorldPos)
{
    float2 Drifted = WorldPos.xz + M.WindOffset * 0.5;
    float2 P = mul(kWeatherRotation, Drifted) / max(M.ShapeScale * kWeatherScaleRatio, 1.0);
    float4 Layout = SampleTexture3DLevel(M.NoiseIndex, SAMPLER_LINEAR_WRAP, float3(P.x, 0.37, P.y), 0.0);

    float Regional = smoothstep(0.3, 0.8, Layout.g * 0.6 + Layout.b * 0.4);

    FCloudWeather Weather;
    Weather.Coverage = saturate(M.Coverage * lerp(0.25, 1.75, Regional));
    Weather.Billow   = saturate(M.Billow * lerp(0.55, 1.35, Layout.a));
    Weather.Warp     = (float2(Layout.r, Layout.a) - 0.5) * (M.ShapeScale * kWeatherWarp);
    return Weather;
}

float Cloud_SampleDensityM(FCloudMedium M, float3 WorldPos, float HeightFrac, bool bCheap)
{
    FCloudWeather Weather = Cloud_SampleWeather(M, WorldPos);
    if (Weather.Coverage <= 0.01)
    {
        return 0.0;
    }

    float2 ShapeXZ = WorldPos.xz + M.WindOffset + Weather.Warp;
    float3 ShapeUVW = float3(ShapeXZ.x, WorldPos.y, ShapeXZ.y) / max(M.ShapeScale, 1.0);

    // Explicit level, since this runs in compute and the noise volume has a single mip.
    float4 Shape = SampleTexture3DLevel(M.NoiseIndex, SAMPLER_LINEAR_WRAP, ShapeUVW, 0.0);

    // The Worley octaves ride on the base as a noise floor, which is what carves the billows.
    float LowFreq = Shape.g * 0.625 + Shape.b * 0.25 + Shape.a * 0.125;
    float Base    = saturate(Cloud_Remap(Shape.r, LowFreq - 1.0, 1.0, 0.0, 1.0));

    Base *= Cloud_HeightGradient(HeightFrac, Weather.Billow);

    // Coverage as the remap floor erodes inward, so puffs shrink rather than uniformly fading out.
    float Floor   = lerp(kCoverageClearFloor, kCoverageOvercastFloor, Weather.Coverage);
    float Density = saturate(Cloud_Remap(Base, Floor, 1.0, 0.0, 1.0));
    if (Density <= 0.0 || bCheap)
    {
        return Density * M.Density * kCloudExtinctionPerMeter;
    }

    float3 DetailUVW = (WorldPos + float3(M.DetailWindOffset.x, 0.0, M.DetailWindOffset.y))
                     / max(M.DetailScale, 1.0);

    float4 Detail    = SampleTexture3DLevel(M.NoiseIndex, SAMPLER_LINEAR_WRAP, DetailUVW, 0.0);
    float  DetailFBM = Detail.g * 0.625 + Detail.b * 0.25 + Detail.a * 0.125;

    // Wispy at the base and billowed aloft, which is the direction real erosion runs.
    float Erosion = lerp(1.0 - DetailFBM, DetailFBM, saturate(HeightFrac * 2.0));
    Density = saturate(Cloud_Remap(Density, Erosion * M.DetailStrength, 1.0, 0.0, 1.0));

    return Density * M.Density * kCloudExtinctionPerMeter;
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
