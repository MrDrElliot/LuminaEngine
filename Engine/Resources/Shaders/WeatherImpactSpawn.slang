#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Weather.slang"
#include "Includes/RainCommon.slang"

// Mirrors FWeatherImpactGPU in SceneRenderTypes.h.
struct FWeatherImpact
{
    float3 Position;
    float  Scale;
    float3 Normal;
    uint   Surface;
    uint   Seed;
    uint   _Pad0;
    uint   _Pad1;
    uint   _Pad2;
};

struct FPushConstants
{
    GPUSpan<FWeatherImpact> Impacts;
    GPUSpan<FRainSplash>    Splashes;
    uint                    Cursor;
    float                   Time;
    uint                    _Pad0;
    uint                    _Pad1;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

static const uint kSurfaceWater = 2u;
// Below this much water a footfall only darkens the ground, it throws nothing.
static const float kMinSplashWater = 0.05;

// How much water lies where a foot came down, which only the GPU knows exactly since puddles are shaded here.
float WaterUnderImpact(FWeatherImpact Impact)
{
    if (Impact.Surface == kSurfaceWater)
    {
        return 1.0;
    }
    if (!HasWeather())
    {
        return 0.0;
    }

    const FWeatherData W = GetWeather();
    const float Exposure = RainExposure(W, Impact.Position, true);
    float Flat       = saturate((Impact.Normal.y - 0.9) / 0.08);
    float Depression = 0.0;
    const FRainGroundShape Ground = RainGroundShape(W, Impact.Position);
    if (Ground.bValid)
    {
        Flat       = saturate((Ground.Normal.y - 0.97) / 0.025);
        Depression = Ground.Depression;
    }

    // Soaked ground without a puddle still flicks up a little water.
    const float Film = W.Surface.x * Exposure * 0.25;
    return max(PuddleAmountAt(W, Impact.Position, Flat, Depression, Exposure), Film);
}

// Turns this frame's impacts into splashes on the interaction ring, scaled by how much water each one hit.
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (!Pass().Impacts.Contains(DTid.x) || Pass().Splashes.IsEmpty())
    {
        return;
    }

    const FWeatherImpact Impact = Pass().Impacts.Get(DTid.x);
    const float Water = WaterUnderImpact(Impact);

    FRainSplash Splash;
    Splash.Position      = Impact.Position;
    Splash.SpawnTime     = Pass().Time;
    Splash.PackedNormal  = PackUnitVector(Impact.Surface == kSurfaceWater ? float3(0.0, 1.0, 0.0) : normalize(Impact.Normal));
    Splash.PackedLightRG = 0u;
    Splash.PackedLightB  = 0u;
    Splash.Seed          = 0u;

    if (Water >= kMinSplashWater)
    {
        const float3 Normal = UnpackUnitVector(Splash.PackedNormal);
        const float3 Light  = RainLightAt(Impact.Position + Normal * 0.1);
        const float  Scale  = Impact.Scale * lerp(0.35, 1.0, saturate(Water)) * (Impact.Surface == kSurfaceWater ? 1.0 : 0.55);
        Splash.PackedLightRG = PackHalf2(Light.rg);
        Splash.PackedLightB  = f32tof16(Light.b) | (f32tof16(Scale) << 16u);
        Splash.Seed          = Impact.Seed | 1u;
    }

    Pass().Splashes.Set((Pass().Cursor + DTid.x) % Pass().Splashes.Count, Splash);
}
