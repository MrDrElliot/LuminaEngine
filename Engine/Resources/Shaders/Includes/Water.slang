#pragma once
#include "SceneGlobals.slang"

// Shared water data + Gerstner wave evaluation for WaterVert/WaterPixel. The water pass binds no private
// set: the per-body array arrives by device address and the scene color/depth by bindless index, all in
// the push constant. FGPUWater must stay byte-identical to FGPUWater (SceneRenderTypes.h).

struct FGPUWater
{
    float4x4 WaterToWorld;     // local plane [-0.5,0.5] (XZ) -> world (Extent baked into XZ scale)
    float4x4 WorldToWater;     // inverse
    float4   ShallowColor;     // rgb shallow tint
    float4   DeepColor;        // rgb deep tint
    float4   FoamColor;        // rgb foam tint
    float4   WindAndWave;      // xy = wind dir, z = wind speed, w = wave amplitude
    float4   WaveParams;       // x = choppiness, y = wave scale, z = wave count, w = detail strength
    float4   RefractReflect;   // x = refraction, y = reflection, z = roughness, w = fresnel power
    float4   FoamAbsorb;       // x = shoreline foam width, y = crest foam amount, z = depth fade, w = absorption
    float4   SSRSpecOpacity;   // x = ssr max dist, y = ssr step count, z = specular intensity, w = opacity
    float4   DetailParams;     // x = detail tile size in meters, y = detail scroll speed, z = foam tiling, w = foam intensity
    uint     DetailNormalIndex;
    uint     FoamTextureIndex;
    uint     GridResolution;
    float    HorizonScale;     // outer ring's distance as a multiple of the body's, 1 when it has no horizon skirt
};

// Per-pass push (identical VS+PS so they share one range). The water-body index arrives via
// SV_VulkanInstanceID (the draw's FirstInstance), never the push constant.
struct FWaterPushConstants
{
    GPUSpan<FGPUWater> Waters;        // the per-frame water array
    uint       SceneColorIndex; // bindless 2D SRV: HDR scene copy (refraction + SSR color)
    uint       SceneDepthIndex; // bindless 2D SRV: opaque scene depth (raw reverse-Z NDC [0,1])

    // The aerial perspective LUT, with the in-scatter index at WATER_INVALID_TEX when the view has none.
    uint       AerialInScatterIndex;
    uint       AerialTransmittanceIndex;
    float      AerialRange;
    float      AerialIntensity;
};
FWaterPushConstants* Pass() { return GetPassArgs<FWaterPushConstants>(); }

static const uint  WATER_INVALID_TEX = 0xFFFFFFFFu;

// BuoyancySystem.cpp evaluates the same geometry waves on the CPU, so a change to the series must be made there too.
static const int   WATER_MAX_WAVES        = 40;
static const float WATER_LENGTH_RATIO     = 0.86;

// Amplitude falls a little faster than wavelength, so fine ripples are gentler than the swell as on a real sea.
static const float WATER_AMPLITUDE_POWER  = 1.25;
static const float WATER_MAX_STEEPNESS    = 0.42;

struct FWaterWave
{
    float2 Direction;
    float  K;          // wavenumber
    float  Amplitude;
    float  Omega;      // angular frequency, deep-water dispersion
    float  Phase;
    float  Chop;       // Gerstner Q, zero for the detail waves that only shade
};

// The waves the mesh carries; the ones after these only add ripples to the shading normal.
int WaterGeometryWaveCount(FGPUWater W)
{
    return (int)clamp(W.WaveParams.z, 1.0, 8.0) * 3;
}

