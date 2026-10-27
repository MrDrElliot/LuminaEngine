#pragma once
#include "SceneGlobals.slang"

bool HasWeather() { return !Scene().Weather.IsEmpty(); }

FWeatherData GetWeather() { return Scene().Weather.GetAligned(0u); }

uint WeatherHash(uint V)
{
    uint State = V * 747796405u + 2891336453u;
    uint Word  = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
    return (Word >> 22u) ^ Word;
}

float WeatherRandom(inout uint Seed)
{
    Seed = WeatherHash(Seed);
    return float(Seed >> 8u) * (1.0 / 16777216.0);
}

// The ripple, foam and print field at one point, faded to nothing toward the window's edge so it never ends in a seam.
struct FWeatherFieldSample
{
    // Height gradient along world x and z.
    float2 Slope;
    // Print gradient along world x and z, which tilts the walls of a footprint.
    float2 TrailSlope;
    float  Foam;
    float  Trail;
    float  Weight;
};

FWeatherFieldSample EmptyWeatherField()
{
    FWeatherFieldSample Sample;
    Sample.Slope      = float2(0.0, 0.0);
    Sample.TrailSlope = float2(0.0, 0.0);
    Sample.Foam   = 0.0;
    Sample.Trail  = 0.0;
    Sample.Weight = 0.0;
    return Sample;
}

FWeatherFieldSample SampleWeatherField(FWeatherData W, float2 WorldXZ)
{
    FWeatherFieldSample Sample = EmptyWeatherField();
    if (!W.InteractionField.IsValid())
    {
        return Sample;
    }

    const float2 UV = (WorldXZ - W.FieldParams.xy) * W.FieldParams.z;
    if (any(UV <= 0.0) || any(UV >= 1.0))
    {
        return Sample;
    }

    constexpr float kEdgeFade = 10.0;
    const float2 Edge  = min(UV, 1.0 - UV);
    Sample.Weight = saturate(min(Edge.x, Edge.y) * kEdgeFade);

    const float  Texel   = W.FieldParams.w;
    const float  TexelUV = Texel * W.FieldParams.z;
    const float4 Center  = W.InteractionField.SampleLevel(Samplers::LinearClamp, UV, 0.0);
    const float4 Left    = W.InteractionField.SampleLevel(Samplers::LinearClamp, UV - float2(TexelUV, 0.0), 0.0);
    const float4 Right   = W.InteractionField.SampleLevel(Samplers::LinearClamp, UV + float2(TexelUV, 0.0), 0.0);
    const float4 Back    = W.InteractionField.SampleLevel(Samplers::LinearClamp, UV - float2(0.0, TexelUV), 0.0);
    const float4 Front   = W.InteractionField.SampleLevel(Samplers::LinearClamp, UV + float2(0.0, TexelUV), 0.0);

    const float Gradient = Sample.Weight / (2.0 * Texel);
    Sample.Slope      = float2(Right.r - Left.r, Front.r - Back.r) * Gradient;
    Sample.TrailSlope = float2(Right.a - Left.a, Front.a - Back.a) * Gradient;
    Sample.Foam   = Center.b * Sample.Weight;
    Sample.Trail  = Center.a * Sample.Weight;
    return Sample;
}

// Matches InteractorHash in SceneRenderer_Weather.cpp, which fills the table this probes.
uint InteractorHash(uint X)
{
    X ^= X >> 16u;
    X *= 0x7FEB352Du;
    X ^= X >> 15u;
    X *= 0x846CA68Bu;
    X ^= X >> 16u;
    return X;
}

static const uint kInteractorMaxProbes = 16u;

struct FInteractorWetness
{
    float Wetness;
    bool  bFound;
};

// How wet the entity a surface belongs to is, from the rain it soaked up and the water line it waded to.
FInteractorWetness InteractorWetness(FWeatherData W, uint EntityID, float WorldY)
{
    FInteractorWetness Result;
    Result.Wetness = 0.0;
    Result.bFound  = false;
    if (W.InteractorTable.IsEmpty())
    {
        return Result;
    }

    uint Slot = InteractorHash(EntityID) & W.InteractorTableMask;
    [loop]
    for (uint Probe = 0u; Probe < kInteractorMaxProbes; ++Probe)
    {
        const uint2 Entry = W.InteractorTable.Get(Slot);
        if (Entry.x == EntityID)
        {
            // A few centimeters of soft edge, so the line reads as cloth wicking rather than a painted stripe.
            constexpr float kWaterlineSoftness = 30.0;
            const float4 Record = W.Interactors.Get(Entry.y);
            const float  Below  = saturate((Record.x + Record.z - WorldY) * kWaterlineSoftness + 0.5);
            Result.Wetness = max(Record.y, Record.w * Below);
            Result.bFound  = true;
            return Result;
        }
        if (Entry.x == 0xFFFFFFFFu)
        {
            return Result;
        }
        Slot = (Slot + 1u) & W.InteractorTableMask;
    }
    return Result;
}
