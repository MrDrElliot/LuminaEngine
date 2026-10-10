#pragma once
#include "SceneGlobals.slang"

// Every pass that reconstructs geometry must displace identically, so the LOD gate keys off the main camera.

struct FWindResult
{
    float3 Offset;   // World-space displacement, added as WorldPositionOffset
    float  Weight;   // Combined LOD and mask weight, 0 when wind is culled
    float  Noise;    // Raw field in [-1, 1] before mask and LOD scaling
};

// Incommensurate, non-axis-aligned directions so the waves never re-phase into a visible grid.
float WindWave(float3 P, float T)
{
    float A = sin(dot(P, float3(0.71, 0.33, 0.62)) + T);
    float B = sin(dot(P, float3(-0.41, 0.55, 0.73)) + T * 1.37 + 2.1);
    return (A + B) * 0.5;
}

// Low octaves sway and high ones flutter, and each advances faster so the detail does not look painted on.
float WindTurbulence(float3 P, float T, float Frequency, float Lacunarity, float Gain, int Octaves, float Phase)
{
    float Sum  = 0.0;
    float Norm = 0.0;
    float Amp  = 1.0;
    float Freq = Frequency;

    for (int i = 0; i < Octaves; ++i)
    {
        Sum  += Amp * WindWave(P * Freq, T * (1.0 + float(i) * 0.63) + Phase);
        Norm += Amp;
        Freq *= Lacunarity;
        Amp  *= Gain;
    }

    return Sum / max(Norm, 1e-4);
}

// Faded rather than snapped, since a hard cutoff tears triangles on a mesh straddling the boundary.
float WindLODWeight(float3 WorldPos, float FadeStart, float FadeEnd)
{
    float D    = distance(GetCameraPosition(), WorldPos);
    float Span = max(FadeEnd - FadeStart, 1e-3);
    return 1.0 - saturate((D - FadeStart) / Span);
}

// Mask is per-vertex stiffness (0 pins, 1 full Strength). Gusts modulate the whole displacement rather than adding.
FWindResult ComputeWind(
    float3 WorldPos,
    float3 WindDir,
    float  Strength,
    float  Speed,
    float  Frequency,
    float  Lacunarity,
    float  Gain,
    int    Octaves,
    float  Mask,
    float  Phase,
    float  Gustiness,
    float  LODWeight)
{
    FWindResult Result;
    Result.Offset = float3(0.0, 0.0, 0.0);
    Result.Weight = 0.0;
    Result.Noise  = 0.0;

    float W = saturate(LODWeight) * max(Mask, 0.0);
    if (W <= 0.0)
    {
        return Result;
    }

    float T = MaterialTime() * Speed;

    // Horizontal only, since a vertical component drives vertices through the ground rather than bending them.
    float3 Dir     = float3(WindDir.x, 0.0, WindDir.z);
    float  DirLen2 = dot(Dir, Dir);
    Dir = DirLen2 > 1e-6 ? Dir * rsqrt(DirLen2) : float3(1.0, 0.0, 0.0);

    // Keying phase on dot(WorldPos, Dir) makes a field ripple in bands instead of swelling together.
    float Gust    = 0.5 + 0.5 * sin(dot(WorldPos, Dir) * Frequency * 0.25 - T * 0.6 + Phase);
    float GustAmp = lerp(1.0, Gust, saturate(Gustiness));

    Result.Noise = WindTurbulence(WorldPos, T, Frequency, Lacunarity, Gain, Octaves, Phase);

    float Bend = Result.Noise * GustAmp;

    // Bending rotates about the anchor, so a lateral push needs the matching drop or the mesh stretches.
    float Lateral = Bend;
    float Drop    = 1.0 - sqrt(saturate(1.0 - Lateral * Lateral));

    Result.Offset = (Dir * Lateral - float3(0.0, Drop, 0.0)) * Strength * W;
    Result.Weight = W;
    return Result;
}
