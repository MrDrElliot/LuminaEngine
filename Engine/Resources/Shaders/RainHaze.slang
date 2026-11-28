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
// Distant glowing rain is mostly dark gaps between drops, so the veil carries only a share of their emission.
static const float kHazeEmissiveShare  = 0.2;

// Rain past the drawn drops shows as sheets of streaks on cylinders around the camera, each a multiple of the drop radius out.
static const uint  kCurtainShells         = 3u;
static const float kCurtainShellRadius[3] = { 1.15, 2.1, 3.8 };
// Meters between streak columns on the nearest shell, wider on farther shells so all of them stay a few pixels apart.
static const float kCurtainColumnWorld    = 0.11;
static const float kCurtainPeriodWorld    = 2.2;
static const float kCurtainStreakShare    = 0.5;
static const float kCurtainOpacity        = 0.22;

float CurtainHash(float Column, uint Shell, uint Salt)
{
    return float(PCGHash(asuint(Column) * 747796405u ^ (Shell * 2891336453u + Salt)) >> 8u) * (1.0 / 16777216.0);
}

// Coverage of the streaks a view ray crosses on one shell, 0 where the scene is nearer or the shell lies under cover.
float CurtainShell(FWeatherData W, float3 Camera, float3 Dir, float SceneDist, uint Shell, float Intensity)
{
    const float Radius   = W.RainTint.w * kCurtainShellRadius[Shell];
    const float Flat     = length(Dir.xz);
    if (Flat < 0.1)
    {
        return 0.0;
    }
    const float T = Radius / Flat;
    if (T >= SceneDist)
    {
        return 0.0;
    }

    const float3 P       = Camera + Dir * T;
    const float2 Around  = Dir.xz / Flat;
    const float2 Tangent = float2(-Around.y, Around.x);
    const float  Scale   = kCurtainShellRadius[Shell] / kCurtainShellRadius[0];

    // Streaks lean with the wind across the view and slide down at the fall speed, carried sideways by the integrated wind.
    const float  Fall    = max(W.RainFall.w, 0.5);
    const float  Lean    = dot(W.Motion.xy * W.RainWind.z, Tangent) / Fall;
    const float  Height  = P.y - Camera.y;
    const float  Across  = atan2(Around.y, Around.x) * Radius - dot(W.RainWind.xy * W.RainWind.z, Tangent) - Height * Lean;
    const float  Along   = Height + W.Motion.z;

    const float Column   = floor(Across / (kCurtainColumnWorld * Scale));
    const float InColumn = frac(Across / (kCurtainColumnWorld * Scale));
    if (CurtainHash(Column, Shell, 0u) > Intensity * 0.8 + 0.1)
    {
        return 0.0;
    }

    const float Offset = 0.5 + (CurtainHash(Column, Shell, 1u) - 0.5) * 0.6;
    const float Width  = 0.11;
    const float Profile = exp(-pow((InColumn - Offset) / Width, 2.0));

    const float Speed  = 0.85 + 0.3 * CurtainHash(Column, Shell, 2u);
    const float Phase  = frac(Along * Speed / (kCurtainPeriodWorld * Scale) + CurtainHash(Column, Shell, 3u));
    const float Streak = smoothstep(0.0, 0.08, Phase) * (1.0 - smoothstep(kCurtainStreakShare * 0.6, kCurtainStreakShare, Phase));

    return Profile * Streak * RainExposure(W, P);
}

// Sunlight the rain scatters toward the eye along Dir, its forward lobe glowing around the sun.
float3 RainSunScatter(FWeatherData W, float3 Dir)
{
    if (!HasSunLight())
    {
        return float3(0.0, 0.0, 0.0);
    }
    const FLight Sun  = GetLightAt(0);
    const float  Lobe = 1.0 + W.RainOptics.y * ForwardScatterRatio(dot(-normalize(GetSunDirection()), -Dir));
    return UnpackColor(Sun.Color).rgb * Sun.Intensity * (0.25 * INV_PI) * Lobe * SampleDirectionalCloudShadow(GetCameraPosition());
}

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
    if (Density <= 0.0 && W.RainOptics.z * W.RainDrops.x <= 0.0)
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

    // Lit like the drops, so a storm's veil and its curtains glow toward the sun and take the water's color, with a little of their own glow.
    const float3 Light = (GetVolumetricAmbientRadiance() * kHazeLightGain + RainSunScatter(W, Dir)) * W.RainTint.rgb * W.RainLook.y + W.RainEmissive.rgb * kHazeEmissiveShare;

    const float Traced = min(Path, kHazeTraceDistance);
    float Open = 0.0;
    [unroll]
    for (uint i = 0u; i < kHazeSamples; ++i)
    {
        Open += RainExposure(W, Camera + Dir * (Begin + Traced * (float(i) + 0.5) / float(kHazeSamples)));
    }
    Open /= float(kHazeSamples);

    const float Amount = min(1.0 - exp(-Path * Open * Density * kHazePerMeter), bSky ? kSkyHazeMaxOpacity : kHazeMaxOpacity);

    float Curtain = 0.0;
    const float CurtainDensity = W.RainOptics.z * W.RainDrops.x;
    if (CurtainDensity > 0.0)
    {
        [unroll]
        for (uint Shell = 0u; Shell < kCurtainShells; ++Shell)
        {
            // Farther shells sit deeper in the veil, so they read fainter as the haze closes in.
            const float Depth = W.RainTint.w * kCurtainShellRadius[Shell];
            const float Veil  = exp(-Depth * Density * kHazePerMeter);
            Curtain += CurtainShell(W, Camera, Dir, Dist, Shell, W.RainDrops.x) * Veil;
        }
        Curtain = saturate(Curtain * CurtainDensity * kCurtainOpacity);
    }

    const float4 Current = Pass().HDR[Pixel];
    float3 Color = lerp(Current.rgb, Light, saturate(Amount));
    Color = lerp(Color, Light, Curtain);
    Pass().HDR[Pixel] = float4(Color, Current.a);
}
