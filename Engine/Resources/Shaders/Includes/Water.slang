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
    float4   DetailParams;     // x = detail tiling, y = detail scroll speed, z = foam tiling, w = unused
    uint     DetailNormalIndex;
    uint     FoamTextureIndex;
    uint     GridResolution;
    uint     Flags;
};

// Per-pass push (identical VS+PS so they share one range). The water-body index arrives via
// SV_VulkanInstanceID (the draw's FirstInstance), never the push constant.
struct FWaterPushConstants
{
    FGPUWater* Waters;        // device address of the per-frame water array
    uint       SceneColorIndex; // bindless 2D SRV: HDR scene copy (refraction + SSR color)
    uint       SceneDepthIndex; // bindless 2D SRV: opaque scene depth (raw reverse-Z NDC [0,1])
};
FWaterPushConstants* Pass() { return GetPassArgs<FWaterPushConstants>(); }

static const uint  WATER_INVALID_TEX = 0xFFFFFFFFu;

struct FGerstnerResult
{
    float3 Displacement;   // world-space offset added to the flat grid position
    float3 Normal;         // analytic surface normal (GPU Gems closed form)
};

// Sum up to WaveCount Gerstner waves fanned around the wind direction, each octave shorter / lower / faster.
// Returns the world displacement and the exact surface normal so the silhouette and shading agree.
FGerstnerResult EvaluateGerstner(float2 WorldXZ, FGPUWater W, float Time)
{
    float2 WindDir   = normalize(W.WindAndWave.xy + float2(1e-5, 0.0));
    float  WindSpeed = W.WindAndWave.z;
    float  BaseAmp   = W.WindAndWave.w;
    float  Choppy    = W.WaveParams.x;
    float  Scale     = max(W.WaveParams.y, 0.05);
    int    Count     = (int)clamp(W.WaveParams.z, 1.0, 8.0);

    float3 Disp  = float3(0.0, 0.0, 0.0);
    float  SumNx = 0.0;
    float  SumNy = 0.0;
    float  SumNz = 0.0;

    float Wavelength = max(8.0 * Scale, 0.5);
    float Amplitude  = BaseAmp;
    float SpeedScale = 1.0;
    const float AngleStep = 0.45;

    [loop]
    for (int i = 0; i < 8; ++i)
    {
        if (i >= Count)
        {
            break;
        }

        float  Angle = AngleStep * (float(i) - 0.5 * float(Count - 1));
        float  ca = cos(Angle);
        float  sa = sin(Angle);
        float2 D  = float2(WindDir.x * ca - WindDir.y * sa, WindDir.x * sa + WindDir.y * ca);

        float k     = 2.0 * PI / Wavelength;
        float Speed = sqrt(9.81 / max(k, 1e-3)) * (0.5 + 0.5 * saturate(WindSpeed * 0.1)) * SpeedScale;
        float Phi   = k * dot(D, WorldXZ) + Time * Speed;
        float S     = sin(Phi);
        float C     = cos(Phi);

        // Steepness normalized by (k*A*waveCount) so summed peaks never loop back on themselves.
        float Q  = Choppy / max(k * Amplitude * float(Count), 1e-3);
        float WA = k * Amplitude;

        Disp  += float3(Q * Amplitude * D.x * C, Amplitude * S, Q * Amplitude * D.y * C);
        SumNx += D.x * WA * C;
        SumNz += D.y * WA * C;
        SumNy += Q * WA * S;

        Wavelength *= 0.62;
        Amplitude  *= 0.62;
        SpeedScale *= 1.16;
    }

    FGerstnerResult R;
    R.Displacement = Disp;
    R.Normal = normalize(float3(-SumNx, 1.0 - SumNy, -SumNz));
    return R;
}
