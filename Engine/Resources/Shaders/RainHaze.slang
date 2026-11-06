#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Weather.slang"

struct FPushConstants
{
    FRWTexture2D HDR;
    uint         Width;
    uint         Height;
    uint         _Pad0;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

static const uint  kHazeSamples        = 4u;
static const float kHazePerMeter       = 0.004;
static const float kHazeMaxOpacity     = 0.65;
static const float kSkyHazeMaxOpacity  = 0.35;
static const float kHazeSkyDistance    = 1500.0;
static const float kHazeTraceDistance  = 60.0;
static const float kHazeLightGain      = 1.5;

// Veils whatever lies past the drawn drops in falling rain, thinned where the view passes under cover.
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= Pass().Width || DTid.y >= Pass().Height || !HasWeather())
    {
        return;
    }

    const FWeatherData W = GetWeather();
    const float Density = W.RainLook.w * W.RainDrops.x;
    if (Density <= 0.0)
    {
        return;
    }

    const int2   Pixel  = int2(DTid.xy);
    const float  Raw    = Scene().SceneDepth.Load(int3(Pixel, 0)).r;
    const bool   bSky   = Raw <= 0.0;
    const float2 UV     = (float2(Pixel) + 0.5) / float2(Pass().Width, Pass().Height);
    const float3 Camera = GetCameraPosition();
    const float3 Far    = ReconstructWorldPos(UV, bSky ? 1e-4 : Raw);
    const float3 ToFar  = Far - Camera;
    const float3 Dir    = normalize(ToFar);
    const float  Dist   = bSky ? kHazeSkyDistance : length(ToFar);

    const float Begin = W.RainTint.w * 0.4;
    const float Path  = max(Dist - Begin, 0.0);
    if (Path <= 0.0)
    {
        return;
    }

    const float Traced = min(Path, kHazeTraceDistance);
    float Open = 0.0;
    [unroll]
    for (uint i = 0u; i < kHazeSamples; ++i)
    {
        Open += RainExposure(W, Camera + Dir * (Begin + Traced * (float(i) + 0.5) / float(kHazeSamples)));
    }
    Open /= float(kHazeSamples);

    const float Amount = min(1.0 - exp(-Path * Open * Density * kHazePerMeter), bSky ? kSkyHazeMaxOpacity : kHazeMaxOpacity);
    const float3 HazeColor = GetVolumetricAmbientRadiance() * kHazeLightGain * W.RainTint.rgb;

    const float4 Current = Pass().HDR[Pixel];
    Pass().HDR[Pixel] = float4(lerp(Current.rgb, HazeColor, saturate(Amount)), Current.a);
}
