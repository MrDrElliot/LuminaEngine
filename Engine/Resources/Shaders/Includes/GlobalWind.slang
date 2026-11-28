#pragma once
#include "SceneGlobals.slang"

// The weather's wind at a point, gusting as each gust front sweeps along the wind and dying away inside blocker boxes.
struct FGlobalWind
{
    // Unit direction on the XZ plane the air moves toward, including the gust's veer.
    float3 Direction;
    // Meters per second at this point, gusts and shelter included.
    float  Speed;
    // How far the passing gust pushes the speed above or below the steady wind, roughly -1 to 1.
    float  Gust;
    // 1 deep inside a blocker that stops all wind, 0 in the open.
    float  Shelter;
    float3 Velocity;
};

// The twin of GustNoise in WeatherSystem.cpp, so the camera's gust here matches the one the rain and clouds use.
float GlobalWindGustNoise(float T)
{
    return 0.6 * sin(TWO_PI * T) + 0.3 * sin(TWO_PI * 2.71 * T + 1.3) + 0.1 * sin(TWO_PI * 6.13 * T + 0.4);
}

float GlobalWindShelter(float3 WorldPos)
{
    const GPUSpan<FWindBlocker> Blockers = Scene().WindBlockers;
    float Shelter = 0.0;
    [loop]
    for (uint i = 0u; i < Blockers.Count; ++i)
    {
        const FWindBlocker B   = Blockers.Get(i);
        const float3       Box = float3(dot(B.WorldToBoxX.xyz, WorldPos) + B.WorldToBoxX.w,
                                        dot(B.WorldToBoxY.xyz, WorldPos) + B.WorldToBoxY.w,
                                        dot(B.WorldToBoxZ.xyz, WorldPos) + B.WorldToBoxZ.w);
        // Negative inside, how far the point sits in from the nearest face.
        const float3 Past  = abs(Box) - B.HalfExtent.xyz;
        const float  Depth = -max(Past.x, max(Past.y, Past.z));
        Shelter = max(Shelter, saturate(Depth / max(B.HalfExtent.w, 1e-3)) * B.Params.x);
    }
    return Shelter;
}

FGlobalWind SampleGlobalWind(float3 WorldPos)
{
    const float4 Steady = Scene().WindDirection;
    const float4 Gusts  = Scene().WindGust;
    const float  Speed  = Steady.z;

    // A gust reaches points downwind later, so each one rolls across the world at the speed of the wind.
    const float Phase = Gusts.x - dot(WorldPos.xz, Steady.xy) * Gusts.y / max(Speed, 0.5);
    const float Noise = GlobalWindGustNoise(Phase);
    const float Veer  = Steady.w * 0.3 * GlobalWindGustNoise(Phase * 0.37 + 11.0);
    const float2 Dir  = float2(Steady.x * cos(Veer) - Steady.y * sin(Veer), Steady.x * sin(Veer) + Steady.y * cos(Veer));

    FGlobalWind Wind;
    Wind.Shelter   = GlobalWindShelter(WorldPos);
    const float Open = 1.0 - Wind.Shelter;
    Wind.Direction = float3(Dir.x, 0.0, Dir.y);
    Wind.Speed     = max(Speed * (1.0 + Steady.w * Noise), 0.0) * Open;
    Wind.Gust      = Steady.w * Noise * Open;
    Wind.Velocity  = Wind.Direction * Wind.Speed;
    return Wind;
}
