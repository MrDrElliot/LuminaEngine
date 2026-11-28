#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Weather.slang"
#include "Includes/RainCommon.slang"

struct FPushConstants
{
    GPUSpan<FRainDrip> Drips;
    uint               Cursor;
    uint               Count;
    uint               FrameSeed;
    // Share of the occlusion map's half width that drips are looked for in, so they gather near the camera.
    float              SearchExtent;
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// How far below an edge the next surface must lie for water to fall off it rather than run on.
static const float kDripMinDrop    = 0.3;
static const float kDripEdgeTexels = 2.0;

float3 OcclusionWorld(FWeatherData W, float2 UV, float Depth)
{
    const float4 World = mul(W.RainInverseViewProjection, float4(UV * 2.0 - 1.0, Depth, 1.0));
    return World.xyz / World.w;
}

// Picks random texels of the rain map and keeps those on an edge the rain falls past, where water gathers and drips.
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= Pass().Count || !HasWeather() || Pass().Drips.IsEmpty())
    {
        return;
    }

    const FWeatherData W = GetWeather();
    if (!W.RainOcclusion.IsValid())
    {
        return;
    }

    uint Width, Height;
    W.RainOcclusion.GetDimensions(Width, Height);
    const float2 Texel = 1.0 / float2(Width, Height);

    uint Seed = PCGHash(Pass().FrameSeed ^ (DTid.x * 0x9E3779B9u));
    const float2 UV = 0.5 + (float2(WeatherRandom(Seed), WeatherRandom(Seed)) - 0.5) * Pass().SearchExtent;
    const float  Depth = W.RainOcclusion.SampleLevel(Samplers::PointClamp, UV, 0.0).r;
    if (Depth >= 0.9999)
    {
        return;
    }

    // The neighbor the map sees deepest along the fall, which is where the water leaves this surface.
    const float2 Offsets[4] = { float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0) };
    float  Deepest = Depth;
    float2 Toward  = float2(0.0, 0.0);
    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        const float Neighbor = W.RainOcclusion.SampleLevel(Samplers::PointClamp, UV + Offsets[i] * Texel * kDripEdgeTexels, 0.0).r;
        if (Neighbor > Deepest)
        {
            Deepest = Neighbor;
            Toward  = Offsets[i];
        }
    }
    if ((Deepest - Depth) * W.OcclusionDepthRange < kDripMinDrop)
    {
        return;
    }

    const float3 Origin  = OcclusionWorld(W, UV + Toward * Texel * 0.5, Depth);
    const float3 Landing = OcclusionWorld(W, UV + Toward * Texel * kDripEdgeTexels, min(Deepest, 0.9999));
    const float  Fall    = Origin.y - Landing.y;
    if (Fall < kDripMinDrop || length(Origin - GetCameraPosition()) > W.RainTint.w)
    {
        return;
    }

    FRainDrip Drip;
    Drip.Origin        = Origin - float3(0.0, 0.01, 0.0);
    Drip.SpawnTime     = W.Surface2.z - WeatherRandom(Seed) * kDripSiteLifetime * 0.5;
    Drip.FallHeight    = Fall;
    const float3 ToEye = normalize(GetCameraPosition() - Origin);
    const float3 Light = RainLightAt(Origin - float3(0.0, 0.05, 0.0), ToEye, W.RainOptics.y);
    Drip.PackedLightRG = PackHalf2(Light.rg);
    Drip.PackedLightB  = PackHalf(Light.b);
    Drip.Seed          = PCGHash(Seed) | 1u;

    Pass().Drips.Set((Pass().Cursor + DTid.x) % Pass().Drips.Count, Drip);
}