FWaterWave GetWaterWave(FGPUWater W, int Index, int GeometryCount)
{
    float2 Wind  = normalize(W.WindAndWave.xy + float2(1e-5, 0.0));
    float  Ratio = pow(WATER_LENGTH_RATIO, float(Index));

    // Hashed directions and phases, since a regular fan of waves interferes into visible stripes.
    float Spread = 1.0 + 1.6 * saturate(float(Index) / 15.0);
    float Angle  = (frac(float(Index) * 0.6180339887 + 0.13) - 0.5) * Spread;
    float ca = cos(Angle);
    float sa = sin(Angle);

    FWaterWave Wave;
    Wave.Direction = float2(Wind.x * ca - Wind.y * sa, Wind.x * sa + Wind.y * ca);
    Wave.K         = 2.0 * PI / (max(W.WaveParams.y, 0.5) * Ratio);
    Wave.Amplitude = min(W.WindAndWave.w * pow(Ratio, WATER_AMPLITUDE_POWER), WATER_MAX_STEEPNESS / Wave.K);
    Wave.Omega     = sqrt(9.81 * Wave.K) * (0.5 + 0.5 * saturate(W.WindAndWave.z * 0.1));
    Wave.Phase     = frac(float(Index) * 0.7548776662 + 0.31) * 2.0 * PI;

    // Normalized over the geometry waves so their summed crests never loop back on themselves.
    Wave.Chop = Index < GeometryCount ? W.WaveParams.x / max(Wave.K * Wave.Amplitude * float(GeometryCount), 1e-3) : 0.0;
    return Wave;
}

float WaterWavePhase(FWaterWave Wave, float2 WorldXZ, float Time)
{
    return Wave.K * dot(Wave.Direction, WorldXZ) - Wave.Omega * Time + Wave.Phase;
}

// Waves shorter than a few grid cells are left to the pixel shader, since a grid that cannot hold one aliases it into false swells.
float3 WaterDisplacement(float2 WorldXZ, FGPUWater W, float Time, float CellSize)
{
    int    Count = WaterGeometryWaveCount(W);
    float3 Disp  = float3(0.0, 0.0, 0.0);

    [loop]
    for (int i = 0; i < Count; ++i)
    {
        FWaterWave Wave = GetWaterWave(W, i, Count);
        float Fade = smoothstep(2.0, 4.0, (2.0 * PI / Wave.K) / max(CellSize, 1e-3));
        if (Fade <= 0.0)
        {
            break;
        }

        float Phi = WaterWavePhase(Wave, WorldXZ, Time);
        float S = sin(Phi);
        float C = cos(Phi);
        Disp += Fade * float3(Wave.Chop * Wave.Amplitude * Wave.Direction.x * C,
                              Wave.Amplitude * S,
                              Wave.Chop * Wave.Amplitude * Wave.Direction.y * C);
    }
    return Disp;
}

struct FWaterSurface
{
    float3 Normal;
    float  Jacobian;         // horizontal displacement determinant; < 1 compressing, < 0 folded over
    float  FilteredVariance; // slope variance of the waves too fine for this pixel, which widens the specular lobe
};

// Filtered to the pixel footprint, so a wave smaller than a pixel becomes roughness rather than noise.
FWaterSurface EvaluateWaterSurface(float2 WorldXZ, FGPUWater W, float Time, float Footprint)
{
    int   GeometryCount = WaterGeometryWaveCount(W);
    float SumNx = 0.0;
    float SumNy = 0.0;
    float SumNz = 0.0;
    float Jxx = 0.0;
    float Jzz = 0.0;
    float Jxz = 0.0;
    float Variance = 0.0;

    [loop]
    for (int i = 0; i < WATER_MAX_WAVES; ++i)
    {
        FWaterWave Wave  = GetWaterWave(W, i, GeometryCount);
        float      Slope = Wave.K * Wave.Amplitude;
        float      Fade  = smoothstep(1.5, 3.0, (2.0 * PI / Wave.K) / max(Footprint, 1e-4));
        Variance += 0.5 * Slope * Slope * (1.0 - Fade);
        if (Fade <= 0.0)
        {
            continue;
        }

        float Phi = WaterWavePhase(Wave, WorldXZ, Time);
        float S = sin(Phi);
        float C = cos(Phi);
        float WA = Slope * Fade;
        SumNx += Wave.Direction.x * WA * C;
        SumNz += Wave.Direction.y * WA * C;
        SumNy += Wave.Chop * WA * S;

        float QAk = Wave.Chop * WA * S;
        Jxx += QAk * Wave.Direction.x * Wave.Direction.x;
        Jzz += QAk * Wave.Direction.y * Wave.Direction.y;
        Jxz += QAk * Wave.Direction.x * Wave.Direction.y;
    }

    FWaterSurface Surface;
    Surface.Normal = normalize(float3(-SumNx, 1.0 - SumNy, -SumNz));
    Surface.Jacobian = (1.0 - Jxx) * (1.0 - Jzz) - Jxz * Jxz;
    Surface.FilteredVariance = Variance;
    return Surface;
}
